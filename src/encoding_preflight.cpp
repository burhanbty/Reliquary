/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#include "encoding_preflight.h"

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "fast_local_codec.h"
#include "fast_local_format.h"
#include "video_encoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
    constexpr uint64_t gibibyte = 1024ull * 1024ull * 1024ull;
    constexpr uint64_t actual_encode_temporary_bytes = 0;

    uint64_t checked_add(const uint64_t left, const uint64_t right,
                         const char *message) {
        if (right > std::numeric_limits<uint64_t>::max() - left) {
            throw std::overflow_error(message);
        }
        return left + right;
    }

    uint64_t checked_multiply(const uint64_t left, const uint64_t right,
                              const char *message) {
        if (left != 0 &&
            right > std::numeric_limits<uint64_t>::max() / left) {
            throw std::overflow_error(message);
        }
        return left * right;
    }

    uint64_t checked_ceil_tenth(const uint64_t value) {
        return value / 10 + (value % 10 != 0 ? 1 : 0);
    }

    uint64_t checked_rounded(const long double value, const bool round_up) {
        if (!std::isfinite(value) || value < 0.0L ||
            value > static_cast<long double>(
                        std::numeric_limits<uint64_t>::max())) {
            throw std::overflow_error("estimated output size overflow");
        }
        return static_cast<uint64_t>(
            round_up ? std::ceil(value) : std::floor(value + 0.5L));
    }

    void append_message(std::string &destination, const std::string &message) {
        if (message.empty()) return;
        if (!destination.empty()) destination += "; ";
        destination += message;
    }

    struct InputMetadata {
        uint64_t size = 0;
        int64_t last_write_time_ticks = 0;
        uint64_t path_fingerprint = 0;
    };

    uint64_t path_fingerprint(const std::filesystem::path &path) {
        std::error_code error;
        auto normalized = std::filesystem::weakly_canonical(path, error);
        if (error) {
            error.clear();
            normalized = std::filesystem::absolute(path, error);
            if (error) throw std::filesystem::filesystem_error(
                "cannot normalize input path", path, error);
            normalized = normalized.lexically_normal();
        }
        std::string text = normalized.generic_string();
#if defined(_WIN32)
        std::transform(
            text.begin(), text.end(), text.begin(),
            [](const unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
#endif
        constexpr uint64_t offset_basis = 14695981039346656037ull;
        constexpr uint64_t prime = 1099511628211ull;
        uint64_t hash = offset_basis;
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= prime;
        }
        return hash;
    }

    InputMetadata inspect_input_metadata(
        const std::filesystem::path &input_path) {
        std::error_code error;
        const auto status = std::filesystem::status(input_path, error);
        if (error) throw std::filesystem::filesystem_error(
            "cannot inspect input file", input_path, error);
        if (!std::filesystem::is_regular_file(status)) {
            throw std::invalid_argument(
                "input path must refer to a regular file");
        }

        const uintmax_t file_size =
            std::filesystem::file_size(input_path, error);
        if (error) throw std::filesystem::filesystem_error(
            "cannot read input file size", input_path, error);
        if (file_size > std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("input file size overflow");
        }

        const auto write_time =
            std::filesystem::last_write_time(input_path, error);
        if (error) throw std::filesystem::filesystem_error(
            "cannot read input last-write time", input_path, error);
        const auto raw_ticks = write_time.time_since_epoch().count();
        const long double ticks = static_cast<long double>(raw_ticks);
        if (ticks <
                static_cast<long double>(
                    std::numeric_limits<int64_t>::min()) ||
            ticks >
                static_cast<long double>(
                    std::numeric_limits<int64_t>::max())) {
            throw std::overflow_error(
                "input last-write time is not representable");
        }

        return {
            static_cast<uint64_t>(file_size),
            static_cast<int64_t>(raw_ticks),
            path_fingerprint(input_path),
        };
    }

    void validate_output_target(
        const std::filesystem::path &input_path,
        const std::filesystem::path &output_path) {
        std::error_code error;
        if (std::filesystem::exists(output_path, error)) {
            if (error) throw std::filesystem::filesystem_error(
                "cannot inspect output path", output_path, error);
            if (std::filesystem::is_directory(output_path, error)) {
                throw std::invalid_argument(
                    "output path refers to a directory");
            }
            if (error) throw std::filesystem::filesystem_error(
                "cannot inspect output path", output_path, error);
            if (std::filesystem::equivalent(
                    input_path, output_path, error)) {
                throw std::invalid_argument(
                    "input and output paths refer to the same file");
            }
            if (error) throw std::filesystem::filesystem_error(
                "cannot compare input and output paths",
                output_path, error);
        } else if (error &&
                   error !=
                       std::errc::no_such_file_or_directory) {
            throw std::filesystem::filesystem_error(
                "cannot inspect output path", output_path, error);
        }
    }

    class TemporaryProbeFile {
    public:
        explicit TemporaryProbeFile(std::string extension) {
            static std::atomic<uint64_t> sequence{0};
            const auto temp = std::filesystem::temp_directory_path();
            if (extension.empty()) extension = ".probe";
            std::random_device random;
            for (unsigned attempt = 0; attempt < 32; ++attempt) {
                const auto ticks = std::chrono::high_resolution_clock::now()
                                       .time_since_epoch()
                                       .count();
                path_ = temp /
                        ("vidstorex-preflight-" +
                         std::to_string(static_cast<uint64_t>(ticks)) + "-" +
                         std::to_string(random()) + "-" +
                         std::to_string(sequence.fetch_add(1)) + extension);
                std::error_code error;
                if (!std::filesystem::exists(path_, error) && !error) return;
            }
            throw std::runtime_error(
                "could not allocate a unique probe file name");
        }

        ~TemporaryProbeFile() {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }

        TemporaryProbeFile(const TemporaryProbeFile &) = delete;
        TemporaryProbeFile &operator=(const TemporaryProbeFile &) = delete;

        [[nodiscard]] const std::filesystem::path &path() const {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    std::vector<std::size_t> representative_chunk_indices(
        const std::size_t chunk_count, const std::size_t sample_count) {
        if (chunk_count == 0 || sample_count == 0) return {};
        if (sample_count == 1) return {(chunk_count - 1) / 2};
        if (sample_count == 2) return {0, chunk_count - 1};
        return {0, (chunk_count - 1) / 2, chunk_count - 1};
    }

    long double student_t_95(const std::size_t sample_count) {
        const std::size_t degrees = sample_count - 1;
        if (degrees <= 1) return 12.706L;
        if (degrees == 2) return 4.303L;
        if (degrees <= 4) return 2.776L;
        if (degrees <= 9) return 2.262L;
        if (degrees <= 19) return 2.093L;
        if (degrees <= 29) return 2.045L;
        return 1.96L;
    }

    struct ProbeResult {
        uint64_t likely_bytes = 0;
        uint64_t minimum_bytes = 0;
        uint64_t maximum_bytes = 0;
        uint64_t frame_count = 0;
        bool encoded_entire_output = false;
        std::string method;
    };

    ProbeResult run_fast_local_probe(
        const EncodingPreflightRequest &request,
        const uint64_t total_frames) {
        TemporaryProbeFile temporary(".mkv");
        const uint64_t exact_frame_limit =
            std::min<uint64_t>(request.maximum_probe_frames, 12);
        if (request.encrypted &&
            total_frames <= exact_frame_limit) {
            std::array<uint64_t, 3> measured{};
            for (std::size_t index = 0;
                 index < measured.size(); ++index) {
                TemporaryProbeFile encrypted_sample(".mkv");
                measured[index] = encode_fast_local(
                    request.input_path, encrypted_sample.path(), true,
                    request.password, nullptr).output_bytes;
            }
            const uint64_t sum = checked_add(
                checked_add(
                    measured[0], measured[1],
                    "encrypted probe size overflow"),
                measured[2], "encrypted probe size overflow");
            return {
                sum / measured.size(),
                *std::min_element(measured.begin(), measured.end()),
                *std::max_element(measured.begin(), measured.end()),
                checked_multiply(
                    total_frames, measured.size(),
                    "encrypted probe frame count overflow"),
                false,
                "three complete bounded Fast Local encrypted FFV1 "
                "measurements; observed output range",
            };
        }
        if (total_frames <= exact_frame_limit) {
            const auto statistics = encode_fast_local(
                request.input_path, temporary.path(), request.encrypted,
                request.password, nullptr);
            return {
                statistics.output_bytes,
                statistics.output_bytes,
                statistics.output_bytes,
                statistics.total_frames,
                true,
                "complete bounded Fast Local FFV1 encode "
                "(exact for this input)",
            };
        }

        const uint64_t plain_capacity =
            fast_local_plain_capacity(request.encrypted);
        const std::array<uint64_t, 3> indices{
            0, (total_frames - 1) / 2, total_frames - 1};
        const auto file_id = request.encrypted
            ? make_encoding_file_id()
            : std::array<std::byte, 16>{};
        std::array<std::byte, CRYPTO_KEY_BYTES> key{};
        if (request.encrypted) {
            key = derive_key(request.password, file_id);
        }
        struct KeyCleaner {
            std::array<std::byte, CRYPTO_KEY_BYTES> &key;
            bool enabled;
            ~KeyCleaner() {
                if (enabled) secure_zero(key);
            }
        } key_cleaner{key, request.encrypted};

        FastLocalFileHeader file_header;
        file_header.flags =
            request.encrypted ? FastLocalEncrypted : 0;
        file_header.original_size =
            std::filesystem::file_size(request.input_path);
        file_header.total_frames = total_frames;
        file_header.plain_frame_capacity = plain_capacity;
        file_header.file_id = file_id;
        const auto serialized_file =
            serialize_fast_local_file_header(file_header);

        std::ifstream input(request.input_path, std::ios::binary);
        if (!input) throw std::runtime_error(
            "could not open Fast Local probe input");
        VideoEncoder video(temporary.path().string());
        std::vector<std::byte> plain(
            static_cast<std::size_t>(plain_capacity));
        std::vector<std::byte> frame(
            FAST_LOCAL_FRAME_BYTES, std::byte{0});
        for (const uint64_t index : indices) {
            const uint64_t offset = index * plain_capacity;
            const uint64_t remaining =
                file_header.original_size - offset;
            const std::size_t length = static_cast<std::size_t>(
                std::min<uint64_t>(remaining, plain_capacity));
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            if (!input) throw std::runtime_error(
                "Fast Local probe seek failed");
            input.read(reinterpret_cast<char *>(plain.data()),
                       static_cast<std::streamsize>(length));
            if (input.gcount() != static_cast<std::streamsize>(length)) {
                throw std::runtime_error(
                    "Fast Local probe read failed");
            }
            std::span<const std::byte> stored(plain.data(), length);
            std::vector<std::byte> encrypted;
            if (request.encrypted) {
                encrypted = encrypt_chunk(
                    stored, key, file_id,
                    static_cast<uint32_t>(index));
                stored = encrypted;
            }
            FastLocalFrameHeader frame_header;
            frame_header.flags =
                request.encrypted ? FastLocalEncrypted : 0;
            frame_header.frame_index =
                static_cast<uint32_t>(index);
            frame_header.total_frames =
                static_cast<uint32_t>(total_frames);
            frame_header.payload_length =
                static_cast<uint32_t>(stored.size());
            frame_header.plain_length =
                static_cast<uint32_t>(length);
            frame_header.payload_checksum = crc32c(stored);
            const auto serialized_frame =
                serialize_fast_local_frame_header(frame_header);
            std::fill(frame.begin(), frame.end(), std::byte{0});
            if (index == 0) {
                std::copy(serialized_file.begin(),
                          serialized_file.end(), frame.begin());
            }
            std::copy(serialized_frame.begin(), serialized_frame.end(),
                      frame.begin() + FAST_LOCAL_FILE_HEADER_SIZE);
            std::copy(stored.begin(), stored.end(),
                      frame.begin() + FAST_LOCAL_RESERVED_PREFIX);
            video.encode_gray8_frame(frame);
        }
        video.finalize();

        const uint64_t probe_file_bytes =
            std::filesystem::file_size(temporary.path());
        const auto &statistics = video.statistics();
        if (statistics.encoded_packet_bytes.size() != indices.size()) {
            throw std::runtime_error(
                "Fast Local probe produced an unexpected frame count");
        }
        const uint64_t compressed_total = std::accumulate(
            statistics.encoded_packet_bytes.begin(),
            statistics.encoded_packet_bytes.end(), uint64_t{0});
        const uint64_t overhead =
            probe_file_bytes >= compressed_total
                ? probe_file_bytes - compressed_total : 0;
        std::vector<long double> samples;
        for (const uint64_t bytes :
             statistics.encoded_packet_bytes) {
            samples.push_back(static_cast<long double>(bytes));
        }
        const long double mean = std::accumulate(
            samples.begin(), samples.end(), 0.0L) / samples.size();
        long double squared = 0.0L;
        for (const long double sample : samples) {
            const long double difference = sample - mean;
            squared += difference * difference;
        }
        const long double deviation =
            std::sqrt(squared / (samples.size() - 1));
        const long double half_width =
            student_t_95(samples.size()) * deviation /
            std::sqrt(static_cast<long double>(samples.size()));
        const long double frames =
            static_cast<long double>(total_frames);
        return {
            checked_rounded(overhead + mean * frames, false),
            checked_rounded(
                overhead + std::max(0.0L, mean - half_width) * frames,
                true),
            checked_rounded(
                overhead + (mean + half_width) * frames, true),
            static_cast<uint64_t>(indices.size()),
            false,
            "representative start/middle/end Fast Local frames; "
            "measured Matroska overhead; 95% Student-t interval",
        };
    }

    ProbeResult run_probe(const EncodingPreflightRequest &request,
                          const EncodingReliabilityEstimate &deterministic) {
        const std::size_t chunk_size =
            request.encrypted ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
        FileChunkReader reader(request.input_path.string().c_str(), chunk_size);

        const auto video_config =
            resilient_video_config_for_mode(request.mode);
        const uint64_t packets_per_frame = static_cast<uint64_t>(
            VideoEncoder::packets_per_frame(video_config));
        const uint64_t full_source =
            calculate_source_packet_count(CHUNK_SIZE_BYTES);
        const uint64_t full_repair = calculate_repair_packet_count(
            full_source, request.reliability.repair_ratio);
        const uint64_t full_packets = checked_add(
            full_source, full_repair, "probe packet count overflow");
        const uint64_t probe_packet_budget =
            std::max<uint64_t>(packets_per_frame,
                               checked_multiply(
                                   request.maximum_probe_frames,
                                   packets_per_frame,
                                   "probe frame budget overflow"));
        const uint64_t generation_budget = checked_multiply(
            probe_packet_budget, 4, "probe generation budget overflow");
        const std::size_t affordable_chunks = static_cast<std::size_t>(
            std::max<uint64_t>(
                1, std::min<uint64_t>(3, generation_budget /
                                                std::max<uint64_t>(
                                                    1, full_packets))));
        const std::size_t sample_count =
            std::min(reader.num_chunks(), affordable_chunks);
        const auto indices =
            representative_chunk_indices(reader.num_chunks(), sample_count);

        const auto file_id = make_encoding_file_id();
        const Encoder encoder(
            file_id, request.hash_algorithm, request.reliability);

        std::array<std::byte, CRYPTO_KEY_BYTES> key{};
        struct KeyCleaner {
            std::array<std::byte, CRYPTO_KEY_BYTES> &key;
            bool enabled;
            ~KeyCleaner() {
                if (enabled) secure_zero(std::span<std::byte>(key));
            }
        } key_cleaner{key, request.encrypted};
        if (request.encrypted) {
            key = derive_key(request.password, file_id);
        }

        TemporaryProbeFile temporary(
            request.output_path.extension().string());
        VideoEncoder video(temporary.path().string(), video_config);
        uint64_t selected_packets = 0;

        uint64_t remaining_packet_budget = probe_packet_budget;
        for (std::size_t sample_index = 0;
             sample_index < indices.size(); ++sample_index) {
            const std::size_t index = indices[sample_index];
            std::span<const std::byte> data = reader.chunk_view(index);
            std::vector<std::byte> encrypted_data;
            if (request.encrypted) {
                encrypted_data = encrypt_chunk(
                    data, key, file_id, static_cast<uint32_t>(index));
                data = encrypted_data;
            }
            const auto encoded = encoder.encode_chunk(
                static_cast<uint32_t>(index), data,
                index + 1 == reader.num_chunks(), request.encrypted);
            const uint64_t samples_left =
                static_cast<uint64_t>(indices.size() - sample_index);
            const uint64_t segment_budget =
                remaining_packet_budget / samples_left +
                (remaining_packet_budget % samples_left != 0 ? 1 : 0);
            const std::size_t take = static_cast<std::size_t>(
                std::min<uint64_t>(
                    encoded.first.size(), segment_budget));
            std::size_t begin = 0;
            if (take < encoded.first.size()) {
                if (sample_index + 1 == indices.size()) {
                    begin = encoded.first.size() - take;
                } else if (sample_index != 0) {
                    begin = (encoded.first.size() - take) / 2;
                }
            }
            for (std::size_t packet_index = begin;
                 packet_index < begin + take; ++packet_index) {
                video.add_packet(encoded.first[packet_index]);
            }
            selected_packets = checked_add(
                selected_packets,
                static_cast<uint64_t>(take),
                "probe packet count overflow");
            remaining_packet_budget -=
                static_cast<uint64_t>(take);
        }

        video.finalize();
        const uint64_t probe_file_bytes =
            std::filesystem::file_size(temporary.path());
        const auto &statistics = video.statistics();
        if (statistics.encoded_packet_bytes.empty() ||
            video.frames_written() <= 0) {
            throw std::runtime_error("probe produced no encoded frames");
        }

        ProbeResult result;
        result.frame_count =
            static_cast<uint64_t>(video.frames_written());
        result.encoded_entire_output =
            indices.size() == reader.num_chunks() &&
            selected_packets == deterministic.total_packet_count;
        if (result.encoded_entire_output) {
            result.likely_bytes = probe_file_bytes;
            result.minimum_bytes = probe_file_bytes;
            result.maximum_bytes = probe_file_bytes;
            result.method =
                "complete bounded FFV1 encode (exact for this input)";
            return result;
        }

        const uint64_t compressed_total = std::accumulate(
            statistics.encoded_packet_bytes.begin(),
            statistics.encoded_packet_bytes.end(), uint64_t{0},
            [](const uint64_t sum, const uint64_t value) {
                return checked_add(
                    sum, value, "probe compressed byte count overflow");
            });
        const uint64_t measured_overhead =
            probe_file_bytes >= compressed_total
                ? probe_file_bytes - compressed_total
                : 0;
        const uint64_t reported_fixed_overhead = checked_add(
            statistics.container_header_bytes,
            statistics.container_trailer_bytes,
            "probe container overhead overflow");
        const uint64_t fixed_overhead =
            std::min(measured_overhead, reported_fixed_overhead);
        const uint64_t variable_overhead =
            measured_overhead - fixed_overhead;
        const long double overhead_per_frame =
            static_cast<long double>(variable_overhead) /
            static_cast<long double>(
                statistics.encoded_packet_bytes.size());

        std::vector<long double> frame_samples;
        frame_samples.reserve(statistics.encoded_packet_bytes.size());
        for (const uint64_t bytes : statistics.encoded_packet_bytes) {
            frame_samples.push_back(
                static_cast<long double>(bytes) + overhead_per_frame);
        }
        const long double sum = std::accumulate(
            frame_samples.begin(), frame_samples.end(), 0.0L);
        const long double mean =
            sum / static_cast<long double>(frame_samples.size());
        long double squared_difference = 0.0L;
        for (const long double sample : frame_samples) {
            const long double difference = sample - mean;
            squared_difference += difference * difference;
        }
        const long double standard_deviation =
            frame_samples.size() > 1
                ? std::sqrt(
                      squared_difference /
                      static_cast<long double>(frame_samples.size() - 1))
                : 0.0L;
        const long double half_width =
            frame_samples.size() > 1
                ? student_t_95(frame_samples.size()) *
                      standard_deviation /
                      std::sqrt(
                          static_cast<long double>(frame_samples.size()))
                : 0.0L;
        const long double total_frames =
            static_cast<long double>(deterministic.frame_count);
        const long double fixed =
            static_cast<long double>(fixed_overhead);
        const long double likely = fixed + mean * total_frames;
        const long double minimum =
            fixed + std::max(0.0L, mean - half_width) * total_frames;
        const long double maximum =
            fixed + (mean + half_width) * total_frames;

        result.likely_bytes = checked_rounded(likely, false);
        result.minimum_bytes = checked_rounded(minimum, true);
        result.maximum_bytes = checked_rounded(maximum, true);
        result.method =
            (indices.size() >= 3
                 ? "representative start/middle/end FFV1 frames; "
                 : "bounded representative FFV1 frames; ") +
            std::string(
                "measured Matroska overhead; 95% Student-t interval over "
                "compressed bytes/frame");
        return result;
    }

    std::filesystem::path existing_space_query_path(
        const std::filesystem::path &output_path,
        bool &parent_exists) {
        std::error_code error;
        std::filesystem::path absolute =
            std::filesystem::absolute(output_path, error);
        if (error) throw std::filesystem::filesystem_error(
            "cannot resolve output path", output_path, error);
        std::filesystem::path parent = absolute.parent_path();
        if (parent.empty()) parent = std::filesystem::current_path();

        const auto path_status = [](const std::filesystem::path &path) {
            std::error_code status_error;
            const auto status =
                std::filesystem::status(path, status_error);
            if (status_error ==
                std::errc::no_such_file_or_directory) {
                return std::filesystem::file_status{
                    std::filesystem::file_type::not_found};
            }
            if (status_error) {
                throw std::filesystem::filesystem_error(
                    "cannot inspect output filesystem",
                    path, status_error);
            }
            return status;
        };

        parent_exists =
            std::filesystem::is_directory(path_status(parent));

        std::filesystem::path candidate = parent;
        while (!candidate.empty()) {
            if (std::filesystem::exists(path_status(candidate))) {
                return candidate;
            }
            const auto next = candidate.parent_path();
            if (next == candidate) break;
            candidate = next;
        }
        throw std::runtime_error(
            "no existing output-path ancestor for disk query");
    }

    struct DiskQueryResult {
        bool output_parent_exists = true;
        std::optional<uint64_t> available_bytes;
        std::string warning;
    };

    DiskQueryResult query_output_disk(
        const std::filesystem::path &output_path) {
        DiskQueryResult result;
        try {
            const auto query_path = existing_space_query_path(
                output_path, result.output_parent_exists);
#if defined(VIDSTOREX_ENABLE_TEST_HOOKS)
            if (const char *unknown =
                    std::getenv("VIDSTOREX_TEST_DISK_UNKNOWN");
                unknown && std::string_view(unknown) == "1") {
                result.warning =
                    "available disk space could not be determined "
                    "(test override)";
                return result;
            }
            if (const char *available =
                    std::getenv(
                        "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES");
                available && available[0] != '\0') {
                const std::string value(available);
                std::size_t parsed = 0;
                const unsigned long long bytes =
                    std::stoull(value, &parsed, 10);
                if (parsed != value.size()) {
                    throw std::invalid_argument(
                        "invalid test disk-space override");
                }
                result.available_bytes =
                    static_cast<uint64_t>(bytes);
                return result;
            }
#endif
            const auto information = std::filesystem::space(query_path);
            if (information.available ==
                    static_cast<uintmax_t>(-1) ||
                information.available >
                    std::numeric_limits<uint64_t>::max()) {
                result.warning =
                    "available disk space is not representable";
            } else {
                result.available_bytes =
                    static_cast<uint64_t>(information.available);
            }
        } catch (const std::exception &) {
            result.warning =
                "available disk space could not be determined";
        }
        return result;
    }
}

EncodingDiskRequirement calculate_encoding_disk_requirement(
    const uint64_t estimated_output_max_bytes) {
    const uint64_t safety_margin =
        std::max(
            gibibyte,
            checked_ceil_tenth(estimated_output_max_bytes));
    return {
        safety_margin,
        checked_add(
            checked_add(
                estimated_output_max_bytes,
                safety_margin,
                "required disk space overflow"),
            actual_encode_temporary_bytes,
            "required disk space overflow"),
    };
}

EncodingPreflightEstimate estimate_encoding_preflight(
    const EncodingPreflightRequest &request) {
    const auto started = std::chrono::steady_clock::now();
    EncodingPreflightEstimate estimate;
    estimate.mode = request.mode;
    estimate.repair_ratio = request.reliability.repair_ratio;
    try {
        estimate.repair_percentage =
            repair_ratio_to_percentage(request.reliability.repair_ratio);
        if (request.input_path.empty() || request.output_path.empty()) {
            throw std::invalid_argument(
                "input and output paths must be specified");
        }
        if (request.encrypted && request.password.empty()) {
            throw std::invalid_argument(
                "encrypted preflight requires a password");
        }

        const InputMetadata metadata =
            inspect_input_metadata(request.input_path);
        estimate.input_size_bytes = metadata.size;
        estimate.input_last_write_time_ticks =
            metadata.last_write_time_ticks;
        estimate.input_path_fingerprint =
            metadata.path_fingerprint;
        validate_output_target(
            request.input_path, request.output_path);
        EncodingReliabilityEstimate deterministic;
        if (request.mode == EncodingMode::FastLocal) {
            std::string extension =
                request.output_path.extension().string();
            std::transform(
                extension.begin(), extension.end(), extension.begin(),
                [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            if (extension != ".mkv") {
                throw std::invalid_argument(
                    "Fast Local output must use the .mkv extension");
            }
            estimate.repair_percentage = 0.0;
            estimate.repair_ratio = 0.0;
            estimate.estimated_frame_count =
                fast_local_frame_count(
                    estimate.input_size_bytes, request.encrypted);
            estimate.chunk_count = estimate.estimated_frame_count;
            estimate.estimated_video_duration_seconds =
                static_cast<double>(estimate.estimated_frame_count) /
                FRAME_FPS;
            estimate.header_bytes =
                FAST_LOCAL_FILE_HEADER_SIZE +
                estimate.estimated_frame_count *
                    FAST_LOCAL_FRAME_HEADER_SIZE;
            estimate.frame_payload_capacity =
                fast_local_plain_capacity(request.encrypted);
            estimate.payload_bytes = estimate.input_size_bytes +
                (request.encrypted
                     ? estimate.estimated_frame_count *
                           FAST_LOCAL_CRYPTO_OVERHEAD
                     : 0);
            estimate.padding_bytes = fast_local_padding_bytes(
                estimate.input_size_bytes, request.encrypted);
            estimate.estimation_method =
                "deterministic Fast Local frame count";
            deterministic.frame_count =
                estimate.estimated_frame_count;
        } else {
            const auto video_config =
                resilient_video_config_for_mode(request.mode);
            deterministic = estimate_encoding_reliability(
                estimate.input_size_bytes, request.encrypted,
                request.reliability,
                static_cast<uint64_t>(
                    VideoEncoder::packets_per_frame(video_config)),
                static_cast<uint32_t>(video_config.fps));
            estimate.chunk_count = deterministic.chunk_count;
            estimate.source_packet_count =
                deterministic.source_packet_count;
            estimate.repair_packet_count =
                deterministic.repair_packet_count;
            estimate.total_packet_count =
                deterministic.total_packet_count;
            estimate.estimated_frame_count = deterministic.frame_count;
            estimate.estimated_video_duration_seconds =
                deterministic.video_duration_seconds;
        }

        const DiskQueryResult disk =
            query_output_disk(request.output_path);
        estimate.available_disk_bytes = disk.available_bytes;
        estimate.disk_space_known =
            disk.available_bytes.has_value();
        append_message(estimate.warning, disk.warning);
        if (!disk.output_parent_exists) {
            append_message(
                estimate.error,
                "output parent directory does not exist");
        }

        if (request.enable_probe) {
            try {
                const ProbeResult probe =
                    request.mode == EncodingMode::FastLocal
                        ? run_fast_local_probe(
                              request, estimate.estimated_frame_count)
                        : run_probe(request, deterministic);
                estimate.estimated_output_bytes = probe.likely_bytes;
                estimate.estimated_output_min_bytes =
                    probe.minimum_bytes;
                estimate.estimated_output_max_bytes =
                    probe.maximum_bytes;
                estimate.output_size_estimate_available = true;
                estimate.estimation_method = probe.method;
                estimate.probe_frame_count = probe.frame_count;
                estimate.probe_duration_seconds =
                    static_cast<double>(probe.frame_count) /
                    static_cast<double>(FRAME_FPS);
            } catch (const std::exception &error) {
                std::cerr
                    << "[preflight] output-size probe failed: "
                    << error.what() << '\n';
                estimate.debug_detail = error.what();
                append_message(
                    estimate.warning,
                    "output-size probe failed; size and required disk "
                    "space are unavailable");
            }
        } else {
            append_message(
                estimate.warning,
                "output-size probe was disabled");
        }

        if (estimate.output_size_estimate_available) {
            const uint64_t maximum =
                *estimate.estimated_output_max_bytes;
            const auto requirement =
                calculate_encoding_disk_requirement(maximum);
            estimate.safety_margin_bytes =
                requirement.safety_margin_bytes;
            estimate.required_disk_bytes =
                requirement.required_disk_bytes;

            if (estimate.disk_space_known) {
                estimate.disk_space_sufficient =
                    *estimate.available_disk_bytes >=
                    *estimate.required_disk_bytes;
                if (!*estimate.disk_space_sufficient) {
                    append_message(
                        estimate.error,
                        "insufficient disk space for the estimated "
                        "maximum output and safety margin");
                }
            }
        } else {
            append_message(
                estimate.warning,
                "required disk space cannot be verified without an "
                "output-size estimate");
        }

        estimate.can_start_encoding = estimate.error.empty();
        estimate.low_disk_override_permitted =
            !estimate.can_start_encoding &&
            estimate.disk_space_sufficient.has_value() &&
            !*estimate.disk_space_sufficient &&
            estimate.error ==
                "insufficient disk space for the estimated maximum "
                "output and safety margin";
    } catch (const std::filesystem::filesystem_error &error) {
        estimate.debug_detail = error.what();
        std::error_code exists_error;
        if (!std::filesystem::exists(
                request.input_path, exists_error) &&
            (!exists_error ||
             exists_error ==
                 std::errc::no_such_file_or_directory)) {
            estimate.error = "input file was not found";
        } else {
            estimate.error =
                "filesystem metadata or disk space could not be inspected";
        }
        estimate.can_start_encoding = false;
    } catch (const std::exception &error) {
        estimate.error = error.what();
        estimate.debug_detail = error.what();
        estimate.can_start_encoding = false;
    }
    estimate.preflight_duration_seconds =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
    return estimate;
}

EncodingStartValidation validate_encoding_preflight_for_start(
    const EncodingPreflightRequest &request,
    const EncodingPreflightEstimate &estimate,
    const bool allow_low_disk) {
    EncodingStartValidation validation;
    try {
        const InputMetadata current =
            inspect_input_metadata(request.input_path);
        validate_output_target(
            request.input_path, request.output_path);

        validation.metadata_matches =
            current.size == estimate.input_size_bytes &&
            current.last_write_time_ticks ==
                estimate.input_last_write_time_ticks &&
            current.path_fingerprint ==
                estimate.input_path_fingerprint;
        if (!validation.metadata_matches) {
            validation.error =
                "preflight estimate is stale because the input file "
                "metadata changed";
            return validation;
        }

        bool deterministic_matches = false;
        if (request.mode == EncodingMode::FastLocal &&
            estimate.mode == EncodingMode::FastLocal) {
            deterministic_matches =
                fast_local_frame_count(current.size, request.encrypted) ==
                    estimate.estimated_frame_count &&
                estimate.frame_payload_capacity ==
                    fast_local_plain_capacity(request.encrypted);
        } else if (request.mode != EncodingMode::FastLocal &&
                   estimate.mode == request.mode) {
            const auto video_config =
                resilient_video_config_for_mode(request.mode);
            const auto deterministic = estimate_encoding_reliability(
                current.size, request.encrypted, request.reliability,
                static_cast<uint64_t>(
                    VideoEncoder::packets_per_frame(video_config)),
                static_cast<uint32_t>(video_config.fps));
            deterministic_matches =
                deterministic.chunk_count == estimate.chunk_count &&
                deterministic.source_packet_count ==
                    estimate.source_packet_count &&
                deterministic.repair_packet_count ==
                    estimate.repair_packet_count &&
                deterministic.total_packet_count ==
                    estimate.total_packet_count &&
                deterministic.frame_count ==
                    estimate.estimated_frame_count;
        }
        if (!deterministic_matches) {
            validation.error =
                "preflight deterministic counts no longer match the "
                "current encode configuration";
            return validation;
        }

        const DiskQueryResult disk =
            query_output_disk(request.output_path);
        validation.available_disk_bytes = disk.available_bytes;
        validation.disk_space_known =
            disk.available_bytes.has_value();
        append_message(validation.warning, disk.warning);
        if (!disk.output_parent_exists) {
            append_message(
                validation.error,
                "output parent directory does not exist");
        }

        if (estimate.required_disk_bytes.has_value()) {
            if (validation.disk_space_known) {
                validation.disk_space_sufficient =
                    *validation.available_disk_bytes >=
                    *estimate.required_disk_bytes;
                if (!*validation.disk_space_sufficient) {
                    if (allow_low_disk) {
                        append_message(
                            validation.warning,
                            "known insufficient disk space was "
                            "explicitly overridden");
                    } else {
                        append_message(
                            validation.error,
                            "insufficient disk space for the estimated "
                            "maximum output and safety margin");
                    }
                }
            } else {
                append_message(
                    validation.warning,
                    "disk space is unknown; encoding is allowed but "
                    "capacity could not be verified");
            }
        } else {
            append_message(
                validation.warning,
                "output size is unavailable; required disk space "
                "cannot be verified");
        }

        validation.can_start_encoding = validation.error.empty();
    } catch (const std::filesystem::filesystem_error &error) {
        validation.error =
            "filesystem metadata or disk space could not be inspected";
        validation.warning = error.what();
    } catch (const std::exception &error) {
        validation.error = error.what();
    }
    return validation;
}
