#include "video_set.h"

#include "configuration.h"
#include "safe_output.h"
#include "video_encoder.h"
#include "libs/picosha2.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <sodium.h>

namespace video_set {
namespace {

constexpr std::size_t kFixedHeaderBytes = 204;
constexpr std::size_t kMinimumHeaderBytes = kFixedHeaderBytes + 4;
constexpr uint64_t kMiB = 1024ull * 1024ull;

void append_u16(std::vector<std::byte> &out, const uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xff));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void append_u32(std::vector<std::byte> &out, const uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

void append_u64(std::vector<std::byte> &out, const uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

uint16_t read_u16(std::span<const std::byte> bytes, const std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2)
        throw std::runtime_error("truncated 16-bit field");
    return static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[at])) |
           static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[at + 1])) << 8;
}

uint32_t read_u32(std::span<const std::byte> bytes, const std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4)
        throw std::runtime_error("truncated 32-bit field");
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        result |= static_cast<uint32_t>(
            std::to_integer<uint8_t>(bytes[at + i])) << (8 * i);
    return result;
}

uint64_t read_u64(std::span<const std::byte> bytes, const std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 8)
        throw std::runtime_error("truncated 64-bit field");
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result |= static_cast<uint64_t>(
            std::to_integer<uint8_t>(bytes[at + i])) << (8 * i);
    return result;
}

template <std::size_t N>
void append_array(std::vector<std::byte> &out,
                  const std::array<std::byte, N> &value) {
    out.insert(out.end(), value.begin(), value.end());
}

template <std::size_t N>
std::array<std::byte, N> read_array(std::span<const std::byte> bytes,
                                    const std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < N)
        throw std::runtime_error("truncated byte array");
    std::array<std::byte, N> result{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), N,
                result.begin());
    return result;
}

bool valid_utf8(const std::string &text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7f) { ++i; continue; }
        unsigned continuation = 0;
        uint32_t code = 0;
        if ((c & 0xe0) == 0xc0) { continuation = 1; code = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { continuation = 2; code = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { continuation = 3; code = c & 0x07; }
        else return false;
        if (i + continuation >= text.size()) return false;
        for (unsigned j = 1; j <= continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && code < 0x80) ||
            (continuation == 2 && code < 0x800) ||
            (continuation == 3 && code < 0x10000) ||
            code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += continuation + 1;
    }
    return true;
}

Sha256Digest digest_bytes(const std::string &value) {
    return sha256(std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(value.data()), value.size()));
}

std::string digest_hex_lower(const Sha256Digest &digest) {
    std::string value = digest.hexValue();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

Sha256Digest digest_from_hex(const std::string &hex) {
    if (hex.size() != 64) throw std::runtime_error("SHA-256 must contain 64 hex characters");
    Sha256Digest result;
    for (std::size_t i = 0; i < result.bytes.size(); ++i) {
        const auto decode = [](const char c) -> unsigned {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::runtime_error("invalid SHA-256 hexadecimal value");
        };
        result.bytes[i] = static_cast<std::byte>(
            (decode(hex[i * 2]) << 4) | decode(hex[i * 2 + 1]));
    }
    return result;
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
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4)
                                  << std::setfill('0') << static_cast<int>(c)
                                  << std::dec;
                else out << static_cast<char>(c);
        }
    }
    return out.str();
}

std::string q(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}

std::string canonical_descriptor(const SetPlan &plan) {
    std::ostringstream out;
    out << "vidstorex.video_set|1|" << id_hex(plan.set_id) << '|'
        << plan.original_filename << '|' << plan.original_file_size << '|'
        << digest_hex_lower(plan.original_file_sha256) << '|'
        << static_cast<int>(plan.profile) << '|' << plan.config_id << '|'
        << plan.block_size << '|' << plan.bits_per_symbol << '|'
        << std::fixed << std::setprecision(6) << plan.signal_strength << '|'
        << plan.repair_percent << '|' << plan.width << '|' << plan.height << '|'
        << plan.fps << '|' << plan.target_duration_seconds << '|'
        << plan.maximum_actual_video_size_bytes << '|'
        << plan.reserve_percent << '|' << plan.selected_chunk_payload_bytes << '|'
        << plan.parts.size();
    for (const auto &part : plan.parts)
        out << '|' << part.part_index << ':' << part.chunk_offset << ':'
            << part.chunk_size << ':' << digest_hex_lower(part.chunk_sha256);
    return out.str();
}

Id128 derive_part_id(const SetPlan &plan, const PartPlan &part) {
    std::ostringstream seed;
    seed << id_hex(plan.set_id) << '|' << part.part_index << '|'
         << part.chunk_offset << '|' << part.chunk_size << '|'
         << digest_hex_lower(part.chunk_sha256) << '|'
         << digest_hex_lower(plan.descriptor_hash);
    const auto digest = digest_bytes(seed.str());
    Id128 result{};
    std::copy_n(digest.bytes.begin(), result.size(), result.begin());
    return result;
}

std::string profile_token(const ReliabilityProfile profile) {
    return std::string(reliability_profile_definition(profile).cli_name);
}

std::string base_name_for(const std::string &filename) {
    const auto dot = filename.find_last_of('.');
    auto base = sanitize_filename(
        dot == std::string::npos ? filename : filename.substr(0, dot));
    if (base.size() > 72) base.resize(72);
    return base.empty() ? "archive" : base;
}

void validate_plan_options(const PlanOptions &options) {
    if (options.target_duration_seconds == 0)
        throw std::invalid_argument("target duration must be positive");
    if (!std::isfinite(options.reserve_percent) ||
        options.reserve_percent < 0.0 || options.reserve_percent >= 100.0)
        throw std::invalid_argument("reserve percent must be in [0, 100)");
    if (options.fps == 0)
        throw std::invalid_argument("FPS must be positive");
    if (options.forced_chunk_payload_bytes.has_value() &&
        *options.forced_chunk_payload_bytes == 0)
        throw std::invalid_argument("forced chunk size must be positive");
}

