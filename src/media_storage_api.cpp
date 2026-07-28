/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "media_storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <omp.h>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <thread>

#include "chunker.h"
#include "configuration.h"
#include "crypto.h"
#include "decoder.h"
#include "encoding_preflight.h"
#include "encoding_mode.h"
#include "encoding_reliability.h"
#include "encoder.h"
#include "fast_local_codec.h"
#include "performance_profiler.h"
#include "safe_output.h"
#include "stream.h"
#include "video_decoder.h"
#include "video_encoder.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static HashAlgorithm to_internal_hash(ms_hash_algorithm_t algo);

namespace {
    constexpr double bytes_per_mib = 1024.0 * 1024.0;

    constexpr const char *stage_names[MS_PERF_STAGE_COUNT] = {
        "Input file read/open",
        "Encryption / preprocessing",
        "Chunk creation",
        "FEC / repair packet generation",
        "Packets to frames",
        "FFmpeg video encoding",
        "Mux and disk write",
        "Video read and demux",
        "Frame decode",
        "Packet extraction",
        "FEC / chunk recovery",
        "Decryption / postprocessing",
        "Output file write",
    };

    constexpr const char *stage_json_names[MS_PERF_STAGE_COUNT] = {
        "input_read",
        "preprocess",
        "chunk_creation",
        "fec_repair_generation",
        "packet_to_frame",
        "ffmpeg_encode",
        "mux_disk_write",
        "video_read_demux",
        "frame_decode",
        "packet_extraction",
        "fec_chunk_recovery",
        "postprocess",
        "output_write",
    };

    const char *operation_name(const ms_operation_t operation) {
        switch (operation) {
            case MS_OPERATION_ENCODE: return "encode";
            case MS_OPERATION_DECODE: return "decode";
            case MS_OPERATION_STREAM_ENCODE: return "stream_encode";
            case MS_OPERATION_STREAM_DECODE: return "stream_decode";
            default: return "unknown";
        }
    }

    EncodingMode to_internal_mode(const ms_encoding_mode_t mode) {
        switch (mode) {
            case MS_ENCODING_MODE_RESILIENT:
                return EncodingMode::Resilient;
            case MS_ENCODING_MODE_FAST_LOCAL:
                return EncodingMode::FastLocal;
            default:
                throw std::invalid_argument("invalid encoding mode");
        }
    }

    ms_encoding_mode_t to_public_mode(const EncodingMode mode) {
        return mode == EncodingMode::FastLocal
            ? MS_ENCODING_MODE_FAST_LOCAL
            : MS_ENCODING_MODE_RESILIENT;
    }

    bool stage_applies(const ms_operation_t operation, const std::size_t index) {
        if (operation == MS_OPERATION_ENCODE ||
            operation == MS_OPERATION_STREAM_ENCODE) {
            return index <= MS_PERF_MUX_DISK_WRITE;
        }
        if (operation == MS_OPERATION_DECODE ||
            operation == MS_OPERATION_STREAM_DECODE) {
            return index >= MS_PERF_VIDEO_READ_DEMUX;
        }
        return true;
    }

