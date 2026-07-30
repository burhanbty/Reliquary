#include "youtube_capacity_lab.h"

#include "chunker.h"
#include "decoder.h"
#include "encoder.h"
#include "encoding_reliability.h"
#include "libs/picosha2.h"
#include "safe_output.h"
#include "video_decoder.h"
#include "video_encoder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace youtube_capacity_lab {
namespace {

using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kProductionStrength = COEFFICIENT_STRENGTH;
constexpr uint64_t kPayloadSeed = 0x5653584341504c42ULL; // "VSXCAPLB"

uint64_t checked_mul(const uint64_t a, const uint64_t b,
                     const char *label) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        throw std::overflow_error(std::string(label) + " overflow");
    return a * b;
}

uint64_t ceil_div(const uint64_t n, const uint64_t d) {
    if (d == 0) throw std::invalid_argument("division by zero");
    return n / d + (n % d != 0);
}

std::string now_utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value =
        std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string compact_utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value =
        std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y%m%dT%H%M%SZ");
    return out.str();
}

std::string json_escape(const std::string &value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20)
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(c)
                        << std::dec;
                else
                    out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string q(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}

std::string stable_hash_id(const std::string &canonical,
                           const std::size_t characters = 12) {
    std::string hash = picosha2::hash256_hex_string(canonical);
    std::transform(hash.begin(), hash.end(), hash.begin(),
                   [](const unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return hash.substr(0, characters);
}

uint64_t stable_seed(const ExperimentConfig &config) {
    ExperimentConfig fair = config;
    fair.repair_basis_points = 0;
    const std::string hash =
        picosha2::hash256_hex_string(fair.canonical_serialization());
    uint64_t result = kPayloadSeed;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = hash[i];
        result = result * 33 +
            static_cast<unsigned char>(c);
    }
    return result;
}

std::string relative_path(const std::filesystem::path &root,
                          const std::filesystem::path &path) {
    const auto relative =
        path.lexically_normal().lexically_relative(
            root.lexically_normal());
    if (relative.empty() || relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == ".."))
        throw std::runtime_error(
            "artifact path is outside the experiment folder");
    return relative.generic_string();
}

std::filesystem::path resolve_path(
    const std::filesystem::path &root, const std::string &relative) {
    if (relative.empty()) return {};
    const std::filesystem::path path(relative);
    if (path.is_absolute())
        throw std::runtime_error(
            "Capacity manifest contains an absolute artifact path");
    const auto normalized = (root / path).lexically_normal();
    const auto canonical_root =
        std::filesystem::weakly_canonical(root);
    const auto canonical_parent =
        std::filesystem::weakly_canonical(normalized.parent_path());
    const auto candidate = canonical_parent / normalized.filename();
    const auto [root_end, candidate_end] = std::mismatch(
        canonical_root.begin(), canonical_root.end(),
        candidate.begin(), candidate.end());
    (void) candidate_end;
    if (root_end != canonical_root.end())
        throw std::runtime_error(
            "Capacity manifest path escapes experiment folder");
    return candidate;
}

std::string case_token(const ExperimentConfig &config) {
    std::ostringstream out;
    out << (config.resolution_height == 2160 ? "2160p" : "1080p")
        << "_b" << config.block_width
        << "_" << config.bits_per_block << "bit_s"
        << config.signal_milli / 10
        << "_r" << config.repair_basis_points / 100;
    return out.str();
}

std::string candidate_filename(const CapacityCase &test_case) {
    return "VSX_CAP_" + test_case.config_id + "_" +
        case_token(test_case.config) + ".mp4";
}

double signal_strength(const ExperimentConfig &config) {
    return kProductionStrength * config.signal_multiplier();
}

std::array<double, 4> modulation_levels(
    const ExperimentConfig &config) {
    const double strength = signal_strength(config);
    if (config.bits_per_block == 1)
        return {-strength, strength, 0.0, 0.0};
    return {-1.5 * strength, -0.5 * strength,
             0.5 * strength, 1.5 * strength};
}

const std::array<double, 4> &effective_modulation_levels(
    const ExperimentConfig &config) {
    static std::mutex mutex;
    static std::map<std::string, std::array<double, 4>> cache;
    const std::string key = config.canonical_serialization();
    static thread_local std::string last_key;
    static thread_local const std::array<double, 4> *last = nullptr;
    if (last && last_key == key) return *last;
    std::scoped_lock lock(mutex);
    auto [it, inserted] = cache.try_emplace(key);
    if (inserted) {
        const int count =
            config.bits_per_block == 1 ? 2 : 4;
        for (int symbol = 0; symbol < count; ++symbol) {
            const auto block = make_symbol_block(
                config, static_cast<uint8_t>(symbol));
            const int n = config.block_width;
            std::vector<double> pixels(block.begin(), block.end());
            std::vector<double> coefficients;
            forward_dct(pixels, n, coefficients);
            it->second[static_cast<std::size_t>(symbol)] =
                coefficients[1];
        }
    }
    last_key = key;
    last = &it->second;
    return *last;
}

void encode_frame_bytes(const std::span<const std::byte> bytes,
                        const ExperimentConfig &config,
                        std::vector<std::byte> &frame) {
    const auto geometry = compute_geometry(config);
    frame.assign(
        checked_mul(static_cast<uint64_t>(config.resolution_width),
                    static_cast<uint64_t>(config.resolution_height),
                    "frame bytes"),
        std::byte{128});
    const auto *source =
        reinterpret_cast<const uint8_t *>(bytes.data());
    const uint64_t total_bits = checked_mul(bytes.size(), 8, "frame bits");
    const uint64_t active_blocks = std::min(
        geometry.data_blocks,
        ceil_div(total_bits,
                 static_cast<uint64_t>(config.bits_per_block)));
    std::array<std::vector<uint8_t>, 4> patterns;
    const int symbol_count = config.bits_per_block == 1 ? 2 : 4;
    for (int i = 0; i < symbol_count; ++i)
        patterns[static_cast<std::size_t>(i)] =
            make_symbol_block(config, static_cast<uint8_t>(i));

    for (uint64_t block_index = 0;
         block_index < active_blocks; ++block_index) {
        uint8_t raw_bits = 0;
        for (int bit = 0; bit < config.bits_per_block; ++bit) {
            const uint64_t bit_index =
                block_index * config.bits_per_block + bit;
            raw_bits <<= 1;
            if (bit_index < total_bits) {
                const uint64_t byte_index = bit_index / 8;
                const int shift = 7 - static_cast<int>(bit_index % 8);
                raw_bits |= (source[byte_index] >> shift) & 1U;
            }
        }
        const uint8_t symbol =
            config.bits_per_block == 1
                ? raw_bits : gray_symbol_for_bits(raw_bits);
        const auto &block = patterns[symbol];
        const int block_row =
            static_cast<int>(block_index /
                             geometry.blocks_per_row);
        const int block_column =
            static_cast<int>(block_index %
                             geometry.blocks_per_row);
        const int base_x = block_column * config.block_width;
        const int base_y = block_row * config.block_height;
        for (int y = 0; y < config.block_height; ++y) {
            auto *destination = reinterpret_cast<uint8_t *>(
                frame.data() +
                static_cast<std::size_t>(base_y + y) *
                    config.resolution_width +
                base_x);
            std::memcpy(
                destination,
                block.data() +
                    static_cast<std::size_t>(y) * config.block_width,
                static_cast<std::size_t>(config.block_width));
        }
    }
}

void extract_frame_bytes(
    const std::vector<std::byte> &frame,
    const ExperimentConfig &config,
    std::vector<std::byte> &bytes,
    std::vector<SymbolDecision> *decisions = nullptr) {
    const auto geometry = compute_geometry(config);
    bytes.assign(geometry.raw_bytes_per_frame, std::byte{0});
    auto *out = reinterpret_cast<uint8_t *>(bytes.data());
    if (decisions) {
        decisions->clear();
        decisions->reserve(static_cast<std::size_t>(
            geometry.data_blocks));
    }
    uint64_t output_bit = 0;
    for (uint64_t block_index = 0;
         block_index < geometry.data_blocks; ++block_index) {
        const int block_row =
            static_cast<int>(block_index /
                             geometry.blocks_per_row);
        const int block_column =
            static_cast<int>(block_index %
                             geometry.blocks_per_row);
        const int base_x = block_column * config.block_width;
        const int base_y = block_row * config.block_height;
        const auto decision = decode_symbol(
            reinterpret_cast<const uint8_t *>(
                frame.data() +
                static_cast<std::size_t>(base_y) *
                    config.resolution_width +
                base_x),
            config.resolution_width, config);
        if (decisions) decisions->push_back(decision);
        uint8_t raw_bits =
            config.bits_per_block == 1
                ? decision.symbol
                : bits_for_gray_symbol(decision.symbol);
        for (int bit = config.bits_per_block - 1;
             bit >= 0; --bit) {
            if (output_bit >= geometry.raw_bytes_per_frame * 8)
                break;
            const uint8_t value =
                (raw_bits >> bit) & 1U;
            out[output_bit / 8] |= static_cast<uint8_t>(
                value << (7 - output_bit % 8));
            ++output_bit;
        }
    }
}

struct EncodedArtifacts {
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    uint64_t total_packets = 0;
    uint64_t frames = 0;
    double seconds = 0.0;
    std::vector<std::byte> packet_stream;
};

EncodedArtifacts encode_master(
    const std::filesystem::path &payload,
    const std::filesystem::path &master,
    const ExperimentConfig &config) {
    const auto started = Clock::now();
    const EncodingReliabilityOptions reliability{
        repair_percentage_to_ratio(config.repair_percent())};
    FileChunkReader reader(payload.string().c_str());
    Encoder encoder(make_encoding_file_id(),
                    HashAlgorithm::CRC32, reliability);
    EncodedArtifacts result;
    SafeOutputFile safe(master);
    ResilientVideoConfig video_config{
        .width = config.resolution_width,
        .height = config.resolution_height,
        .fps = config.fps,
        .codec = VIDEO_CODEC,
        .container = VIDEO_CONTAINER,
        .explicit_frame_duration = true};
    const auto geometry = compute_geometry(config);
    std::vector<std::byte> pending;
    pending.reserve(static_cast<std::size_t>(
        geometry.raw_bytes_per_frame * 2));
    std::vector<std::byte> frame;
    {
        VideoEncoder video(safe.partial_path().string(), video_config);
        auto flush_frame = [&](const std::size_t count) {
            encode_frame_bytes(
                std::span<const std::byte>(pending.data(), count),
                config, frame);
            video.encode_gray8_frame(frame);
            pending.erase(
                pending.begin(),
                pending.begin() +
                    static_cast<std::ptrdiff_t>(count));
        };
        for (std::size_t chunk = 0;
             chunk < reader.num_chunks(); ++chunk) {
            const auto [packets, entry] = encoder.encode_chunk(
                static_cast<uint32_t>(chunk),
                reader.chunk_view(chunk),
                chunk + 1 == reader.num_chunks(), false);
            result.source_packets += entry.N;
            result.repair_packets +=
                packets.size() - entry.N;
            result.total_packets += packets.size();
            for (const auto &packet : packets) {
                pending.insert(pending.end(),
                               packet.bytes.begin(),
                               packet.bytes.end());
                result.packet_stream.insert(
                    result.packet_stream.end(),
                    packet.bytes.begin(), packet.bytes.end());
                while (pending.size() >=
                       geometry.raw_bytes_per_frame)
                    flush_frame(static_cast<std::size_t>(
                        geometry.raw_bytes_per_frame));
            }
        }
        if (!pending.empty()) flush_frame(pending.size());
        video.finalize();
        result.frames =
            static_cast<uint64_t>(video.frames_written());
    }
    safe.commit();
    result.seconds = std::chrono::duration<double>(
        Clock::now() - started).count();
    return result;
}

std::size_t packet_size_from_header(
    const std::span<const std::byte> bytes) {
    if (bytes.size() < 5)
        return PACKET_SIZE;
    return static_cast<uint8_t>(bytes[4]) == VERSION_ID_V2
        ? HEADER_SIZE_V2 + SYMBOL_SIZE_BYTES
        : HEADER_SIZE + SYMBOL_SIZE_BYTES;
}

void scan_packets(std::vector<std::byte> &buffer,
                  std::vector<std::vector<std::byte>> &packets) {
    std::size_t offset = 0;
    while (offset + MAGIC_SIZE <= buffer.size()) {
        uint32_t magic = 0;
        std::memcpy(&magic, buffer.data() + offset, sizeof(magic));
        if (magic != MAGIC_ID) {
            ++offset;
            continue;
        }
        const auto remaining = std::span<const std::byte>(
            buffer.data() + offset, buffer.size() - offset);
        const std::size_t packet_size =
            packet_size_from_header(remaining);
        if (offset + packet_size > buffer.size()) break;
        packets.emplace_back(
            buffer.begin() + static_cast<std::ptrdiff_t>(offset),
            buffer.begin() +
                static_cast<std::ptrdiff_t>(offset + packet_size));
        offset += packet_size;
    }
    buffer.erase(
        buffer.begin(),
        buffer.begin() + static_cast<std::ptrdiff_t>(offset));
}