uint64_t estimate_output_bytes(const double duration_seconds) {
    const long double value = static_cast<long double>(duration_seconds) *
                              static_cast<long double>(FRAME_BITRATE) * 1000.0L / 8.0L;
    if (value >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(std::ceil(value));
}

std::optional<std::string> json_string_after(const std::string &json,
                                              const std::string &key,
                                              const std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, start);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos || json[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        if (pos >= json.size()) throw std::runtime_error("invalid JSON string escape");
        const char e = json[pos++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: throw std::runtime_error("unsupported JSON escape");
        }
    }
    throw std::runtime_error("unterminated JSON string");
}

std::optional<uint64_t> json_u64_after(const std::string &json,
                                       const std::string &key,
                                       const std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, start);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    bool quoted = json[pos] == '"';
    if (quoted) ++pos;
    const auto begin = pos;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) ++pos;
    if (begin == pos || (quoted && (pos >= json.size() || json[pos] != '"')))
        throw std::runtime_error("invalid unsigned manifest field: " + key);
    uint64_t result = 0;
    const auto conversion = std::from_chars(json.data() + begin, json.data() + pos, result);
    if (conversion.ec != std::errc{} || conversion.ptr != json.data() + pos)
        throw std::runtime_error("manifest integer overflow: " + key);
    return result;
}

std::optional<double> json_double_after(const std::string &json,
                                        const std::string &key,
                                        const std::size_t start = 0) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle, start);
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return std::nullopt;
    char *end = nullptr;
    const double value = std::strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos || !std::isfinite(value))
        throw std::runtime_error("invalid manifest number: " + key);
    return value;
}

std::string require_string(const std::string &json, const std::string &key,
                           const std::size_t start = 0) {
    auto value = json_string_after(json, key, start);
    if (!value) throw std::runtime_error("missing manifest field: " + key);
    return *value;
}

uint64_t require_u64(const std::string &json, const std::string &key,
                     const std::size_t start = 0) {
    auto value = json_u64_after(json, key, start);
    if (!value) throw std::runtime_error("missing manifest field: " + key);
    return *value;
}

std::vector<std::pair<std::size_t, std::size_t>> part_object_ranges(
    const std::string &json) {
    const auto parts_key = json.find("\"parts\"");
    if (parts_key == std::string::npos) throw std::runtime_error("missing manifest parts");
    const auto array = json.find('[', parts_key);
    if (array == std::string::npos) throw std::runtime_error("invalid manifest parts array");
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    std::size_t begin = 0;
    for (std::size_t i = array + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{') { if (depth++ == 0) begin = i; }
        else if (c == '}') {
            if (depth <= 0) throw std::runtime_error("invalid manifest part object");
            if (--depth == 0) ranges.emplace_back(begin, i + 1);
        } else if (c == ']' && depth == 0) return ranges;
    }
    throw std::runtime_error("unterminated manifest parts array");
}

void copy_file_range(std::ifstream &input, std::ofstream &output,
                     uint64_t size, picosha2::hash256_one_by_one *hasher = nullptr) {
    std::vector<unsigned char> buffer(1024 * 1024);
    while (size != 0) {
        const auto amount = static_cast<std::streamsize>(
            (std::min)(size, static_cast<uint64_t>(buffer.size())));
        input.read(reinterpret_cast<char *>(buffer.data()), amount);
        if (input.gcount() != amount) throw std::runtime_error("unexpected end of source file");
        if (hasher) hasher->process(buffer.begin(), buffer.begin() + amount);
        if (output.is_open()) {
            output.write(reinterpret_cast<const char *>(buffer.data()), amount);
            if (!output) throw std::runtime_error("could not write payload file");
        }
        size -= static_cast<uint64_t>(amount);
    }
}

Sha256Digest finished_digest(picosha2::hash256_one_by_one &hasher) {
    hasher.finish();
    std::array<unsigned char, 32> raw{};
    hasher.get_hash_bytes(raw.begin(), raw.end());
    Sha256Digest result;
    for (std::size_t i = 0; i < raw.size(); ++i)
        result.bytes[i] = static_cast<std::byte>(raw[i]);
    return result;
}

} // namespace

std::vector<std::byte> serialize_envelope(const PartEnvelopeV1 &envelope) {
    if (envelope.part_count == 0 || envelope.part_index >= envelope.part_count)
        throw std::invalid_argument("invalid Video Set part index/count");
    if (envelope.chunk_offset > envelope.original_file_size ||
        envelope.chunk_size > envelope.original_file_size - envelope.chunk_offset)
        throw std::invalid_argument("Video Set chunk range is outside source file");
    if (envelope.encoder_config_id.size() > 64)
        throw std::invalid_argument("encoder config ID is too long");
    if (envelope.original_filename.size() > kMaximumFilenameBytes ||
        !valid_utf8(envelope.original_filename))
        throw std::invalid_argument("original filename is not bounded valid UTF-8");
    const std::size_t header_size = kMinimumHeaderBytes +
        envelope.encoder_config_id.size() + envelope.original_filename.size();
    if (header_size > kMaximumHeaderBytes || header_size > std::numeric_limits<uint16_t>::max())
        throw std::invalid_argument("Video Set header is too large");

    std::vector<std::byte> out;
    out.reserve(header_size);
    append_array(out, kMagic);
    append_u16(out, kSchemaVersion);
    append_u16(out, static_cast<uint16_t>(header_size));
    append_u32(out, envelope.flags);
    append_array(out, envelope.set_id);
    append_u32(out, envelope.part_index);
    append_u32(out, envelope.part_count);
    append_array(out, envelope.part_id);
    append_u64(out, envelope.original_file_size);
    append_u64(out, envelope.chunk_offset);
    append_u64(out, envelope.chunk_size);
    append_array(out, envelope.original_file_sha256.bytes);
    append_array(out, envelope.chunk_sha256.bytes);
    append_u16(out, envelope.profile_stable_id);
    append_u16(out, envelope.block_size);
    append_u16(out, envelope.bits_per_symbol);
    append_u16(out, 0);
    append_u32(out, envelope.signal_milli);
    append_u32(out, envelope.repair_basis_points);
    append_u16(out, envelope.width);
    append_u16(out, envelope.height);
    append_u16(out, envelope.fps);
    append_u16(out, static_cast<uint16_t>(envelope.encoder_config_id.size()));
    append_u16(out, static_cast<uint16_t>(envelope.original_filename.size()));
    append_u16(out, 0);
    append_array(out, envelope.descriptor_hash.bytes);
    out.insert(out.end(), reinterpret_cast<const std::byte *>(envelope.encoder_config_id.data()),
               reinterpret_cast<const std::byte *>(envelope.encoder_config_id.data() + envelope.encoder_config_id.size()));
    out.insert(out.end(), reinterpret_cast<const std::byte *>(envelope.original_filename.data()),
               reinterpret_cast<const std::byte *>(envelope.original_filename.data() + envelope.original_filename.size()));
    append_u32(out, crc32c(out));
    return out;
}