    void fill_result(ms_result_t *result,
                     PerformanceProfiler &profiler,
                     const ms_operation_t operation,
                     const uint64_t input_size,
                     const uint64_t output_size,
                     const uint64_t chunks,
                     const uint64_t packets,
                     const uint64_t source_packets,
                     const uint64_t repair_packets,
                     const uint64_t frames,
                     const double selected_repair_ratio = 0.0) {
        if (!result) return;

        profiler.finish();
        *result = {};
        result->actual_inside_estimated_range = -1;
        result->input_size = input_size;
        result->output_size = output_size;
        result->total_chunks = chunks;
        result->total_packets = packets;
        result->total_frames = frames;
        result->source_packets = source_packets;
        result->repair_packets = repair_packets;
        result->operation = operation;
        result->total_seconds = profiler.total_seconds();
        result->average_frames_per_second =
            result->total_seconds > 0.0
                ? static_cast<double>(frames) / result->total_seconds
                : 0.0;

        const uint64_t processed_bytes =
            operation == MS_OPERATION_DECODE ||
            operation == MS_OPERATION_STREAM_DECODE
                ? output_size
                : input_size;
        result->throughput_mib_per_second =
            result->total_seconds > 0.0
                ? static_cast<double>(processed_bytes) /
                      bytes_per_mib / result->total_seconds
                : 0.0;
        result->output_input_ratio =
            input_size > 0
                ? static_cast<double>(output_size) /
                      static_cast<double>(input_size)
                : 0.0;
        if (operation == MS_OPERATION_ENCODE ||
            operation == MS_OPERATION_STREAM_ENCODE) {
            result->selected_repair_ratio = selected_repair_ratio;
            result->selected_repair_percentage =
                repair_ratio_to_percentage(selected_repair_ratio);
        }
        result->repair_source_ratio =
            source_packets > 0
                ? static_cast<double>(repair_packets) /
                      static_cast<double>(source_packets)
                : 0.0;

        for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
            const auto stage = static_cast<PerformanceStage>(i);
            auto &timing = result->stage_timings[i];
            timing.seconds = profiler.stage_seconds(stage);
            timing.percent_of_total =
                result->total_seconds > 0.0
                    ? timing.seconds * 100.0 / result->total_seconds
                    : 0.0;
            timing.invocations = profiler.stage_invocations(stage);
        }
    }

    void fill_estimate_validation(
        ms_result_t *result,
        const ms_encoding_estimate_t &estimate,
        const uint64_t actual_output_bytes,
        const double preflight_duration_seconds) {
        if (!result) return;
        result->actual_output_bytes = actual_output_bytes;
        result->preflight_duration_seconds =
            preflight_duration_seconds;
        result->actual_encode_duration_seconds =
            result->total_seconds;
        if (!estimate.output_size_estimate_available) return;

        result->estimate_validation_available = 1;
        result->estimated_output_bytes =
            estimate.estimated_output_bytes;
        result->estimated_output_min_bytes =
            estimate.estimated_output_min_bytes;
        result->estimated_output_max_bytes =
            estimate.estimated_output_max_bytes;
        result->estimate_absolute_error_bytes =
            actual_output_bytes >= estimate.estimated_output_bytes
                ? actual_output_bytes -
                      estimate.estimated_output_bytes
                : estimate.estimated_output_bytes -
                      actual_output_bytes;
        result->actual_inside_estimated_range =
            actual_output_bytes >= estimate.estimated_output_min_bytes &&
                    actual_output_bytes <=
                        estimate.estimated_output_max_bytes
                ? 1
                : 0;
        if (estimate.estimated_output_bytes != 0) {
            result->estimate_relative_error_available = 1;
            result->estimate_relative_error_percent =
                static_cast<double>(
                    static_cast<long double>(
                        result->estimate_absolute_error_bytes) *
                    100.0L /
                    static_cast<long double>(
                        estimate.estimated_output_bytes));
        }
    }

    void classify_packet(const std::span<const std::byte> packet,
                         uint64_t &source_packets,
                         uint64_t &repair_packets) {
        if (packet.size() <= FLAGS_OFF) return;
        if ((static_cast<uint8_t>(packet[FLAGS_OFF]) & IsRepairSymbol) != 0) {
            ++repair_packets;
        } else {
            ++source_packets;
        }
    }

    EncodingReliabilityOptions reliability_from_encode_options(
        const double repair_ratio,
        const int repair_ratio_is_set) {
        const EncodingReliabilityOptions reliability{
            repair_ratio_is_set == 1
                ? repair_ratio
                : DEFAULT_REPAIR_RATIO
        };
        if (!is_valid_repair_ratio(reliability.repair_ratio)) {
            throw std::invalid_argument("invalid repair ratio");
        }
        return reliability;
    }

    template <std::size_t Capacity>
    void copy_text(char (&destination)[Capacity], const std::string &source) {
        static_assert(Capacity > 0);
        const std::size_t copied =
            std::min(source.size(), Capacity - 1);
        std::memcpy(destination, source.data(), copied);
        destination[copied] = '\0';
    }

    template <std::size_t Capacity>
    std::string bounded_text(const char (&source)[Capacity]) {
        const void *terminator = std::memchr(source, '\0', Capacity);
        const std::size_t length = terminator
            ? static_cast<const char *>(terminator) - source
            : Capacity;
        return {source, length};
    }

    void copy_preflight_estimate(
        const EncodingPreflightEstimate &source,
        ms_encoding_estimate_t &destination) {
        destination = {};
        destination.struct_size = sizeof(ms_encoding_estimate_t);
        destination.struct_version = MS_ENCODING_ESTIMATE_VERSION;
        destination.input_size_bytes = source.input_size_bytes;
        destination.input_last_write_time_ticks =
            source.input_last_write_time_ticks;
        destination.input_path_fingerprint =
            source.input_path_fingerprint;
        destination.repair_percentage = source.repair_percentage;
        destination.repair_ratio = source.repair_ratio;
        destination.chunk_count = source.chunk_count;
        destination.source_packet_count = source.source_packet_count;
        destination.repair_packet_count = source.repair_packet_count;
        destination.total_packet_count = source.total_packet_count;
        destination.estimated_frame_count =
            source.estimated_frame_count;
        destination.estimated_video_duration_seconds =
            source.estimated_video_duration_seconds;
        destination.output_size_estimate_available =
            source.output_size_estimate_available ? 1 : 0;
        destination.estimated_output_bytes =
            source.estimated_output_bytes.value_or(0);
        destination.estimated_output_min_bytes =
            source.estimated_output_min_bytes.value_or(0);
        destination.estimated_output_max_bytes =
            source.estimated_output_max_bytes.value_or(0);
        destination.disk_space_known =
            source.disk_space_known ? 1 : 0;
        destination.available_disk_bytes =
            source.available_disk_bytes.value_or(0);
        destination.safety_margin_bytes =
            source.safety_margin_bytes.value_or(0);
        destination.required_disk_bytes =
            source.required_disk_bytes.value_or(0);
        destination.required_disk_space_known =
            source.required_disk_bytes.has_value() ? 1 : 0;
        destination.disk_space_sufficient =
            source.disk_space_sufficient.has_value()
                ? (*source.disk_space_sufficient ? 1 : 0)
                : -1;
        destination.can_start_encoding =
            source.can_start_encoding ? 1 : 0;
        destination.low_disk_override_permitted =
            source.low_disk_override_permitted ? 1 : 0;
        copy_text(
            destination.estimation_method, source.estimation_method);
        destination.probe_frame_count = source.probe_frame_count;
        destination.probe_duration_seconds =
            source.probe_duration_seconds;
        destination.preflight_duration_seconds =
            source.preflight_duration_seconds;
        destination.encoding_mode =
            to_public_mode(source.mode);
        destination.header_bytes = source.header_bytes;
        destination.frame_payload_capacity =
            source.frame_payload_capacity;
        destination.payload_bytes = source.payload_bytes;
        destination.padding_bytes = source.padding_bytes;
        copy_text(destination.warning, source.warning);
        copy_text(destination.error, source.error);
    }

    EncodingPreflightEstimate internal_preflight_estimate(
        const ms_encoding_estimate_t &source) {
        EncodingPreflightEstimate destination;
        destination.input_size_bytes = source.input_size_bytes;
        destination.input_last_write_time_ticks =
            source.input_last_write_time_ticks;
        destination.input_path_fingerprint =
            source.input_path_fingerprint;
        destination.repair_percentage = source.repair_percentage;
        destination.repair_ratio = source.repair_ratio;
        destination.chunk_count = source.chunk_count;
        destination.source_packet_count =
            source.source_packet_count;
        destination.repair_packet_count =
            source.repair_packet_count;
        destination.total_packet_count =
            source.total_packet_count;
        destination.estimated_frame_count =
            source.estimated_frame_count;
        destination.estimated_video_duration_seconds =
            source.estimated_video_duration_seconds;
        destination.output_size_estimate_available =
            source.output_size_estimate_available != 0;
        if (destination.output_size_estimate_available) {
            if (source.estimated_output_min_bytes >
                    source.estimated_output_bytes ||
                source.estimated_output_bytes >
                    source.estimated_output_max_bytes) {
                throw std::invalid_argument(
                    "invalid output estimate range");
            }
            destination.estimated_output_bytes =
                source.estimated_output_bytes;
            destination.estimated_output_min_bytes =
                source.estimated_output_min_bytes;
            destination.estimated_output_max_bytes =
                source.estimated_output_max_bytes;
        }
        destination.disk_space_known =
            source.disk_space_known != 0;
        if (destination.disk_space_known) {
            destination.available_disk_bytes =
                source.available_disk_bytes;
        }
        if (source.required_disk_space_known) {
            if (!source.output_size_estimate_available) {
                throw std::invalid_argument(
                    "required disk space needs an output estimate");
            }
            const auto requirement =
                calculate_encoding_disk_requirement(
                    source.estimated_output_max_bytes);
            if (requirement.safety_margin_bytes !=
                    source.safety_margin_bytes ||
                requirement.required_disk_bytes !=
                    source.required_disk_bytes) {
                throw std::invalid_argument(
                    "inconsistent disk requirement");
            }
            destination.safety_margin_bytes =
                source.safety_margin_bytes;
            destination.required_disk_bytes =
                source.required_disk_bytes;
        }
        if (source.disk_space_sufficient >= 0) {
            destination.disk_space_sufficient =
                source.disk_space_sufficient != 0;
        }
        destination.can_start_encoding =
            source.can_start_encoding != 0;
        destination.low_disk_override_permitted =
            source.low_disk_override_permitted != 0;
        destination.estimation_method =
            bounded_text(source.estimation_method);
        destination.probe_frame_count =
            source.probe_frame_count;
        destination.probe_duration_seconds =
            source.probe_duration_seconds;
        destination.preflight_duration_seconds =
            source.preflight_duration_seconds;
        destination.mode =
            to_internal_mode(source.encoding_mode);
        destination.header_bytes = source.header_bytes;
        destination.frame_payload_capacity =
            source.frame_payload_capacity;
        destination.payload_bytes = source.payload_bytes;
        destination.padding_bytes = source.padding_bytes;
        destination.warning = bounded_text(source.warning);
        destination.error = bounded_text(source.error);
        return destination;
    }

    EncodingPreflightRequest make_preflight_request(
        const ms_encode_options_t &options,
        const bool enable_probe) {
        const std::span<const std::byte> password =
            options.password && options.password_len > 0
                ? std::span<const std::byte>(
                      reinterpret_cast<const std::byte *>(
                          options.password),
                      options.password_len)
                : std::span<const std::byte>{};
        EncodingPreflightRequest request{
            std::filesystem::path(options.input_path),
            std::filesystem::path(options.output_path),
            options.encrypt != 0,
            password,
            to_internal_hash(options.hash_algorithm),
            reliability_from_encode_options(
                options.repair_ratio,
                options.repair_ratio_is_set),
            enable_probe,
            90,
        };
        request.mode = to_internal_mode(options.encoding_mode);
        return request;
    }
}