CaseResult decode_video(
    const std::filesystem::path &video_path,
    const std::filesystem::path &payload_path,
    const std::filesystem::path &restored_path,
    const ExperimentConfig &config,
    const EncodedArtifacts *expected,
    const std::string &source_type,
    const std::string &profile) {
    CaseResult result;
    result.source_type = source_type;
    result.simulation_profile = profile;
    result.file_size = std::filesystem::file_size(video_path);
    const auto technical =
        youtube_test_lab::analyze_video(video_path);
    result.codec = technical.codec;
    result.returned_width = technical.width;
    result.returned_height = technical.height;
    result.returned_fps = technical.fps;
    result.bitrate = technical.bitrate;
    result.metadata_valid =
        technical.width == config.resolution_width &&
        technical.height == config.resolution_height &&
        std::abs(technical.fps - config.fps) < 0.1 &&
        technical.duration_seconds >= 1.95;
    const auto started = Clock::now();
    Decoder decoder;
    std::unordered_set<uint64_t> packet_ids;
    std::unordered_map<uint32_t, uint32_t> chunk_thresholds;
    std::vector<std::byte> accumulated;
    std::vector<std::byte> raw_frame;
    std::vector<std::byte> decoded_bytes;
    std::vector<SymbolDecision> decisions;
    double confidence_sum = 0.0;
    result.telemetry.minimum_confidence = 1.0;
    std::size_t expected_offset = 0;
    uint64_t symbols_remaining =
        expected ? checked_mul(
            expected->packet_stream.size(), 8,
            "expected bit count") /
            config.bits_per_block : 0;
    try {
        VideoDecoder video(video_path.string());
        while (video.decode_next_gray8_frame(raw_frame)) {
            ++result.telemetry.frames_read;
            extract_frame_bytes(
                raw_frame, config, decoded_bytes,
                expected ? &decisions : nullptr);
            if (expected) {
                const std::size_t compare_bytes = std::min(
                    decoded_bytes.size(),
                    expected->packet_stream.size() -
                        std::min(expected_offset,
                                 expected->packet_stream.size()));
                for (std::size_t i = 0; i < compare_bytes; ++i) {
                    const auto actual =
                        static_cast<uint8_t>(decoded_bytes[i]);
                    const auto wanted = static_cast<uint8_t>(
                        expected->packet_stream[expected_offset + i]);
                    result.telemetry.bit_errors +=
                        static_cast<uint64_t>(
                            std::popcount(
                                static_cast<unsigned>(actual ^ wanted)));
                    result.telemetry.bits_compared += 8;
                }
                const uint64_t frame_symbols = std::min<uint64_t>(
                    decisions.size(), symbols_remaining);
                for (uint64_t i = 0; i < frame_symbols; ++i) {
                    const uint64_t bit_offset =
                        static_cast<uint64_t>(expected_offset) * 8 +
                        i * config.bits_per_block;
                    uint8_t wanted_bits = 0;
                    for (int bit = 0;
                         bit < config.bits_per_block; ++bit) {
                        wanted_bits <<= 1;
                        const uint64_t index = bit_offset + bit;
                        const auto byte = static_cast<uint8_t>(
                            expected->packet_stream[index / 8]);
                        wanted_bits |=
                            (byte >> (7 - index % 8)) & 1U;
                    }
                    const uint8_t wanted_symbol =
                        config.bits_per_block == 1
                            ? wanted_bits
                            : gray_symbol_for_bits(wanted_bits);
                    result.telemetry.symbol_errors +=
                        decisions[i].symbol != wanted_symbol;
                    ++result.telemetry.symbols_compared;
                    confidence_sum += decisions[i].confidence;
                    result.telemetry.minimum_confidence = std::min(
                        result.telemetry.minimum_confidence,
                        decisions[i].confidence);
                }
                symbols_remaining -= frame_symbols;
                expected_offset += compare_bytes;
            }
            accumulated.insert(
                accumulated.end(),
                decoded_bytes.begin(), decoded_bytes.end());
            std::vector<std::vector<std::byte>> packets;
            scan_packets(accumulated, packets);
            for (const auto &raw : packets) {
                ++result.telemetry.extracted_packets;
                const auto parsed = Decoder::parse_packet(raw);
                if (!parsed ||
                    !Decoder::validate_packet_crc(*parsed)) {
                    ++result.telemetry.crc_invalid_packets;
                    continue;
                }
                const auto &header = parsed->header;
                const uint64_t id =
                    static_cast<uint64_t>(header.chunk_index) << 32 |
                    header.esi;
                if (!packet_ids.insert(id).second) {
                    ++result.telemetry.duplicate_packets;
                    continue;
                }
                ++result.telemetry.valid_unique_packets;
                if ((header.flags & IsRepairSymbol) != 0)
                    ++result.telemetry.repair_packets;
                else
                    ++result.telemetry.source_packets;
                chunk_thresholds.emplace(
                    header.chunk_index, header.k);
                try {
                    (void) decoder.process_packet(*parsed, false);
                } catch (...) {
                    ++result.telemetry.crc_invalid_packets;
                }
            }
        }
        result.telemetry.required_packet_threshold =
            expected ? expected->source_packets :
            std::accumulate(
                chunk_thresholds.begin(), chunk_thresholds.end(),
                uint64_t{0}, [](const uint64_t sum, const auto &entry) {
                    return sum + entry.second;
                });
        if (expected) {
            result.telemetry.missing_packets =
                expected->total_packets >
                        result.telemetry.valid_unique_packets
                    ? expected->total_packets -
                          result.telemetry.valid_unique_packets
                    : 0;
            result.telemetry.packet_recovery_percent =
                expected->total_packets == 0 ? 0.0 :
                100.0 * result.telemetry.valid_unique_packets /
                    expected->total_packets;
        }
        result.telemetry.recovery_margin_packets =
            static_cast<int64_t>(
                result.telemetry.valid_unique_packets) -
            static_cast<int64_t>(
                result.telemetry.required_packet_threshold);
        result.telemetry.recovery_margin_percent =
            result.telemetry.required_packet_threshold == 0 ? 0.0 :
            100.0 * result.telemetry.recovery_margin_packets /
                result.telemetry.required_packet_threshold;
        result.telemetry.raw_ber =
            result.telemetry.bits_compared == 0 ? 0.0 :
            static_cast<double>(result.telemetry.bit_errors) /
                result.telemetry.bits_compared;
        result.telemetry.raw_ser =
            result.telemetry.symbols_compared == 0 ? 0.0 :
            static_cast<double>(result.telemetry.symbol_errors) /
                result.telemetry.symbols_compared;
        result.telemetry.average_confidence =
            result.telemetry.symbols_compared == 0 ? 0.0 :
            confidence_sum /
                result.telemetry.symbols_compared;
        if (result.telemetry.symbols_compared == 0)
            result.telemetry.minimum_confidence = 0.0;
        const uint64_t payload_size =
            std::filesystem::file_size(payload_path);
        const uint32_t chunks = static_cast<uint32_t>(
            ceil_div(payload_size, CHUNK_SIZE_BYTES));
        SafeOutputFile restored(restored_path);
        if (!decoder.write_assembled_file(
                restored.partial_path().string(), chunks))
            throw std::runtime_error(
                "insufficient packets to assemble payload");
        restored.commit();
        result.restored_sha256 =
            youtube_test_lab::sha256_file(restored_path);
        result.sha256_match =
            result.restored_sha256 ==
            youtube_test_lab::sha256_file(payload_path);
        result.decode_completed = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    result.decode_seconds = std::chrono::duration<double>(
        Clock::now() - started).count();
    return result;
}

uint64_t directory_size(const std::filesystem::path &root) {
    std::error_code error;
    uint64_t total = 0;
    if (!std::filesystem::exists(root, error)) return 0;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(root, error)) {
        if (error) break;
        if (entry.is_regular_file(error))
            total += entry.file_size(error);
    }
    return total;
}

bool gate_passes(const CaseResult &result,
                 const bool require_metadata) {
    return result.decode_completed &&
        result.sha256_match &&
        (!require_metadata || result.metadata_valid) &&
        result.telemetry.packet_recovery_percent >= 98.0 &&
        result.telemetry.raw_ber <= 0.02 &&
        result.telemetry.raw_ser <= 0.02;
}