ParseResult parse_envelope(const std::span<const std::byte> bytes) {
    ParseResult result;
    if (bytes.size() < kMagic.size() ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return result;
    result.kind = ParseKind::Invalid;
    try {
        if (bytes.size() < kMinimumHeaderBytes) throw std::runtime_error("truncated Video Set header");
        const uint16_t version = read_u16(bytes, 8);
        if (version != kSchemaVersion) throw std::runtime_error("unsupported Video Set envelope version");
        const uint16_t header_length = read_u16(bytes, 10);
        if (header_length < kMinimumHeaderBytes || header_length > kMaximumHeaderBytes)
            throw std::runtime_error("invalid Video Set header length");
        if (bytes.size() < header_length) throw std::runtime_error("truncated Video Set header");
        const auto header = bytes.first(header_length);
        const uint32_t stored_crc = read_u32(header, header_length - 4);
        if (crc32c(header.first(header_length - 4)) != stored_crc)
            throw std::runtime_error("Video Set header checksum mismatch");

        auto &e = result.envelope;
        e.header_length = header_length;
        e.header_checksum = stored_crc;
        e.flags = read_u32(header, 12);
        e.set_id = read_array<16>(header, 16);
        e.part_index = read_u32(header, 32);
        e.part_count = read_u32(header, 36);
        e.part_id = read_array<16>(header, 40);
        e.original_file_size = read_u64(header, 56);
        e.chunk_offset = read_u64(header, 64);
        e.chunk_size = read_u64(header, 72);
        e.original_file_sha256.bytes = read_array<32>(header, 80);
        e.chunk_sha256.bytes = read_array<32>(header, 112);
        e.profile_stable_id = read_u16(header, 144);
        e.block_size = read_u16(header, 146);
        e.bits_per_symbol = read_u16(header, 148);
        e.signal_milli = read_u32(header, 152);
        e.repair_basis_points = read_u32(header, 156);
        e.width = read_u16(header, 160);
        e.height = read_u16(header, 162);
        e.fps = read_u16(header, 164);
        const uint16_t config_length = read_u16(header, 166);
        const uint16_t filename_length = read_u16(header, 168);
        e.descriptor_hash.bytes = read_array<32>(header, 172);
        const std::size_t string_bytes = static_cast<std::size_t>(config_length) + filename_length;
        if (config_length > 64 || filename_length > kMaximumFilenameBytes ||
            string_bytes != header_length - kMinimumHeaderBytes)
            throw std::runtime_error("invalid Video Set string lengths");
        e.encoder_config_id.assign(
            reinterpret_cast<const char *>(header.data() + kFixedHeaderBytes), config_length);
        e.original_filename.assign(
            reinterpret_cast<const char *>(header.data() + kFixedHeaderBytes + config_length), filename_length);
        if (!valid_utf8(e.original_filename)) throw std::runtime_error("invalid UTF-8 filename in Video Set header");
        if (e.part_count == 0 || e.part_index >= e.part_count)
            throw std::runtime_error("invalid Video Set part index/count");
        if (e.chunk_offset > e.original_file_size ||
            e.chunk_size > e.original_file_size - e.chunk_offset)
            throw std::runtime_error("Video Set chunk range is outside source file");
        if (e.block_size != 4 && e.block_size != 8)
            throw std::runtime_error("unsupported Video Set block geometry");
        if (e.bits_per_symbol != 1 || e.fps == 0)
            throw std::runtime_error("unsupported Video Set encoding metadata");
        result.payload_offset = header_length;
        result.kind = ParseKind::Valid;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}

ParseResult parse_envelope_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {ParseKind::Invalid, {}, 0, "could not open logical payload"};
    std::array<std::byte, kMaximumHeaderBytes> buffer{};
    input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    return parse_envelope(std::span<const std::byte>(buffer.data(), count));
}

bool has_video_set_magic(const std::filesystem::path &path) {
    return parse_envelope_file(path).kind != ParseKind::NotVideoSet;
}

Id128 generate_set_id() {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialization failed");
    Id128 id{};
    randombytes_buf(id.data(), id.size());
    return id;
}

std::string id_hex(const Id128 &id) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : id)
        out << std::setw(2) << static_cast<unsigned>(std::to_integer<uint8_t>(value));
    return out.str();
}

Id128 id_from_hex(const std::string &hex) {
    if (hex.size() != 32) throw std::runtime_error("set/part ID must contain 32 hex characters");
    Id128 result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        unsigned value = 0;
        const auto conversion = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, value, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != hex.data() + i * 2 + 2)
            throw std::runtime_error("invalid hexadecimal set/part ID");
        result[i] = static_cast<std::byte>(value);
    }
    return result;
}