static HashAlgorithm to_internal_hash(const ms_hash_algorithm_t algo) {
    switch (algo) {
        case MS_HASH_XXHASH32: return HashAlgorithm::XXHash32;
        default: return HashAlgorithm::CRC32;
    }
}

ms_status_t ms_estimate_encode(
    const ms_encode_options_t *options,
    const int enable_probe,
    ms_encoding_estimate_t *estimate) {
    if (!options || !estimate || !options->input_path ||
        !options->output_path || options->input_path[0] == '\0' ||
        options->output_path[0] == '\0') {
        return MS_ERR_INVALID_ARGS;
    }
    *estimate = {};
    estimate->struct_size = sizeof(ms_encoding_estimate_t);
    estimate->struct_version = MS_ENCODING_ESTIMATE_VERSION;
    estimate->disk_space_sufficient = -1;
    if (options->encrypt &&
        (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }
    try {
        const auto internal = estimate_encoding_preflight(
            make_preflight_request(*options, enable_probe != 0));
        copy_preflight_estimate(internal, *estimate);
        if (internal.error == "input file was not found") {
            return MS_ERR_FILE_NOT_FOUND;
        }
        return MS_OK;
    } catch (const std::invalid_argument &) {
        return MS_ERR_INVALID_ARGS;
    } catch (...) {
        return MS_ERR_IO;
    }
}

ms_status_t ms_encode(const ms_encode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }
    if (options->encrypt && (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }
    if (result) {
        *result = {};
        result->actual_inside_estimated_range = -1;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);
    EncodingMode mode;
    try {
        mode = to_internal_mode(options->encoding_mode);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }
    EncodingReliabilityOptions reliability;
    try {
        reliability = reliability_from_encode_options(
            options->repair_ratio, options->repair_ratio_is_set);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    ms_encoding_estimate_t generated_preflight{};
    const ms_encoding_estimate_t *preflight =
        options->preflight_estimate;
    double preflight_duration_seconds =
        options->preflight_duration_seconds;
    if (!preflight) {
        const auto preflight_started =
            std::chrono::steady_clock::now();
        const ms_status_t status =
            ms_estimate_encode(options, 1, &generated_preflight);
        if (status != MS_OK) return status;
        preflight_duration_seconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() -
                preflight_started).count();
        preflight = &generated_preflight;
    }
    if (preflight->struct_version !=
            MS_ENCODING_ESTIMATE_VERSION ||
        preflight->struct_size < sizeof(ms_encoding_estimate_t)) {
        return MS_ERR_INVALID_ARGS;
    }

    EncodingPreflightEstimate internal_preflight;
    EncodingStartValidation start_validation;
    try {
        internal_preflight =
            internal_preflight_estimate(*preflight);
        start_validation =
            validate_encoding_preflight_for_start(
                make_preflight_request(*options, false),
                internal_preflight,
                options->allow_low_disk == 1);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }
    if (!start_validation.metadata_matches) {
        return MS_ERR_PREFLIGHT_STALE;
    }
    if (!start_validation.can_start_encoding) {
        if (start_validation.disk_space_sufficient.has_value() &&
            !*start_validation.disk_space_sufficient) {
            return MS_ERR_INSUFFICIENT_DISK;
        }
        return MS_ERR_IO;
    }

    PerformanceProfiler profiler;

    if (mode == EncodingMode::FastLocal) {
        try {
            const std::span<const std::byte> password =
                options->password && options->password_len > 0
                    ? std::span<const std::byte>(
                          reinterpret_cast<const std::byte *>(
                              options->password),
                          options->password_len)
                    : std::span<const std::byte>{};
            const auto statistics = encode_fast_local(
                input_path, output_path, options->encrypt != 0,
                password, &profiler, options->progress,
                options->progress_user);
            fill_result(
                result, profiler, MS_OPERATION_ENCODE,
                statistics.input_bytes, statistics.output_bytes,
                statistics.total_frames, 0, 0, 0,
                statistics.total_frames, 0.0);
            if (result) {
                result->encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
                result->frame_payload_capacity =
                    statistics.frame_payload_capacity;
                result->header_bytes = statistics.header_bytes;
                result->payload_bytes = statistics.payload_bytes;
                result->padding_bytes = statistics.padding_bytes;
            }
            fill_estimate_validation(
                result, *preflight, statistics.output_bytes,
                preflight_duration_seconds);
            return MS_OK;
        } catch (const std::invalid_argument &) {
            return MS_ERR_INVALID_ARGS;
        } catch (...) {
            return MS_ERR_ENCODE_FAILED;
        }
    }

    const bool encrypt = options->encrypt != 0;
    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    uint64_t input_size = 0;
    std::unique_ptr<FileChunkReader> reader_storage;
    try {
        ScopedTimer timer(&profiler, PerformanceStage::InputRead);
        input_size = std::filesystem::file_size(input_path);
        reader_storage = std::make_unique<FileChunkReader>(
            input_path.c_str(), chunk_size);
    } catch (...) {
        return MS_ERR_IO;
    }
    const FileChunkReader &reader = *reader_storage;
    std::size_t num_chunks = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
        num_chunks = reader.num_chunks();
    }

    const auto file_id = make_encoding_file_id();
    const Encoder encoder(
        file_id, to_internal_hash(options->hash_algorithm), reliability);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
        const std::span pw(
            reinterpret_cast<const std::byte *>(options->password),
            options->password_len);
        key = derive_key(pw, file_id);
    }

    uint64_t total_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    int64_t total_frames = 0;

    uint64_t output_size = 0;
    try {
        SafeOutputFile safe_output(output_path);
        {
            VideoEncoder video_encoder(
                safe_output.partial_path().string(), &profiler);

            const int batch_size = std::max(1, omp_get_max_threads());

            using BatchResults = std::vector<std::pair<std::vector<Packet>, ChunkManifestEntry>>;

            auto fec_encode_batch = [&](const std::size_t batch_start, const int batch_count) -> BatchResults {
                BatchResults results(batch_count);
                bool batch_error = false;

#pragma omp parallel for schedule(dynamic)
                for (int j = 0; j < batch_count; ++j) {
                    if (batch_error) continue;
                    try {
                        const std::size_t i = batch_start + j;
                        std::span<const std::byte> data_to_encode;
                        {
                            ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
                            data_to_encode = reader.chunk_view(i);
                        }
                        std::vector<std::byte> encrypted_buf;
                        if (encrypt) {
                            ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
                            encrypted_buf = encrypt_chunk(
                                data_to_encode, key, file_id,
                                static_cast<uint32_t>(i));
                            data_to_encode = encrypted_buf;
                        }
                        const bool is_last = (i == num_chunks - 1);
                        {
                            ScopedTimer timer(
                                &profiler, PerformanceStage::FecRepairGeneration);
                            results[j] = encoder.encode_chunk(
                                static_cast<uint32_t>(i), data_to_encode,
                                is_last, encrypt);
                        }
                    } catch (...) {
                        batch_error = true;
                    }
                }

                if (batch_error) throw std::runtime_error("batch FEC encoding failed");
                return results;
            };

            const auto first_end = std::min(static_cast<std::size_t>(batch_size), num_chunks);
            std::future<BatchResults> pending =
                std::async(std::launch::async, fec_encode_batch,
                           static_cast<std::size_t>(0), static_cast<int>(first_end));

            for (std::size_t batch_start = 0; batch_start < num_chunks;
                 batch_start += batch_size) {
                const std::size_t batch_end =
                    std::min(batch_start + static_cast<std::size_t>(batch_size),
                             num_chunks);
                const int batch_count = static_cast<int>(batch_end - batch_start);

                if (options->progress) {
                    if (options->progress(static_cast<uint64_t>(batch_start),
                                          static_cast<uint64_t>(num_chunks),
                                          options->progress_user) != 0) {
                        if (pending.valid()) pending.wait();
                        if (encrypt) secure_zero(std::span<std::byte>(key));
                        return MS_ERR_ENCODE_FAILED;
                    }
                }

                auto results = pending.get();

                if (const std::size_t next_start = batch_start + batch_size; next_start < num_chunks) {
                    const auto next_end = std::min(
                        next_start + static_cast<std::size_t>(batch_size), num_chunks);
                    const int next_count = static_cast<int>(next_end - next_start);
                    pending = std::async(std::launch::async,
                                         fec_encode_batch, next_start, next_count);
                }

                for (int j = 0; j < batch_count; ++j) {
                    if (results[j].first.size() >
                        std::numeric_limits<uint64_t>::max() -
                            total_packets) {
                        throw std::overflow_error(
                            "encoded packet count overflow");
                    }
                    total_packets +=
                        static_cast<uint64_t>(results[j].first.size());
                    for (const auto &packet: results[j].first) {
                        classify_packet(std::span(packet.bytes), source_packets,
                                        repair_packets);
                    }
                    video_encoder.encode_packets(results[j].first);
                }
            }

            video_encoder.finalize();
            total_frames = video_encoder.frames_written();
            output_size = std::filesystem::file_size(
                safe_output.partial_path());
        }
        {
            ScopedTimer timer(
                &profiler, PerformanceStage::MuxDiskWrite);
            safe_output.commit();
        }
    } catch (...) {
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    }

    if (encrypt) secure_zero(std::span<std::byte>(key));

    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_ENCODE,
                input_size, output_size, num_chunks, total_packets,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames),
                 reliability.repair_ratio);
    if (result) {
        result->encoding_mode = MS_ENCODING_MODE_RESILIENT;
    }
    fill_estimate_validation(
        result, *preflight, output_size,
        preflight_duration_seconds);

    return MS_OK;
}