std::vector<std::string> extract_objects(
    const std::string &json, const std::string &array_key) {
    const std::string marker = "\"" + array_key + "\"";
    const auto key = json.find(marker);
    if (key == std::string::npos) return {};
    const auto array_start = json.find('[', key + marker.size());
    if (array_start == std::string::npos) return {};
    std::vector<std::string> objects;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    std::size_t object_start = std::string::npos;
    for (std::size_t i = array_start + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            if (depth++ == 0) object_start = i;
        } else if (c == '}') {
            if (--depth == 0 && object_start != std::string::npos) {
                objects.push_back(
                    json.substr(object_start, i - object_start + 1));
                object_start = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

std::vector<std::string> json_string_array(
    const std::string &json, const std::string &array_key) {
    const std::string marker = "\"" + array_key + "\"";
    const auto key = json.find(marker);
    if (key == std::string::npos) return {};
    const auto begin = json.find('[', key + marker.size());
    const auto end =
        begin == std::string::npos
            ? std::string::npos
            : json.find(']', begin + 1);
    if (begin == std::string::npos ||
        end == std::string::npos)
        return {};
    std::vector<std::string> values;
    const std::string body =
        json.substr(begin + 1, end - begin - 1);
    const std::regex string_value("\"([^\"]*)\"");
    for (std::sregex_iterator it(
             body.begin(), body.end(), string_value), last;
         it != last; ++it)
        values.push_back((*it)[1].str());
    return values;
}

std::optional<std::string> json_string(
    const std::string &object, const std::string &key) {
    const std::regex expression(
        "\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(object, match, expression))
        return std::nullopt;
    return match[1].str();
}

std::optional<double> json_number(
    const std::string &object, const std::string &key) {
    const std::regex expression(
        "\"" + key +
        "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?"
        "(?:[eE][+-]?[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(object, match, expression))
        return std::nullopt;
    return std::stod(match[1].str());
}

std::optional<uint64_t> json_u64(
    const std::string &object, const std::string &key) {
    const std::regex expression(
        "\"" + key + "\"\\s*:\\s*([0-9]+)");
    std::smatch match;
    if (!std::regex_search(object, match, expression))
        return std::nullopt;
    uint64_t value = 0;
    const std::string token = match[1].str();
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), value);
    if (error != std::errc{} ||
        end != token.data() + token.size())
        throw std::runtime_error(
            "invalid unsigned Capacity Lab JSON value");
    return value;
}

bool json_bool(const std::string &object, const std::string &key,
               const bool fallback = false) {
    const std::regex expression(
        "\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch match;
    if (!std::regex_search(object, match, expression))
        return fallback;
    return match[1].str() == "true";
}

} // namespace

ExperimentConfig production_baseline_config() {
    ExperimentConfig config;
    config.block_width = 8;
    config.block_height = 8;
    config.bits_per_block = 1;
    config.signal_milli = 1000;
    config.repair_basis_points = 500;
    config.resolution_width = 1920;
    config.resolution_height = 1080;
    config.modulation_version = kModulation1Version;
    return config;
}

bool ExperimentConfig::valid(std::string *reason) const {
    auto fail = [&](const char *message) {
        if (reason) *reason = message;
        return false;
    };
    if (block_width != block_height ||
        (block_width != 4 && block_width != 6 &&
         block_width != 8))
        return fail("block size must be 4, 6, or 8");
    if (bits_per_block != 1 && bits_per_block != 2)
        return fail("bits per block must be 1 or 2");
    if (signal_milli != 750 && signal_milli != 1000 &&
        signal_milli != 1250 && signal_milli != 1500)
        return fail("signal must be 0.75, 1.00, 1.25, or 1.50");
    if (repair_basis_points != 0 &&
        repair_basis_points != 100 &&
        repair_basis_points != 200 &&
        repair_basis_points != 500)
        return fail("repair must be 0, 1, 2, or 5 percent");
    if (!((resolution_width == 1920 &&
           resolution_height == 1080) ||
          (resolution_width == 3840 &&
           resolution_height == 2160)))
        return fail("resolution must be 1920x1080 or 3840x2160");
    if (fps != kFps) return fail("Capacity Lab requires 30 FPS");
    if (packet_symbol_size !=
        static_cast<int>(SYMBOL_SIZE_BYTES))
        return fail("packet symbol size is fixed at 256 bytes");
    if (transform_version != kTransformVersion)
        return fail("unsupported transform version");
    const std::string expected =
        bits_per_block == 1
            ? kModulation1Version : kModulation2Version;
    if (modulation_version != expected)
        return fail("modulation version does not match bit depth");
    return true;
}

std::string ExperimentConfig::canonical_serialization() const {
    std::ostringstream out;
    out << "capacity-config-v1"
        << "|block=" << block_width << "x" << block_height
        << "|bits=" << bits_per_block
        << "|signal_milli=" << signal_milli
        << "|repair_bp=" << repair_basis_points
        << "|resolution=" << resolution_width << "x"
        << resolution_height
        << "|fps=" << fps
        << "|symbol_bytes=" << packet_symbol_size
        << "|transform=" << transform_version
        << "|modulation=" << modulation_version
        << "|threshold=" << decoder_threshold_version
        << "|interleaving=" << (interleaving ? 1 : 0)
        << "|created_with=" << created_with_version;
    return out.str();
}

std::string ExperimentConfig::config_id() const {
    std::string reason;
    if (!valid(&reason))
        throw std::invalid_argument(
            "invalid Capacity Lab config: " + reason);
    return stable_hash_id(canonical_serialization());
}

BlockGeometry compute_geometry(const ExperimentConfig &config) {
    std::string reason;
    if (!config.valid(&reason))
        throw std::invalid_argument(
            "invalid Capacity Lab geometry config: " + reason);
    BlockGeometry result;
    result.frame_width = config.resolution_width;
    result.frame_height = config.resolution_height;
    result.block_width = config.block_width;
    result.block_height = config.block_height;
    result.blocks_per_row =
        config.resolution_width / config.block_width;
    result.blocks_per_column =
        config.resolution_height / config.block_height;
    result.total_blocks = checked_mul(
        static_cast<uint64_t>(result.blocks_per_row),
        static_cast<uint64_t>(result.blocks_per_column),
        "total blocks");
    // Production has no frame-level reserved blocks. Packet MAGIC/header
    // bytes provide synchronization and integrity inside the bitstream.
    result.header_sync_blocks = 0;
    result.data_blocks = result.total_blocks;
    result.raw_bits_per_frame = checked_mul(
        result.data_blocks,
        static_cast<uint64_t>(config.bits_per_block),
        "raw bits");
    result.raw_bytes_per_frame =
        result.raw_bits_per_frame / 8;
    if (result.raw_bytes_per_frame < PACKET_SIZE)
        throw std::invalid_argument(
            "configuration has zero packet capacity");
    result.packets_per_frame =
        result.raw_bytes_per_frame / PACKET_SIZE;
    result.packet_header_bytes_per_frame = checked_mul(
        result.packets_per_frame,
        static_cast<uint64_t>(HEADER_SIZE_V2),
        "packet header overhead");
    result.source_payload_bytes_per_frame = checked_mul(
        result.packets_per_frame,
        static_cast<uint64_t>(SYMBOL_SIZE_BYTES),
        "source payload capacity");
    result.unused_right_pixels =
        config.resolution_width -
        result.blocks_per_row * config.block_width;
    result.unused_bottom_pixels =
        config.resolution_height -
        result.blocks_per_column * config.block_height;
    return result;
}

CapacityMetrics compute_capacity(
    const ExperimentConfig &config,
    const ExperimentConfig &baseline) {
    CapacityMetrics result;
    result.geometry = compute_geometry(config);
    const auto baseline_geometry = compute_geometry(baseline);
    result.useful_bits_per_frame = checked_mul(
        result.geometry.packets_per_frame,
        static_cast<uint64_t>(SYMBOL_SIZE_BYTES * 8),
        "useful frame bits");
    result.source_payload_bytes_per_frame =
        result.geometry.source_payload_bytes_per_frame;
    const uint64_t target_packets = checked_mul(
        result.geometry.packets_per_frame, kMinimumFrames,
        "minimum packet count");
    const double repair_ratio = config.repair_percent() / 100.0;
    const uint64_t source_packets = static_cast<uint64_t>(
        std::ceil(target_packets / (1.0 + repair_ratio)));
    result.minimum_payload_bytes = checked_mul(
        source_packets, SYMBOL_SIZE_BYTES, "minimum payload");
    result.expected_source_packets =
        ceil_div(result.minimum_payload_bytes, SYMBOL_SIZE_BYTES);
    result.expected_repair_packets = static_cast<uint64_t>(
        std::ceil(result.expected_source_packets * repair_ratio));
    result.expected_total_packets =
        result.expected_source_packets +
        result.expected_repair_packets;
    result.expected_frames = ceil_div(
        checked_mul(result.expected_total_packets, PACKET_SIZE,
                    "packet bytes"),
        result.geometry.raw_bytes_per_frame);
    result.expected_frames =
        std::max<uint64_t>(kMinimumFrames, result.expected_frames);
    result.expected_duration_seconds =
        static_cast<double>(result.expected_frames) / config.fps;
    result.useful_payload_bytes_per_second =
        result.minimum_payload_bytes /
        result.expected_duration_seconds;
    const uint64_t pixels_per_frame = checked_mul(
        static_cast<uint64_t>(config.resolution_width),
        static_cast<uint64_t>(config.resolution_height),
        "pixels");
    result.estimated_master_bytes = checked_mul(
        pixels_per_frame, result.expected_frames,
        "master estimate") / 3;
    // This is a conservative preflight estimate. Actual candidate size is
    // always replaced by the probed local encode size in results.
    result.estimated_candidate_bytes = checked_mul(
        static_cast<uint64_t>(config.resolution_width),
        static_cast<uint64_t>(config.resolution_height),
        "candidate estimate") *
        result.expected_frames / 18;
    result.estimated_required_disk_bytes =
        result.minimum_payload_bytes +
        result.estimated_master_bytes +
        result.estimated_candidate_bytes * 2;
    result.estimated_peak_memory_bytes =
        checked_mul(pixels_per_frame, 5, "memory estimate") +
        checked_mul(result.expected_total_packets, PACKET_SIZE,
                    "packet memory estimate");
    result.raw_capacity_gain =
        static_cast<double>(result.geometry.raw_bits_per_frame) /
        baseline_geometry.raw_bits_per_frame;
    const double baseline_useful =
        baseline_geometry.source_payload_bytes_per_frame;
    result.useful_payload_gain =
        baseline_useful == 0 ? 0.0 :
        result.source_payload_bytes_per_frame / baseline_useful;
    result.frame_reduction =
        1.0 - 1.0 / std::max(1.0, result.useful_payload_gain);
    result.duration_reduction = result.frame_reduction;
    return result;
}

const std::vector<double> &dct_basis(const int block_size) {
    if (block_size != 4 && block_size != 6 && block_size != 8)
        throw std::invalid_argument("DCT block size must be 4, 6, or 8");
    static std::mutex mutex;
    static std::map<int, std::vector<double>> cache;
    std::scoped_lock lock(mutex);
    auto [it, inserted] = cache.try_emplace(block_size);
    if (inserted) {
        auto &basis = it->second;
        basis.resize(static_cast<std::size_t>(
            block_size * block_size));
        for (int frequency = 0;
             frequency < block_size; ++frequency) {
            for (int position = 0;
                 position < block_size; ++position) {
                basis[static_cast<std::size_t>(
                    frequency * block_size + position)] =
                    std::cos(
                        (2.0 * position + 1.0) * frequency *
                        kPi / (2.0 * block_size));
            }
        }
    }
    return it->second;
}

void forward_dct(const std::vector<double> &pixels,
                 const int block_size,
                 std::vector<double> &coefficients) {
    if (pixels.size() !=
        static_cast<std::size_t>(block_size * block_size))
        throw std::invalid_argument("forward DCT input size mismatch");
    const auto &basis = dct_basis(block_size);
    coefficients.assign(pixels.size(), 0.0);
    const double scale = 2.0 / block_size;
    for (int u = 0; u < block_size; ++u) {
        const double alpha_u =
            u == 0 ? std::sqrt(0.5) : 1.0;
        for (int v = 0; v < block_size; ++v) {
            const double alpha_v =
                v == 0 ? std::sqrt(0.5) : 1.0;
            double sum = 0.0;
            for (int y = 0; y < block_size; ++y)
                for (int x = 0; x < block_size; ++x)
                    sum += pixels[static_cast<std::size_t>(
                               y * block_size + x)] *
                        basis[static_cast<std::size_t>(
                            u * block_size + y)] *
                        basis[static_cast<std::size_t>(
                            v * block_size + x)];
            coefficients[static_cast<std::size_t>(
                u * block_size + v)] =
                scale * alpha_u * alpha_v * sum;
        }
    }
}

void inverse_dct(const std::vector<double> &coefficients,
                 const int block_size,
                 std::vector<double> &pixels) {
    if (coefficients.size() !=
        static_cast<std::size_t>(block_size * block_size))
        throw std::invalid_argument("inverse DCT input size mismatch");
    const auto &basis = dct_basis(block_size);
    pixels.assign(coefficients.size(), 0.0);
    const double scale = 2.0 / block_size;
    for (int y = 0; y < block_size; ++y) {
        for (int x = 0; x < block_size; ++x) {
            double sum = 0.0;
            for (int u = 0; u < block_size; ++u) {
                const double alpha_u =
                    u == 0 ? std::sqrt(0.5) : 1.0;
                for (int v = 0; v < block_size; ++v) {
                    const double alpha_v =
                        v == 0 ? std::sqrt(0.5) : 1.0;
                    sum += alpha_u * alpha_v *
                        coefficients[static_cast<std::size_t>(
                            u * block_size + v)] *
                        basis[static_cast<std::size_t>(
                            u * block_size + y)] *
                        basis[static_cast<std::size_t>(
                            v * block_size + x)];
                }
            }
            pixels[static_cast<std::size_t>(
                y * block_size + x)] = scale * sum;
        }
    }
}

std::vector<uint8_t> make_symbol_block(
    const ExperimentConfig &config, const uint8_t symbol,
    bool *clamped) {
    std::string reason;
    if (!config.valid(&reason))
        throw std::invalid_argument(reason);
    const int count = config.bits_per_block == 1 ? 2 : 4;
    if (symbol >= count)
        throw std::invalid_argument("symbol is out of range");
    const int n = config.block_width;
    std::vector<double> coefficients(
        static_cast<std::size_t>(n * n), 0.0);
    coefficients[0] = n * 128.0;
    coefficients[1] = modulation_levels(config)[symbol];
    std::vector<double> pixels;
    inverse_dct(coefficients, n, pixels);
    std::vector<uint8_t> result(pixels.size());
    bool did_clamp = false;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const double bounded =
            std::clamp(pixels[i], 0.0, 255.0);
        did_clamp = did_clamp || bounded != pixels[i];
        // Production's precomputed block uses truncation, not rounding.
        result[i] = static_cast<uint8_t>(bounded);
    }
    if (clamped) *clamped = did_clamp;
    return result;
}

SymbolDecision decode_symbol(
    const uint8_t *pixels, const int stride,
    const ExperimentConfig &config) {
    if (!pixels || stride < config.block_width)
        throw std::invalid_argument("invalid symbol block");
    const int n = config.block_width;
    const auto &basis = dct_basis(n);
    double projection = 0.0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            projection += pixels[y * stride + x] *
                basis[static_cast<std::size_t>(n + x)];
    const double coefficient =
        (2.0 / n) * std::sqrt(0.5) * projection;
    const auto &levels = effective_modulation_levels(config);
    const int count = config.bits_per_block == 1 ? 2 : 4;
    int nearest = 0;
    double distance = std::abs(coefficient - levels[0]);
    for (int i = 1; i < count; ++i) {
        const double candidate =
            std::abs(coefficient - levels[i]);
        if (candidate < distance) {
            nearest = i;
            distance = candidate;
        }
    }
    const double left_spacing =
        nearest > 0
            ? levels[nearest] - levels[nearest - 1]
            : levels[1] - levels[0];
    const double right_spacing =
        nearest + 1 < count
            ? levels[nearest + 1] - levels[nearest]
            : levels[count - 1] - levels[count - 2];
    const double spacing =
        std::max(1e-9, std::min(left_spacing, right_spacing));
    double boundary_distance = 0.0;
    if (config.bits_per_block == 1)
        boundary_distance = std::abs(coefficient);
    else if (nearest == 0)
        boundary_distance =
            (levels[0] + levels[1]) / 2.0 - coefficient;
    else if (nearest == count - 1)
        boundary_distance =
            coefficient -
            (levels[count - 2] + levels[count - 1]) / 2.0;
    else
        boundary_distance = std::min(
            coefficient -
                (levels[nearest - 1] + levels[nearest]) / 2.0,
            (levels[nearest] + levels[nearest + 1]) / 2.0 -
                coefficient);
    SymbolDecision result;
    result.symbol = static_cast<uint8_t>(nearest);
    result.coefficient = coefficient;
    result.nearest_level_distance = distance;
    result.confidence = std::clamp(
        boundary_distance / (spacing / 2.0), 0.0, 1.0);
    return result;
}

uint8_t gray_symbol_for_bits(const uint8_t two_bits) {
    static constexpr std::array<uint8_t, 4> map{0, 1, 3, 2};
    if (two_bits > 3)
        throw std::invalid_argument("2-bit value is out of range");
    return map[two_bits];
}

uint8_t bits_for_gray_symbol(const uint8_t symbol) {
    static constexpr std::array<uint8_t, 4> map{0, 1, 3, 2};
    if (symbol > 3)
        throw std::invalid_argument("Gray symbol is out of range");
    return map[symbol];
}

std::vector<ExperimentConfig> smoke_configs() {
    std::vector<ExperimentConfig> configs;
    for (const int signal : {1000, 1250}) {
        for (const int block : {8, 6, 4}) {
            for (const int bits : {1, 2}) {
                ExperimentConfig config;
                config.block_width = block;
                config.block_height = block;
                config.bits_per_block = bits;
                config.signal_milli = signal;
                config.repair_basis_points = 200;
                config.modulation_version =
                    bits == 1 ? kModulation1Version
                              : kModulation2Version;
                configs.push_back(config);
            }
        }
    }
    return configs;
}

std::vector<ExperimentConfig> stage1_configs() {
    std::vector<ExperimentConfig> configs;
    for (const int block : {8, 6, 4}) {
        for (const int bits : {1, 2}) {
            for (const int signal : {750, 1000, 1250, 1500}) {
                ExperimentConfig config;
                config.block_width = block;
                config.block_height = block;
                config.bits_per_block = bits;
                config.signal_milli = signal;
                config.repair_basis_points = 200;
                config.modulation_version =
                    bits == 1 ? kModulation1Version
                              : kModulation2Version;
                configs.push_back(config);
            }
        }
    }
    return configs;
}

std::vector<CapacityCase> build_initial_cases(
    const RunOptions &options, const std::string &experiment_id) {
    std::vector<ExperimentConfig> configs;
    if (options.preset == Preset::Smoke)
        configs = smoke_configs();
    else if (options.preset == Preset::Staged)
        configs = stage1_configs();
    else {
        const uint64_t raw_count = checked_mul(
            checked_mul(
                checked_mul(options.block_sizes.size(),
                            options.bits_per_block.size(),
                            "custom matrix"),
                options.signal_milli.size(), "custom matrix"),
            checked_mul(options.repair_basis_points.size(),
                        options.resolutions.size(),
                        "custom matrix"),
            "custom matrix");
        if (raw_count > kAbsoluteMaximumCases)
            throw std::invalid_argument(
                "custom Capacity Lab matrix exceeds 192 cases");
        if (raw_count > options.maximum_cases)
            throw std::invalid_argument(
                "custom Capacity Lab matrix exceeds --max-cases");
        for (const auto &[width, height] : options.resolutions)
            for (const int block : options.block_sizes)
                for (const int bits : options.bits_per_block)
                    for (const int signal : options.signal_milli)
                        for (const int repair :
                             options.repair_basis_points) {
                            ExperimentConfig config;
                            config.block_width = block;
                            config.block_height = block;
                            config.bits_per_block = bits;
                            config.signal_milli = signal;
                            config.repair_basis_points = repair;
                            config.resolution_width = width;
                            config.resolution_height = height;
                            config.modulation_version =
                                bits == 1
                                    ? kModulation1Version
                                    : kModulation2Version;
                            std::string reason;
                            if (!config.valid(&reason))
                                throw std::invalid_argument(reason);
                            configs.push_back(config);
                        }
    }
    if (configs.size() > options.maximum_cases)
        throw std::invalid_argument(
            "preset exceeds configured maximum cases");
    std::vector<CapacityCase> cases;
    cases.reserve(configs.size());
    std::size_t sequence = 0;
    for (const auto &config : configs) {
        CapacityCase test_case;
        test_case.config = config;
        test_case.config_id = config.config_id();
        test_case.stage = 1;
        std::ostringstream id;
        id << "CAP-" << test_case.config_id << "-"
           << std::setw(3) << std::setfill('0') << sequence++;
        test_case.case_id = id.str();
        test_case.capacity = compute_capacity(config);
        // Repair comparisons use the exact payload required by the 0%
        // member of their otherwise-identical group.
        ExperimentConfig fair = config;
        fair.repair_basis_points = 0;
        const auto fair_capacity = compute_capacity(fair);
        test_case.requested_payload_bytes =
            fair_capacity.minimum_payload_bytes;
        test_case.effective_payload_bytes =
            fair_capacity.minimum_payload_bytes;
        test_case.payload_seed = stable_seed(config);
        test_case.requested_simulation_profile =
            options.simulations.empty()
                ? "yt-sim-1080p-medium"
                : options.simulations.front();
        (void) experiment_id;
        cases.push_back(std::move(test_case));
    }
    return cases;
}

Preflight estimate(const RunOptions &options) {
    if (options.maximum_cases == 0 ||
        options.maximum_cases > kAbsoluteMaximumCases)
        throw std::invalid_argument(
            "maximum cases must be between 1 and 192");
    if (options.maximum_disk_bytes <
        kMinimumFreeDiskBytes)
        throw std::invalid_argument(
            "maximum disk must be at least 512 MiB");
    Preflight result;
    result.raw_combination_count =
        3ULL * 2 * 4 * 4 * 2;
    result.staged_maximum_cases =
        options.preset == Preset::Smoke ? 12 :
        options.preset == Preset::Staged
            ? std::min<uint64_t>(options.maximum_cases,
                                 24 + 16 + 18)
            : std::min<uint64_t>(
                  options.maximum_cases,
                  checked_mul(
                      checked_mul(options.block_sizes.size(),
                                  options.bits_per_block.size(),
                                  "matrix"),
                      checked_mul(
                          checked_mul(options.signal_milli.size(),
                                      options.repair_basis_points.size(),
                                      "matrix"),
                          options.resolutions.size(), "matrix"),
                      "matrix"));
    const auto cases = build_initial_cases(
        options, "estimate");
    for (const auto &test_case : cases) {
        result.estimated_total_frames +=
            test_case.capacity.expected_frames * 2;
        result.estimated_output_bytes +=
            test_case.capacity.estimated_required_disk_bytes;
        ++result.estimated_transcodes;
    }
    if (options.preset == Preset::Staged) {
        // Upper bound for selected Stage 2/3 candidates; selection keeps
        // the actual run well below the raw 192-case matrix.
        result.estimated_transcodes += 16 + 18;
        result.estimated_output_bytes =
            result.estimated_output_bytes * 3;
        result.estimated_total_frames *= 3;
    }
    result.safety_margin_bytes =
        std::max<uint64_t>(
            kMinimumFreeDiskBytes,
            result.estimated_output_bytes / 10);
    result.required_disk_bytes =
        result.estimated_output_bytes +
        result.safety_margin_bytes;
    result.estimated_seconds =
        result.estimated_total_frames *
        0.055;
    if (!options.output_root.empty()) {
        std::error_code error;
        auto probe = options.output_root;
        while (!probe.empty() &&
               !std::filesystem::exists(probe, error))
            probe = probe.parent_path();
        if (!probe.empty()) {
            const auto space =
                std::filesystem::space(probe, error);
            if (!error)
                result.available_disk_bytes = space.available;
        }
    }
    result.disk_space_sufficient =
        result.required_disk_bytes <= options.maximum_disk_bytes &&
        (!result.available_disk_bytes ||
         *result.available_disk_bytes >=
             result.required_disk_bytes);
    return result;
}

void update_pareto_and_categories(
    std::vector<CapacityCase> &cases) {
    for (auto &test_case : cases) {
        test_case.pareto = false;
        test_case.dominated = false;
        test_case.category.clear();
    }
    auto candidate_result = [](const CapacityCase &test_case)
        -> const CaseResult * {
        for (auto it = test_case.results.rbegin();
             it != test_case.results.rend(); ++it)
            if (it->source_type == "upload-candidate" ||
                it->source_type == "local-simulation" ||
                it->source_type == "real-youtube-roundtrip")
                return &*it;
        return nullptr;
    };
    auto metrics = [&](const CapacityCase &test_case) {
        const CaseResult *result = candidate_result(test_case);
        const double margin =
            result ? result->telemetry.recovery_margin_percent : -1e9;
        const double errors =
            result ? result->telemetry.raw_ber +
                         result->telemetry.raw_ser
                   : 1e9;
        const double ratio =
            test_case.effective_payload_bytes == 0 ? 1e9 :
            static_cast<double>(test_case.candidate_size) /
                test_case.effective_payload_bytes;
        const double time =
            result ? result->encode_seconds +
                         result->transcode_seconds +
                         result->decode_seconds
                   : 1e9;
        return std::array<double, 5>{
            test_case.capacity.useful_payload_bytes_per_second,
            margin, errors, ratio, time};
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (!cases[i].mandatory_gates_passed) continue;
        const auto a = metrics(cases[i]);
        bool dominated = false;
        for (std::size_t j = 0; j < cases.size(); ++j) {
            if (i == j || !cases[j].mandatory_gates_passed)
                continue;
            const auto b = metrics(cases[j]);
            const bool no_worse =
                b[0] >= a[0] && b[1] >= a[1] &&
                b[2] <= a[2] && b[3] <= a[3] &&
                b[4] <= a[4];
            const bool strictly_better =
                b[0] > a[0] || b[1] > a[1] ||
                b[2] < a[2] || b[3] < a[3] ||
                b[4] < a[4];
            if (no_worse && strictly_better) {
                dominated = true;
                break;
            }
        }
        cases[i].dominated = dominated;
        cases[i].pareto = !dominated;
    }
    std::vector<CapacityCase *> frontier;
    for (auto &test_case : cases)
        if (test_case.pareto) frontier.push_back(&test_case);
    if (frontier.empty()) return;
    auto capacity = *std::max_element(
        frontier.begin(), frontier.end(), [](const auto *a, const auto *b) {
            return a->capacity.useful_payload_bytes_per_second <
                   b->capacity.useful_payload_bytes_per_second;
        });
    auto robust = *std::max_element(
        frontier.begin(), frontier.end(), [&](const auto *a, const auto *b) {
            return metrics(*a)[1] < metrics(*b)[1];
        });
    auto upload = *std::min_element(
        frontier.begin(), frontier.end(), [&](const auto *a, const auto *b) {
            return metrics(*a)[3] < metrics(*b)[3];
        });
    std::vector<CapacityCase *> balanced_pool;
    for (auto *test_case : frontier) {
        const auto value = metrics(*test_case);
        const auto *result = candidate_result(*test_case);
        if (value[1] >= 1.0 && value[2] == 0.0 &&
            result && result->telemetry.average_confidence >= 0.25)
            balanced_pool.push_back(test_case);
    }
    if (balanced_pool.empty())
        balanced_pool = frontier;
    // "Balanced" is deliberately lexicographic and auditable: after
    // requiring a positive recovery margin, zero observed errors and usable
    // confidence, prefer capacity, then margin, upload efficiency and time.
    // No hidden weighted score is used.
    auto balanced = *std::max_element(
        balanced_pool.begin(), balanced_pool.end(),
        [&](const auto *a, const auto *b) {
            const auto am = metrics(*a);
            const auto bm = metrics(*b);
            if (am[0] != bm[0]) return am[0] < bm[0];
            if (am[1] != bm[1]) return am[1] < bm[1];
            if (am[3] != bm[3]) return am[3] > bm[3];
            return am[4] > bm[4];
        });
    robust->category = "Most robust";
    balanced->category =
        balanced->category.empty()
            ? "Best balanced"
            : balanced->category + "; Best balanced";
    capacity->category =
        capacity->category.empty()
            ? "Highest capacity"
            : capacity->category + "; Highest capacity";
    upload->category =
        upload->category.empty()
            ? "Smallest upload"
            : upload->category + "; Smallest upload";
    for (auto *test_case : frontier) {
        const CaseResult *result = candidate_result(*test_case);
        if (result &&
            (result->telemetry.recovery_margin_percent < 1.0 ||
             result->telemetry.average_confidence < 0.25)) {
            test_case->category =
                test_case->category.empty()
                    ? "Experimental/risky"
                    : test_case->category + "; Experimental/risky";
        }
    }
}

std::vector<std::size_t> select_shortlist(
    std::vector<CapacityCase> &cases,
    const std::size_t maximum_videos) {
    update_pareto_and_categories(cases);
    std::vector<std::size_t> eligible;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (cases[i].state == CaseState::Shortlisted)
            cases[i].state = CaseState::Passed;
        cases[i].shortlisted = false;
        cases[i].shortlist_reason.clear();
        if (cases[i].pareto &&
            cases[i].mandatory_gates_passed)
            eligible.push_back(i);
    }
    std::stable_sort(
        eligible.begin(), eligible.end(),
        [&](const std::size_t a, const std::size_t b) {
            const bool categorized =
                !cases[a].category.empty();
            const bool other_categorized =
                !cases[b].category.empty();
            if (categorized != other_categorized)
                return categorized;
            return cases[a].capacity
                       .useful_payload_bytes_per_second >
                   cases[b].capacity
                       .useful_payload_bytes_per_second;
        });
    std::vector<std::size_t> unique;
    std::unordered_set<std::string> config_ids;
    for (const auto index : eligible) {
        if (config_ids.insert(cases[index].config_id).second)
            unique.push_back(index);
        if (unique.size() == maximum_videos)
            break;
    }
    eligible = std::move(unique);
    for (const auto index : eligible) {
        auto &test_case = cases[index];
        test_case.shortlisted = true;
        test_case.state = CaseState::Shortlisted;
        test_case.shortlist_reason =
            test_case.category.empty()
                ? "Non-dominated candidate passing all local gates"
                : test_case.category +
                    "; non-dominated and passed all local gates";
    }
    return eligible;
}

namespace {

std::string canonical_profile_name(const std::string &value) {
    if (value == "h264-light") return "yt-sim-1080p-light";
    if (value == "h264-medium") return "yt-sim-1080p-medium";
    if (value == "h264-heavy") return "yt-sim-1080p-heavy";
    return value;
}

std::string readable_profile_name(const std::string &value) {
    const auto canonical = canonical_profile_name(value);
    if (canonical == "yt-sim-1080p-light") return "H.264 light";
    if (canonical == "yt-sim-1080p-medium") return "H.264 medium";
    if (canonical == "yt-sim-1080p-heavy") return "H.264 heavy";
    return canonical;
}

const CaseResult *result_by_source(
    const CapacityCase &test_case, const std::string &source) {
    for (auto it = test_case.results.rbegin();
         it != test_case.results.rend(); ++it)
        if (it->source_type == source)
            return &*it;
    return nullptr;
}

bool is_non_gating_profile(const CapacityCase &test_case) {
    return canonical_profile_name(
               test_case.requested_simulation_profile) ==
           "yt-sim-720p-downscale";
}

bool result_exact_gate(const CaseResult *result,
                       const bool require_metadata) {
    return result && result->decode_completed &&
        result->sha256_match &&
        (!require_metadata || result->metadata_valid) &&
        result->telemetry.packet_recovery_percent >= 98.0 &&
        result->telemetry.raw_ber <= 0.02 &&
        result->telemetry.raw_ser <= 0.02;
}

std::vector<std::size_t> config_case_indices(
    const ExperimentManifest &manifest, const std::string &config_id) {
    std::vector<std::size_t> result;
    for (std::size_t index = 0;
         index < manifest.cases.size(); ++index)
        if (manifest.cases[index].config_id == config_id)
            result.push_back(index);
    return result;
}

} // namespace

EligibilityDecision evaluate_shortlist_eligibility(
    const ExperimentManifest &manifest,
    const std::string &config_id) {
    EligibilityDecision decision;
    decision.config_id = config_id;
    const auto indices = config_case_indices(manifest, config_id);
    if (indices.empty()) {
        decision.incomplete = true;
        decision.reason = "Config has no experiment cases";
        return decision;
    }
    int target_stage = 0;
    for (const auto index : indices)
        if (!is_non_gating_profile(manifest.cases[index]))
            target_stage = std::max(
                target_stage, manifest.cases[index].stage);
    const auto &required =
        target_stage >= 3
            ? manifest.mandatory_stage3_profiles
            : manifest.mandatory_stage1_profiles;
    if (required.empty()) {
        decision.incomplete = true;
        decision.reason = "Manifest has no mandatory simulation profiles";
        return decision;
    }

    for (const auto &required_value : required) {
        const std::string required_profile =
            canonical_profile_name(required_value);
        const CapacityCase *matched = nullptr;
        std::size_t matched_index = 0;
        for (const auto index : indices) {
            const auto &test_case = manifest.cases[index];
            if (test_case.stage != target_stage ||
                is_non_gating_profile(test_case))
                continue;
            const std::string case_profile =
                canonical_profile_name(
                    test_case.requested_simulation_profile);
            const auto *candidate =
                result_by_source(test_case, "upload-candidate");
            const std::string result_profile =
                candidate
                    ? canonical_profile_name(
                          candidate->simulation_profile)
                    : case_profile;
            if (case_profile == required_profile ||
                result_profile == required_profile) {
                matched = &test_case;
                matched_index = index;
                break;
            }
        }
        if (!matched) {
            decision.incomplete = true;
            decision.failed_mandatory_profile = required_profile;
            decision.reason =
                "Missing mandatory " +
                readable_profile_name(required_profile) +
                " simulation result";
            return decision;
        }
        if (matched->state == CaseState::Pending ||
            matched->state == CaseState::Running ||
            matched->state == CaseState::Cancelled) {
            decision.incomplete = true;
            decision.failed_mandatory_profile = required_profile;
            decision.reason =
                "Incomplete mandatory " +
                readable_profile_name(required_profile) +
                " simulation result";
            return decision;
        }
        const auto *master =
            result_by_source(*matched, "master-lossless");
        if (!result_exact_gate(master, false)) {
            decision.rejected = true;
            decision.failed_mandatory_profile = required_profile;
            decision.reason =
                "Mandatory master roundtrip failed for " +
                readable_profile_name(required_profile);
            return decision;
        }
        const auto *candidate =
            result_by_source(*matched, "upload-candidate");
        if (!result_exact_gate(candidate, true)) {
            decision.rejected = true;
            decision.failed_mandatory_profile = required_profile;
            if (candidate && !candidate->sha256_match)
                decision.reason =
                    "Mandatory " +
                    readable_profile_name(required_profile) +
                    " simulation failed: SHA-256 mismatch";
            else if (candidate && !candidate->metadata_valid)
                decision.reason =
                    "Mandatory " +
                    readable_profile_name(required_profile) +
                    " simulation failed: media metadata validation";
            else if (candidate &&
                     candidate->telemetry.packet_recovery_percent < 98.0)
                decision.reason =
                    "Mandatory " +
                    readable_profile_name(required_profile) +
                    " simulation failed: packet recovery below threshold";
            else
                decision.reason =
                    "Mandatory " +
                    readable_profile_name(required_profile) +
                    " simulation failed";
            return decision;
        }
        if (!decision.representative_case ||
            required_profile == "yt-sim-1080p-medium")
            decision.representative_case = matched_index;
    }
    decision.eligible = true;
    decision.reason = "All mandatory local simulation gates passed";
    return decision;
}

void recompute_experiment_decisions(ExperimentManifest &manifest) {
    std::map<std::string, EligibilityDecision> decisions;
    for (const auto &test_case : manifest.cases)
        if (!decisions.contains(test_case.config_id))
            decisions.emplace(
                test_case.config_id,
                evaluate_shortlist_eligibility(
                    manifest, test_case.config_id));
    for (auto &test_case : manifest.cases) {
        const auto &decision = decisions.at(test_case.config_id);
        if (test_case.state == CaseState::Shortlisted)
            test_case.state = CaseState::Passed;
        test_case.shortlisted = false;
        test_case.shortlist_reason.clear();
        test_case.pareto = false;
        test_case.dominated = false;
        test_case.category.clear();
        test_case.eligible_for_shortlist = decision.eligible;
        test_case.incomplete = decision.incomplete;
        test_case.failed_mandatory_profile =
            decision.failed_mandatory_profile;
        test_case.shortlist_exclusion_reason =
            decision.eligible ? "" : decision.reason;
        test_case.local_gate_status =
            decision.eligible ? "Passed" :
            decision.incomplete ? "Incomplete" : "Rejected";
        test_case.mandatory_gates_passed =
            decision.eligible &&
            decision.representative_case &&
            &test_case ==
                &manifest.cases[*decision.representative_case];
    }
    update_pareto_and_categories(manifest.cases);
}

std::vector<std::size_t> select_shortlist(
    ExperimentManifest &manifest,
    const std::size_t maximum_videos) {
    manifest.maximum_shortlist_videos = maximum_videos;
    recompute_experiment_decisions(manifest);
    std::vector<std::size_t> selected;
    for (std::size_t index = 0;
         index < manifest.cases.size(); ++index)
        if (manifest.cases[index].pareto &&
            manifest.cases[index].eligible_for_shortlist)
            selected.push_back(index);
    std::stable_sort(
        selected.begin(), selected.end(),
        [&](const std::size_t a, const std::size_t b) {
            const bool categorized =
                !manifest.cases[a].category.empty();
            const bool other_categorized =
                !manifest.cases[b].category.empty();
            if (categorized != other_categorized)
                return categorized;
            return manifest.cases[a].capacity
                       .useful_payload_bytes_per_second >
                   manifest.cases[b].capacity
                       .useful_payload_bytes_per_second;
        });
    if (selected.size() > maximum_videos)
        selected.resize(maximum_videos);
    for (const auto index : selected) {
        auto &test_case = manifest.cases[index];
        test_case.shortlisted = true;
        test_case.state = CaseState::Shortlisted;
        test_case.shortlist_reason =
            test_case.category.empty()
                ? "Non-dominated config passing every mandatory gate"
                : test_case.category +
                    "; non-dominated config passing every mandatory gate";
    }
    return selected;
}

namespace {

youtube_test_lab::SimulationProfile simulation_for(
    const ExperimentConfig &config, const std::string &requested) {
    std::string lookup = requested;
    if (lookup == "h264-light")
        lookup = "yt-sim-1080p-light";
    else if (lookup == "h264-medium")
        lookup = "yt-sim-1080p-medium";
    else if (lookup == "h264-heavy")
        lookup = "yt-sim-1080p-heavy";
    else if (lookup == "vp9-medium" ||
             lookup == "av1-medium") {
        auto profile = youtube_test_lab::find_simulation_profile(
            "yt-sim-1080p-medium");
        if (!profile)
            throw std::runtime_error(
                "built-in simulation profile is unavailable");
        profile->name = lookup;
        profile->codec =
            lookup == "vp9-medium"
                ? "unavailable-vp9"
                : "unavailable-av1";
        return *profile;
    }
    auto found =
        youtube_test_lab::find_simulation_profile(lookup);
    if (!found)
        throw std::invalid_argument(
            "unknown simulation profile: " + requested);
    auto profile = *found;
    if (lookup == "yt-sim-720p-downscale")
        return profile;
    profile.width = config.resolution_width;
    profile.height = config.resolution_height;
    profile.scale = true;
    if (config.resolution_height == 2160) {
        const auto suffix = profile.name.find("1080p");
        if (suffix != std::string::npos)
            profile.name.replace(suffix, 5, "4k");
    }
    return profile;
}

void prepare_directories(const std::filesystem::path &root) {
    for (const auto *directory : {
             "payloads", "masters", "simulations",
             "youtube_shortlist", "imported", "restored",
             "reports"})
        std::filesystem::create_directories(root / directory);
}

void execute_case(
    ExperimentManifest &manifest, CapacityCase &test_case,
    const std::filesystem::path &root,
    const std::string &simulation_name) {
    test_case.state = CaseState::Running;
    test_case.requested_simulation_profile = simulation_name;
    test_case.rejection_reason.clear();
    test_case.results.clear();
    const std::string fair_id = [&] {
        ExperimentConfig fair = test_case.config;
        fair.repair_basis_points = 0;
        return fair.config_id();
    }();
    const auto payload =
        root / "payloads" / ("payload_" + fair_id + ".bin");
    const auto master =
        root / "masters" /
        (test_case.case_id + ".mkv");
    const auto profile =
        simulation_for(test_case.config, simulation_name);
    if (profile.codec.starts_with("unavailable-")) {
        CaseResult unavailable;
        unavailable.source_type = "local-simulation";
        unavailable.simulation_profile = profile.name;
        unavailable.codec =
            profile.codec.substr(std::string("unavailable-").size());
        unavailable.error =
            "Unavailable: optional codec simulation is not enabled "
            "in this Capacity Lab build";
        test_case.results.push_back(std::move(unavailable));
        test_case.state = CaseState::Unavailable;
        test_case.rejection_reason =
            test_case.results.back().error;
        return;
    }
    const auto simulation_dir =
        root / "simulations" / profile.name;
    std::filesystem::create_directories(simulation_dir);
    const auto candidate =
        simulation_dir / candidate_filename(test_case);
    const auto restored_master =
        root / "restored" /
        (test_case.case_id + "_master.bin");
    const auto restored_candidate =
        root / "restored" /
        (test_case.case_id + "_" + profile.name + ".bin");
    if (!std::filesystem::exists(payload)) {
        youtube_test_lab::generate_payload(
            payload, youtube_test_lab::DataType::Random,
            test_case.effective_payload_bytes,
            test_case.payload_seed);
    } else if (std::filesystem::file_size(payload) !=
               test_case.effective_payload_bytes) {
        throw std::runtime_error(
            "fairness payload exists with an unexpected size");
    }
    test_case.source_sha256 =
        youtube_test_lab::sha256_file(payload);
    test_case.payload_path = relative_path(root, payload);
    test_case.master_path = relative_path(root, master);
    test_case.candidate_path = relative_path(root, candidate);
    test_case.restored_path =
        relative_path(root, restored_candidate);

    const EncodedArtifacts encoded =
        encode_master(payload, master, test_case.config);
    test_case.master_size =
        std::filesystem::file_size(master);
    test_case.capacity.expected_source_packets =
        encoded.source_packets;
    test_case.capacity.expected_repair_packets =
        encoded.repair_packets;
    test_case.capacity.expected_total_packets =
        encoded.total_packets;
    test_case.capacity.expected_frames = encoded.frames;
    test_case.capacity.expected_duration_seconds =
        static_cast<double>(encoded.frames) /
        test_case.config.fps;
    if (encoded.frames < kMinimumFrames)
        throw std::runtime_error(
            "generated master has fewer than 60 real data frames");

    auto master_result = decode_video(
        master, payload, restored_master, test_case.config,
        &encoded, "master-lossless", "master-lossless");
    master_result.encode_seconds = encoded.seconds;
    test_case.results.push_back(master_result);
    // Matroska/FFV1 may expose a muxer time-base-derived avg_frame_rate
    // (for example 1000) even though decoded frame count and timestamps are
    // correct. Master is a lossless internal artifact, so exact payload,
    // dimensions and packet telemetry are the gate; strict FPS/container
    // metadata remains mandatory for the upload candidate.
    if (!gate_passes(master_result, false)) {
        test_case.state = CaseState::Rejected;
        test_case.rejection_reason =
            "master exact roundtrip gate failed: " +
            (master_result.error.empty()
                 ? "SHA/telemetry mismatch"
                 : master_result.error);
        return;
    }

    const auto transcode_started = Clock::now();
    youtube_test_lab::transcode_simulation_video(
        master, candidate, profile, manifest.experiment_id,
        test_case.case_id);
    const double transcode_seconds =
        std::chrono::duration<double>(
            Clock::now() - transcode_started).count();
    test_case.candidate_size =
        std::filesystem::file_size(candidate);
    auto candidate_result = decode_video(
        candidate, payload, restored_candidate,
        test_case.config, &encoded,
        "upload-candidate", profile.name);
    candidate_result.encode_seconds = encoded.seconds;
    candidate_result.transcode_seconds = transcode_seconds;
    test_case.results.push_back(candidate_result);
    if (profile.name == "yt-sim-720p-downscale") {
        test_case.state = CaseState::ResolutionUnsupported;
        test_case.rejection_reason =
            "Resolution-change unsupported; recorded as a non-gating "
            "robustness observation";
        return;
    }
    if (!gate_passes(candidate_result, true)) {
        test_case.state = CaseState::Rejected;
        test_case.rejection_reason =
            "mandatory local simulation gate failed: " +
            (candidate_result.error.empty()
                 ? "SHA/metadata/telemetry threshold"
                 : candidate_result.error);
        return;
    }
    test_case.mandatory_gates_passed = true;
    test_case.state = CaseState::Passed;
}

std::vector<CapacityCase *> passing_sorted(
    std::vector<CapacityCase> &cases) {
    std::vector<CapacityCase *> result;
    for (auto &test_case : cases)
        if (test_case.mandatory_gates_passed)
            result.push_back(&test_case);
    std::stable_sort(
        result.begin(), result.end(),
        [](const auto *a, const auto *b) {
            if (a->pareto != b->pareto)
                return a->pareto;
            const auto margin = [](const CapacityCase *value) {
                return value->results.empty() ? -1e9 :
                    value->results.back().telemetry
                        .recovery_margin_percent;
            };
            if (a->capacity.useful_payload_bytes_per_second !=
                b->capacity.useful_payload_bytes_per_second)
                return a->capacity.useful_payload_bytes_per_second >
                    b->capacity.useful_payload_bytes_per_second;
            return margin(a) > margin(b);
        });
    return result;
}

CapacityCase make_derived_case(
    const CapacityCase &source, const ExperimentConfig &config,
    const int stage, const std::size_t sequence) {
    CapacityCase result;
    result.config = config;
    result.config_id = config.config_id();
    result.stage = stage;
    std::ostringstream id;
    id << "CAP-" << result.config_id << "-S" << stage << "-"
       << std::setw(3) << std::setfill('0') << sequence;
    result.case_id = id.str();
    result.capacity = compute_capacity(config);
    ExperimentConfig fair = config;
    fair.repair_basis_points = 0;
    result.effective_payload_bytes =
        compute_capacity(fair).minimum_payload_bytes;
    result.requested_payload_bytes =
        result.effective_payload_bytes;
    result.payload_seed = stable_seed(config);
    result.requested_simulation_profile =
        source.requested_simulation_profile;
    return result;
}

void ensure_preflight(const RunOptions &options,
                      const Preflight &preflight) {
    if (preflight.required_disk_bytes >
        options.maximum_disk_bytes)
        throw std::runtime_error(
            "Capacity Lab estimate exceeds maximum disk limit");
    if (!preflight.disk_space_sufficient &&
        !options.allow_low_disk)
        throw std::runtime_error(
            "Capacity Lab preflight found insufficient free disk; "
            "use --allow-low-disk only after reviewing the estimate");
}

} // namespace

ExperimentManifest run(
    const RunOptions &options,
    const ProgressCallback &progress) {
    if (options.output_root.empty())
        throw std::invalid_argument(
            "Capacity Lab output folder is required");
    const auto preflight = estimate(options);
    ensure_preflight(options, preflight);
    ExperimentManifest manifest;
    manifest.experiment_id =
        compact_utc() + "-" +
        stable_hash_id(
            std::to_string(
                Clock::now().time_since_epoch().count()), 6);
    manifest.created_at = now_utc();
    manifest.preset = options.preset;
    manifest.baseline = production_baseline_config();
    manifest.maximum_cases = options.maximum_cases;
    manifest.maximum_shortlist_videos =
        options.maximum_shortlist_videos;
    manifest.maximum_disk_bytes =
        options.maximum_disk_bytes;
    if (options.preset == Preset::Custom &&
        !options.simulations.empty()) {
        manifest.mandatory_stage1_profiles.clear();
        for (const auto &profile : options.simulations)
            manifest.mandatory_stage1_profiles.push_back(
                canonical_profile_name(profile));
    }
    manifest.cases = build_initial_cases(
        options, manifest.experiment_id);
    const auto root =
        options.output_root / "youtube_capacity_lab" /
        manifest.experiment_id;
    prepare_directories(root);
    const auto manifest_path = root / "manifest.json";
    write_manifest_atomic(manifest, manifest_path);
    if (options.estimate_only) {
        write_reports(manifest, root / "reports");
        return manifest;
    }

    std::size_t completed = 0;
    auto continue_running = [&](const CapacityCase &test_case) {
        return !progress || progress({
            completed, manifest.cases.size(), test_case.stage,
            test_case.config_id, directory_size(root)});
    };
    auto run_range = [&](const std::size_t begin,
                         const std::string &profile) {
        for (std::size_t i = begin;
             i < manifest.cases.size(); ++i) {
            auto &test_case = manifest.cases[i];
            if (!continue_running(test_case)) {
                manifest.cancelled = true;
                test_case.state = CaseState::Cancelled;
                write_manifest_atomic(manifest, manifest_path);
                return false;
            }
            try {
                execute_case(
                    manifest, test_case, root, profile);
            } catch (const std::exception &error) {
                test_case.state = CaseState::Failed;
                test_case.rejection_reason = error.what();
            }
            ++completed;
            write_manifest_atomic(manifest, manifest_path);
            write_reports(manifest, root / "reports");
            if (directory_size(root) >
                options.maximum_disk_bytes) {
                manifest.cancelled = true;
                test_case.rejection_reason +=
                    (test_case.rejection_reason.empty() ? "" : "; ") +
                    std::string("experiment disk limit reached");
                write_manifest_atomic(manifest, manifest_path);
                return false;
            }
        }
        return true;
    };

    const std::string initial_profile =
        options.simulations.empty()
            ? "yt-sim-1080p-medium"
            : options.simulations.front();
    if (!run_range(0, initial_profile))
        return manifest;
    recompute_experiment_decisions(manifest);

    if (options.preset == Preset::Staged) {
        auto selected = passing_sorted(manifest.cases);
        if (selected.size() > 4) selected.resize(4);
        std::vector<ExperimentConfig> selected_configs;
        for (const auto *value : selected)
            selected_configs.push_back(value->config);
        const std::size_t stage2_begin =
            manifest.cases.size();
        std::size_t sequence = 0;
        for (const auto &selected_config : selected_configs) {
            for (const int repair : {0, 100, 200, 500}) {
                if (manifest.cases.size() >=
                    options.maximum_cases)
                    break;
                ExperimentConfig config = selected_config;
                config.repair_basis_points = repair;
                manifest.cases.push_back(make_derived_case(
                    manifest.cases.front(), config, 2, sequence++));
            }
        }
        write_manifest_atomic(manifest, manifest_path);
        if (!run_range(stage2_begin,
                       "yt-sim-1080p-medium"))
            return manifest;
        recompute_experiment_decisions(manifest);

        selected = passing_sorted(manifest.cases);
        if (selected.size() > 3) selected.resize(3);
        selected_configs.clear();
        for (const auto *value : selected)
            selected_configs.push_back(value->config);
        const std::size_t stage3_begin =
            manifest.cases.size();
        sequence = 0;
        std::vector<std::string> stage3_profiles;
        for (const auto &selected_config : selected_configs) {
            for (const auto &[width, height] :
                 std::array<std::pair<int, int>, 2>{
                     std::pair{1920, 1080},
                     std::pair{3840, 2160}}) {
                for (const std::string &profile :
                     {"yt-sim-1080p-light",
                      "yt-sim-1080p-medium",
                      "yt-sim-1080p-heavy"}) {
                    if (manifest.cases.size() >=
                        options.maximum_cases)
                        break;
                    ExperimentConfig config = selected_config;
                    config.resolution_width = width;
                    config.resolution_height = height;
                    manifest.cases.push_back(make_derived_case(
                        manifest.cases.front(), config, 3, sequence++));
                    manifest.cases.back().requested_simulation_profile =
                        profile;
                    stage3_profiles.push_back(profile);
                }
            }
        }
        write_manifest_atomic(manifest, manifest_path);
        for (std::size_t i = stage3_begin;
             i < manifest.cases.size(); ++i) {
            auto &test_case = manifest.cases[i];
            if (!continue_running(test_case)) {
                manifest.cancelled = true;
                test_case.state = CaseState::Cancelled;
                write_manifest_atomic(manifest, manifest_path);
                return manifest;
            }
            try {
                execute_case(
                    manifest, test_case, root,
                    stage3_profiles[i - stage3_begin]);
            } catch (const std::exception &error) {
                test_case.state = CaseState::Failed;
                test_case.rejection_reason = error.what();
            }
            ++completed;
            write_manifest_atomic(manifest, manifest_path);
            write_reports(manifest, root / "reports");
        }
    }
    (void) select_shortlist(
        manifest, options.maximum_shortlist_videos);
    write_manifest_atomic(manifest, manifest_path);
    write_reports(manifest, root / "reports");
    (void) generate_shortlist(
        manifest_path, options.maximum_shortlist_videos);
    return read_manifest(manifest_path);
}

void resume(const std::filesystem::path &manifest_path,
            const ProgressCallback &progress) {
    auto manifest = read_manifest(manifest_path);
    const auto root =
        std::filesystem::absolute(manifest_path).parent_path();
    manifest.cancelled = false;
    const auto retryable_rejection = [](const CapacityCase &test_case) {
        return test_case.state == CaseState::Rejected &&
            test_case.stage == 3 &&
            !test_case.results.empty() &&
            test_case.results.front().source_type ==
                "master-lossless" &&
            test_case.results.front().decode_completed &&
            test_case.results.front().sha256_match;
    };
    std::size_t completed = 0;
    for (const auto &test_case : manifest.cases)
        if (test_case.state == CaseState::Passed ||
            (test_case.state == CaseState::Rejected &&
             !retryable_rejection(test_case)) ||
            test_case.state == CaseState::Unavailable ||
            test_case.state == CaseState::ResolutionUnsupported ||
            test_case.state == CaseState::Shortlisted)
            ++completed;
    const auto process_pending = [&](const std::size_t begin) {
        for (std::size_t index = begin;
             index < manifest.cases.size(); ++index) {
            auto &test_case = manifest.cases[index];
            if (test_case.state == CaseState::Passed ||
                (test_case.state == CaseState::Rejected &&
                 !retryable_rejection(test_case)) ||
                test_case.state == CaseState::Unavailable ||
                test_case.state == CaseState::ResolutionUnsupported ||
                test_case.state == CaseState::Shortlisted)
                continue;
            if (progress && !progress({
                    completed, manifest.cases.size(),
                    test_case.stage, test_case.config_id,
                    directory_size(root)})) {
                manifest.cancelled = true;
                test_case.state = CaseState::Cancelled;
                write_manifest_atomic(manifest, manifest_path);
                return false;
            }
            try {
                execute_case(
                    manifest, test_case, root,
                    test_case.requested_simulation_profile);
            } catch (const std::exception &error) {
                test_case.state = CaseState::Failed;
                test_case.rejection_reason = error.what();
            }
            ++completed;
            write_manifest_atomic(manifest, manifest_path);
            write_reports(manifest, root / "reports");
        }
        return true;
    };
    if (!process_pending(0)) return;
    if (manifest.preset == Preset::Staged) {
        const bool has_stage2 = std::any_of(
            manifest.cases.begin(), manifest.cases.end(),
            [](const CapacityCase &value) {
                return value.stage == 2;
            });
        if (!has_stage2) {
            recompute_experiment_decisions(manifest);
            auto selected = passing_sorted(manifest.cases);
            if (selected.size() > 4) selected.resize(4);
            std::vector<ExperimentConfig> configs;
            for (const auto *value : selected)
                configs.push_back(value->config);
            const std::size_t stage2_begin = manifest.cases.size();
            std::size_t sequence = 0;
            for (const auto &base : configs)
                for (const int repair : {0, 100, 200, 500}) {
                    if (manifest.cases.size() >=
                        manifest.maximum_cases)
                        break;
                    auto config = base;
                    config.repair_basis_points = repair;
                    auto derived = make_derived_case(
                        manifest.cases.front(), config, 2,
                        sequence++);
                    derived.requested_simulation_profile =
                        "yt-sim-1080p-medium";
                    manifest.cases.push_back(std::move(derived));
                }
            write_manifest_atomic(manifest, manifest_path);
            if (!process_pending(stage2_begin)) return;
        }
        const bool has_stage3 = std::any_of(
            manifest.cases.begin(), manifest.cases.end(),
            [](const CapacityCase &value) {
                return value.stage == 3;
            });
        if (!has_stage3) {
            recompute_experiment_decisions(manifest);
            auto selected = passing_sorted(manifest.cases);
            if (selected.size() > 3) selected.resize(3);
            std::vector<ExperimentConfig> configs;
            for (const auto *value : selected)
                configs.push_back(value->config);
            const std::size_t stage3_begin = manifest.cases.size();
            std::size_t sequence = 0;
            for (const auto &base : configs)
                for (const auto &[width, height] :
                     std::array<std::pair<int, int>, 2>{
                         std::pair{1920, 1080},
                         std::pair{3840, 2160}})
                    for (const std::string &profile :
                         {"yt-sim-1080p-light",
                          "yt-sim-1080p-medium",
                          "yt-sim-1080p-heavy"}) {
                        if (manifest.cases.size() >=
                            manifest.maximum_cases)
                            break;
                        auto config = base;
                        config.resolution_width = width;
                        config.resolution_height = height;
                        auto derived = make_derived_case(
                            manifest.cases.front(), config, 3,
                            sequence++);
                        derived.requested_simulation_profile = profile;
                        manifest.cases.push_back(std::move(derived));
                    }
            write_manifest_atomic(manifest, manifest_path);
            if (!process_pending(stage3_begin)) return;
        }
    }
    (void) select_shortlist(
        manifest, manifest.maximum_shortlist_videos);
    write_manifest_atomic(manifest, manifest_path);
    write_reports(manifest, root / "reports");
    (void) generate_shortlist(
        manifest_path, manifest.maximum_shortlist_videos);
}

ShortlistRegenerationReport generate_shortlist(
    const std::filesystem::path &manifest_path,
    const std::size_t maximum_videos) {
    ShortlistRegenerationReport report;
    auto manifest = read_manifest(manifest_path);
    const auto root =
        std::filesystem::absolute(manifest_path).parent_path();
    const auto indices =
        select_shortlist(manifest, maximum_videos);
    const auto directory = root / "youtube_shortlist";
    const std::string operation_id =
        compact_utc() + "-" + stable_hash_id(
            std::to_string(
                Clock::now().time_since_epoch().count()), 6);
    const auto next_directory =
        root / ("youtube_shortlist.next-" + operation_id);
    const auto history_root = root / "shortlist_history";
    const auto archive_directory =
        history_root / operation_id;
    std::filesystem::create_directories(next_directory);

    std::set<std::string> old_videos;
    if (std::filesystem::is_directory(directory))
        for (const auto &entry :
             std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file() &&
                entry.path().extension() == ".mp4")
                old_videos.insert(
                    entry.path().filename().string());
    std::set<std::string> new_videos;
    for (const auto index : indices) {
        auto &test_case = manifest.cases[index];
        const auto source =
            resolve_path(root, test_case.candidate_path);
        if (!std::filesystem::exists(source)) {
            std::filesystem::remove_all(next_directory);
            throw std::runtime_error(
                "selected candidate artifact is missing: " +
                source.string());
        }
        const auto destination =
            next_directory / candidate_filename(test_case);
        std::error_code error;
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::none,
            error);
        if (error) {
            std::filesystem::remove_all(next_directory);
            throw std::runtime_error(
                "could not prepare shortlist video: " +
                error.message());
        }
        new_videos.insert(destination.filename().string());
        report.created_files.push_back(
            destination.filename().string());
        std::ofstream sidecar(
            destination.string() + ".json",
            std::ios::binary | std::ios::trunc);
        if (!sidecar) {
            std::filesystem::remove_all(next_directory);
            throw std::runtime_error(
                "could not write shortlist sidecar");
        }
        sidecar << "{\n"
            << "  \"config_id\": " << q(test_case.config_id) << ",\n"
            << "  \"block_size\": " << test_case.config.block_width << ",\n"
            << "  \"bits_per_block\": "
            << test_case.config.bits_per_block << ",\n"
            << "  \"signal_multiplier\": "
            << std::fixed << std::setprecision(2)
            << test_case.config.signal_multiplier() << ",\n"
            << "  \"repair_percent\": "
            << test_case.config.repair_percent() << ",\n"
            << "  \"resolution\": "
            << q(std::to_string(test_case.config.resolution_width) +
                 "x" +
                 std::to_string(test_case.config.resolution_height))
            << ",\n"
            << "  \"payload_bytes\": "
            << test_case.effective_payload_bytes << ",\n"
            << "  \"useful_bytes_per_second\": "
            << test_case.capacity.useful_payload_bytes_per_second
            << ",\n"
            << "  \"capacity_gain\": "
            << test_case.capacity.useful_payload_gain << ",\n"
            << "  \"local_gates_passed\": true,\n"
            << "  \"youtube_proven\": false,\n"
            << "  \"reason\": " << q(test_case.shortlist_reason)
            << "\n}\n";
        if (!sidecar) {
            sidecar.close();
            std::filesystem::remove_all(next_directory);
            throw std::runtime_error(
                "could not flush shortlist sidecar");
        }
    }
    for (const auto &old_file : old_videos)
        if (!new_videos.contains(old_file))
            report.removed_files.push_back(old_file);

    const auto backup =
        std::filesystem::path(
            manifest_path.string() + ".backup-" +
            operation_id + ".json");
    std::filesystem::copy_file(
        manifest_path, backup,
        std::filesystem::copy_options::none);
    report.manifest_backup = backup;

    bool archived = false;
    try {
        if (std::filesystem::exists(directory)) {
            std::filesystem::create_directories(history_root);
            std::filesystem::rename(
                directory, archive_directory);
            archived = true;
            report.previous_shortlist_archive =
                archive_directory;
        }
        std::filesystem::rename(next_directory, directory);
        try {
            write_manifest_atomic(manifest, manifest_path);
            write_reports(manifest, root / "reports");
        } catch (...) {
            std::filesystem::remove_all(directory);
            if (archived)
                std::filesystem::rename(
                    archive_directory, directory);
            throw;
        }
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(
            next_directory, cleanup_error);
        if (!std::filesystem::exists(directory) &&
            archived &&
            std::filesystem::exists(archive_directory))
            std::filesystem::rename(
                archive_directory, directory);
        throw;
    }

    std::set<std::string> config_ids;
    for (const auto &test_case : manifest.cases)
        config_ids.insert(test_case.config_id);
    for (const auto &config_id : config_ids) {
        const auto decision =
            evaluate_shortlist_eligibility(
                manifest, config_id);
        report.eligible_configs += decision.eligible;
        if (decision.rejected || decision.incomplete)
            report.rejected_configs.push_back(
                config_id + ": " + decision.reason);
    }
    report.selected_configs = indices.size();
    return report;
}