std::string sanitize_filename(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (const auto slash = value.find_last_of('/'); slash != std::string::npos)
        value.erase(0, slash + 1);
    if (!valid_utf8(value)) value = "recovered-file.bin";
    for (char &c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    if (value.empty() || value == "." || value == "..") value = "recovered-file.bin";
    const auto dot = value.find_last_of('.');
    std::string stem = dot == std::string::npos ? value : value.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const std::set<std::string> reserved{
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (reserved.contains(stem)) value = "_" + value;
    if (value.size() > kMaximumFilenameBytes) value.resize(kMaximumFilenameBytes);
    return value;
}

Sha256Digest sha256_file(const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);
    return sha256_file_range(path, 0, size);
}

Sha256Digest sha256_file_range(const std::filesystem::path &path,
                               const uint64_t offset, const uint64_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open file for SHA-256: " + path.string());
    const auto total = std::filesystem::file_size(path);
    if (offset > total || size > total - offset) throw std::runtime_error("SHA-256 range is outside file");
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) throw std::runtime_error("could not seek source file");
    picosha2::hash256_one_by_one hasher;
    std::ofstream sink;
    copy_file_range(input, sink, size, &hasher);
    return finished_digest(hasher);
}

SetPlan plan_file(const std::filesystem::path &source,
                  const PlanOptions &options) {
    validate_plan_options(options);
    if (!std::filesystem::is_regular_file(source))
        throw std::invalid_argument("Video Set input must be a regular file");
    SetPlan plan;
    plan.set_id = options.deterministic_set_id.value_or(generate_set_id());
    const auto source_name = source.filename().u8string();
    plan.original_filename = sanitize_filename(std::string(
        reinterpret_cast<const char *>(source_name.data()), source_name.size()));
    plan.original_file_size = std::filesystem::file_size(source);
    plan.profile = options.profile;
    const auto &definition = reliability_profile_definition(options.profile);
    plan.profile_name = std::string(definition.cli_name);
    plan.config_id = reliability_profile_config_id(options.profile);
    plan.block_size = definition.block_size;
    plan.bits_per_symbol = definition.bits_per_symbol;
    plan.signal_strength = definition.signal_strength;
    plan.repair_percent = definition.repair_percentage;
    plan.width = definition.width;
    plan.height = definition.height;
    plan.fps = static_cast<int>(options.fps);
    plan.target_duration_seconds = options.target_duration_seconds;
    plan.maximum_actual_video_size_bytes = options.maximum_actual_video_size_bytes;
    plan.reserve_percent = options.reserve_percent;

    ResilientVideoConfig config;
    config.width = definition.width;
    config.height = definition.height;
    config.fps = static_cast<int>(options.fps);
    config.block_size = definition.block_size;
    config.bits_per_symbol = definition.bits_per_symbol;
    config.signal_strength = definition.signal_strength;
    const uint64_t packets_per_frame = VideoEncoder::packets_per_frame(config);
    const auto reliability = reliability_options_for_profile(options.profile);
    const long double usable_duration = static_cast<long double>(options.target_duration_seconds) *
                                        (100.0L - options.reserve_percent) / 100.0L;
    const uint64_t max_frames = static_cast<uint64_t>(std::floor(usable_duration * options.fps));
    if (max_frames == 0) throw std::invalid_argument("target duration/reserve leaves no usable frames");
    const auto fits = [&](const uint64_t payload) {
        if (payload > std::numeric_limits<uint64_t>::max() - kMaximumHeaderBytes) return false;
        const auto estimate = estimate_encoding_reliability(
            payload + kMaximumHeaderBytes, false, reliability,
            packets_per_frame, options.fps);
        if (estimate.frame_count > max_frames) return false;
        if (options.maximum_actual_video_size_bytes != 0) {
            const long double reserved_cap =
                static_cast<long double>(options.maximum_actual_video_size_bytes) *
                (100.0L - options.reserve_percent) / 100.0L;
            if (static_cast<long double>(estimate_output_bytes(
                    estimate.video_duration_seconds)) > reserved_cap)
                return false;
        }
        return true;
    };
    uint64_t max_chunk = 0;
    if (options.forced_chunk_payload_bytes) {
        max_chunk = *options.forced_chunk_payload_bytes;
        if (!fits(max_chunk)) throw std::invalid_argument("forced chunk payload exceeds target duration");
    } else {
        uint64_t low = 1;
        uint64_t high = (std::max)(uint64_t{1}, plan.original_file_size);
        if (!fits(low)) throw std::invalid_argument("target duration cannot fit a Video Set envelope");
        if (fits(high)) max_chunk = high;
        else {
            while (low < high) {
                const uint64_t mid = low + (high - low + 1) / 2;
                if (fits(mid)) low = mid; else high = mid - 1;
            }
            max_chunk = low;
        }
    }
    plan.selected_chunk_payload_bytes = max_chunk;
    const uint64_t part_count_64 = plan.original_file_size == 0 ? 1 :
        plan.original_file_size / max_chunk + (plan.original_file_size % max_chunk != 0);
    if (part_count_64 > std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("Video Set needs too many parts");
    const auto count = static_cast<uint32_t>(part_count_64);
    plan.parts.reserve(count);
    const std::string base = base_name_for(plan.original_filename);
    const std::string set8 = id_hex(plan.set_id).substr(0, 8);
    for (uint32_t index = 0; index < count; ++index) {
        PartPlan part;
        part.part_index = index;
        part.chunk_offset = static_cast<uint64_t>(index) * max_chunk;
        part.chunk_size = plan.original_file_size == 0 ? 0 :
            (std::min)(max_chunk, plan.original_file_size - part.chunk_offset);
        const auto estimate = estimate_encoding_reliability(
            part.chunk_size + kMaximumHeaderBytes, false, reliability,
            packets_per_frame, options.fps);
        part.estimated_frames = estimate.frame_count;
        part.estimated_duration_seconds = estimate.video_duration_seconds;
        part.estimated_output_bytes = estimate_output_bytes(part.estimated_duration_seconds);
        std::ostringstream name;
        name << base << "_VSXSET_" << set8 << "_P" << std::setw(4)
             << std::setfill('0') << (index + 1) << "-of-" << std::setw(4)
             << count << "_pending_" << profile_token(plan.profile) << ".mkv";
        part.expected_video_filename = name.str();
        plan.parts.push_back(std::move(part));
    }
    return plan;
}

void populate_chunk_hashes(const std::filesystem::path &source, SetPlan &plan) {
    plan.original_file_sha256 = sha256_file(source);
    for (auto &part : plan.parts)
        part.chunk_sha256 = sha256_file_range(source, part.chunk_offset, part.chunk_size);
    recompute_descriptor_and_part_ids(plan);
}

void recompute_descriptor_and_part_ids(SetPlan &plan) {
    plan.descriptor_hash = digest_bytes(canonical_descriptor(plan));
    const std::string base = base_name_for(plan.original_filename);
    const std::string set8 = id_hex(plan.set_id).substr(0, 8);
    for (auto &part : plan.parts) {
        part.part_id = derive_part_id(plan, part);
        std::ostringstream name;
        name << base << "_VSXSET_" << set8 << "_P" << std::setw(4)
             << std::setfill('0') << (part.part_index + 1) << "-of-"
             << std::setw(4) << plan.parts.size() << '_' << id_hex(part.part_id).substr(0, 12)
             << '_' << profile_token(plan.profile) << ".mkv";
        part.expected_video_filename = name.str();
    }
}

PartEnvelopeV1 envelope_for_part(const SetPlan &plan, const PartPlan &part) {
    PartEnvelopeV1 envelope;
    envelope.set_id = plan.set_id;
    envelope.part_index = part.part_index;
    envelope.part_count = static_cast<uint32_t>(plan.parts.size());
    envelope.part_id = part.part_id;
    envelope.original_file_size = plan.original_file_size;
    envelope.chunk_offset = part.chunk_offset;
    envelope.chunk_size = part.chunk_size;
    envelope.original_file_sha256 = plan.original_file_sha256;
    envelope.chunk_sha256 = part.chunk_sha256;
    envelope.profile_stable_id = static_cast<uint16_t>(plan.profile);
    envelope.block_size = static_cast<uint16_t>(plan.block_size);
    envelope.bits_per_symbol = static_cast<uint16_t>(plan.bits_per_symbol);
    envelope.signal_milli = static_cast<uint32_t>(std::lround(plan.signal_strength * 1000.0));
    envelope.repair_basis_points = static_cast<uint32_t>(std::lround(plan.repair_percent * 100.0));
    envelope.width = static_cast<uint16_t>(plan.width);
    envelope.height = static_cast<uint16_t>(plan.height);
    envelope.fps = static_cast<uint16_t>(plan.fps);
    envelope.encoder_config_id = plan.config_id;
    envelope.original_filename = plan.original_filename;
    envelope.descriptor_hash = plan.descriptor_hash;
    return envelope;
}

void write_logical_payload(const std::filesystem::path &source,
                           const SetPlan &plan, const PartPlan &part,
                           const std::filesystem::path &output) {
    const auto header = serialize_envelope(envelope_for_part(plan, part));
    SafeOutputFile safe(output);
    std::ofstream out(safe.partial_path(), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("could not create logical payload");
    out.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
    std::ifstream input(source, std::ios::binary);
    if (!input) throw std::runtime_error("could not open source file");
    input.seekg(static_cast<std::streamoff>(part.chunk_offset));
    copy_file_range(input, out, part.chunk_size);
    out.close();
    if (!out) throw std::runtime_error("could not finalize logical payload");
    safe.commit();
}

bool verify_logical_payload(const std::filesystem::path &payload,
                            PartEnvelopeV1 *envelope, std::string *error) {
    const auto parsed = parse_envelope_file(payload);
    if (parsed.kind != ParseKind::Valid) {
        if (error) *error = parsed.kind == ParseKind::NotVideoSet ?
            "decoded payload is not a Video Set part" : parsed.error;
        return false;
    }
    const auto size = std::filesystem::file_size(payload);
    if (size != parsed.payload_offset + parsed.envelope.chunk_size) {
        if (error) *error = "decoded payload length does not match embedded chunk size";
        return false;
    }
    const auto chunk_hash = sha256_file_range(payload, parsed.payload_offset,
                                              parsed.envelope.chunk_size);
    if (chunk_hash != parsed.envelope.chunk_sha256) {
        if (error) *error = "decoded source chunk SHA-256 mismatch";
        return false;
    }
    if (envelope) *envelope = parsed.envelope;
    return true;
}

std::string manifest_json(const SetPlan &plan) {
    std::ostringstream out;
    out << "{\n  \"schema\": \"vidstorex.video_set\",\n"
        << "  \"version\": 1,\n"
        << "  \"created_by_version\": \"1.4.0\",\n"
        << "  \"set_id\": " << q(id_hex(plan.set_id)) << ",\n"
        << "  \"descriptor_hash\": " << q(digest_hex_lower(plan.descriptor_hash)) << ",\n"
        << "  \"original\": {\"filename\": " << q(plan.original_filename)
        << ", \"size\": " << plan.original_file_size << ", \"sha256\": "
        << q(digest_hex_lower(plan.original_file_sha256)) << "},\n"
        << "  \"profile\": {\"name\": " << q(plan.profile_name)
        << ", \"stable_id\": " << static_cast<int>(plan.profile)
        << ", \"block_size\": " << plan.block_size
        << ", \"bits_per_symbol\": " << plan.bits_per_symbol
        << ", \"signal_strength\": " << std::fixed << std::setprecision(3)
        << plan.signal_strength << ", \"repair_percent\": " << plan.repair_percent
        << ", \"width\": " << plan.width << ", \"height\": " << plan.height
        << ", \"fps\": " << plan.fps << ", \"config_id\": " << q(plan.config_id) << "},\n"
        << "  \"split_policy\": {\"target_duration_seconds\": "
        << plan.target_duration_seconds << ", \"max_actual_video_size_bytes\": "
        << plan.maximum_actual_video_size_bytes << ", \"reserve_percent\": "
        << plan.reserve_percent << ", \"selected_chunk_payload_bytes\": "
        << plan.selected_chunk_payload_bytes << ", \"adaptive_retry_count\": "
        << plan.adaptive_retry_count << "},\n"
        << "  \"part_count\": " << plan.parts.size() << ",\n  \"parts\": [\n";
    for (std::size_t i = 0; i < plan.parts.size(); ++i) {
        const auto &p = plan.parts[i];
        out << "    {\"part_index\": " << p.part_index
            << ", \"display_part_number\": " << p.part_index + 1
            << ", \"part_id\": " << q(id_hex(p.part_id))
            << ", \"chunk_offset\": " << p.chunk_offset
            << ", \"chunk_size\": " << p.chunk_size
            << ", \"chunk_sha256\": " << q(digest_hex_lower(p.chunk_sha256))
            << ", \"expected_video_filename\": " << q(p.expected_video_filename)
            << ", \"estimated_frame_count\": " << p.estimated_frames
            << ", \"estimated_duration_seconds\": " << p.estimated_duration_seconds
            << ", \"estimated_output_size\": " << p.estimated_output_bytes
            << ", \"actual_frame_count\": " << p.actual_frames
            << ", \"actual_duration_seconds\": " << p.actual_duration_seconds
            << ", \"actual_output_size\": " << p.actual_output_bytes
            << ", \"video_sha256\": " << q(p.video_sha256)
            << ", \"local_encode_state\": " << q(p.local_encode_state)
            << ", \"local_decode_verification_state\": " << q(p.local_decode_state)
            << ", \"upload_state\": " << q(p.upload_state)
            << ", \"returned_video_path\": " << q(p.returned_video_path)
            << ", \"recovered_state\": " << q(p.recovered_state)
            << ", \"notes\": " << q(p.notes) << "}";
        if (i + 1 != plan.parts.size()) out << ',';
        out << '\n';
    }
    out << "  ],\n  \"aggregate_state\": " << q(plan.aggregate_state)
        << ",\n  \"final_output_path\": " << q(plan.final_output_path) << "\n}\n";
    return out.str();
}

void write_manifest_atomic(const std::filesystem::path &path,
                           const SetPlan &plan) {
    std::filesystem::create_directories(path.parent_path());
    const auto backup = path.string() + ".bak";
    if (std::filesystem::exists(path)) {
        std::error_code ignored;
        std::filesystem::copy_file(path, backup,
            std::filesystem::copy_options::overwrite_existing, ignored);
    }
    SafeOutputFile safe(path);
    std::ofstream out(safe.partial_path(), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("could not create Video Set manifest");
    out << manifest_json(plan);
    out.close();
    if (!out) throw std::runtime_error("could not write Video Set manifest");
    safe.commit();
}

SetPlan read_manifest(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open Video Set manifest");
    std::string json((std::istreambuf_iterator<char>(input)), {});
    if (require_string(json, "schema") != "vidstorex.video_set")
        throw std::runtime_error("unsupported manifest schema");
    if (require_u64(json, "version") != 1)
        throw std::runtime_error("unsupported Video Set manifest major version");
    SetPlan plan;
    plan.set_id = id_from_hex(require_string(json, "set_id"));
    plan.descriptor_hash = digest_from_hex(require_string(json, "descriptor_hash"));
    const auto original_at = json.find("\"original\"");
    const auto profile_at = json.find("\"profile\"");
    const auto split_at = json.find("\"split_policy\"");
    if (original_at == std::string::npos || profile_at == std::string::npos || split_at == std::string::npos)
        throw std::runtime_error("manifest is missing required sections");
    plan.original_filename = sanitize_filename(require_string(json, "filename", original_at));
    plan.original_file_size = require_u64(json, "size", original_at);
    plan.original_file_sha256 = digest_from_hex(require_string(json, "sha256", original_at));
    plan.profile_name = require_string(json, "name", profile_at);
    plan.profile = reliability_profile_from_id(static_cast<int>(require_u64(json, "stable_id", profile_at)));
    plan.block_size = static_cast<int>(require_u64(json, "block_size", profile_at));
    plan.bits_per_symbol = static_cast<int>(require_u64(json, "bits_per_symbol", profile_at));
    plan.signal_strength = json_double_after(json, "signal_strength", profile_at).value_or(1.0);
    plan.repair_percent = json_double_after(json, "repair_percent", profile_at).value_or(5.0);
    plan.width = static_cast<int>(require_u64(json, "width", profile_at));
    plan.height = static_cast<int>(require_u64(json, "height", profile_at));
    plan.fps = static_cast<int>(require_u64(json, "fps", profile_at));
    plan.config_id = require_string(json, "config_id", profile_at);
    plan.target_duration_seconds = require_u64(json, "target_duration_seconds", split_at);
    plan.maximum_actual_video_size_bytes = require_u64(json, "max_actual_video_size_bytes", split_at);
    plan.reserve_percent = json_double_after(json, "reserve_percent", split_at).value_or(kDefaultReservePercent);
    plan.selected_chunk_payload_bytes = require_u64(json, "selected_chunk_payload_bytes", split_at);
    plan.adaptive_retry_count = static_cast<uint32_t>(require_u64(json, "adaptive_retry_count", split_at));
    const uint64_t expected_count = require_u64(json, "part_count");
    const auto ranges = part_object_ranges(json);
    if (ranges.size() != expected_count) throw std::runtime_error("manifest part_count does not match parts array");
    std::set<uint32_t> indices;
    for (const auto &[begin, end] : ranges) {
        const std::string object = json.substr(begin, end - begin);
        PartPlan p;
        p.part_index = static_cast<uint32_t>(require_u64(object, "part_index"));
        if (p.part_index >= expected_count || !indices.insert(p.part_index).second)
            throw std::runtime_error("duplicate or out-of-range manifest part index");
        p.part_id = id_from_hex(require_string(object, "part_id"));
        p.chunk_offset = require_u64(object, "chunk_offset");
        p.chunk_size = require_u64(object, "chunk_size");
        if (p.chunk_offset > plan.original_file_size ||
            p.chunk_size > plan.original_file_size - p.chunk_offset)
            throw std::runtime_error("manifest chunk range is outside source file");
        p.chunk_sha256 = digest_from_hex(require_string(object, "chunk_sha256"));
        p.expected_video_filename = require_string(object, "expected_video_filename");
        p.estimated_frames = require_u64(object, "estimated_frame_count");
        p.estimated_duration_seconds = json_double_after(object, "estimated_duration_seconds").value_or(0);
        p.estimated_output_bytes = require_u64(object, "estimated_output_size");
        p.actual_frames = require_u64(object, "actual_frame_count");
        p.actual_duration_seconds = json_double_after(object, "actual_duration_seconds").value_or(0);
        p.actual_output_bytes = require_u64(object, "actual_output_size");
        p.video_sha256 = require_string(object, "video_sha256");
        p.local_encode_state = require_string(object, "local_encode_state");
        p.local_decode_state = require_string(object, "local_decode_verification_state");
        p.upload_state = require_string(object, "upload_state");
        p.returned_video_path = require_string(object, "returned_video_path");
        p.recovered_state = require_string(object, "recovered_state");
        p.notes = require_string(object, "notes");
        plan.parts.push_back(std::move(p));
    }
    std::sort(plan.parts.begin(), plan.parts.end(),
              [](const PartPlan &a, const PartPlan &b) { return a.part_index < b.part_index; });
    uint64_t next = 0;
    for (const auto &p : plan.parts) {
        if (p.chunk_offset != next) throw std::runtime_error("manifest has a gap or overlap");
        next += p.chunk_size;
    }
    if (next != plan.original_file_size) throw std::runtime_error("manifest ranges do not cover source exactly");
    if (digest_bytes(canonical_descriptor(plan)) != plan.descriptor_hash)
        throw std::runtime_error("manifest descriptor hash mismatch");
    plan.aggregate_state = json_string_after(json, "aggregate_state").value_or("Planned");
    plan.final_output_path = json_string_after(json, "final_output_path").value_or("");
    return plan;
}

std::string infer_status(const SetPlan &plan,
                         const std::span<const uint32_t> missing,
                         const std::span<const uint32_t> conflicts,
                         const std::span<const uint32_t> corrupt,
                         const bool recovering, const bool recovered_exact,
                         const bool global_hash_failed) {
    if (recovered_exact) return "Recovered exact";
    if (global_hash_failed) return "Failed global SHA validation";
    if (!conflicts.empty()) return "Conflict detected";
    if (!corrupt.empty()) return "Corrupt parts detected";
    if (!missing.empty()) return "Incomplete: missing parts";
    if (recovering) return "Recovering";
    const bool verified = !plan.parts.empty() && std::all_of(plan.parts.begin(), plan.parts.end(),
        [](const PartPlan &p) { return p.local_decode_state == "Exact"; });
    if (verified) return "Locally verified";
    const bool encoding = std::any_of(plan.parts.begin(), plan.parts.end(),
        [](const PartPlan &p) { return p.local_encode_state == "Encoding" || p.local_encode_state == "Encoded"; });
    return encoding ? "Encoding" : "Planned";
}

void write_reports(const std::filesystem::path &set_root, const SetPlan &plan,
                   const std::vector<uint32_t> &missing,
                   const std::vector<uint32_t> &duplicates,
                   const std::vector<uint32_t> &conflicts,
                   const std::vector<uint32_t> &corrupt) {
    const auto reports = set_root / "reports";
    std::filesystem::create_directories(reports);
    const std::string status = infer_status(plan, missing, conflicts, corrupt,
        false, plan.aggregate_state == "Recovered exact",
        plan.aggregate_state == "Failed global SHA validation");
    {
        SafeOutputFile safe(reports / "set_report.md");
        std::ofstream out(safe.partial_path());
        out << "# Reliquary Video Set report\n\n"
            << "- Status: **" << status << "**\n- Set ID: `" << id_hex(plan.set_id)
            << "`\n- Source: `" << plan.original_filename << "` (" << plan.original_file_size
            << " bytes)\n- Source SHA-256: `" << digest_hex_lower(plan.original_file_sha256)
            << "`\n- Profile/config: " << plan.profile_name << " / `" << plan.config_id
            << "`\n- Split policy: " << plan.target_duration_seconds << " s, "
            << plan.maximum_actual_video_size_bytes << " byte cap, " << plan.reserve_percent
            << "% reserve\n- Parts: " << plan.parts.size() << "\n\n"
            << "|Part|Offset|Bytes|Chunk SHA-256|Video bytes|Local verification|Recovery|\n"
            << "|---:|---:|---:|---|---:|---|---|\n";
        for (const auto &p : plan.parts)
            out << '|' << p.part_index + 1 << '|' << p.chunk_offset << '|' << p.chunk_size
                << "|`" << digest_hex_lower(p.chunk_sha256) << "`|" << p.actual_output_bytes
                << '|' << p.local_decode_state << '|' << p.recovered_state << "|\n";
        const auto list = [&](const char *label, const std::vector<uint32_t> &values) {
            out << "\n- " << label << ": ";
            if (values.empty()) out << "none";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) out << ", "; out << values[i] + 1;
            }
        };
        list("Missing", missing); list("Duplicates", duplicates);
        list("Conflicts", conflicts); list("Corrupt", corrupt); out << '\n';
        out.close(); safe.commit();
    }
    {
        SafeOutputFile safe(reports / "set_summary.json");
        std::ofstream out(safe.partial_path());
        out << "{\"schema\":\"vidstorex.video_set.summary\",\"version\":1,"
            << "\"set_id\":" << q(id_hex(plan.set_id)) << ",\"status\":" << q(status)
            << ",\"part_count\":" << plan.parts.size() << ",\"missing_count\":" << missing.size()
            << ",\"duplicate_count\":" << duplicates.size() << ",\"conflict_count\":" << conflicts.size()
            << ",\"corrupt_count\":" << corrupt.size() << "}\n";
        out.close(); safe.commit();
    }
    {
        SafeOutputFile safe(reports / "set_parts.csv");
        std::ofstream out(safe.partial_path());
        out << "part_index,part_id,offset,chunk_size,chunk_sha256,video,actual_size,local_verification,recovery\n";
        for (const auto &p : plan.parts)
            out << p.part_index << ',' << id_hex(p.part_id) << ',' << p.chunk_offset << ','
                << p.chunk_size << ',' << digest_hex_lower(p.chunk_sha256) << ','
                << q(p.expected_video_filename) << ',' << p.actual_output_bytes << ','
                << q(p.local_decode_state) << ',' << q(p.recovered_state) << '\n';
        out.close(); safe.commit();
    }
}

void write_manual_workflow_files(const std::filesystem::path &set_root,
                                 const SetPlan &plan,
                                 const uint32_t upload_batch_size) {
    const uint32_t batch = upload_batch_size == 0 ? 10 : upload_batch_size;
    std::filesystem::create_directories(set_root / "tools");
    std::filesystem::create_directories(set_root / "returned");
    {
        SafeOutputFile safe(set_root / "upload_checklist.md");
        std::ofstream out(safe.partial_path());
        out << "# Upload checklist\n\nSet ID: `" << id_hex(plan.set_id) << "`  \nSource: `"
            << plan.original_filename << "` / `" << digest_hex_lower(plan.original_file_sha256).substr(0, 16)
            << "`  \nProfile/config: " << plan.profile_name << " / `" << plan.config_id
            << "`  \nPart count: " << plan.parts.size()
            << "\n\nUpload as **Unlisted**, wait for 1080p processing, then download YouTube's re-encoded video-only stream.\n\n"
            << "|Part|Batch|Part ID|Chunk SHA|Video|Bytes|Seconds|Suggested title|\n|---:|---:|---|---|---|---:|---:|---|\n";
        for (const auto &p : plan.parts)
            out << '|' << p.part_index + 1 << '|' << p.part_index / batch + 1 << "|`"
                << id_hex(p.part_id).substr(0, 12) << "`|`" << digest_hex_lower(p.chunk_sha256).substr(0, 16)
                << "`|`videos/" << p.expected_video_filename << "`|" << p.actual_output_bytes
                << '|' << p.actual_duration_seconds << "|Reliquary " << id_hex(plan.set_id).substr(0, 8)
                << " part " << p.part_index + 1 << " of " << plan.parts.size() << "|\n";
        out.close(); safe.commit();
    }
    {
        SafeOutputFile safe(set_root / "upload_checklist.csv");
        std::ofstream out(safe.partial_path());
        out << "set_id,part,batch,part_id,chunk_sha256,video,actual_bytes,duration_seconds,suggested_title\n";
        for (const auto &p : plan.parts)
            out << id_hex(plan.set_id) << ',' << p.part_index + 1 << ',' << p.part_index / batch + 1 << ','
                << id_hex(p.part_id) << ',' << digest_hex_lower(p.chunk_sha256) << ',' << q(p.expected_video_filename)
                << ',' << p.actual_output_bytes << ',' << p.actual_duration_seconds << ','
                << q("Reliquary " + id_hex(plan.set_id).substr(0, 8) + " part " +
                     std::to_string(p.part_index + 1) + " of " + std::to_string(plan.parts.size())) << '\n';
        out.close(); safe.commit();
    }
    {
        SafeOutputFile safe(set_root / "README_NEXT_STEPS.md");
        std::ofstream out(safe.partial_path());
        out << "# Next steps\n\n1. Upload the videos in batches as Unlisted.\n2. Wait until every video has finished 1080p processing.\n3. Add the videos to a playlist.\n4. Run `tools/download_returned_playlist.ps1` with the playlist URL.\n5. Keep downloads in `returned/`.\n6. Run `media_storage set-status` or `set-recover`.\n7. Re-download only missing or corrupt parts.\n8. Trust recovery only after **Recovered exact** full-file SHA-256 validation.\n\nFilenames and playlist order are never used as part identity.\n";
        out.close(); safe.commit();
    }
    {
        SafeOutputFile safe(set_root / "tools" / "download_returned_playlist.ps1");
        std::ofstream out(safe.partial_path());
        out << "[CmdletBinding()]\nparam(\n  [Parameter(Mandatory=$true)][string]$PlaylistUrl,\n  [Parameter(Mandatory=$true)][string]$OutputDirectory\n)\n"
            << "$ErrorActionPreference = 'Stop'\n$ytDlp = Get-Command yt-dlp -ErrorAction SilentlyContinue\n"
            << "if (-not $ytDlp) { throw 'yt-dlp was not found on PATH. Install yt-dlp and retry.' }\n"
            << "New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null\n"
            << "$log = Join-Path $OutputDirectory 'download_returned_playlist.log'\n"
            << "$template = Join-Path $OutputDirectory '%(title)s [%(id)s].%(ext)s'\n"
            << "& $ytDlp.Source '--no-part' '--format' 'bestvideo[height=1080]/bestvideo[height<=1080]' '--output' $template $PlaylistUrl 2>&1 | Tee-Object -FilePath $log\n"
            << "if ($LASTEXITCODE -ne 0) { throw \"yt-dlp failed with exit code $LASTEXITCODE\" }\n"
            << "Write-Output \"Downloads complete. Recovery reads embedded Video Set metadata, not names or playlist order. Log: $log\"\n";
        out.close(); safe.commit();
    }
}

} // namespace video_set