ms_status_t ms_decode(const ms_decode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string output_path(options->output_path);
    PerformanceProfiler profiler;

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    uint64_t video_size = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::VideoReadDemux);
        video_size = std::filesystem::file_size(input_path);
    }

    if (fast_local_has_magic(input_path)) {
        try {
            const std::span<const std::byte> password =
                options->password && options->password_len > 0
                    ? std::span<const std::byte>(
                          reinterpret_cast<const std::byte *>(
                              options->password),
                          options->password_len)
                    : std::span<const std::byte>{};
            const auto statistics = decode_fast_local(
                input_path, output_path, password, &profiler,
                options->progress, options->progress_user);
            fill_result(
                result, profiler, MS_OPERATION_DECODE,
                statistics.input_bytes, statistics.output_bytes,
                statistics.total_frames, 0, 0, 0,
                statistics.total_frames);
            if (result) {
                result->encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
                result->frame_payload_capacity =
                    statistics.frame_payload_capacity;
                result->header_bytes = statistics.header_bytes;
                result->payload_bytes = statistics.payload_bytes;
                result->padding_bytes = statistics.padding_bytes;
            }
            return MS_OK;
        } catch (const FastLocalError &error) {
            switch (error.code()) {
                case FastLocalErrorCode::Crypto:
                    return MS_ERR_CRYPTO;
                case FastLocalErrorCode::Incomplete:
                    return MS_ERR_INCOMPLETE;
                case FastLocalErrorCode::UnsupportedVersion:
                    return MS_ERR_UNSUPPORTED_FORMAT;
                case FastLocalErrorCode::Corrupt:
                case FastLocalErrorCode::InvalidFormat:
                    return MS_ERR_CORRUPT;
                default:
                    return MS_ERR_IO;
            }
        } catch (...) {
            return MS_ERR_DECODE_FAILED;
        }
    }

    Decoder decoder;
    std::size_t total_extracted = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;
    int64_t total_frames_read = 0;

    try {
        VideoDecoder video_decoder(input_path, &profiler);
        const int64_t total = video_decoder.total_frames();

        while (!video_decoder.is_eof()) {
            if (found_last_chunk && decoded_chunks >= last_chunk_index + 1)
                break;

            if (options->progress) {
                const auto cur = static_cast<uint64_t>(video_decoder.frames_read());
                if (const uint64_t tot = total >= 0 ? static_cast<uint64_t>(total) : 0; options->progress(cur, tot, options->progress_user) != 0) {
                    return MS_ERR_DECODE_FAILED;
                }
            }

            auto frame_packets = video_decoder.decode_next_frame();
            if (frame_packets.empty()) continue;

            for (auto &pkt_data : frame_packets) {
                ++total_extracted;
                classify_packet(std::span<const std::byte>(
                                    pkt_data.data(), pkt_data.size()),
                                source_packets, repair_packets);

                {
                    ScopedTimer timer(
                        &profiler, PerformanceStage::FecChunkRecovery);
                    if (pkt_data.size() >= HEADER_SIZE &&
                        Decoder::validate_raw_packet_crc(
                            std::span<const std::byte>(
                                pkt_data.data(), pkt_data.size()))) {
                        const auto flags = static_cast<uint8_t>(
                            pkt_data[FLAGS_OFF]);
                        uint32_t chunk_idx = 0;
                        std::memcpy(
                            &chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF,
                            sizeof(chunk_idx));
                        if (chunk_idx > max_chunk_index) {
                            max_chunk_index = chunk_idx;
                        }
                        if (flags & LastChunk) {
                            found_last_chunk = true;
                            last_chunk_index = chunk_idx;
                        }
                    }

                    const std::span<const std::byte> data(
                        pkt_data.data(), pkt_data.size());
                    if (auto res = decoder.process_packet(data, false);
                        res && res->success) {
                        ++decoded_chunks;
                    }
                }
            }
        }

        total_frames_read = video_decoder.frames_read();
    } catch (...) {
        return MS_ERR_DECODE_FAILED;
    }

    if (total_extracted == 0) {
        return MS_ERR_DECODE_FAILED;
    }

    const uint32_t expected_chunks = found_last_chunk
        ? last_chunk_index + 1
        : max_chunk_index + 1;

    if (decoded_chunks < expected_chunks) {
        return MS_ERR_INCOMPLETE;
    }

    if (decoder.is_encrypted()) {
        if (!options->password || options->password_len == 0) {
            return MS_ERR_CRYPTO;
        }
        {
            ScopedTimer timer(&profiler, PerformanceStage::Postprocess);
            const std::span<const std::byte> pw(
                reinterpret_cast<const std::byte *>(options->password),
                options->password_len);
            auto key = derive_key(pw, *decoder.file_id());
            decoder.set_decrypt_key(key);
            secure_zero(std::span<std::byte>(key));
        }
    }

    if (!decoder.write_assembled_file(
            output_path, expected_chunks, &profiler)) {
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return MS_ERR_DECODE_FAILED;
    }

    if (decoder.is_encrypted()) decoder.clear_decrypt_key();

    const auto output_size = std::filesystem::file_size(output_path);
    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_DECODE,
                video_size, output_size, expected_chunks, total_extracted,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames_read));
    if (result) {
        result->encoding_mode = MS_ENCODING_MODE_RESILIENT;
    }

    return MS_OK;
}