ValidationReport validate_experiment(
    const std::filesystem::path &manifest_path) {
    ValidationReport report;
    const auto original = read_manifest(manifest_path);
    auto derived = original;
    (void) select_shortlist(
        derived, derived.maximum_shortlist_videos);
    std::set<std::string> config_ids;
    for (const auto &test_case : original.cases)
        config_ids.insert(test_case.config_id);
    report.total_configs = config_ids.size();
    for (const auto &config_id : config_ids) {
        const auto decision =
            evaluate_shortlist_eligibility(
                original, config_id);
        report.eligible_configs += decision.eligible;
        report.rejected_configs += decision.rejected;
        report.incomplete_configs += decision.incomplete;
        const bool persisted_shortlisted = std::any_of(
            original.cases.begin(), original.cases.end(),
            [&](const CapacityCase &test_case) {
                return test_case.config_id == config_id &&
                    test_case.shortlisted;
            });
        const bool persisted_rejected = std::any_of(
            original.cases.begin(), original.cases.end(),
            [&](const CapacityCase &test_case) {
                return test_case.config_id == config_id &&
                    (test_case.state == CaseState::Rejected ||
                     test_case.state == CaseState::Failed);
            });
        const bool persisted_pareto = std::any_of(
            original.cases.begin(), original.cases.end(),
            [&](const CapacityCase &test_case) {
                return test_case.config_id == config_id &&
                    test_case.pareto;
            });
        if (persisted_shortlisted &&
            (persisted_rejected || !decision.eligible))
            report.issues.push_back({
                "rejected_shortlisted", config_id,
                decision.reason});
        if (persisted_pareto && !decision.eligible)
            report.issues.push_back({
                "ineligible_pareto", config_id,
                decision.reason});
        if (decision.incomplete)
            report.issues.push_back({
                "missing_mandatory_profile", config_id,
                decision.reason});
    }
    std::set<std::string> expected_files;
    for (const auto &test_case : derived.cases) {
        report.pareto_configs += test_case.pareto;
        report.shortlisted_configs += test_case.shortlisted;
        if (test_case.shortlisted)
            expected_files.insert(
                candidate_filename(test_case));
    }
    const auto shortlist_directory =
        std::filesystem::absolute(manifest_path)
            .parent_path() / "youtube_shortlist";
    std::set<std::string> actual_files;
    if (std::filesystem::is_directory(shortlist_directory))
        for (const auto &entry :
             std::filesystem::directory_iterator(
                 shortlist_directory))
            if (entry.is_regular_file() &&
                entry.path().extension() == ".mp4")
                actual_files.insert(
                    entry.path().filename().string());
    for (const auto &actual : actual_files)
        if (!expected_files.contains(actual))
            report.issues.push_back({
                "unexpected_shortlist_file", "",
                actual});
    for (const auto &expected : expected_files)
        if (!actual_files.contains(expected))
            report.issues.push_back({
                "missing_shortlist_file", "",
                expected});
    return report;
}