ms_status_t ms_stream_encode(const ms_stream_encode_options_t *options, ms_result_t *result) {
    if (!options || !options->input_path || !options->stream_url) {
        return MS_ERR_INVALID_ARGS;
    }
    if (options->encrypt && (!options->password || options->password_len == 0)) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string input_path(options->input_path);
    const std::string stream_url(options->stream_url);
    PerformanceProfiler profiler;
    EncodingReliabilityOptions reliability;
    try {
        reliability = reliability_from_encode_options(
            options->repair_ratio, options->repair_ratio_is_set);
    } catch (...) {
        return MS_ERR_INVALID_ARGS;
    }

    if (!std::filesystem::exists(input_path)) {
        return MS_ERR_FILE_NOT_FOUND;
    }

    const bool encrypt = options->encrypt != 0;
    const std::size_t chunk_size = encrypt ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : 0;
    uint64_t input_size = 0;
    std::unique_ptr<FileChunkReader> reader_storage;
    try {
        ScopedTimer timer(&profiler, PerformanceStage::InputRead);
        input_size = std::filesystem::file_size(input_path);
        reader_storage = std::make_unique<FileChunkReader>(
            input_path.c_str(), chunk_size);
    } catch (...) {
        return MS_ERR_IO;
    }
    const FileChunkReader &reader = *reader_storage;
    std::size_t num_chunks = 0;
    {
        ScopedTimer timer(&profiler, PerformanceStage::ChunkCreation);
        num_chunks = reader.num_chunks();
    }

    const auto file_id = make_encoding_file_id();
    const Encoder encoder(
        file_id, to_internal_hash(options->hash_algorithm), reliability);

    std::array<std::byte, CRYPTO_KEY_BYTES> key{};
    if (encrypt) {
        ScopedTimer timer(&profiler, PerformanceStage::Preprocess);
        const std::span pw(reinterpret_cast<const std::byte *>(options->password),
                           options->password_len);
        key = derive_key(pw, file_id);
    }

    std::size_t total_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    int64_t total_frames = 0;
    const int bitrate = options->bitrate_kbps > 0 ? options->bitrate_kbps : FRAME_BITRATE;
    const int width = options->width > 0 ? options->width : FRAME_WIDTH_STREAM;
    const int height = options->height > 0 ? options->height : FRAME_HEIGHT_STREAM;
    const int fps = options->fps > 0 ? options->fps : FRAME_FPS;

    try {
        StreamEncoder stream_encoder(
            stream_url, bitrate, width, height, fps, &profiler);

        const int batch_size = std::max(1, omp_get_max_threads());

        using BatchResults = std::vector<std::pair<std::vector<Packet>, ChunkManifestEntry>>;

        auto fec_encode_batch = [&](const std::size_t batch_start, const int batch_count) -> BatchResults {
            BatchResults results(batch_count);
            bool batch_error = false;

#pragma omp parallel for schedule(dynamic)
            for (int j = 0; j < batch_count; ++j) {
                if (batch_error) continue;
                try {
                    const std::size_t i = batch_start + j;
                    std::span<const std::byte> data_to_encode;
                    {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::ChunkCreation);
                        data_to_encode = reader.chunk_view(i);
                    }
                    std::vector<std::byte> encrypted_buf;
                    if (encrypt) {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::Preprocess);
                        encrypted_buf = encrypt_chunk(
                            data_to_encode, key, file_id,
                            static_cast<uint32_t>(i));
                        data_to_encode = encrypted_buf;
                    }
                    const bool is_last = (i == num_chunks - 1);
                    {
                        ScopedTimer timer(
                            &profiler, PerformanceStage::FecRepairGeneration);
                        results[j] = encoder.encode_chunk(
                            static_cast<uint32_t>(i), data_to_encode,
                            is_last, encrypt);
                    }
                } catch (...) {
                    batch_error = true;
                }
            }

            if (batch_error) throw std::runtime_error("batch FEC encoding failed");
            return results;
        };

        const auto first_end = std::min(static_cast<std::size_t>(batch_size), num_chunks);
        std::future<BatchResults> pending =
            std::async(std::launch::async, fec_encode_batch,
                       static_cast<std::size_t>(0), static_cast<int>(first_end));

        for (std::size_t batch_start = 0; batch_start < num_chunks;
             batch_start += batch_size) {
            const std::size_t batch_end =
                std::min(batch_start + static_cast<std::size_t>(batch_size),
                         num_chunks);
            const int batch_count = static_cast<int>(batch_end - batch_start);

            if (options->progress) {
                if (options->progress(static_cast<uint64_t>(batch_start),
                                      static_cast<uint64_t>(num_chunks),
                                      options->progress_user) != 0) {
                    if (pending.valid()) pending.wait();
                    if (encrypt) secure_zero(std::span<std::byte>(key));
                    return MS_ERR_ENCODE_FAILED;
                }
            }

            auto results = pending.get();

            if (const std::size_t next_start = batch_start + batch_size; next_start < num_chunks) {
                const auto next_end = std::min(
                    next_start + static_cast<std::size_t>(batch_size), num_chunks);
                const int next_count = static_cast<int>(next_end - next_start);
                pending = std::async(std::launch::async,
                                     fec_encode_batch, next_start, next_count);
            }

            for (int j = 0; j < batch_count; ++j) {
                total_packets += results[j].first.size();
                for (const auto &packet: results[j].first) {
                    classify_packet(std::span(packet.bytes), source_packets,
                                    repair_packets);
                }
                stream_encoder.encode_packets(results[j].first);
            }
        }

        stream_encoder.finalize();
        total_frames = stream_encoder.frames_written();
    } catch (const std::exception &e) {
        fprintf(stderr, "Stream encode error: %s\n", e.what());
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    } catch (...) {
        fprintf(stderr, "Stream encode error: unknown exception\n");
        if (encrypt) secure_zero(std::span<std::byte>(key));
        return MS_ERR_ENCODE_FAILED;
    }

    if (encrypt) secure_zero(std::span<std::byte>(key));

    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_STREAM_ENCODE,
                input_size, 0, num_chunks, total_packets,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames),
                reliability.repair_ratio);

    return MS_OK;
}