void analyze_folder(
    const std::filesystem::path &manifest_path,
    const std::filesystem::path &folder,
    const std::string &session_label) {
    if (!std::filesystem::is_directory(folder))
        throw std::invalid_argument(
            "returned-video folder does not exist");
    auto manifest = read_manifest(manifest_path);
    const auto root =
        std::filesystem::absolute(manifest_path).parent_path();
    std::unordered_set<std::string> seen;
    for (const auto &test_case : manifest.cases)
        for (const auto &result : test_case.results)
            if (!result.analyzed_file_sha256.empty())
                seen.insert(result.analyzed_file_sha256);
    for (const auto &entry :
         std::filesystem::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension().string();
        if (extension != ".mp4" && extension != ".webm" &&
            extension != ".mkv")
            continue;
        CapacityCase *matched = nullptr;
        const std::string filename =
            entry.path().filename().string();
        for (auto &test_case : manifest.cases) {
            if (filename.find(test_case.config_id) !=
                std::string::npos) {
                if (matched && matched->config_id !=
                               test_case.config_id) {
                    matched = nullptr;
                    break;
                }
                matched = &test_case;
            }
        }
        if (!matched) continue;
        const std::string file_hash =
            youtube_test_lab::sha256_file(entry.path());
        if (!seen.insert(file_hash).second)
            continue;
        EncodedArtifacts expected;
        expected.source_packets =
            matched->capacity.expected_source_packets;
        expected.repair_packets =
            matched->capacity.expected_repair_packets;
        expected.total_packets =
            matched->capacity.expected_total_packets;
        const auto master =
            resolve_path(root, matched->master_path);
        if (std::filesystem::exists(master)) {
            // Reconstruct expected raw packet telemetry from the lossless
            // master with the manifest-provided config. A wrong config will
            // fail packet extraction/SHA rather than silently passing.
            std::vector<std::byte> gray;
            std::vector<std::byte> decoded;
            VideoDecoder reader(master.string());
            while (reader.decode_next_gray8_frame(gray)) {
                extract_frame_bytes(
                    gray, matched->config, decoded);
                expected.packet_stream.insert(
                    expected.packet_stream.end(),
                    decoded.begin(), decoded.end());
            }
            const uint64_t packet_bytes =
                checked_mul(expected.total_packets, PACKET_SIZE,
                            "expected packet bytes");
            if (expected.packet_stream.size() > packet_bytes)
                expected.packet_stream.resize(
                    static_cast<std::size_t>(packet_bytes));
        }
        const auto imported =
            root / "imported" / entry.path().filename();
        std::filesystem::copy_file(
            entry.path(), imported,
            std::filesystem::copy_options::overwrite_existing);
        const auto payload =
            resolve_path(root, matched->payload_path);
        const auto restored =
            root / "restored" /
            (matched->case_id + "_youtube_" +
             stable_hash_id(file_hash, 8) + ".bin");
        auto result = decode_video(
            imported, payload, restored, matched->config,
            expected.packet_stream.empty() ? nullptr : &expected,
            "real-youtube-roundtrip", {});
        result.analysis_session_label =
            session_label.empty() ? "Initial YouTube test"
                                  : session_label;
        result.analyzed_file_sha256 = file_hash;
        matched->results.push_back(std::move(result));
    }
    write_manifest_atomic(manifest, manifest_path);
    write_reports(manifest, root / "reports");
}

std::string to_string(const Preset value) {
    switch (value) {
        case Preset::Smoke: return "smoke";
        case Preset::Staged: return "staged";
        case Preset::Custom: return "custom";
    }
    return "unknown";
}

std::string to_string(const CaseState value) {
    switch (value) {
        case CaseState::Pending: return "Pending";
        case CaseState::Running: return "Running";
        case CaseState::Passed: return "Local-only candidate";
        case CaseState::Rejected: return "Rejected";
        case CaseState::Cancelled: return "Cancelled";
        case CaseState::Shortlisted:
            return "Ready for real YouTube test";
        case CaseState::Unavailable: return "Unavailable";
        case CaseState::ResolutionUnsupported:
            return "Resolution-change unsupported";
        case CaseState::Failed: return "Failed";
    }
    return "Unknown";
}

namespace {

Preset preset_from_string(const std::string &value) {
    if (value == "smoke") return Preset::Smoke;
    if (value == "staged") return Preset::Staged;
    if (value == "custom") return Preset::Custom;
    throw std::runtime_error(
        "unknown Capacity Lab preset: " + value);
}

CaseState state_from_string(const std::string &value) {
    if (value == "Pending") return CaseState::Pending;
    if (value == "Running") return CaseState::Running;
    if (value == "Local-only candidate")
        return CaseState::Passed;
    if (value == "Rejected") return CaseState::Rejected;
    if (value == "Cancelled") return CaseState::Cancelled;
    if (value == "Ready for real YouTube test")
        return CaseState::Shortlisted;
    if (value == "Unavailable")
        return CaseState::Unavailable;
    if (value == "Resolution-change unsupported")
        return CaseState::ResolutionUnsupported;
    if (value == "Failed") return CaseState::Failed;
    throw std::runtime_error(
        "unknown Capacity Lab case state: " + value);
}

void write_result_json(std::ostream &out,
                       const CaseResult &result) {
    out << "{"
        << "\"source_type\":" << q(result.source_type)
        << ",\"simulation_profile\":"
        << q(result.simulation_profile)
        << ",\"analysis_session_label\":"
        << q(result.analysis_session_label)
        << ",\"analyzed_file_sha256\":"
        << q(result.analyzed_file_sha256)
        << ",\"codec\":" << q(result.codec)
        << ",\"returned_width\":" << result.returned_width
        << ",\"returned_height\":" << result.returned_height
        << ",\"returned_fps\":" << result.returned_fps
        << ",\"bitrate\":" << result.bitrate
        << ",\"file_size\":" << result.file_size
        << ",\"encode_seconds\":" << result.encode_seconds
        << ",\"transcode_seconds\":" << result.transcode_seconds
        << ",\"decode_seconds\":" << result.decode_seconds
        << ",\"metadata_valid\":"
        << (result.metadata_valid ? "true" : "false")
        << ",\"decode_completed\":"
        << (result.decode_completed ? "true" : "false")
        << ",\"sha256_match\":"
        << (result.sha256_match ? "true" : "false")
        << ",\"restored_sha256\":"
        << q(result.restored_sha256)
        << ",\"error\":" << q(result.error)
        << ",\"frames_read\":"
        << result.telemetry.frames_read
        << ",\"symbols_compared\":"
        << result.telemetry.symbols_compared
        << ",\"symbol_errors\":"
        << result.telemetry.symbol_errors
        << ",\"bits_compared\":"
        << result.telemetry.bits_compared
        << ",\"bit_errors\":"
        << result.telemetry.bit_errors
        << ",\"extracted_packets\":"
        << result.telemetry.extracted_packets
        << ",\"valid_unique_packets\":"
        << result.telemetry.valid_unique_packets
        << ",\"duplicate_packets\":"
        << result.telemetry.duplicate_packets
        << ",\"crc_invalid_packets\":"
        << result.telemetry.crc_invalid_packets
        << ",\"missing_packets\":"
        << result.telemetry.missing_packets
        << ",\"source_packets\":"
        << result.telemetry.source_packets
        << ",\"repair_packets\":"
        << result.telemetry.repair_packets
        << ",\"required_packet_threshold\":"
        << result.telemetry.required_packet_threshold
        << ",\"recovery_margin_packets\":"
        << result.telemetry.recovery_margin_packets
        << ",\"recovery_margin_percent\":"
        << result.telemetry.recovery_margin_percent
        << ",\"packet_recovery_percent\":"
        << result.telemetry.packet_recovery_percent
        << ",\"raw_ber\":" << result.telemetry.raw_ber
        << ",\"raw_ser\":" << result.telemetry.raw_ser
        << ",\"average_confidence\":"
        << result.telemetry.average_confidence
        << ",\"minimum_confidence\":"
        << result.telemetry.minimum_confidence
        << "}";
}

CaseResult parse_result(const std::string &object) {
    CaseResult result;
    result.source_type =
        json_string(object, "source_type").value_or("");
    result.simulation_profile =
        json_string(object, "simulation_profile").value_or("");
    result.analysis_session_label =
        json_string(object, "analysis_session_label").value_or("");
    result.analyzed_file_sha256 =
        json_string(object, "analyzed_file_sha256").value_or("");
    result.codec =
        json_string(object, "codec").value_or("");
    result.returned_width = static_cast<int>(
        json_number(object, "returned_width").value_or(0));
    result.returned_height = static_cast<int>(
        json_number(object, "returned_height").value_or(0));
    result.returned_fps =
        json_number(object, "returned_fps").value_or(0);
    result.bitrate = static_cast<int64_t>(
        json_number(object, "bitrate").value_or(0));
    result.file_size = static_cast<uint64_t>(
        json_number(object, "file_size").value_or(0));
    result.encode_seconds =
        json_number(object, "encode_seconds").value_or(0);
    result.transcode_seconds =
        json_number(object, "transcode_seconds").value_or(0);
    result.decode_seconds =
        json_number(object, "decode_seconds").value_or(0);
    result.metadata_valid =
        json_bool(object, "metadata_valid");
    result.decode_completed =
        json_bool(object, "decode_completed");
    result.sha256_match =
        json_bool(object, "sha256_match");
    result.restored_sha256 =
        json_string(object, "restored_sha256").value_or("");
    result.error =
        json_string(object, "error").value_or("");
#define READ_TELEMETRY(field) \
    result.telemetry.field = static_cast<decltype(result.telemetry.field)>( \
        json_number(object, #field).value_or(0))
    READ_TELEMETRY(frames_read);
    READ_TELEMETRY(symbols_compared);
    READ_TELEMETRY(symbol_errors);
    READ_TELEMETRY(bits_compared);
    READ_TELEMETRY(bit_errors);
    READ_TELEMETRY(extracted_packets);
    READ_TELEMETRY(valid_unique_packets);
    READ_TELEMETRY(duplicate_packets);
    READ_TELEMETRY(crc_invalid_packets);
    READ_TELEMETRY(missing_packets);
    READ_TELEMETRY(source_packets);
    READ_TELEMETRY(repair_packets);
    READ_TELEMETRY(required_packet_threshold);
    READ_TELEMETRY(recovery_margin_packets);
    READ_TELEMETRY(recovery_margin_percent);
    READ_TELEMETRY(packet_recovery_percent);
    READ_TELEMETRY(raw_ber);
    READ_TELEMETRY(raw_ser);
    READ_TELEMETRY(average_confidence);
    READ_TELEMETRY(minimum_confidence);
#undef READ_TELEMETRY
    // Older schema-v4 manifests could be loaded and rewritten by a parser
    // that dropped JSON scientific-notation exponents. Integer counters are
    // authoritative, so always derive BER/SER from them during migration.
    if (result.telemetry.bits_compared != 0)
        result.telemetry.raw_ber =
            static_cast<double>(result.telemetry.bit_errors) /
            result.telemetry.bits_compared;
    if (result.telemetry.symbols_compared != 0)
        result.telemetry.raw_ser =
            static_cast<double>(result.telemetry.symbol_errors) /
            result.telemetry.symbols_compared;
    return result;
}

} // namespace

void write_manifest_atomic(
    const ExperimentManifest &manifest,
    const std::filesystem::path &path) {
    std::filesystem::create_directories(path.parent_path());
    SafeOutputFile safe(path);
    {
        std::ofstream out(
            safe.partial_path(),
            std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error(
                "could not write Capacity Lab manifest");
        out << std::setprecision(17)
            << "{\n"
            << "  \"schema_version\": "
            << manifest.schema_version << ",\n"
            << "  \"manifest_type\": "
            << q("youtube-capacity-lab") << ",\n"
            << "  \"experiment_id\": "
            << q(manifest.experiment_id) << ",\n"
            << "  \"created_at\": "
            << q(manifest.created_at) << ",\n"
            << "  \"vidstorex_version\": "
            << q(manifest.vidstorex_version) << ",\n"
            << "  \"preset\": "
            << q(to_string(manifest.preset)) << ",\n"
            << "  \"maximum_cases\": "
            << manifest.maximum_cases << ",\n"
            << "  \"maximum_shortlist_videos\": "
            << manifest.maximum_shortlist_videos << ",\n"
            << "  \"maximum_disk_bytes\": "
            << manifest.maximum_disk_bytes << ",\n"
            << "  \"cancelled\": "
            << (manifest.cancelled ? "true" : "false")
            << ",\n"
            << "  \"baseline_config_id\": "
            << q(manifest.baseline.config_id()) << ",\n"
            << "  \"mandatory_stage1_profiles\": [";
        for (std::size_t i = 0;
             i < manifest.mandatory_stage1_profiles.size(); ++i) {
            if (i != 0) out << ",";
            out << q(manifest.mandatory_stage1_profiles[i]);
        }
        out << "],\n"
            << "  \"mandatory_stage3_profiles\": [";
        for (std::size_t i = 0;
             i < manifest.mandatory_stage3_profiles.size(); ++i) {
            if (i != 0) out << ",";
            out << q(manifest.mandatory_stage3_profiles[i]);
        }
        out << "],\n"
            << "  \"cases\": [\n";
        for (std::size_t i = 0;
             i < manifest.cases.size(); ++i) {
            const auto &c = manifest.cases[i];
            const auto &levels =
                effective_modulation_levels(c.config);
            const int level_count =
                c.config.bits_per_block == 1 ? 2 : 4;
            bool signal_clamped = false;
            for (int level = 0; level < level_count; ++level) {
                bool level_clamped = false;
                (void) make_symbol_block(
                    c.config, static_cast<uint8_t>(level),
                    &level_clamped);
                signal_clamped =
                    signal_clamped || level_clamped;
            }
            out << "    {\n"
                << "      \"case_id\": " << q(c.case_id) << ",\n"
                << "      \"config_id\": " << q(c.config_id) << ",\n"
                << "      \"stage\": " << c.stage << ",\n"
                << "      \"block_width\": "
                << c.config.block_width << ",\n"
                << "      \"block_height\": "
                << c.config.block_height << ",\n"
                << "      \"bits_per_block\": "
                << c.config.bits_per_block << ",\n"
                << "      \"signal_milli\": "
                << c.config.signal_milli << ",\n"
                << "      \"repair_basis_points\": "
                << c.config.repair_basis_points << ",\n"
                << "      \"resolution_width\": "
                << c.config.resolution_width << ",\n"
                << "      \"resolution_height\": "
                << c.config.resolution_height << ",\n"
                << "      \"fps\": " << c.config.fps << ",\n"
                << "      \"packet_symbol_size\": "
                << c.config.packet_symbol_size << ",\n"
                << "      \"transform_version\": "
                << q(c.config.transform_version) << ",\n"
                << "      \"modulation_version\": "
                << q(c.config.modulation_version) << ",\n"
                << "      \"decoder_threshold_version\": "
                << q(c.config.decoder_threshold_version) << ",\n"
                << "      \"interleaving\": "
                << (c.config.interleaving ? "true" : "false")
                << ",\n"
                << "      \"created_with_version\": "
                << q(c.config.created_with_version) << ",\n"
                << "      \"nominal_coefficient_strength\": "
                << signal_strength(c.config) << ",\n"
                << "      \"signal_clamped\": "
                << (signal_clamped ? "true" : "false")
                << ",\n"
                << "      \"effective_levels\": [";
            for (int level = 0; level < level_count; ++level) {
                if (level != 0) out << ",";
                out << levels[level];
            }
            out << "],\n"
                << "      \"decoder_thresholds\": [";
            for (int level = 0;
                 level + 1 < level_count; ++level) {
                if (level != 0) out << ",";
                out << (levels[level] + levels[level + 1]) / 2.0;
            }
            out << "],\n"
                << "      \"requested_payload_bytes\": "
                << c.requested_payload_bytes << ",\n"
                << "      \"effective_payload_bytes\": "
                << c.effective_payload_bytes << ",\n"
                << "      \"payload_seed\": "
                << c.payload_seed << ",\n"
                << "      \"source_sha256\": "
                << q(c.source_sha256) << ",\n"
                << "      \"payload_path\": "
                << q(c.payload_path) << ",\n"
                << "      \"master_path\": "
                << q(c.master_path) << ",\n"
                << "      \"candidate_path\": "
                << q(c.candidate_path) << ",\n"
                << "      \"restored_path\": "
                << q(c.restored_path) << ",\n"
                << "      \"requested_simulation_profile\": "
                << q(c.requested_simulation_profile) << ",\n"
                << "      \"master_size\": "
                << c.master_size << ",\n"
                << "      \"candidate_size\": "
                << c.candidate_size << ",\n"
                << "      \"state\": "
                << q(to_string(c.state)) << ",\n"
                << "      \"mandatory_gates_passed\": "
                << (c.mandatory_gates_passed ? "true" : "false")
                << ",\n"
                << "      \"dominated\": "
                << (c.dominated ? "true" : "false") << ",\n"
                << "      \"pareto\": "
                << (c.pareto ? "true" : "false") << ",\n"
                << "      \"shortlisted\": "
                << (c.shortlisted ? "true" : "false") << ",\n"
                << "      \"eligible_for_shortlist\": "
                << (c.eligible_for_shortlist
                        ? "true" : "false") << ",\n"
                << "      \"incomplete\": "
                << (c.incomplete ? "true" : "false") << ",\n"
                << "      \"category\": " << q(c.category) << ",\n"
                << "      \"rejection_reason\": "
                << q(c.rejection_reason) << ",\n"
                << "      \"shortlist_reason\": "
                << q(c.shortlist_reason) << ",\n"
                << "      \"shortlist_exclusion_reason\": "
                << q(c.shortlist_exclusion_reason) << ",\n"
                << "      \"failed_mandatory_profile\": "
                << q(c.failed_mandatory_profile) << ",\n"
                << "      \"local_gate_status\": "
                << q(c.local_gate_status) << ",\n"
                << "      \"raw_bits_per_frame\": "
                << c.capacity.geometry.raw_bits_per_frame << ",\n"
                << "      \"raw_bytes_per_frame\": "
                << c.capacity.geometry.raw_bytes_per_frame << ",\n"
                << "      \"packets_per_frame\": "
                << c.capacity.geometry.packets_per_frame << ",\n"
                << "      \"unused_right_pixels\": "
                << c.capacity.geometry.unused_right_pixels << ",\n"
                << "      \"unused_bottom_pixels\": "
                << c.capacity.geometry.unused_bottom_pixels << ",\n"
                << "      \"useful_payload_bytes_per_second\": "
                << c.capacity.useful_payload_bytes_per_second << ",\n"
                << "      \"useful_payload_gain\": "
                << c.capacity.useful_payload_gain << ",\n"
                << "      \"expected_source_packets\": "
                << c.capacity.expected_source_packets << ",\n"
                << "      \"expected_repair_packets\": "
                << c.capacity.expected_repair_packets << ",\n"
                << "      \"expected_total_packets\": "
                << c.capacity.expected_total_packets << ",\n"
                << "      \"expected_frames\": "
                << c.capacity.expected_frames << ",\n"
                << "      \"expected_duration_seconds\": "
                << c.capacity.expected_duration_seconds << ",\n"
                << "      \"estimated_peak_memory_bytes\": "
                << c.capacity.estimated_peak_memory_bytes << ",\n"
                << "      \"results\": [";
            for (std::size_t r = 0; r < c.results.size(); ++r) {
                if (r != 0) out << ",";
                out << "\n        ";
                write_result_json(out, c.results[r]);
            }
            if (!c.results.empty()) out << "\n      ";
            out << "]\n    }";
            if (i + 1 != manifest.cases.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n}\n";
        if (!out)
            throw std::runtime_error(
                "could not flush Capacity Lab manifest");
    }
    safe.commit();
}

ExperimentManifest read_manifest(
    const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error(
            "could not open Capacity Lab manifest");
    const std::string json{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (json_string(json, "manifest_type").value_or("") !=
        "youtube-capacity-lab")
        throw std::runtime_error(
            "manifest is not a YouTube Capacity Lab experiment");
    const int schema = static_cast<int>(
        json_number(json, "schema_version").value_or(0));
    if (schema != kManifestSchemaVersion)
        throw std::runtime_error(
            "unsupported Capacity Lab manifest schema");
    ExperimentManifest manifest;
    manifest.schema_version = schema;
    manifest.experiment_id =
        json_string(json, "experiment_id").value_or("");
    manifest.created_at =
        json_string(json, "created_at").value_or("");
    manifest.vidstorex_version =
        json_string(json, "vidstorex_version").value_or("");
    manifest.preset = preset_from_string(
        json_string(json, "preset").value_or("smoke"));
    manifest.maximum_cases = static_cast<std::size_t>(
        json_u64(json, "maximum_cases").value_or(
            kDefaultMaximumCases));
    manifest.maximum_shortlist_videos =
        static_cast<std::size_t>(
            json_u64(
                json, "maximum_shortlist_videos")
                .value_or(8));
    manifest.maximum_disk_bytes =
        json_u64(json, "maximum_disk_bytes").value_or(
            kDefaultMaximumDiskBytes);
    const auto stage1_profiles =
        json_string_array(
            json, "mandatory_stage1_profiles");
    if (!stage1_profiles.empty())
        manifest.mandatory_stage1_profiles =
            stage1_profiles;
    const auto stage3_profiles =
        json_string_array(
            json, "mandatory_stage3_profiles");
    if (!stage3_profiles.empty())
        manifest.mandatory_stage3_profiles =
            stage3_profiles;
    manifest.cancelled = json_bool(json, "cancelled");
    manifest.baseline = production_baseline_config();
    for (const auto &object : extract_objects(json, "cases")) {
        CapacityCase c;
        c.case_id =
            json_string(object, "case_id").value_or("");
        c.config_id =
            json_string(object, "config_id").value_or("");
        c.stage = static_cast<int>(
            json_number(object, "stage").value_or(1));
        c.config.block_width = static_cast<int>(
            json_number(object, "block_width").value_or(8));
        c.config.block_height = static_cast<int>(
            json_number(object, "block_height").value_or(8));
        c.config.bits_per_block = static_cast<int>(
            json_number(object, "bits_per_block").value_or(1));
        c.config.signal_milli = static_cast<int>(
            json_number(object, "signal_milli").value_or(1000));
        c.config.repair_basis_points = static_cast<int>(
            json_number(object, "repair_basis_points").value_or(500));
        c.config.resolution_width = static_cast<int>(
            json_number(object, "resolution_width").value_or(1920));
        c.config.resolution_height = static_cast<int>(
            json_number(object, "resolution_height").value_or(1080));
        c.config.fps = static_cast<int>(
            json_number(object, "fps").value_or(kFps));
        c.config.packet_symbol_size = static_cast<int>(
            json_number(object, "packet_symbol_size").value_or(
                SYMBOL_SIZE_BYTES));
        c.config.transform_version =
            json_string(object, "transform_version")
                .value_or(kTransformVersion);
        c.config.modulation_version =
            json_string(object, "modulation_version")
                .value_or(c.config.bits_per_block == 1
                    ? kModulation1Version
                    : kModulation2Version);
        c.config.decoder_threshold_version =
            json_string(object, "decoder_threshold_version")
                .value_or(kThresholdVersion);
        c.config.interleaving =
            json_bool(object, "interleaving");
        c.config.created_with_version =
            json_string(object, "created_with_version")
                .value_or("1.4.0");
        if (c.config.config_id() != c.config_id)
            throw std::runtime_error(
                "Capacity manifest config fingerprint mismatch");
        c.capacity = compute_capacity(c.config);
        c.requested_payload_bytes =
            json_u64(object, "requested_payload_bytes").value_or(0);
        c.effective_payload_bytes =
            json_u64(object, "effective_payload_bytes").value_or(0);
        c.payload_seed =
            json_u64(object, "payload_seed").value_or(0);
        c.source_sha256 =
            json_string(object, "source_sha256").value_or("");
        c.payload_path =
            json_string(object, "payload_path").value_or("");
        c.master_path =
            json_string(object, "master_path").value_or("");
        c.candidate_path =
            json_string(object, "candidate_path").value_or("");
        c.restored_path =
            json_string(object, "restored_path").value_or("");
        c.requested_simulation_profile =
            json_string(object, "requested_simulation_profile")
                .value_or("yt-sim-1080p-medium");
        c.master_size =
            json_u64(object, "master_size").value_or(0);
        c.candidate_size =
            json_u64(object, "candidate_size").value_or(0);
        c.state = state_from_string(
            json_string(object, "state").value_or("Pending"));
        c.mandatory_gates_passed =
            json_bool(object, "mandatory_gates_passed");
        c.dominated = json_bool(object, "dominated");
        c.pareto = json_bool(object, "pareto");
        c.shortlisted = json_bool(object, "shortlisted");
        c.eligible_for_shortlist =
            json_bool(object, "eligible_for_shortlist");
        c.incomplete = json_bool(object, "incomplete");
        c.category =
            json_string(object, "category").value_or("");
        c.rejection_reason =
            json_string(object, "rejection_reason").value_or("");
        c.shortlist_reason =
            json_string(object, "shortlist_reason").value_or("");
        c.shortlist_exclusion_reason =
            json_string(
                object, "shortlist_exclusion_reason")
                .value_or("");
        c.failed_mandatory_profile =
            json_string(
                object, "failed_mandatory_profile")
                .value_or("");
        c.local_gate_status =
            json_string(object, "local_gate_status")
                .value_or("");
        c.capacity.expected_source_packets =
            json_u64(object, "expected_source_packets")
                .value_or(c.capacity.expected_source_packets);
        c.capacity.expected_repair_packets =
            json_u64(object, "expected_repair_packets")
                .value_or(c.capacity.expected_repair_packets);
        c.capacity.expected_total_packets =
            json_u64(object, "expected_total_packets")
                .value_or(c.capacity.expected_total_packets);
        c.capacity.expected_frames =
            json_u64(object, "expected_frames")
                .value_or(c.capacity.expected_frames);
        c.capacity.expected_duration_seconds =
            json_number(object, "expected_duration_seconds")
                .value_or(c.capacity.expected_duration_seconds);
        c.capacity.useful_payload_bytes_per_second =
            json_number(
                object, "useful_payload_bytes_per_second")
                .value_or(
                    c.capacity.useful_payload_bytes_per_second);
        c.capacity.useful_payload_gain =
            json_number(object, "useful_payload_gain")
                .value_or(c.capacity.useful_payload_gain);
        for (const auto &result :
             extract_objects(object, "results"))
            c.results.push_back(parse_result(result));
        manifest.cases.push_back(std::move(c));
    }
    if (manifest.experiment_id.empty())
        throw std::runtime_error(
            "Capacity manifest has no experiment id");
    return manifest;
}

void write_reports(
    const ExperimentManifest &source_manifest,
    const std::filesystem::path &reports_directory) {
    auto manifest = source_manifest;
    (void) select_shortlist(
        manifest, manifest.maximum_shortlist_videos);
    std::filesystem::create_directories(reports_directory);
    {
        std::ofstream out(
            reports_directory / "capacity_results.csv",
            std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error(
                "could not write Capacity Lab CSV report");
        out << "config_id,stage,block,bits,signal,repair,resolution,"
               "useful_kib_s,gain,candidate_bytes,packet_recovery,"
               "recovery_margin,ber,ser,average_confidence,sha,"
               "eligible,shortlisted,incomplete,failed_mandatory_profile,"
               "pareto,category,status,exclusion_reason,"
               "shortlist_filename\n";
        for (const auto &c : manifest.cases) {
            const CaseResult *result =
                c.results.empty() ? nullptr : &c.results.back();
            out << c.config_id << "," << c.stage << ","
                << c.config.block_width << ","
                << c.config.bits_per_block << ","
                << c.config.signal_multiplier() << ","
                << c.config.repair_percent() << ","
                << c.config.resolution_width << "x"
                << c.config.resolution_height << ","
                << c.capacity.useful_payload_bytes_per_second / 1024.0
                << "," << c.capacity.useful_payload_gain << ","
                << c.candidate_size << ","
                << (result ? result->telemetry
                                 .packet_recovery_percent : 0.0)
                << ","
                << (result ? result->telemetry
                                 .recovery_margin_percent : 0.0)
                << ","
                << (result ? result->telemetry.raw_ber : 0.0)
                << ","
                << (result ? result->telemetry.raw_ser : 0.0)
                << ","
                << (result ? result->telemetry
                                 .average_confidence : 0.0)
                << ","
                << (result && result->sha256_match
                        ? "exact" : "not-exact")
                << "," << (c.eligible_for_shortlist
                        ? "yes" : "no")
                << "," << (c.shortlisted ? "yes" : "no")
                << "," << (c.incomplete ? "yes" : "no")
                << "," << q(c.failed_mandatory_profile)
                << "," << (c.pareto ? "yes" : "no")
                << "," << q(c.category)
                << "," << q(to_string(c.state))
                << "," << q(c.shortlisted
                    ? ""
                    : c.shortlist_exclusion_reason)
                << "," << q(c.shortlisted
                    ? candidate_filename(c) : "")
                << "\n";
        }
    }
    {
        std::ofstream out(
            reports_directory / "capacity_report.md",
            std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error(
                "could not write Capacity Lab Markdown report");
        std::set<std::string> report_config_ids;
        for (const auto &test_case : manifest.cases)
            report_config_ids.insert(test_case.config_id);
        uint64_t eligible_configs = 0;
        uint64_t rejected_configs = 0;
        uint64_t incomplete_configs = 0;
        std::map<std::string, EligibilityDecision>
            report_decisions;
        for (const auto &config_id : report_config_ids) {
            auto decision =
                evaluate_shortlist_eligibility(
                    manifest, config_id);
            eligible_configs += decision.eligible;
            rejected_configs += decision.rejected;
            incomplete_configs += decision.incomplete;
            report_decisions.emplace(
                config_id, std::move(decision));
        }
        out << "# YouTube Capacity Lab\n\n"
            << "Experiment: `" << manifest.experiment_id << "`  \n"
            << "Preset: " << to_string(manifest.preset) << "  \n"
            << "Created: " << manifest.created_at << "\n\n"
            << "> Local simulations are not real YouTube evidence. "
               "No configuration below is marked YouTube-proven until a "
               "real returned-video observation passes exact SHA-256.\n\n"
            << "## Decision summary\n\n"
            << "- Total configs: " << report_config_ids.size() << "\n"
            << "- Eligible configs: " << eligible_configs << "\n"
            << "- Rejected configs: " << rejected_configs << "\n"
            << "- Incomplete configs: " << incomplete_configs << "\n"
            << "- Pareto configs: "
            << std::count_if(
                   manifest.cases.begin(), manifest.cases.end(),
                   [](const CapacityCase &value) {
                       return value.pareto;
                   })
            << "\n"
            << "- Shortlisted configs: "
            << std::count_if(
                   manifest.cases.begin(), manifest.cases.end(),
                   [](const CapacityCase &value) {
                       return value.shortlisted;
                   })
            << "\n\n"
            << "## A. Baseline\n\n"
            << "- 8x8 / 1-bit / 1.00x / 5% repair\n"
            << "- Production coefficient: "
            << kProductionStrength << "\n"
            << "- Decoder threshold: coefficient sign at 0\n"
            << "- Known external result: existing Quick Suite reported "
               "6/6 exact matches; this report does not reclassify that "
               "evidence as a Capacity Lab observation.\n\n"
            << "## B. Stage results\n\n";
        for (const int stage : {1, 2, 3}) {
            uint64_t passed = 0;
            uint64_t rejected = 0;
            for (const auto &c : manifest.cases)
                if (c.stage == stage) {
                    passed += c.mandatory_gates_passed;
                    rejected += !c.mandatory_gates_passed &&
                        (c.state == CaseState::Rejected ||
                         c.state == CaseState::Failed);
                }
            out << "- Stage " << stage << ": "
                << passed << " passed, "
                << rejected << " rejected\n";
        }
        out << "\n### Config eligibility\n\n"
            << "| Config | Eligible | Selected | Gate | "
               "Failed mandatory profile | Rejection/exclusion reason | "
               "Pareto | Category | Shortlist filename |\n"
            << "|---|---|---|---|---|---|---|---|---|\n";
        for (const auto &[config_id, decision] :
             report_decisions) {
            const auto selected = std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const CapacityCase &value) {
                    return value.config_id == config_id &&
                        value.shortlisted;
                });
            const auto representative = std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const CapacityCase &value) {
                    return value.config_id == config_id &&
                        (value.shortlisted ||
                         value.mandatory_gates_passed);
                });
            out << "|" << config_id
                << "|" << (decision.eligible ? "Yes" : "No")
                << "|" << (selected != manifest.cases.end()
                                ? "Yes" : "No")
                << "|" << (decision.eligible
                                ? "Passed"
                                : decision.incomplete
                                    ? "Incomplete" : "Rejected")
                << "|" << decision.failed_mandatory_profile
                << "|" << (decision.eligible
                                ? ""
                                : decision.reason)
                << "|" << (representative != manifest.cases.end() &&
                                representative->pareto
                                ? "Yes" : "No")
                << "|" << (representative != manifest.cases.end()
                                ? representative->category : "")
                << "|" << (selected != manifest.cases.end()
                                ? candidate_filename(*selected) : "")
                << "|\n";
        }
        out << "\n## C. Pareto frontier\n\n"
            << "| Config | Block | Bits | Signal | Repair | Resolution | "
               "Useful KiB/s | Gain | Recovery margin | BER/SER | "
               "Candidate ratio | Result |\n"
            << "|---|---:|---:|---:|---:|---|---:|---:|---:|---:|---:|---|\n";
        for (const auto &c : manifest.cases) {
            if (!c.pareto) continue;
            const CaseResult *result =
                c.results.empty() ? nullptr : &c.results.back();
            const double ratio =
                c.effective_payload_bytes == 0 ? 0.0 :
                static_cast<double>(c.candidate_size) /
                    c.effective_payload_bytes;
            out << "|" << c.config_id
                << "|" << c.config.block_width
                << "|" << c.config.bits_per_block
                << "|" << c.config.signal_multiplier()
                << "|" << c.config.repair_percent()
                << "|" << c.config.resolution_width << "x"
                << c.config.resolution_height
                << "|" << c.capacity
                              .useful_payload_bytes_per_second / 1024.0
                << "|" << c.capacity.useful_payload_gain
                << "|" << (result ? result->telemetry
                                        .recovery_margin_percent : 0.0)
                << "|" << (result ? result->telemetry.raw_ber : 0.0)
                << "/" << (result ? result->telemetry.raw_ser : 0.0)
                << "|" << ratio
                << "|" << to_string(c.state) << "|\n";
        }
        out << "\n## D. Shortlist\n\n";
        for (const auto &c : manifest.cases)
            if (c.shortlisted)
                out << "- `" << candidate_filename(c) << "`: "
                    << c.shortlist_reason << "\n";
        out << "\n## E. Real YouTube observations\n\n"
            << "| Config | Session | Codec | Resolution | Bitrate | "
               "Recovery | SHA | Result |\n"
            << "|---|---|---|---|---:|---:|---|---|\n";
        bool any_real = false;
        for (const auto &c : manifest.cases)
            for (const auto &result : c.results)
                if (result.source_type ==
                    "real-youtube-roundtrip") {
                    any_real = true;
                    out << "|" << c.config_id
                        << "|" << result.analysis_session_label
                        << "|" << result.codec
                        << "|" << result.returned_width << "x"
                        << result.returned_height
                        << "|" << result.bitrate
                        << "|" << result.telemetry
                                      .packet_recovery_percent
                        << "|" << (result.sha256_match
                            ? "exact" : "mismatch")
                        << "|" << (result.sha256_match
                            ? "Real YouTube exact pass"
                            : "Real YouTube failed")
                        << "|\n";
                }
        if (!any_real)
            out << "|-|Insufficient observations|-|-|-|-|-|"
                   "Insufficient observations|\n";
        out << "\n## F. Baseline comparison\n\n"
            << "Capacity gain and duration reduction are computed from "
               "packetized useful payload. Candidate sizes come only from "
               "actual local encodes, never from theoretical capacity.\n\n";
        for (const auto &c : manifest.cases)
            if (c.mandatory_gates_passed)
                out << "- `" << c.config_id << "`: "
                    << std::fixed << std::setprecision(2)
                    << c.capacity.useful_payload_gain << "x useful gain, "
                    << c.capacity.duration_reduction * 100.0
                    << "% estimated duration reduction, "
                    << c.candidate_size << " candidate bytes.\n";
    }
    {
        std::ofstream out(
            reports_directory / "capacity_summary.json",
            std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error(
                "could not write Capacity Lab JSON report");
        uint64_t eligible = 0;
        uint64_t rejected = 0;
        uint64_t incomplete = 0;
        uint64_t pareto = 0;
        uint64_t shortlist = 0;
        std::set<std::string> config_ids;
        for (const auto &c : manifest.cases) {
            config_ids.insert(c.config_id);
            pareto += c.pareto;
            shortlist += c.shortlisted;
        }
        std::vector<EligibilityDecision> decisions;
        for (const auto &config_id : config_ids) {
            auto decision =
                evaluate_shortlist_eligibility(
                    manifest, config_id);
            eligible += decision.eligible;
            rejected += decision.rejected;
            incomplete += decision.incomplete;
            decisions.push_back(std::move(decision));
        }
        out << "{\n"
            << "  \"experiment_id\": "
            << q(manifest.experiment_id) << ",\n"
            << "  \"schema_version\": "
            << manifest.schema_version << ",\n"
            << "  \"cases\": " << manifest.cases.size() << ",\n"
            << "  \"configs\": " << config_ids.size() << ",\n"
            << "  \"eligible\": " << eligible << ",\n"
            << "  \"rejected\": " << rejected << ",\n"
            << "  \"incomplete\": " << incomplete << ",\n"
            << "  \"pareto\": " << pareto << ",\n"
            << "  \"shortlisted\": " << shortlist << ",\n"
            << "  \"config_decisions\": [";
        bool first_decision = true;
        for (const auto &decision : decisions) {
            const auto representative = std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const CapacityCase &value) {
                    return value.config_id == decision.config_id &&
                        (value.shortlisted ||
                         value.mandatory_gates_passed);
                });
            if (!first_decision) out << ",";
            first_decision = false;
            out << "\n    {\"config_id\": "
                << q(decision.config_id)
                << ", \"eligible\": "
                << (decision.eligible ? "true" : "false")
                << ", \"selected_for_shortlist\": "
                << (representative != manifest.cases.end() &&
                        representative->shortlisted
                        ? "true" : "false")
                << ", \"incomplete\": "
                << (decision.incomplete ? "true" : "false")
                << ", \"failed_mandatory_profile\": "
                << q(decision.failed_mandatory_profile)
                << ", \"rejection_reason\": "
                << q(decision.eligible ? "" : decision.reason)
                << ", \"pareto\": "
                << (representative != manifest.cases.end() &&
                        representative->pareto
                        ? "true" : "false")
                << ", \"category\": "
                << q(representative != manifest.cases.end()
                         ? representative->category : "")
                << ", \"shortlist_filename\": "
                << q(representative != manifest.cases.end() &&
                         representative->shortlisted
                         ? candidate_filename(*representative)
                         : "")
                << "}";
        }
        if (!first_decision) out << "\n  ";
        out << "],\n"
            << "  \"shortlist_exclusions\": [";
        bool first_exclusion = true;
        for (const auto &decision : decisions) {
            if (decision.eligible) continue;
            if (!first_exclusion) out << ",";
            first_exclusion = false;
            out << "\n    {\"config_id\": "
                << q(decision.config_id)
                << ", \"failed_mandatory_profile\": "
                << q(decision.failed_mandatory_profile)
                << ", \"reason\": " << q(decision.reason)
                << "}";
        }
        if (!first_exclusion) out << "\n  ";
        out << "],\n"
            << "  \"youtube_proven\": false\n"
            << "}\n";
    }
}

} // namespace youtube_capacity_lab