ms_status_t ms_stream_decode(const ms_stream_decode_options_t *options, ms_result_t *result) {
    if (!options || !options->stream_url || !options->output_path) {
        return MS_ERR_INVALID_ARGS;
    }

    const std::string stream_url(options->stream_url);
    const std::string output_path(options->output_path);
    PerformanceProfiler profiler;

    Decoder decoder;
    std::size_t total_extracted = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    std::size_t decoded_chunks = 0;
    uint32_t max_chunk_index = 0;
    bool found_last_chunk = false;
    uint32_t last_chunk_index = 0;
    int64_t total_frames_read = 0;

    try {
        const int max_retries = options->timeout_sec > 0 ? options->timeout_sec : 30;
        std::unique_ptr<VideoDecoder> vdec;
        for (int attempt = 0; attempt < max_retries; ++attempt) {
            try {
                vdec = std::make_unique<VideoDecoder>(
                    stream_url, &profiler);
                break;
            } catch (...) {
                if (attempt + 1 >= max_retries) throw;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        const int64_t total = vdec->total_frames();

        while (!vdec->is_eof()) {
            if (found_last_chunk && decoded_chunks >= last_chunk_index + 1)
                break;

            if (options->progress) {
                const auto cur = static_cast<uint64_t>(vdec->frames_read());
                if (const uint64_t tot = total >= 0 ? static_cast<uint64_t>(total) : 0; options->progress(cur, tot, options->progress_user) != 0) {
                    return MS_ERR_DECODE_FAILED;
                }
            }

            auto frame_packets = vdec->decode_next_frame();
            if (frame_packets.empty()) continue;

            for (auto &pkt_data : frame_packets) {
                ++total_extracted;
                classify_packet(std::span<const std::byte>(
                                    pkt_data.data(), pkt_data.size()),
                                source_packets, repair_packets);

                {
                    ScopedTimer timer(
                        &profiler, PerformanceStage::FecChunkRecovery);
                    if (pkt_data.size() >= HEADER_SIZE &&
                        Decoder::validate_raw_packet_crc(
                            std::span<const std::byte>(
                                pkt_data.data(), pkt_data.size()))) {
                        const auto flags = static_cast<uint8_t>(
                            pkt_data[FLAGS_OFF]);
                        uint32_t chunk_idx = 0;
                        std::memcpy(
                            &chunk_idx, pkt_data.data() + CHUNK_INDEX_OFF,
                            sizeof(chunk_idx));
                        if (chunk_idx > max_chunk_index) {
                            max_chunk_index = chunk_idx;
                        }
                        if (flags & LastChunk) {
                            found_last_chunk = true;
                            last_chunk_index = chunk_idx;
                        }
                    }

                    const std::span<const std::byte> data(
                        pkt_data.data(), pkt_data.size());
                    if (const auto res = decoder.process_packet(data, false);
                        res && res->success) {
                        ++decoded_chunks;
                    }
                }
            }
        }

        total_frames_read = vdec->frames_read();
    } catch (const std::exception &e) {
        fprintf(stderr, "Stream decode error: %s\n", e.what());
        return MS_ERR_DECODE_FAILED;
    } catch (...) {
        fprintf(stderr, "Stream decode error: unknown exception\n");
        return MS_ERR_DECODE_FAILED;
    }

    if (total_extracted == 0) {
        return MS_ERR_DECODE_FAILED;
    }

    const uint32_t expected_chunks = found_last_chunk
        ? last_chunk_index + 1
        : max_chunk_index + 1;

    if (decoded_chunks < expected_chunks) {
        return MS_ERR_INCOMPLETE;
    }

    if (decoder.is_encrypted()) {
        if (!options->password || options->password_len == 0) {
            return MS_ERR_CRYPTO;
        }
        {
            ScopedTimer timer(&profiler, PerformanceStage::Postprocess);
            const std::span<const std::byte> pw(
                reinterpret_cast<const std::byte *>(options->password),
                options->password_len);
            auto key = derive_key(pw, *decoder.file_id());
            decoder.set_decrypt_key(key);
            secure_zero(std::span<std::byte>(key));
        }
    }

    if (!decoder.write_assembled_file(
            output_path, expected_chunks, &profiler)) {
        if (decoder.is_encrypted()) decoder.clear_decrypt_key();
        return MS_ERR_DECODE_FAILED;
    }

    if (decoder.is_encrypted()) decoder.clear_decrypt_key();

    const auto output_size = std::filesystem::file_size(output_path);
    profiler.finish();
    fill_result(result, profiler, MS_OPERATION_STREAM_DECODE,
                0, output_size, expected_chunks, total_extracted,
                source_packets, repair_packets,
                static_cast<uint64_t>(total_frames_read));

    return MS_OK;
}

const char *ms_status_string(const ms_status_t status) {
    switch (status) {
        case MS_OK:              return "success";
        case MS_ERR_INVALID_ARGS: return "invalid arguments";
        case MS_ERR_FILE_NOT_FOUND: return "file not found";
        case MS_ERR_IO:          return "I/O error";
        case MS_ERR_ENCODE_FAILED: return "encoding failed";
        case MS_ERR_DECODE_FAILED: return "decoding failed";
        case MS_ERR_CRYPTO:      return "encryption/decryption error";
        case MS_ERR_INCOMPLETE:  return "incomplete data";
        case MS_ERR_INSUFFICIENT_DISK: return "insufficient disk space";
        case MS_ERR_PREFLIGHT_STALE: return "preflight estimate is stale";
        case MS_ERR_UNSUPPORTED_FORMAT: return "unsupported video format";
        case MS_ERR_CORRUPT: return "corrupted Fast Local data";
        default:                 return "unknown error";
    }
}

const char *ms_version(void) {
    return "1.3.0";
}

size_t ms_format_performance_report(const ms_result_t *result,
                                    char *buffer,
                                    const size_t buffer_size) {
    if (!result) return 0;

    std::ostringstream out;
    out << "\n=== Performance report (" << operation_name(result->operation)
        << ") ===\n"
        << "Encoding mode: "
        << (result->encoding_mode == MS_ENCODING_MODE_FAST_LOCAL
                ? "Fast Local" : "Resilient / Platform")
        << "\n"
        << "Input / output: " << result->input_size << " B -> "
        << result->output_size << " B";
    if (result->input_size > 0) {
        out << "  (ratio " << std::fixed << std::setprecision(3)
            << result->output_input_ratio << "x)";
    }
    out << "\n";
    if ((result->operation == MS_OPERATION_ENCODE ||
         result->operation == MS_OPERATION_STREAM_ENCODE) &&
        result->encoding_mode != MS_ENCODING_MODE_FAST_LOCAL) {
        out << "Reliability:\n"
            << std::fixed << std::setprecision(2)
            << "  Repair percentage: "
            << result->selected_repair_percentage << "%\n"
            << std::setprecision(4)
            << "  Repair ratio:      "
            << result->selected_repair_ratio << "\n"
            << "  Source packets:    " << result->source_packets << "\n"
            << "  Repair packets:    " << result->repair_packets << "\n"
            << "  Repair/source:     "
            << result->repair_source_ratio << "\n"
            << "  Total packets:     " << result->total_packets << "\n";
    }
    out << "Chunks: " << result->total_chunks
        << "  Packets: " << result->total_packets
        << " (source " << result->source_packets
        << ", repair " << result->repair_packets << ")"
        << "  Frames: " << result->total_frames << "\n"
        << std::fixed << std::setprecision(3)
        << "Rates: " << result->average_frames_per_second << " frames/s, "
        << result->throughput_mib_per_second << " MiB/s\n";
    if (result->encoding_mode == MS_ENCODING_MODE_FAST_LOCAL) {
        out << "Fast Local layout:\n"
            << "  Frame payload capacity: "
            << result->frame_payload_capacity << " B\n"
            << "  Header bytes:           "
            << result->header_bytes << " B\n"
            << "  Stored payload bytes:   "
            << result->payload_bytes << " B\n"
            << "  Raw-frame padding:      "
            << result->padding_bytes << " B\n";
    }
    if (result->operation == MS_OPERATION_ENCODE) {
        out << "Estimate validation:\n";
        if (result->estimate_validation_available) {
            out << "  Estimated likely output: "
                << result->estimated_output_bytes << " B\n"
                << "  Estimated minimum:       "
                << result->estimated_output_min_bytes << " B\n"
                << "  Estimated maximum:       "
                << result->estimated_output_max_bytes << " B\n"
                << "  Actual output:           "
                << result->actual_output_bytes << " B\n"
                << "  Absolute error:          "
                << result->estimate_absolute_error_bytes << " B\n"
                << "  Relative error:          ";
            if (result->estimate_relative_error_available) {
                out << result->estimate_relative_error_percent << "%\n";
            } else {
                out << "unavailable\n";
            }
            out << "  Actual inside range:     "
                << (result->actual_inside_estimated_range == 1
                        ? "yes"
                        : "no")
                << "\n";
        } else {
            out << "  Output-size estimate unavailable; "
                   "accuracy metrics were not calculated.\n";
        }
        out << "  Preflight duration:      "
            << result->preflight_duration_seconds << " s\n"
            << "  Actual encode duration:  "
            << result->actual_encode_duration_seconds << " s\n";
    }
    out
        << "Stage timings:\n";

    for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
        if (!stage_applies(result->operation, i)) continue;
        const auto &timing = result->stage_timings[i];
        out << "  " << std::left << std::setw(34) << stage_names[i]
            << std::right << std::setw(10) << std::setprecision(6)
            << timing.seconds << " s  "
            << std::setw(8) << std::setprecision(2)
            << timing.percent_of_total << "%  "
            << "(" << timing.invocations << " calls)\n";
    }
    out << "  " << std::left << std::setw(34) << "Total wall time"
        << std::right << std::setw(10) << std::setprecision(6)
        << result->total_seconds << " s    100.00%\n"
        << "Note: timings from parallel stages are accumulated work time; "
           "their percentages may overlap.\n";

    const std::string report = out.str();
    const size_t required = report.size() + 1;
    if (buffer && buffer_size > 0) {
        const size_t copied = (std::min)(report.size(), buffer_size - 1);
        std::memcpy(buffer, report.data(), copied);
        buffer[copied] = '\0';
    }
    return required;
}

ms_status_t ms_write_benchmark_json(const ms_result_t *result,
                                    const char *output_path) {
    if (!result || !output_path || output_path[0] == '\0') {
        return MS_ERR_INVALID_ARGS;
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) return MS_ERR_IO;

    out << std::fixed << std::setprecision(9)
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"operation\": \"" << operation_name(result->operation) << "\",\n"
        << "  \"encoding_mode\": \""
        << (result->encoding_mode == MS_ENCODING_MODE_FAST_LOCAL
                ? "fast-local" : "resilient")
        << "\",\n"
        << "  \"input_size_bytes\": " << result->input_size << ",\n"
        << "  \"output_size_bytes\": " << result->output_size << ",\n"
        << "  \"output_input_ratio\": " << result->output_input_ratio << ",\n";
    if ((result->operation == MS_OPERATION_ENCODE ||
         result->operation == MS_OPERATION_STREAM_ENCODE) &&
        result->encoding_mode != MS_ENCODING_MODE_FAST_LOCAL) {
        out << "  \"reliability\": {\n"
            << "    \"repair_percentage\": "
            << result->selected_repair_percentage << ",\n"
            << "    \"repair_ratio\": "
            << result->selected_repair_ratio << ",\n"
            << "    \"source_packet_count\": "
            << result->source_packets << ",\n"
            << "    \"repair_packet_count\": "
            << result->repair_packets << ",\n"
            << "    \"repair_source_ratio\": "
            << result->repair_source_ratio << ",\n"
            << "    \"total_packet_count\": "
            << result->total_packets << "\n"
            << "  },\n";
    }
    if (result->encoding_mode == MS_ENCODING_MODE_FAST_LOCAL) {
        out << "  \"fast_local\": {\n"
            << "    \"format_version\": "
            << FAST_LOCAL_FORMAT_VERSION << ",\n"
            << "    \"frame_payload_capacity\": "
            << result->frame_payload_capacity << ",\n"
            << "    \"header_bytes\": "
            << result->header_bytes << ",\n"
            << "    \"payload_bytes\": "
            << result->payload_bytes << ",\n"
            << "    \"padding_bytes\": "
            << result->padding_bytes << "\n"
            << "  },\n";
    }
    if (result->operation == MS_OPERATION_ENCODE) {
        out << "  \"estimate_validation\": {\n"
            << "    \"available\": "
            << (result->estimate_validation_available
                    ? "true"
                    : "false")
            << ",\n"
            << "    \"estimated_output_bytes\": ";
        if (result->estimate_validation_available) {
            out << result->estimated_output_bytes;
        } else {
            out << "null";
        }
        out << ",\n    \"estimated_output_min_bytes\": ";
        if (result->estimate_validation_available) {
            out << result->estimated_output_min_bytes;
        } else {
            out << "null";
        }
        out << ",\n    \"estimated_output_max_bytes\": ";
        if (result->estimate_validation_available) {
            out << result->estimated_output_max_bytes;
        } else {
            out << "null";
        }
        out << ",\n    \"actual_output_bytes\": "
            << result->actual_output_bytes
            << ",\n    \"estimate_absolute_error_bytes\": ";
        if (result->estimate_validation_available) {
            out << result->estimate_absolute_error_bytes;
        } else {
            out << "null";
        }
        out << ",\n    \"estimate_relative_error_percent\": ";
        if (result->estimate_relative_error_available) {
            out << result->estimate_relative_error_percent;
        } else {
            out << "null";
        }
        out << ",\n    \"actual_inside_estimated_range\": ";
        if (result->actual_inside_estimated_range >= 0) {
            out << (result->actual_inside_estimated_range
                        ? "true"
                        : "false");
        } else {
            out << "null";
        }
        out << ",\n    \"preflight_duration_seconds\": "
            << result->preflight_duration_seconds
            << ",\n    \"actual_encode_duration_seconds\": "
            << result->actual_encode_duration_seconds
            << "\n  },\n";
    }
    out << "  \"chunks\": " << result->total_chunks << ",\n"
        << "  \"packets\": {\n"
        << "    \"total\": " << result->total_packets << ",\n"
        << "    \"source\": " << result->source_packets << ",\n"
        << "    \"repair\": " << result->repair_packets << "\n"
        << "  },\n"
        << "  \"frames\": " << result->total_frames << ",\n"
        << "  \"average_frames_per_second\": "
        << result->average_frames_per_second << ",\n"
        << "  \"throughput_mib_per_second\": "
        << result->throughput_mib_per_second << ",\n"
        << "  \"total_seconds\": " << result->total_seconds << ",\n"
        << "  \"parallel_stage_time_note\": "
           "\"Stage durations are accumulated work time and may overlap.\",\n"
        << "  \"stages\": {\n";

    bool first = true;
    for (std::size_t i = 0; i < MS_PERF_STAGE_COUNT; ++i) {
        if (!stage_applies(result->operation, i)) continue;
        if (!first) out << ",\n";
        first = false;
        const auto &timing = result->stage_timings[i];
        out << "    \"" << stage_json_names[i] << "\": {"
            << "\"seconds\": " << timing.seconds
            << ", \"percent_of_total\": " << timing.percent_of_total
            << ", \"invocations\": " << timing.invocations << "}";
    }
    out << "\n  }\n}\n";

    return out.good() ? MS_OK : MS_ERR_IO;
}

ms_status_t ms_write_encoding_estimate_json(
    const ms_encoding_estimate_t *estimate,
    const char *output_path) {
    if (!estimate || !output_path || output_path[0] == '\0') {
        return MS_ERR_INVALID_ARGS;
    }
    if (estimate->struct_version != MS_ENCODING_ESTIMATE_VERSION ||
        estimate->struct_size < sizeof(ms_encoding_estimate_t)) {
        return MS_ERR_INVALID_ARGS;
    }

    const auto write_json_string = [](std::ostream &out,
                                      const char *text) {
        out << '"';
        for (const unsigned char character :
             std::string(text ? text : "")) {
            switch (character) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (character < 0x20) {
                        out << "\\u00" << std::hex << std::setw(2)
                            << std::setfill('0')
                            << static_cast<unsigned>(character)
                            << std::dec << std::setfill(' ');
                    } else {
                        out << static_cast<char>(character);
                    }
            }
        }
        out << '"';
    };
    const auto optional_uint = [](std::ostream &out,
                                  const bool available,
                                  const uint64_t value) {
        if (available) out << value;
        else out << "null";
    };

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) return MS_ERR_IO;

    out << std::fixed << std::setprecision(9)
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"encoding_mode\": \""
        << (estimate->encoding_mode == MS_ENCODING_MODE_FAST_LOCAL
                ? "fast-local" : "resilient")
        << "\",\n"
        << "  \"input_size_bytes\": "
        << estimate->input_size_bytes << ",\n"
        << "  \"repair_percentage\": "
        << estimate->repair_percentage << ",\n"
        << "  \"repair_ratio\": "
        << estimate->repair_ratio << ",\n"
        << "  \"chunk_count\": " << estimate->chunk_count << ",\n"
        << "  \"source_packet_count\": "
        << estimate->source_packet_count << ",\n"
        << "  \"repair_packet_count\": "
        << estimate->repair_packet_count << ",\n"
        << "  \"total_packet_count\": "
        << estimate->total_packet_count << ",\n"
        << "  \"estimated_frame_count\": "
        << estimate->estimated_frame_count << ",\n"
        << "  \"estimated_video_duration_seconds\": "
        << estimate->estimated_video_duration_seconds << ",\n"
        << "  \"header_bytes\": "
        << estimate->header_bytes << ",\n"
        << "  \"frame_payload_capacity\": "
        << estimate->frame_payload_capacity << ",\n"
        << "  \"payload_bytes\": "
        << estimate->payload_bytes << ",\n"
        << "  \"padding_bytes\": "
        << estimate->padding_bytes << ",\n"
        << "  \"estimated_output_bytes\": ";
    optional_uint(
        out, estimate->output_size_estimate_available != 0,
        estimate->estimated_output_bytes);
    out << ",\n  \"estimated_output_min_bytes\": ";
    optional_uint(
        out, estimate->output_size_estimate_available != 0,
        estimate->estimated_output_min_bytes);
    out << ",\n  \"estimated_output_max_bytes\": ";
    optional_uint(
        out, estimate->output_size_estimate_available != 0,
        estimate->estimated_output_max_bytes);
    out << ",\n  \"output_size_estimate_available\": "
        << (estimate->output_size_estimate_available ? "true" : "false")
        << ",\n  \"available_disk_bytes\": ";
    optional_uint(
        out, estimate->disk_space_known != 0,
        estimate->available_disk_bytes);
    out << ",\n  \"disk_space_known\": "
        << (estimate->disk_space_known ? "true" : "false")
        << ",\n  \"safety_margin_bytes\": ";
    optional_uint(
        out, estimate->required_disk_space_known != 0,
        estimate->safety_margin_bytes);
    out << ",\n  \"required_disk_bytes\": ";
    optional_uint(
        out, estimate->required_disk_space_known != 0,
        estimate->required_disk_bytes);
    out << ",\n  \"disk_space_sufficient\": ";
    if (estimate->disk_space_sufficient < 0) {
        out << "null";
    } else {
        out << (estimate->disk_space_sufficient ? "true" : "false");
    }
    out << ",\n  \"can_start_encoding\": "
        << (estimate->can_start_encoding ? "true" : "false")
        << ",\n  \"estimation_method\": ";
    write_json_string(out, estimate->estimation_method);
    out << ",\n  \"probe_frame_count\": "
        << estimate->probe_frame_count
        << ",\n  \"probe_duration_seconds\": "
        << estimate->probe_duration_seconds
        << ",\n  \"preflight_duration_seconds\": "
        << estimate->preflight_duration_seconds
        << ",\n  \"warning\": ";
    write_json_string(out, estimate->warning);
    out << ",\n  \"error\": ";
    write_json_string(out, estimate->error);
    out << "\n}\n";

    return out.good() ? MS_OK : MS_ERR_IO;
}
