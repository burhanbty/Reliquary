#include "youtube_test_lab.h"

#include "chunker.h"
#include "decoder.h"
#include "encoder.h"
#include "encoding_preflight.h"
#include "encoding_reliability.h"
#include "integrity.h"
#include "libs/picosha2.h"
#include "safe_output.h"
#include "video_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <variant>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace youtube_test_lab {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint64_t KiB = 1024;
constexpr uint64_t MiB = 1024 * KiB;

std::string iso_timestamp() {
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

std::string iso_timestamp_from_time_t(const std::time_t value) {
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

std::string modified_time_utc(const std::filesystem::path &path) {
    std::error_code error;
    const auto file_time =
        std::filesystem::last_write_time(path, error);
    if (error) return {};
    const auto system_time =
        std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(
            file_time - decltype(file_time)::clock::now() +
            std::chrono::system_clock::now());
    return iso_timestamp_from_time_t(
        std::chrono::system_clock::to_time_t(system_time));
}

std::string created_time_utc(const std::filesystem::path &path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(
            path.c_str(), GetFileExInfoStandard, &data))
        return {};
    ULARGE_INTEGER ticks{};
    ticks.LowPart = data.ftCreationTime.dwLowDateTime;
    ticks.HighPart = data.ftCreationTime.dwHighDateTime;
    constexpr uint64_t windows_to_unix_ticks =
        116444736000000000ULL;
    if (ticks.QuadPart < windows_to_unix_ticks) return {};
    const std::time_t value = static_cast<std::time_t>(
        (ticks.QuadPart - windows_to_unix_ticks) / 10000000ULL);
    return iso_timestamp_from_time_t(value);
#else
    (void) path;
    return {};
#endif
}

std::string compact_timestamp() {
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

std::string create_record_id(const std::string &prefix) {
    static std::atomic<uint64_t> sequence{0};
    std::ostringstream out;
    out << prefix << "-" << compact_timestamp() << "-"
        << std::hex << std::setw(6) << std::setfill('0')
        << (sequence.fetch_add(1) & 0xffffff);
    return out.str();
}

std::string lowercase(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

std::string json_escape(const std::string &value) {
    std::ostringstream out;
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0') << static_cast<int>(c)
                        << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string q(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}

class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    using Value = std::variant<std::nullptr_t, bool, double,
                               std::string, Object, Array>;
    Value value = nullptr;

    Json() = default;
    explicit Json(Value v) : value(std::move(v)) {}

    [[nodiscard]] const Object &object() const {
        if (!std::holds_alternative<Object>(value))
            throw std::runtime_error("Expected JSON object");
        return std::get<Object>(value);
    }
    [[nodiscard]] const Array &array() const {
        if (!std::holds_alternative<Array>(value))
            throw std::runtime_error("Expected JSON array");
        return std::get<Array>(value);
    }
    [[nodiscard]] const Json &at(const std::string &key) const {
        const auto &obj = object();
        const auto it = obj.find(key);
        if (it == obj.end())
            throw std::runtime_error("Missing JSON field: " + key);
        return it->second;
    }
    [[nodiscard]] std::string string() const {
        if (!std::holds_alternative<std::string>(value))
            throw std::runtime_error("Expected JSON string");
        return std::get<std::string>(value);
    }
    [[nodiscard]] double number() const {
        if (!std::holds_alternative<double>(value))
            throw std::runtime_error("Expected JSON number");
        return std::get<double>(value);
    }
    [[nodiscard]] uint64_t u64() const {
        const double n = number();
        if (!std::isfinite(n) || n < 0.0 ||
            n > static_cast<double>(
                    std::numeric_limits<uint64_t>::max()) ||
            std::floor(n) != n)
            throw std::runtime_error("Invalid unsigned JSON integer");
        return static_cast<uint64_t>(n);
    }
    [[nodiscard]] int integer() const {
        const double n = number();
        if (!std::isfinite(n) ||
            n < std::numeric_limits<int>::min() ||
            n > std::numeric_limits<int>::max() ||
            std::floor(n) != n)
            throw std::runtime_error("Invalid JSON integer");
        return static_cast<int>(n);
    }
    [[nodiscard]] bool boolean() const {
        if (!std::holds_alternative<bool>(value))
            throw std::runtime_error("Expected JSON boolean");
        return std::get<bool>(value);
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    Json parse() {
        skip();
        Json result = value();
        skip();
        if (pos_ != text_.size())
            fail("Unexpected trailing data");
        return result;
    }

private:
    std::string text_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error(
            "Invalid manifest JSON at byte " +
            std::to_string(pos_) + ": " + message);
    }
    void skip() {
        while (pos_ < text_.size() &&
               std::isspace(
                   static_cast<unsigned char>(text_[pos_])))
            ++pos_;
    }
    bool consume(const char c) {
        skip();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    Json value() {
        skip();
        if (pos_ >= text_.size()) fail("Unexpected end of input");
        switch (text_[pos_]) {
            case '{': return object();
            case '[': return array();
            case '"': return Json(string());
            case 't':
                literal("true");
                return Json(true);
            case 'f':
                literal("false");
                return Json(false);
            case 'n':
                literal("null");
                return Json(nullptr);
            default: return Json(number());
        }
    }
    void literal(const std::string &expected) {
        if (text_.substr(pos_, expected.size()) != expected)
            fail("Invalid literal");
        pos_ += expected.size();
    }
    Json object() {
        if (!consume('{')) fail("Expected object");
        Json::Object out;
        skip();
        if (consume('}')) return Json(std::move(out));
        while (true) {
            skip();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                fail("Expected object key");
            std::string key = string();
            if (!consume(':')) fail("Expected ':'");
            if (!out.emplace(std::move(key), value()).second)
                fail("Duplicate object key");
            if (consume('}')) break;
            if (!consume(',')) fail("Expected ','");
        }
        return Json(std::move(out));
    }
    Json array() {
        if (!consume('[')) fail("Expected array");
        Json::Array out;
        skip();
        if (consume(']')) return Json(std::move(out));
        while (true) {
            out.push_back(value());
            if (consume(']')) break;
            if (!consume(',')) fail("Expected ','");
        }
        return Json(std::move(out));
    }
    std::string string() {
        if (!consume('"')) fail("Expected string");
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20)
                    fail("Control character in string");
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) fail("Bad escape");
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size())
                        fail("Short unicode escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code += h - '0';
                        else if (h >= 'a' && h <= 'f')
                            code += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            code += h - 'A' + 10;
                        else fail("Bad unicode escape");
                    }
                    if (code <= 0x7f) out.push_back(
                        static_cast<char>(code));
                    else if (code <= 0x7ff) {
                        out.push_back(
                            static_cast<char>(0xc0 | code >> 6));
                        out.push_back(static_cast<char>(
                            0x80 | code & 0x3f));
                    } else {
                        out.push_back(static_cast<char>(
                            0xe0 | code >> 12));
                        out.push_back(static_cast<char>(
                            0x80 | code >> 6 & 0x3f));
                        out.push_back(static_cast<char>(
                            0x80 | code & 0x3f));
                    }
                    break;
                }
                default: fail("Unknown escape");
            }
        }
        fail("Unterminated string");
    }
    double number() {
        skip();
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() &&
               std::isdigit(
                   static_cast<unsigned char>(text_[pos_])))
            ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        if (pos_ < text_.size() &&
            (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() &&
                (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        if (start == pos_) fail("Expected value");
        const std::string token = text_.substr(start, pos_ - start);
        char *end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(parsed))
            fail("Invalid number");
        return parsed;
    }
};

DataType data_type_from_string(const std::string &value) {
    if (value == "compressible") return DataType::Compressible;
    if (value == "random") return DataType::Random;
    if (value == "existing-file") return DataType::ExistingFile;
    throw std::runtime_error("Unknown data type: " + value);
}

CaseState case_state_from_string(const std::string &value) {
    if (value == "Pending") return CaseState::Pending;
    if (value == "Generating") return CaseState::Generating;
    if (value == "Generated") return CaseState::Generated;
    if (value == "Simulating") return CaseState::Simulating;
    if (value == "Waiting for manual upload")
        return CaseState::WaitingForManualUpload;
    if (value == "Imported") return CaseState::Imported;
    if (value == "Analyzed") return CaseState::Analyzed;
    if (value == "Failed") return CaseState::Failed;
    if (value == "Skipped") return CaseState::Skipped;
    throw std::runtime_error("Unknown case state: " + value);
}

ResultSource source_from_string(const std::string &value) {
    if (value == "Local simulation")
        return ResultSource::LocalSimulation;
    if (value == "Real YouTube roundtrip")
        return ResultSource::RealYouTubeRoundtrip;
    throw std::runtime_error("Unknown result source: " + value);
}

FinalStatus status_from_string(const std::string &value) {
    if (value == "Pass — SHA-256 exact match")
        return FinalStatus::Pass;
    if (value == "Recoverable but incomplete")
        return FinalStatus::RecoverableIncomplete;
    if (value == "Decode failed")
        return FinalStatus::DecodeFailed;
    if (value == "Header not found")
        return FinalStatus::HeaderNotFound;
    if (value == "Insufficient packets")
        return FinalStatus::InsufficientPackets;
    if (value == "Corrupt output")
        return FinalStatus::CorruptOutput;
    if (value == "Wrong test case")
        return FinalStatus::WrongTestCase;
    if (value == "Unsupported processed video")
        return FinalStatus::UnsupportedProcessedVideo;
    throw std::runtime_error("Unknown final status: " + value);
}

std::string size_token(const uint64_t bytes) {
    if (bytes % MiB == 0)
        return std::to_string(bytes / MiB) + "MiB";
    if (bytes % KiB == 0)
        return std::to_string(bytes / KiB) + "KiB";
    return std::to_string(bytes) + "B";
}

std::string resolution_token(const int width, const int height) {
    if (width == 1920 && height == 1080) return "1080p";
    if (width == 2560 && height == 1440) return "1440p";
    if (width == 3840 && height == 2160) return "2160p";
    return std::to_string(width) + "x" + std::to_string(height);
}

std::string profile_for_repair(const double repair) {
    if (std::abs(repair - 5.0) < 0.0001) return "Local / Fast";
    if (std::abs(repair - 20.0) < 0.0001) return "Balanced";
    if (std::abs(repair - 50.0) < 0.0001) return "Durable";
    return "Custom";
}

std::filesystem::path suite_root_from_manifest(
    const std::filesystem::path &manifest_path) {
    return std::filesystem::absolute(manifest_path).parent_path();
}

std::filesystem::path resolve_suite_path(
    const std::filesystem::path &suite_root,
    const std::string &relative) {
    const std::filesystem::path rel(relative);
    if (rel.is_absolute())
        throw std::runtime_error(
            "Manifest contains a non-portable absolute path");
    const auto root = std::filesystem::weakly_canonical(suite_root);
    const auto lexical = (root / rel).lexically_normal();
    const auto canonical_parent =
        std::filesystem::weakly_canonical(lexical.parent_path());
    const auto result =
        canonical_parent / lexical.filename();
    const auto [root_end, result_end] =
        std::mismatch(root.begin(), root.end(), result.begin(), result.end());
    if (root_end != root.end())
        throw std::runtime_error(
            "Manifest path escapes the suite directory");
    return result;
}

std::string normalized_relative(
    const std::filesystem::path &path) {
    return path.generic_string();
}

uint64_t xorshift64star(uint64_t &state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}

uint64_t stable_seed(const std::string &value, uint64_t seed) {
    // FNV-1a provides a stable, platform-independent seed derivation. This
    // is reproducibility metadata, not a cryptographic primitive.
    uint64_t hash = 1469598103934665603ULL ^ seed;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? kDefaultPayloadSeed : hash;
}

std::string source_type_for(
    const ResultSource source,
    const std::string &simulation_profile) {
    if (source == ResultSource::RealYouTubeRoundtrip)
        return "real-youtube-roundtrip";
    if (simulation_profile == "master-lossless")
        return "master-lossless";
    if (simulation_profile == "youtube-upload-candidate")
        return "youtube-upload-candidate";
    return "local-simulation";
}

std::string duplicate_key(
    const std::string &suite_id,
    const std::string &case_id,
    const TestResult &result) {
    std::string file_identity = result.source_file_sha256;
    if (file_identity.empty()) {
        file_identity =
            lowercase(result.analyzed_video) + "|" +
            std::to_string(result.source_file_size != 0
                ? result.source_file_size
                : result.downloaded_video_size) + "|" +
            result.video.codec + "|" +
            std::to_string(result.video.width) + "x" +
            std::to_string(result.video.height) + "|" +
            std::to_string(result.decoded_frame_count) + "|" +
            (result.sha256_match ? "match" : "mismatch") + "|" +
            to_string(result.final_status);
    }
    return suite_id + "|" + case_id + "|" +
        (result.source_type.empty()
            ? source_type_for(
                  result.source, result.simulation_profile)
            : result.source_type) + "|" +
        file_identity + "|" +
        (result.analysis_fingerprint.empty()
            ? "vidstorex-legacy-decoder"
            : result.analysis_fingerprint);
}

int result_completeness(const TestResult &result) {
    int score = 0;
    score += !result.observation_id.empty();
    score += !result.source_file_sha256.empty();
    score += !result.analyzed_at_utc.empty();
    score += !result.video.codec.empty();
    score += !result.video.pixel_format.empty();
    score += result.video.duration_seconds > 0.0;
    score += result.decoded_frame_count > 0;
    score += !result.restored_sha256.empty();
    score += result.decode_completed;
    score += result.sha256_match;
    return score;
}

bool invoke_progress(const ProgressCallback &callback,
                     const Progress &progress) {
    return !callback || callback(progress);
}

struct EncodeStats {
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    uint64_t total_packets = 0;
    uint64_t frames = 0;
    double seconds = 0.0;
};

EncodeStats encode_master(
    const std::filesystem::path &input,
    const std::filesystem::path &output,
    const double repair_percentage,
    const ResilientVideoConfig &config,
    const std::function<bool(double)> &case_progress = {}) {
    const auto started = Clock::now();
    const EncodingReliabilityOptions reliability{
        repair_percentage_to_ratio(repair_percentage)};
    FileChunkReader reader(input.string().c_str());
    const auto chunk_count = reader.num_chunks();
    Encoder encoder(make_encoding_file_id(), HashAlgorithm::CRC32,
                    reliability);
    SafeOutputFile safe(output);
    EncodeStats result;
    {
        VideoEncoder video(safe.partial_path().string(), config);
        for (std::size_t i = 0; i < chunk_count; ++i) {
            if (case_progress &&
                !case_progress(chunk_count == 0 ? 1.0 :
                    static_cast<double>(i) / chunk_count))
                throw std::runtime_error("Cancelled");
            const auto [packets, entry] = encoder.encode_chunk(
                static_cast<uint32_t>(i), reader.chunk_view(i),
                i + 1 == chunk_count, false);
            result.source_packets += entry.N;
            result.repair_packets +=
                packets.size() - static_cast<std::size_t>(entry.N);
            result.total_packets += packets.size();
            video.encode_packets(packets);
        }
        video.finalize();
        result.frames =
            static_cast<uint64_t>(video.frames_written());
    }
    safe.commit();
    result.seconds = std::chrono::duration<double>(
        Clock::now() - started).count();
    return result;
}

std::string av_error(const int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

void throw_av(const int code, const std::string &what) {
    if (code < 0)
        throw std::runtime_error(
            what + ": " + av_error(code));
}

void transcode_h264(
    const std::filesystem::path &input,
    const std::filesystem::path &output,
    const SimulationProfile &profile,
    const std::string &suite_id,
    const std::string &case_id,
    const uint64_t minimum_frames = 0,
    const double minimum_duration_seconds = 0.0) {
    AVFormatContext *input_format = nullptr;
    AVFormatContext *output_format = nullptr;
    AVCodecContext *decoder = nullptr;
    AVCodecContext *encoder = nullptr;
    AVFrame *decoded = nullptr;
    AVFrame *converted = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *sws = nullptr;
    SafeOutputFile safe(output);

    const auto cleanup = [&] {
        if (packet) av_packet_free(&packet);
        if (converted) av_frame_free(&converted);
        if (decoded) av_frame_free(&decoded);
        if (sws) sws_freeContext(sws);
        if (encoder) avcodec_free_context(&encoder);
        if (decoder) avcodec_free_context(&decoder);
        if (output_format) {
            if (output_format->pb) avio_closep(&output_format->pb);
            avformat_free_context(output_format);
        }
        if (input_format) avformat_close_input(&input_format);
    };

    try {
        throw_av(avformat_open_input(
                     &input_format, input.string().c_str(),
                     nullptr, nullptr),
                 "Could not open master video");
        throw_av(avformat_find_stream_info(input_format, nullptr),
                 "Could not read master stream info");
        const int stream_index = av_find_best_stream(
            input_format, AVMEDIA_TYPE_VIDEO, -1, -1,
            nullptr, 0);
        throw_av(stream_index, "No video stream");
        AVStream *in_stream = input_format->streams[stream_index];
        const AVCodec *decoder_codec = avcodec_find_decoder(
            in_stream->codecpar->codec_id);
        if (!decoder_codec)
            throw std::runtime_error("No decoder for master video");
        decoder = avcodec_alloc_context3(decoder_codec);
        if (!decoder) throw std::bad_alloc();
        throw_av(avcodec_parameters_to_context(
                     decoder, in_stream->codecpar),
                 "Could not copy decoder parameters");
        throw_av(avcodec_open2(decoder, decoder_codec, nullptr),
                 "Could not open decoder");

        throw_av(avformat_alloc_output_context2(
                     &output_format, nullptr, "mp4",
                     safe.partial_path().string().c_str()),
                 "Could not create MP4 output");
        const AVCodec *encoder_codec =
            avcodec_find_encoder_by_name(profile.codec.c_str());
        if (!encoder_codec)
            encoder_codec =
                avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!encoder_codec)
            throw std::runtime_error("H.264 encoder unavailable");
        AVStream *out_stream =
            avformat_new_stream(output_format, nullptr);
        if (!out_stream) throw std::bad_alloc();
        encoder = avcodec_alloc_context3(encoder_codec);
        if (!encoder) throw std::bad_alloc();
        encoder->width =
            profile.scale ? profile.width : decoder->width;
        encoder->height =
            profile.scale ? profile.height : decoder->height;
        encoder->pix_fmt = AV_PIX_FMT_YUV420P;
        encoder->time_base = {1, profile.fps};
        encoder->framerate = {profile.fps, 1};
        encoder->gop_size = profile.gop;
        // B-frames are intentionally disabled for upload candidates so
        // packet PTS and DTS remain monotonic and straightforward to audit.
        encoder->max_b_frames = 0;
        if (output_format->oformat->flags & AVFMT_GLOBALHEADER)
            encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *codec_options = nullptr;
        av_dict_set(&codec_options, "preset",
                    profile.preset.c_str(), 0);
        av_dict_set(&codec_options, "crf",
                    std::to_string(profile.crf).c_str(), 0);
        av_dict_set(&codec_options, "profile", "high", 0);
        throw_av(avcodec_open2(
                     encoder, encoder_codec, &codec_options),
                 "Could not open H.264 encoder");
        av_dict_free(&codec_options);
        throw_av(avcodec_parameters_from_context(
                     out_stream->codecpar, encoder),
                 "Could not copy encoder parameters");
        out_stream->time_base = {1, profile.fps};
        out_stream->avg_frame_rate = {profile.fps, 1};

        AVDictionary *metadata = nullptr;
        const std::string comment =
            "VidStoreX YouTube Test Lab suite=" + suite_id +
            " case=" + case_id;
        av_dict_set(&metadata, "comment", comment.c_str(), 0);
        output_format->metadata = metadata;
        throw_av(avio_open(&output_format->pb,
                           safe.partial_path().string().c_str(),
                           AVIO_FLAG_WRITE),
                 "Could not open candidate output");
        AVDictionary *mux_options = nullptr;
        av_dict_set(&mux_options, "movflags", "+faststart", 0);
        throw_av(avformat_write_header(
                     output_format, &mux_options),
                 "Could not write MP4 header");
        av_dict_free(&mux_options);

        decoded = av_frame_alloc();
        converted = av_frame_alloc();
        packet = av_packet_alloc();
        if (!decoded || !converted || !packet)
            throw std::bad_alloc();
        converted->format = encoder->pix_fmt;
        converted->width = encoder->width;
        converted->height = encoder->height;
        throw_av(av_frame_get_buffer(converted, 32),
                 "Could not allocate converted frame");
        sws = sws_getContext(
            decoder->width, decoder->height, decoder->pix_fmt,
            encoder->width, encoder->height, encoder->pix_fmt,
            profile.scale ? SWS_LANCZOS : SWS_POINT,
            nullptr, nullptr, nullptr);
        if (!sws)
            throw std::runtime_error(
                "Could not create color/scale converter");

        int64_t pts = 0;
        auto write_encoder_packets = [&] {
            while (true) {
                const int received =
                    avcodec_receive_packet(encoder, packet);
                if (received == AVERROR(EAGAIN) ||
                    received == AVERROR_EOF)
                    return;
                throw_av(received,
                         "Could not receive H.264 packet");
                if (packet->pts == AV_NOPTS_VALUE ||
                    packet->dts == AV_NOPTS_VALUE)
                    throw std::runtime_error(
                        "H.264 encoder returned missing timestamps");
                if (packet->duration <= 0)
                    packet->duration = 1;
                av_packet_rescale_ts(
                    packet, encoder->time_base,
                    out_stream->time_base);
                packet->stream_index = out_stream->index;
                throw_av(av_interleaved_write_frame(
                             output_format, packet),
                         "Could not write H.264 packet");
                av_packet_unref(packet);
            }
        };
        auto convert_and_encode = [&] {
            throw_av(av_frame_make_writable(converted),
                     "Converted frame is not writable");
            sws_scale(sws, decoded->data, decoded->linesize, 0,
                      decoder->height, converted->data,
                      converted->linesize);
            converted->pts = pts++;
            converted->duration = 1;
            throw_av(avcodec_send_frame(encoder, converted),
                     "Could not send converted frame");
            write_encoder_packets();
        };

        while (av_read_frame(input_format, packet) >= 0) {
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            const int sent = avcodec_send_packet(decoder, packet);
            av_packet_unref(packet);
            throw_av(sent, "Could not send master packet");
            while (true) {
                const int received =
                    avcodec_receive_frame(decoder, decoded);
                if (received == AVERROR(EAGAIN) ||
                    received == AVERROR_EOF)
                    break;
                throw_av(received,
                         "Could not decode master frame");
                convert_and_encode();
                av_frame_unref(decoded);
            }
        }
        throw_av(avcodec_send_packet(decoder, nullptr),
                 "Could not drain master decoder");
        while (true) {
            const int received =
                avcodec_receive_frame(decoder, decoded);
            if (received == AVERROR_EOF ||
                received == AVERROR(EAGAIN))
                break;
            throw_av(received, "Could not drain master frame");
            convert_and_encode();
            av_frame_unref(decoded);
        }
        throw_av(avcodec_send_frame(encoder, nullptr),
                 "Could not drain H.264 encoder");
        write_encoder_packets();
        throw_av(av_write_trailer(output_format),
                 "Could not write MP4 trailer");
        avio_closep(&output_format->pb);
        if (minimum_frames > 0) {
            const ResilientVideoConfig expected{
                .width = encoder->width,
                .height = encoder->height,
                .fps = profile.fps};
            const auto validation = validate_upload_candidate(
                safe.partial_path(), expected, minimum_frames,
                minimum_duration_seconds);
            if (!validation.passed)
                throw std::runtime_error(
                    "Upload candidate validation failed: " +
                    validation.error);
        }
        safe.commit();
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
}

void write_video_info(std::ostream &out,
                      const VideoTechnicalInfo &video,
                      const std::string &indent) {
    out << "{\n"
        << indent << "  \"container\": " << q(video.container) << ",\n"
        << indent << "  \"codec\": " << q(video.codec) << ",\n"
        << indent << "  \"profile\": " << q(video.profile) << ",\n"
        << indent << "  \"codec_tag\": "
        << q(video.codec_tag) << ",\n"
        << indent << "  \"pixel_format\": "
        << q(video.pixel_format) << ",\n"
        << indent << "  \"width\": " << video.width << ",\n"
        << indent << "  \"height\": " << video.height << ",\n"
        << indent << "  \"fps\": " << video.fps << ",\n"
        << indent << "  \"frame_count\": " << video.frame_count << ",\n"
        << indent << "  \"duration_seconds\": "
        << video.duration_seconds << ",\n"
        << indent << "  \"bitrate\": " << video.bitrate << ",\n"
        << indent << "  \"stream_bitrate\": "
        << video.stream_bitrate << ",\n"
        << indent << "  \"container_bitrate\": "
        << video.container_bitrate << ",\n"
        << indent << "  \"calculated_bitrate\": "
        << video.calculated_bitrate << ",\n"
        << indent << "  \"bitrate_source\": "
        << q(video.bitrate_source) << ",\n"
        << indent << "  \"display_aspect_ratio\": "
        << q(video.display_aspect_ratio) << ",\n"
        << indent << "  \"time_base\": "
        << q(video.time_base) << ",\n"
        << indent << "  \"file_size\": " << video.file_size << "\n"
        << indent << "}";
}

void write_telemetry(std::ostream &out,
                     const PacketRecoveryTelemetry &t,
                     const std::string &indent) {
    out << "{\n"
        << indent << "  \"frames_read\": " << t.frames_read << ",\n"
        << indent << "  \"frames_with_pattern\": "
        << t.frames_with_pattern << ",\n"
        << indent << "  \"extracted_packets\": "
        << t.extracted_packets << ",\n"
        << indent << "  \"valid_packets\": " << t.valid_packets << ",\n"
        << indent << "  \"invalid_packets\": "
        << t.invalid_packets << ",\n"
        << indent << "  \"duplicate_packets\": "
        << t.duplicate_packets << ",\n"
        << indent << "  \"source_packets\": "
        << t.source_packets << ",\n"
        << indent << "  \"repair_packets\": "
        << t.repair_packets << ",\n"
        << indent << "  \"recovered_chunks\": "
        << t.recovered_chunks << ",\n"
        << indent << "  \"missing_chunks\": "
        << t.missing_chunks << ",\n"
        << indent << "  \"required_packet_threshold\": "
        << t.required_packet_threshold << ",\n"
        << indent << "  \"failure_reason\": "
        << q(t.failure_reason) << "\n"
        << indent << "}";
}

void write_result(std::ostream &out, const TestResult &result,
                  const std::string &indent) {
    out << "{\n"
        << indent << "  \"observation_id\": "
        << q(result.observation_id) << ",\n"
        << indent << "  \"analysis_session_id\": "
        << q(result.analysis_session_id) << ",\n"
        << indent << "  \"suite_id\": "
        << q(result.suite_id) << ",\n"
        << indent << "  \"case_id\": "
        << q(result.case_id) << ",\n"
        << indent << "  \"source_type\": "
        << q(result.source_type) << ",\n"
        << indent << "  \"analyzed_at_utc\": "
        << q(result.analyzed_at_utc) << ",\n"
        << indent << "  \"imported_at_utc\": "
        << q(result.imported_at_utc) << ",\n"
        << indent << "  \"source_file_relative_name\": "
        << q(result.source_file_relative_name) << ",\n"
        << indent << "  \"source_file_size\": "
        << result.source_file_size << ",\n"
        << indent << "  \"source_file_sha256\": "
        << q(result.source_file_sha256) << ",\n"
        << indent << "  \"source_file_created_time_utc\": "
        << q(result.source_file_created_time_utc) << ",\n"
        << indent << "  \"source_file_modified_time_utc\": "
        << q(result.source_file_modified_time_utc) << ",\n"
        << indent << "  \"analysis_fingerprint\": "
        << q(result.analysis_fingerprint) << ",\n"
        << indent << "  \"restored_sha256\": "
        << q(result.restored_sha256) << ",\n"
        << indent << "  \"vidstorex_version\": "
        << q(result.vidstorex_version) << ",\n"
        << indent << "  \"test_case_id\": "
        << q(result.test_case_id) << ",\n"
        << indent << "  \"analyzed_video\": "
        << q(result.analyzed_video) << ",\n"
        << indent << "  \"source\": "
        << q(to_string(result.source)) << ",\n"
        << indent << "  \"simulation_profile\": "
        << q(result.simulation_profile) << ",\n"
        << indent << "  \"video\": ";
    write_video_info(out, result.video, indent + "  ");
    out << ",\n"
        << indent << "  \"downloaded_video_size\": "
        << result.downloaded_video_size << ",\n"
        << indent << "  \"decoded_frame_count\": "
        << result.decoded_frame_count << ",\n"
        << indent << "  \"telemetry\": ";
    write_telemetry(out, result.telemetry, indent + "  ");
    out << ",\n"
        << indent << "  \"packet_recovery_percentage\": "
        << result.packet_recovery_percentage << ",\n"
        << indent << "  \"frame_difference\": "
        << result.frame_difference << ",\n"
        << indent << "  \"decode_completed\": "
        << (result.decode_completed ? "true" : "false") << ",\n"
        << indent << "  \"sha256_match\": "
        << (result.sha256_match ? "true" : "false") << ",\n"
        << indent << "  \"failure_stage\": "
        << q(result.failure_stage) << ",\n"
        << indent << "  \"error_message\": "
        << q(result.error_message) << ",\n"
        << indent << "  \"elapsed_transcode_seconds\": "
        << result.elapsed_transcode_seconds << ",\n"
        << indent << "  \"elapsed_decode_seconds\": "
        << result.elapsed_decode_seconds << ",\n"
        << indent << "  \"final_status\": "
        << q(to_string(result.final_status)) << "\n"
        << indent << "}";
}

void write_case(std::ostream &out, const TestCase &c,
                const std::string &indent) {
    out << "{\n"
        << indent << "  \"test_suite_id\": " << q(c.test_suite_id) << ",\n"
        << indent << "  \"test_case_id\": " << q(c.test_case_id) << ",\n"
        << indent << "  \"created_at\": " << q(c.created_at) << ",\n"
        << indent << "  \"vidstorex_version\": "
        << q(c.vidstorex_version) << ",\n"
        << indent << "  \"encoding_mode\": " << q(c.encoding_mode) << ",\n"
        << indent << "  \"reliability_profile\": "
        << q(c.reliability_profile) << ",\n"
        << indent << "  \"repair_percentage\": "
        << c.repair_percentage << ",\n"
        << indent << "  \"input_data_type\": "
        << q(to_string(c.input_data_type)) << ",\n"
        << indent << "  \"input_size\": " << c.input_size << ",\n"
        << indent << "  \"requested_input_size\": "
        << c.requested_input_size << ",\n"
        << indent << "  \"effective_input_size\": "
        << c.effective_input_size << ",\n"
        << indent << "  \"minimum_duration_seconds\": "
        << c.minimum_duration_seconds << ",\n"
        << indent << "  \"minimum_required_frames\": "
        << c.minimum_required_frames << ",\n"
        << indent << "  \"expected_encoded_frames\": "
        << c.expected_encoded_frames << ",\n"
        << indent << "  \"actual_master_frames\": "
        << c.actual_master_frames << ",\n"
        << indent << "  \"actual_candidate_frames\": "
        << c.actual_candidate_frames << ",\n"
        << indent << "  \"master_duration_seconds\": "
        << c.master_duration_seconds << ",\n"
        << indent << "  \"candidate_duration_seconds\": "
        << c.candidate_duration_seconds << ",\n"
        << indent << "  \"payload_extended_for_duration\": "
        << (c.payload_extended_for_duration ? "true" : "false")
        << ",\n"
        << indent << "  \"payload_extension_seed\": "
        << q(std::to_string(c.payload_extension_seed)) << ",\n"
        << indent << "  \"payload_extension_version\": "
        << q(c.payload_extension_version) << ",\n"
        << indent << "  \"payload_seed\": "
        << q(std::to_string(c.payload_seed)) << ",\n"
        << indent << "  \"input_sha256\": " << q(c.input_sha256) << ",\n"
        << indent << "  \"frame_payload_capacity\": "
        << c.frame_payload_capacity << ",\n"
        << indent << "  \"source_packet_count\": "
        << c.source_packet_count << ",\n"
        << indent << "  \"repair_packet_count\": "
        << c.repair_packet_count << ",\n"
        << indent << "  \"total_packet_count\": "
        << c.total_packet_count << ",\n"
        << indent << "  \"encoded_frame_count\": "
        << c.encoded_frame_count << ",\n"
        << indent << "  \"video\": {\n"
        << indent << "    \"width\": " << c.video.width << ",\n"
        << indent << "    \"height\": " << c.video.height << ",\n"
        << indent << "    \"fps\": " << c.video.fps << ",\n"
        << indent << "    \"codec\": " << q(c.video.codec) << ",\n"
        << indent << "    \"container\": " << q(c.video.container)
        << ",\n"
        << indent << "    \"explicit_frame_duration\": "
        << (c.video.explicit_frame_duration ? "true" : "false")
        << "\n"
        << indent << "  },\n"
        << indent << "  \"block_size\": " << c.block_size << ",\n"
        << indent << "  \"bits_per_block\": "
        << c.bits_per_block << ",\n"
        << indent << "  \"coefficient_strength\": "
        << c.coefficient_strength << ",\n"
        << indent << "  \"payload_path\": " << q(c.payload_path) << ",\n"
        << indent << "  \"master_video_path\": "
        << q(c.master_video_path) << ",\n"
        << indent << "  \"master_video_sha256\": "
        << q(c.master_video_sha256) << ",\n"
        << indent << "  \"master_video_size\": "
        << c.master_video_size << ",\n"
        << indent << "  \"master_encode_seconds\": "
        << c.master_encode_seconds << ",\n"
        << indent << "  \"upload_candidate_path\": "
        << q(c.upload_candidate_path) << ",\n"
        << indent << "  \"upload_candidate_sha256\": "
        << q(c.upload_candidate_sha256) << ",\n"
        << indent << "  \"upload_candidate_size\": "
        << c.upload_candidate_size << ",\n"
        << indent << "  \"upload_candidate_transcode_seconds\": "
        << c.upload_candidate_transcode_seconds << ",\n"
        << indent << "  \"expected_output_filename\": "
        << q(c.expected_output_filename) << ",\n"
        << indent << "  \"master_decode_success\": "
        << (c.master_decode_success ? "true" : "false") << ",\n"
        << indent << "  \"upload_candidate_decode_success\": "
        << (c.upload_candidate_decode_success ? "true" : "false") << ",\n"
        << indent << "  \"upload_candidate_sha256_match\": "
        << (c.upload_candidate_sha256_match ? "true" : "false") << ",\n"
        << indent << "  \"candidate_duration_validation_known\": "
        << (c.candidate_duration_validation_known ? "true" : "false")
        << ",\n"
        << indent << "  \"candidate_timestamps_valid\": "
        << (c.candidate_timestamps_valid ? "true" : "false") << ",\n"
        << indent << "  \"candidate_validation_error\": "
        << q(c.candidate_validation_error) << ",\n"
        << indent << "  \"candidate_ready_for_youtube\": "
        << (c.candidate_ready_for_youtube ? "true" : "false") << ",\n"
        << indent << "  \"processing_state\": "
        << q(to_string(c.state)) << ",\n"
        << indent << "  \"notes\": " << q(c.notes) << ",\n"
        << indent << "  \"results\": [";
    if (!c.results.empty()) out << "\n";
    for (std::size_t i = 0; i < c.results.size(); ++i) {
        out << indent << "    ";
        write_result(out, c.results[i], indent + "    ");
        if (i + 1 != c.results.size()) out << ",";
        out << "\n";
    }
    out << indent << "  ]\n" << indent << "}";
}

VideoTechnicalInfo parse_video_info(const Json &j) {
    VideoTechnicalInfo v;
    v.container = j.at("container").string();
    v.codec = j.at("codec").string();
    v.profile = j.at("profile").string();
    if (const auto it = j.object().find("codec_tag");
        it != j.object().end())
        v.codec_tag = it->second.string();
    v.pixel_format = j.at("pixel_format").string();
    v.width = j.at("width").integer();
    v.height = j.at("height").integer();
    v.fps = j.at("fps").number();
    v.frame_count = static_cast<int64_t>(
        j.at("frame_count").number());
    v.duration_seconds = j.at("duration_seconds").number();
    v.bitrate = static_cast<int64_t>(j.at("bitrate").number());
    if (const auto it = j.object().find("stream_bitrate");
        it != j.object().end())
        v.stream_bitrate =
            static_cast<int64_t>(it->second.number());
    if (const auto it = j.object().find("container_bitrate");
        it != j.object().end())
        v.container_bitrate =
            static_cast<int64_t>(it->second.number());
    if (const auto it = j.object().find("calculated_bitrate");
        it != j.object().end())
        v.calculated_bitrate =
            static_cast<int64_t>(it->second.number());
    if (const auto it = j.object().find("bitrate_source");
        it != j.object().end())
        v.bitrate_source = it->second.string();
    if (const auto it =
            j.object().find("display_aspect_ratio");
        it != j.object().end())
        v.display_aspect_ratio = it->second.string();
    if (const auto it = j.object().find("time_base");
        it != j.object().end())
        v.time_base = it->second.string();
    v.file_size = j.at("file_size").u64();
    if (v.stream_bitrate == 0 && v.bitrate > 0)
        v.stream_bitrate = v.bitrate;
    if (v.bitrate_source == "unavailable" &&
        v.stream_bitrate > 0)
        v.bitrate_source = "reported_stream";
    return v;
}

PacketRecoveryTelemetry parse_telemetry(const Json &j) {
    PacketRecoveryTelemetry t;
    t.frames_read = j.at("frames_read").u64();
    t.frames_with_pattern = j.at("frames_with_pattern").u64();
    t.extracted_packets = j.at("extracted_packets").u64();
    t.valid_packets = j.at("valid_packets").u64();
    t.invalid_packets = j.at("invalid_packets").u64();
    t.duplicate_packets = j.at("duplicate_packets").u64();
    t.source_packets = j.at("source_packets").u64();
    t.repair_packets = j.at("repair_packets").u64();
    t.recovered_chunks = j.at("recovered_chunks").u64();
    t.missing_chunks = j.at("missing_chunks").u64();
    t.required_packet_threshold =
        j.at("required_packet_threshold").u64();
    t.failure_reason = j.at("failure_reason").string();
    return t;
}

TestResult parse_result(const Json &j) {
    TestResult r;
    if (const auto it = j.object().find("observation_id");
        it != j.object().end())
        r.observation_id = it->second.string();
    if (const auto it = j.object().find("analysis_session_id");
        it != j.object().end())
        r.analysis_session_id = it->second.string();
    if (const auto it = j.object().find("suite_id");
        it != j.object().end())
        r.suite_id = it->second.string();
    if (const auto it = j.object().find("case_id");
        it != j.object().end())
        r.case_id = it->second.string();
    if (const auto it = j.object().find("source_type");
        it != j.object().end())
        r.source_type = it->second.string();
    if (const auto it = j.object().find("analyzed_at_utc");
        it != j.object().end())
        r.analyzed_at_utc = it->second.string();
    if (const auto it = j.object().find("imported_at_utc");
        it != j.object().end())
        r.imported_at_utc = it->second.string();
    if (const auto it =
            j.object().find("source_file_relative_name");
        it != j.object().end())
        r.source_file_relative_name = it->second.string();
    if (const auto it = j.object().find("source_file_size");
        it != j.object().end())
        r.source_file_size = it->second.u64();
    if (const auto it = j.object().find("source_file_sha256");
        it != j.object().end())
        r.source_file_sha256 = it->second.string();
    if (const auto it =
            j.object().find("source_file_created_time_utc");
        it != j.object().end())
        r.source_file_created_time_utc = it->second.string();
    if (const auto it =
            j.object().find("source_file_modified_time_utc");
        it != j.object().end())
        r.source_file_modified_time_utc = it->second.string();
    if (const auto it = j.object().find("analysis_fingerprint");
        it != j.object().end())
        r.analysis_fingerprint = it->second.string();
    else
        r.analysis_fingerprint = "vidstorex-legacy-decoder";
    if (const auto it = j.object().find("restored_sha256");
        it != j.object().end())
        r.restored_sha256 = it->second.string();
    if (const auto it = j.object().find("vidstorex_version");
        it != j.object().end())
        r.vidstorex_version = it->second.string();
    r.test_case_id = j.at("test_case_id").string();
    r.analyzed_video = j.at("analyzed_video").string();
    r.source = source_from_string(j.at("source").string());
    r.simulation_profile = j.at("simulation_profile").string();
    r.video = parse_video_info(j.at("video"));
    r.downloaded_video_size =
        j.at("downloaded_video_size").u64();
    r.decoded_frame_count =
        j.at("decoded_frame_count").u64();
    r.telemetry = parse_telemetry(j.at("telemetry"));
    r.packet_recovery_percentage =
        j.at("packet_recovery_percentage").number();
    r.frame_difference =
        static_cast<int64_t>(j.at("frame_difference").number());
    r.decode_completed = j.at("decode_completed").boolean();
    r.sha256_match = j.at("sha256_match").boolean();
    r.failure_stage = j.at("failure_stage").string();
    r.error_message = j.at("error_message").string();
    if (const auto it =
            j.object().find("elapsed_transcode_seconds");
        it != j.object().end())
        r.elapsed_transcode_seconds = it->second.number();
    r.elapsed_decode_seconds =
        j.at("elapsed_decode_seconds").number();
    r.final_status =
        status_from_string(j.at("final_status").string());
    return r;
}

TestCase parse_case(const Json &j) {
    TestCase c;
    c.test_suite_id = j.at("test_suite_id").string();
    c.test_case_id = j.at("test_case_id").string();
    c.created_at = j.at("created_at").string();
    c.vidstorex_version = j.at("vidstorex_version").string();
    c.encoding_mode = j.at("encoding_mode").string();
    if (c.encoding_mode != "resilient")
        throw std::runtime_error(
            "Fast Local is not designed for lossy YouTube processing.");
    c.reliability_profile =
        j.at("reliability_profile").string();
    c.repair_percentage = j.at("repair_percentage").number();
    c.input_data_type =
        data_type_from_string(j.at("input_data_type").string());
    c.input_size = j.at("input_size").u64();
    c.requested_input_size = c.input_size;
    c.effective_input_size = c.input_size;
    if (const auto it = j.object().find("requested_input_size");
        it != j.object().end())
        c.requested_input_size = it->second.u64();
    if (const auto it = j.object().find("effective_input_size");
        it != j.object().end())
        c.effective_input_size = it->second.u64();
    c.input_size = c.effective_input_size;
    if (const auto it = j.object().find("minimum_duration_seconds");
        it != j.object().end())
        c.minimum_duration_seconds = it->second.number();
    if (const auto it = j.object().find("minimum_required_frames");
        it != j.object().end())
        c.minimum_required_frames = it->second.u64();
    if (const auto it = j.object().find("expected_encoded_frames");
        it != j.object().end())
        c.expected_encoded_frames = it->second.u64();
    if (const auto it = j.object().find("actual_master_frames");
        it != j.object().end())
        c.actual_master_frames = it->second.u64();
    if (const auto it = j.object().find("actual_candidate_frames");
        it != j.object().end())
        c.actual_candidate_frames = it->second.u64();
    if (const auto it = j.object().find("master_duration_seconds");
        it != j.object().end())
        c.master_duration_seconds = it->second.number();
    if (const auto it = j.object().find("candidate_duration_seconds");
        it != j.object().end())
        c.candidate_duration_seconds = it->second.number();
    if (const auto it =
            j.object().find("payload_extended_for_duration");
        it != j.object().end())
        c.payload_extended_for_duration = it->second.boolean();
    if (const auto it = j.object().find("payload_extension_seed");
        it != j.object().end())
        c.payload_extension_seed =
            std::stoull(it->second.string());
    if (const auto it =
            j.object().find("payload_extension_version");
        it != j.object().end())
        c.payload_extension_version = it->second.string();
    c.payload_seed = std::stoull(j.at("payload_seed").string());
    c.input_sha256 = j.at("input_sha256").string();
    c.source_packet_count = j.at("source_packet_count").u64();
    c.repair_packet_count = j.at("repair_packet_count").u64();
    c.total_packet_count = j.at("total_packet_count").u64();
    c.encoded_frame_count = j.at("encoded_frame_count").u64();
    if (c.actual_master_frames == 0)
        c.actual_master_frames = c.encoded_frame_count;
    if (c.expected_encoded_frames == 0)
        c.expected_encoded_frames = c.encoded_frame_count;
    const auto &video = j.at("video");
    c.video.width = video.at("width").integer();
    c.video.height = video.at("height").integer();
    c.video.fps = video.at("fps").integer();
    c.video.codec = video.at("codec").string();
    c.video.container = video.at("container").string();
    if (const auto it =
            video.object().find("explicit_frame_duration");
        it != video.object().end())
        c.video.explicit_frame_duration =
            it->second.boolean();
    if (!c.video.valid())
        throw std::runtime_error(
            "Invalid resilient video configuration in manifest");
    c.frame_payload_capacity = static_cast<uint64_t>(
        VideoEncoder::packets_per_frame(c.video));
    if (const auto it = j.object().find("frame_payload_capacity");
        it != j.object().end())
        c.frame_payload_capacity = it->second.u64();
    if (c.payload_extension_seed == 0)
        c.payload_extension_seed = stable_seed(
            c.test_suite_id + "|" + c.test_case_id + "|" +
                c.payload_extension_version,
            c.payload_seed);
    c.block_size = j.at("block_size").integer();
    c.bits_per_block = j.at("bits_per_block").integer();
    c.coefficient_strength =
        j.at("coefficient_strength").number();
    c.payload_path = j.at("payload_path").string();
    c.master_video_path = j.at("master_video_path").string();
    c.master_video_sha256 =
        j.at("master_video_sha256").string();
    c.master_video_size = j.at("master_video_size").u64();
    if (const auto it = j.object().find("master_encode_seconds");
        it != j.object().end())
        c.master_encode_seconds = it->second.number();
    c.upload_candidate_path =
        j.at("upload_candidate_path").string();
    c.upload_candidate_sha256 =
        j.at("upload_candidate_sha256").string();
    c.upload_candidate_size =
        j.at("upload_candidate_size").u64();
    if (const auto it =
            j.object().find("upload_candidate_transcode_seconds");
        it != j.object().end())
        c.upload_candidate_transcode_seconds = it->second.number();
    c.expected_output_filename =
        j.at("expected_output_filename").string();
    c.master_decode_success =
        j.at("master_decode_success").boolean();
    c.upload_candidate_decode_success =
        j.at("upload_candidate_decode_success").boolean();
    c.upload_candidate_sha256_match =
        j.at("upload_candidate_sha256_match").boolean();
    if (const auto it =
            j.object().find("candidate_duration_validation_known");
        it != j.object().end())
        c.candidate_duration_validation_known =
            it->second.boolean();
    if (const auto it =
            j.object().find("candidate_timestamps_valid");
        it != j.object().end())
        c.candidate_timestamps_valid = it->second.boolean();
    if (const auto it =
            j.object().find("candidate_validation_error");
        it != j.object().end())
        c.candidate_validation_error = it->second.string();
    const bool stored_ready =
        j.at("candidate_ready_for_youtube").boolean();
    c.candidate_ready_for_youtube =
        c.candidate_duration_validation_known && stored_ready;
    if (!c.candidate_duration_validation_known)
        c.candidate_validation_error =
            "Duration validation unknown; regenerate candidate";
    c.state =
        case_state_from_string(j.at("processing_state").string());
    c.notes = j.at("notes").string();
    for (const Json &r : j.at("results").array())
        c.results.push_back(parse_result(r));
    return c;
}

std::string csv_escape(const std::string &value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

std::string human_size(uint64_t bytes) {
    const char *units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
        << value << " " << units[unit];
    return out.str();
}

void ensure_suite_directories(const std::filesystem::path &root) {
    std::filesystem::create_directories(root / "cases");
    std::filesystem::create_directories(root / "generated");
    std::filesystem::create_directories(root / "imported");
    std::filesystem::create_directories(root / "restored");
    std::filesystem::create_directories(root / "reports");
}

std::filesystem::path backup_manifest(
    const std::filesystem::path &manifest_path,
    const std::string &reason) {
    const std::filesystem::path backup =
        manifest_path.parent_path() /
        ("manifest." + reason + "." + compact_timestamp() +
         ".json");
    SafeOutputFile safe(backup);
    std::filesystem::copy_file(
        manifest_path, safe.partial_path(),
        std::filesystem::copy_options::overwrite_existing);
    safe.commit();
    return backup;
}

std::optional<std::filesystem::path> backup_before_migration(
    const std::filesystem::path &manifest_path,
    const SuiteManifest &manifest) {
    if (manifest.loaded_schema_version >=
        kManifestSchemaVersion)
        return std::nullopt;
    return backup_manifest(manifest_path, "pre-v3-migration");
}

void recompute_session_counts(SuiteManifest &manifest) {
    for (auto &session : manifest.analysis_sessions)
        session.observation_count = 0;
    for (const auto &test_case : manifest.cases)
        for (const auto &result : test_case.results)
            for (auto &session : manifest.analysis_sessions)
                if (session.analysis_session_id ==
                    result.analysis_session_id) {
                    ++session.observation_count;
                    break;
                }
}

} // namespace

std::string to_string(const DataType value) {
    switch (value) {
        case DataType::Compressible: return "compressible";
        case DataType::Random: return "random";
        case DataType::ExistingFile: return "existing-file";
    }
    return "unknown";
}

std::string to_string(const CaseState value) {
    switch (value) {
        case CaseState::Pending: return "Pending";
        case CaseState::Generating: return "Generating";
        case CaseState::Generated: return "Generated";
        case CaseState::Simulating: return "Simulating";
        case CaseState::WaitingForManualUpload:
            return "Waiting for manual upload";
        case CaseState::Imported: return "Imported";
        case CaseState::Analyzed: return "Analyzed";
        case CaseState::Failed: return "Failed";
        case CaseState::Skipped: return "Skipped";
    }
    return "Failed";
}

std::string to_string(const ResultSource value) {
    return value == ResultSource::LocalSimulation
        ? "Local simulation" : "Real YouTube roundtrip";
}

std::string to_string(const FinalStatus value) {
    switch (value) {
        case FinalStatus::Pass:
            return "Pass — SHA-256 exact match";
        case FinalStatus::RecoverableIncomplete:
            return "Recoverable but incomplete";
        case FinalStatus::DecodeFailed: return "Decode failed";
        case FinalStatus::HeaderNotFound: return "Header not found";
        case FinalStatus::InsufficientPackets:
            return "Insufficient packets";
        case FinalStatus::CorruptOutput: return "Corrupt output";
        case FinalStatus::WrongTestCase: return "Wrong test case";
        case FinalStatus::UnsupportedProcessedVideo:
            return "Unsupported processed video";
    }
    return "Decode failed";
}

MatrixOptions quick_matrix() {
    return {
        .repair_percentages = {5.0, 20.0, 50.0},
        .input_sizes = {64 * KiB},
        .data_types = {DataType::Random},
        .input_variants = {{DataType::Random, 64 * KiB}},
        .resolutions = {{1920, 1080}, {3840, 2160}},
        .fps = FRAME_FPS,
        .minimum_upload_duration_seconds =
            kMinimumUploadDurationSeconds
    };
}

MatrixOptions full_matrix() {
    return {
        .repair_percentages = {5.0, 20.0, 50.0},
        .input_sizes = {64 * KiB, 256 * KiB, MiB},
        .data_types = {DataType::Compressible, DataType::Random},
        .input_variants = {
            {DataType::Compressible, 64 * KiB},
            {DataType::Random, 64 * KiB},
            {DataType::Random, 256 * KiB},
            {DataType::Random, MiB}},
        .resolutions = {
            {FRAME_WIDTH, FRAME_HEIGHT},
            {1920, 1080}, {2560, 1440}, {3840, 2160}},
        .fps = FRAME_FPS,
        .minimum_upload_duration_seconds =
            kMinimumUploadDurationSeconds
    };
}

uint64_t minimum_frames_for_duration(const double duration_seconds,
                                     const double fps) {
    if (!std::isfinite(duration_seconds) || duration_seconds < 0.0)
        throw std::invalid_argument(
            "minimum upload duration must be finite and non-negative");
    if (!std::isfinite(fps) || fps <= 0.0)
        throw std::invalid_argument(
            "frame rate must be finite and positive");
    const long double frames =
        std::ceil(static_cast<long double>(duration_seconds) *
                  static_cast<long double>(fps));
    if (frames >
        static_cast<long double>(std::numeric_limits<uint64_t>::max()))
        throw std::overflow_error("minimum frame count overflow");
    return static_cast<uint64_t>(frames);
}

uint64_t minimum_payload_size_for_frames(
    const uint64_t requested_size,
    const ResilientVideoConfig &video,
    const double repair_percentage,
    const uint64_t minimum_frames) {
    if (requested_size == 0 ||
        requested_size > kMaximumPayloadBytes)
        throw std::invalid_argument(
            "requested payload size must be 1..64 MiB");
    if (!video.valid())
        throw std::invalid_argument("invalid video configuration");
    const auto frame_count = [&](const uint64_t size) {
        return estimate_encoding_reliability(
            size, false,
            {repair_percentage_to_ratio(repair_percentage)},
            static_cast<uint64_t>(
                VideoEncoder::packets_per_frame(video)),
            static_cast<uint32_t>(video.fps)).frame_count;
    };
    if (frame_count(requested_size) >= minimum_frames)
        return requested_size;
    if (frame_count(kMaximumPayloadBytes) < minimum_frames)
        throw std::overflow_error(
            "64 MiB payload safety limit cannot satisfy minimum frames");
    uint64_t low = requested_size;
    uint64_t high = kMaximumPayloadBytes;
    while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        if (frame_count(middle) >= minimum_frames)
            high = middle;
        else
            low = middle + 1;
    }
    return low;
}

std::vector<TestCase> build_matrix(
    const MatrixOptions &options, const std::string &suite_id) {
    if (suite_id.empty())
        throw std::invalid_argument("Suite ID cannot be empty");
    if (options.fps <= 0 || options.repair_percentages.empty() ||
        (options.input_variants.empty() &&
         (options.input_sizes.empty() || options.data_types.empty())) ||
        options.resolutions.empty())
        throw std::invalid_argument("Test matrix cannot be empty");
    if (!std::isfinite(options.minimum_upload_duration_seconds) ||
        options.minimum_upload_duration_seconds <
            kMinimumUploadDurationSeconds)
        throw std::invalid_argument(
            "minimum upload duration must be at least 2 seconds");
    const uint64_t minimum_frames = minimum_frames_for_duration(
        options.minimum_upload_duration_seconds, options.fps);
    std::set<std::string> unique;
    std::vector<TestCase> cases;
    uint64_t sequence = 0;
    std::vector<std::pair<DataType, uint64_t>> variants =
        options.input_variants;
    if (variants.empty())
        for (const DataType type : options.data_types)
            for (const uint64_t size : options.input_sizes)
                variants.emplace_back(type, size);
    std::vector<std::pair<int, int>> resolutions;
    for (const auto resolution : options.resolutions)
        if (std::find(resolutions.begin(), resolutions.end(),
                      resolution) == resolutions.end())
            resolutions.push_back(resolution);
    for (const double repair : options.repair_percentages) {
        (void) repair_percentage_to_ratio(repair);
        for (const auto [width, height] : resolutions) {
            const ResilientVideoConfig video{
                .width = width, .height = height,
                .fps = options.fps,
                .explicit_frame_duration = true};
            if (!video.valid())
                throw std::invalid_argument(
                    "Resolution must be positive and divisible by 8");
            for (const auto [type, size] : variants) {
                    if (size == 0 || size > kMaximumPayloadBytes)
                        throw std::invalid_argument(
                            "Test payload size must be 1..64 MiB");
                    if (++sequence > kMaximumCases)
                        throw std::invalid_argument(
                            "Test matrix exceeds the 256-case safety limit");
                    std::ostringstream id;
                    id << "yt" << std::setw(3) << std::setfill('0')
                       << sequence;
                    TestCase c;
                    c.test_suite_id = suite_id;
                    c.test_case_id = id.str();
                    c.created_at = iso_timestamp();
                    c.vidstorex_version = "1.4.0";
                    c.reliability_profile =
                        profile_for_repair(repair);
                    c.repair_percentage = repair;
                    c.input_data_type = type;
                    c.requested_input_size = size;
                    c.effective_input_size = size;
                    c.input_size = size;
                    c.minimum_duration_seconds =
                        options.minimum_upload_duration_seconds;
                    c.minimum_required_frames = minimum_frames;
                    c.payload_seed =
                        kDefaultPayloadSeed + sequence;
                    c.payload_extension_seed = stable_seed(
                        suite_id + "|" + c.test_case_id + "|" +
                            kPayloadExtensionVersion,
                        c.payload_seed);
                    c.video = video;
                    c.frame_payload_capacity =
                        static_cast<uint64_t>(
                            VideoEncoder::packets_per_frame(video));
                    c.expected_output_filename =
                        c.test_case_id + "_restored.bin";
                    const std::string filename =
                        "VSX_YT_" + suite_id + "_" +
                        c.test_case_id + "_" +
                        resolution_token(width, height) + "_r" +
                        std::to_string(static_cast<int>(repair)) + "_" +
                        to_string(type) + "_" + size_token(size);
                    c.payload_path =
                        normalized_relative(
                            std::filesystem::path("cases") /
                            (filename + ".bin"));
                    c.master_video_path =
                        normalized_relative(
                            std::filesystem::path("generated") /
                            (filename + "_master.mkv"));
                    c.upload_candidate_path =
                        normalized_relative(
                            std::filesystem::path("generated") /
                            (filename + ".mp4"));
                    const std::string key =
                        std::to_string(repair) + "|" +
                        std::to_string(width) + "x" +
                        std::to_string(height) + "|" +
                        to_string(type) + "|" + std::to_string(size);
                    if (!unique.insert(key).second)
                        throw std::invalid_argument(
                            "Duplicate test case in matrix");
                    cases.push_back(std::move(c));
            }
        }
    }
    // Keep reliability comparisons honest: every case in the same
    // resolution/type/requested-size group uses the payload needed by the
    // group's lowest repair percentage.
    for (auto &current : cases) {
        double lowest_repair = current.repair_percentage;
        for (const auto &candidate : cases) {
            if (candidate.video.width == current.video.width &&
                candidate.video.height == current.video.height &&
                candidate.input_data_type == current.input_data_type &&
                candidate.requested_input_size ==
                    current.requested_input_size)
                lowest_repair =
                    std::min(lowest_repair,
                             candidate.repair_percentage);
        }
        const uint64_t effective =
            minimum_payload_size_for_frames(
                current.requested_input_size, current.video,
                lowest_repair, current.minimum_required_frames);
        current.effective_input_size = effective;
        current.input_size = effective;
        current.payload_extended_for_duration =
            effective > current.requested_input_size;
        const std::string comparison_group =
            suite_id + "|" +
            std::to_string(current.video.width) + "x" +
            std::to_string(current.video.height) + "|" +
            to_string(current.input_data_type) + "|" +
            std::to_string(current.requested_input_size);
        current.payload_seed = stable_seed(
            comparison_group, kDefaultPayloadSeed);
        current.payload_extension_seed = stable_seed(
            comparison_group + "|" + kPayloadExtensionVersion,
            current.payload_seed);
        const auto estimate = estimate_encoding_reliability(
            effective, false,
            {repair_percentage_to_ratio(
                current.repair_percentage)},
            current.frame_payload_capacity,
            static_cast<uint32_t>(current.video.fps));
        current.expected_encoded_frames = estimate.frame_count;
    }
    return cases;
}

SuitePreflight estimate_suite(
    const std::vector<TestCase> &cases,
    const std::filesystem::path &output_root) {
    if (cases.size() > kMaximumCases)
        throw std::invalid_argument(
            "Test matrix exceeds the safety limit");
    SuitePreflight result;
    result.case_count = cases.size();
    for (const TestCase &c : cases) {
        const auto counts = estimate_encoding_reliability(
            c.effective_input_size, false,
            {repair_percentage_to_ratio(c.repair_percentage)},
            static_cast<uint64_t>(
                VideoEncoder::packets_per_frame(c.video)),
            static_cast<uint32_t>(c.video.fps));
        if (std::numeric_limits<uint64_t>::max() -
                result.estimated_total_frames < counts.frame_count)
            throw std::overflow_error("Frame estimate overflow");
        result.estimated_total_frames += counts.frame_count;
        result.estimated_total_duration_seconds +=
            counts.video_duration_seconds;
        const uint64_t raw_frame_bytes =
            static_cast<uint64_t>(c.video.width) * c.video.height;
        const uint64_t conservative_master =
            counts.frame_count > 0 &&
            raw_frame_bytes >
                std::numeric_limits<uint64_t>::max() /
                    counts.frame_count
                ? std::numeric_limits<uint64_t>::max()
                : counts.frame_count * raw_frame_bytes;
        const uint64_t conservative_case =
            conservative_master >
                std::numeric_limits<uint64_t>::max() / 2
                ? std::numeric_limits<uint64_t>::max()
                : conservative_master * 2 +
                    c.effective_input_size;
        if (std::numeric_limits<uint64_t>::max() -
                result.estimated_output_bytes < conservative_case)
            throw std::overflow_error("Disk estimate overflow");
        result.estimated_output_bytes += conservative_case;
    }
    result.safety_margin_bytes =
        std::max<uint64_t>(256 * MiB,
            result.estimated_output_bytes / 10);
    if (std::numeric_limits<uint64_t>::max() -
            result.estimated_output_bytes <
        result.safety_margin_bytes)
        throw std::overflow_error("Disk requirement overflow");
    result.required_disk_bytes =
        result.estimated_output_bytes + result.safety_margin_bytes;
    std::error_code error;
    const auto space =
        std::filesystem::space(output_root, error);
    if (!error) {
        result.available_disk_bytes = space.available;
        result.disk_space_sufficient =
            space.available >= result.required_disk_bytes;
    }
    return result;
}

std::vector<SimulationProfile> simulation_profiles() {
    return {
        {"yt-sim-1080p-light", "libx264", 14, "medium",
         "yuv420p", 1920, 1080, 30, true, 60},
        {"yt-sim-1080p-medium", "libx264", 20, "medium",
         "yuv420p", 1920, 1080, 30, true, 60},
        {"yt-sim-1080p-heavy", "libx264", 28, "fast",
         "yuv420p", 1920, 1080, 30, true, 90},
        {"yt-sim-720p-downscale", "libx264", 23, "medium",
         "yuv420p", 1280, 720, 30, true, 60},
        {"yt-sim-4k-medium", "libx264", 20, "medium",
         "yuv420p", 3840, 2160, 30, true, 60}
    };
}

std::optional<SimulationProfile> find_simulation_profile(
    const std::string &name) {
    for (auto profile : simulation_profiles())
        if (profile.name == name) return profile;
    return std::nullopt;
}

void transcode_simulation_video(
    const std::filesystem::path &input,
    const std::filesystem::path &output,
    const SimulationProfile &profile,
    const std::string &suite_id,
    const std::string &case_id) {
    transcode_h264(input, output, profile, suite_id, case_id);
}

std::string create_suite_id() {
    static std::atomic<uint64_t> sequence{0};
    std::ostringstream out;
    out << compact_timestamp() << "-"
        << std::hex << std::setw(4) << std::setfill('0')
        << (sequence.fetch_add(1) & 0xffff);
    return out.str();
}

std::string create_analysis_session(
    SuiteManifest &manifest, const std::string &label,
    const std::string &source_folder,
    const std::string &notes) {
    const std::string id = create_record_id("session");
    manifest.analysis_sessions.push_back({
        .analysis_session_id = id,
        .label = label.empty() ? "Analysis session" : label,
        .created_at_utc = iso_timestamp(),
        .source_folder = source_folder,
        .observation_count = 0,
        .notes = notes});
    manifest.active_analysis_session_id = id;
    return id;
}

std::string sha256_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(
        "Could not open file for SHA-256: " + path.string());
    picosha2::hash256_one_by_one hasher;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char *>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
            hasher.process(buffer.begin(), buffer.begin() + count);
    }
    if (!input.eof())
        throw std::runtime_error(
            "Could not read file for SHA-256");
    hasher.finish();
    return picosha2::get_hash_hex_string(hasher);
}

void generate_payload(const std::filesystem::path &path,
                      const DataType type, const uint64_t size,
                      const uint64_t seed) {
    if (type == DataType::ExistingFile)
        throw std::invalid_argument(
            "Existing-file payloads are not generated");
    SafeOutputFile safe(path);
    std::ofstream output(safe.partial_path(), std::ios::binary);
    if (!output)
        throw std::runtime_error("Could not create payload");
    std::array<unsigned char, 64 * 1024> buffer{};
    uint64_t remaining = size;
    uint64_t state = seed == 0 ? kDefaultPayloadSeed : seed;
    uint64_t offset = 0;
    while (remaining > 0) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, buffer.size()));
        if (type == DataType::Compressible) {
            static constexpr std::string_view pattern =
                "VidStoreX-YouTube-Test-Lab|";
            for (std::size_t i = 0; i < count; ++i)
                buffer[i] = static_cast<unsigned char>(
                    pattern[(offset + i + seed) % pattern.size()]);
        } else {
            for (std::size_t i = 0; i < count; i += 8) {
                const uint64_t random = xorshift64star(state);
                const std::size_t copy =
                    std::min<std::size_t>(8, count - i);
                std::memcpy(buffer.data() + i, &random, copy);
            }
        }
        output.write(reinterpret_cast<const char *>(buffer.data()),
                     static_cast<std::streamsize>(count));
        if (!output)
            throw std::runtime_error("Could not write payload");
        remaining -= count;
        offset += count;
    }
    output.flush();
    if (!output)
        throw std::runtime_error("Could not flush payload");
    output.close();
    safe.commit();
}

void generate_case_payload(const std::filesystem::path &path,
                           const TestCase &test_case) {
    if (test_case.input_data_type == DataType::ExistingFile)
        throw std::invalid_argument(
            "Existing-file payloads are not generated");
    if (test_case.requested_input_size == 0 ||
        test_case.effective_input_size <
            test_case.requested_input_size ||
        test_case.effective_input_size > kMaximumPayloadBytes)
        throw std::invalid_argument(
            "invalid requested/effective payload sizes");

    SafeOutputFile safe(path);
    std::ofstream output(safe.partial_path(), std::ios::binary);
    if (!output)
        throw std::runtime_error("Could not create payload");
    std::array<unsigned char, 64 * 1024> buffer{};
    uint64_t offset = 0;
    uint64_t base_state = test_case.payload_seed == 0
        ? kDefaultPayloadSeed : test_case.payload_seed;
    uint64_t extension_state =
        test_case.payload_extension_seed == 0
            ? stable_seed(
                  test_case.test_suite_id + "|" +
                      test_case.test_case_id + "|" +
                      test_case.payload_extension_version,
                  base_state)
            : test_case.payload_extension_seed;
    while (offset < test_case.effective_input_size) {
        const bool extension =
            offset >= test_case.requested_input_size;
        uint64_t available =
            test_case.effective_input_size - offset;
        if (!extension)
            available = std::min(
                available,
                test_case.requested_input_size - offset);
        const std::size_t count = static_cast<std::size_t>(
            std::min<uint64_t>(available, buffer.size()));
        uint64_t &state = extension
            ? extension_state : base_state;
        if (test_case.input_data_type == DataType::Compressible) {
            static constexpr std::string_view pattern =
                "VidStoreX-YouTube-Test-Lab|";
            for (std::size_t i = 0; i < count; ++i) {
                const uint64_t absolute = offset + i;
                unsigned char value = static_cast<unsigned char>(
                    pattern[(absolute + state) % pattern.size()]);
                // Sparse deterministic markers make blocks distinct while
                // retaining highly compressible data.
                if ((absolute % 4096) < sizeof(uint64_t))
                    value = static_cast<unsigned char>(
                        state >> ((absolute % 8) * 8));
                buffer[i] = value;
            }
            state = xorshift64star(state);
        } else {
            for (std::size_t i = 0; i < count; i += 8) {
                const uint64_t random = xorshift64star(state);
                const std::size_t copy =
                    std::min<std::size_t>(8, count - i);
                std::memcpy(buffer.data() + i, &random, copy);
            }
        }
        output.write(reinterpret_cast<const char *>(buffer.data()),
                     static_cast<std::streamsize>(count));
        if (!output)
            throw std::runtime_error("Could not write payload");
        offset += count;
    }
    output.flush();
    if (!output)
        throw std::runtime_error("Could not flush payload");
    output.close();
    safe.commit();
}

void write_manifest_atomic(const SuiteManifest &manifest,
                           const std::filesystem::path &path) {
    if (manifest.schema_version != kManifestSchemaVersion)
        throw std::invalid_argument(
            "Unsupported manifest schema version");
    SafeOutputFile safe(path);
    std::ofstream out(safe.partial_path(), std::ios::binary);
    if (!out) throw std::runtime_error("Could not create manifest");
    out << "{\n"
        << "  \"schema_version\": " << manifest.schema_version << ",\n"
        << "  \"vidstorex_version\": "
        << q(manifest.vidstorex_version) << ",\n"
        << "  \"suite_id\": " << q(manifest.suite_id) << ",\n"
        << "  \"created_at\": " << q(manifest.created_at) << ",\n"
        << "  \"preset\": " << q(manifest.preset) << ",\n"
        << "  \"active_analysis_session_id\": "
        << q(manifest.active_analysis_session_id) << ",\n"
        << "  \"duplicate_observations_excluded\": "
        << manifest.duplicate_observations_excluded << ",\n"
        << "  \"deduplication_log\": [";
    for (std::size_t i = 0;
         i < manifest.deduplication_log.size(); ++i) {
        if (i != 0) out << ", ";
        out << q(manifest.deduplication_log[i]);
    }
    out << "],\n"
        << "  \"analysis_sessions\": [";
    if (!manifest.analysis_sessions.empty()) out << "\n";
    for (std::size_t i = 0;
         i < manifest.analysis_sessions.size(); ++i) {
        const auto &session = manifest.analysis_sessions[i];
        out << "    {\n"
            << "      \"analysis_session_id\": "
            << q(session.analysis_session_id) << ",\n"
            << "      \"label\": " << q(session.label) << ",\n"
            << "      \"created_at_utc\": "
            << q(session.created_at_utc) << ",\n"
            << "      \"source_folder\": "
            << q(session.source_folder) << ",\n"
            << "      \"observation_count\": "
            << session.observation_count << ",\n"
            << "      \"notes\": " << q(session.notes) << "\n"
            << "    }";
        if (i + 1 != manifest.analysis_sessions.size())
            out << ",";
        out << "\n";
    }
    out << "  ],\n"
        << "  \"cases\": [";
    if (!manifest.cases.empty()) out << "\n";
    for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
        out << "    ";
        write_case(out, manifest.cases[i], "    ");
        if (i + 1 != manifest.cases.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
    out.flush();
    if (!out) throw std::runtime_error("Could not write manifest");
    out.close();
    safe.commit();
}

SuiteManifest read_manifest(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open manifest");
    std::ostringstream contents;
    contents << input.rdbuf();
    const Json root = JsonParser(contents.str()).parse();
    SuiteManifest manifest;
    const int source_schema =
        root.at("schema_version").integer();
    if (source_schema < kOldestSupportedManifestSchemaVersion ||
        source_schema > kManifestSchemaVersion)
        throw std::runtime_error(
            "Unsupported manifest schema version: " +
            std::to_string(source_schema));
    // In-memory manifests are upgraded additively and are written as the
    // current schema the next time the suite is resumed or reported.
    manifest.schema_version = kManifestSchemaVersion;
    manifest.vidstorex_version =
        root.at("vidstorex_version").string();
    manifest.suite_id = root.at("suite_id").string();
    manifest.created_at = root.at("created_at").string();
    manifest.preset = root.at("preset").string();
    manifest.loaded_schema_version = source_schema;
    if (const auto it =
            root.object().find("active_analysis_session_id");
        it != root.object().end())
        manifest.active_analysis_session_id =
            it->second.string();
    if (const auto it =
            root.object().find("duplicate_observations_excluded");
        it != root.object().end())
        manifest.duplicate_observations_excluded =
            it->second.u64();
    if (const auto it = root.object().find("deduplication_log");
        it != root.object().end())
        for (const auto &entry : it->second.array())
            manifest.deduplication_log.push_back(
                entry.string());
    if (const auto it = root.object().find("analysis_sessions");
        it != root.object().end()) {
        for (const auto &entry : it->second.array()) {
            AnalysisSession session;
            session.analysis_session_id =
                entry.at("analysis_session_id").string();
            session.label = entry.at("label").string();
            session.created_at_utc =
                entry.at("created_at_utc").string();
            session.source_folder =
                entry.at("source_folder").string();
            session.observation_count =
                entry.at("observation_count").u64();
            session.notes = entry.at("notes").string();
            manifest.analysis_sessions.push_back(
                std::move(session));
        }
    }
    std::set<std::string> ids;
    for (const Json &item : root.at("cases").array()) {
        auto c = parse_case(item);
        if (c.test_suite_id != manifest.suite_id)
            throw std::runtime_error(
                "Case belongs to a different suite");
        if (!ids.insert(c.test_case_id).second)
            throw std::runtime_error(
                "Duplicate case ID in manifest");
        for (const std::string *relative : {
                 &c.payload_path, &c.master_video_path,
                 &c.upload_candidate_path}) {
            if (!relative->empty() &&
                std::filesystem::path(*relative).is_absolute())
                throw std::runtime_error(
                    "Manifest paths must be relative");
        }
        manifest.cases.push_back(std::move(c));
    }
    if (source_schema < 2) {
        const auto versioned_path = [](const std::string &value) {
            const std::filesystem::path path(value);
            return normalized_relative(
                path.parent_path() /
                (path.stem().string() + "_duration-v2" +
                 path.extension().string()));
        };
        for (auto &current : manifest.cases) {
            double lowest_repair = current.repair_percentage;
            for (const auto &candidate : manifest.cases) {
                if (candidate.video.width == current.video.width &&
                    candidate.video.height == current.video.height &&
                    candidate.input_data_type ==
                        current.input_data_type &&
                    candidate.requested_input_size ==
                        current.requested_input_size)
                    lowest_repair = std::min(
                        lowest_repair,
                        candidate.repair_percentage);
            }
            current.video.explicit_frame_duration = true;
            current.minimum_duration_seconds =
                kMinimumUploadDurationSeconds;
            current.minimum_required_frames =
                minimum_frames_for_duration(
                    current.minimum_duration_seconds,
                    current.video.fps);
            current.effective_input_size =
                minimum_payload_size_for_frames(
                    current.requested_input_size,
                    current.video, lowest_repair,
                    current.minimum_required_frames);
            current.input_size =
                current.effective_input_size;
            current.payload_extended_for_duration =
                current.effective_input_size >
                current.requested_input_size;
            const auto estimate =
                estimate_encoding_reliability(
                    current.effective_input_size, false,
                    {repair_percentage_to_ratio(
                        current.repair_percentage)},
                    current.frame_payload_capacity,
                    static_cast<uint32_t>(
                        current.video.fps));
            current.expected_encoded_frames =
                estimate.frame_count;
            current.payload_path =
                versioned_path(current.payload_path);
            current.master_video_path =
                versioned_path(current.master_video_path);
            current.upload_candidate_path =
                versioned_path(
                    current.upload_candidate_path);
            current.actual_master_frames = 0;
            current.actual_candidate_frames = 0;
            current.master_duration_seconds = 0.0;
            current.candidate_duration_seconds = 0.0;
            current.candidate_duration_validation_known = false;
            current.candidate_timestamps_valid = false;
            current.candidate_ready_for_youtube = false;
            current.candidate_validation_error =
                "Duration validation unknown; resume regenerates a "
                "versioned candidate without overwriting old files";
            if (current.state != CaseState::Skipped)
                current.state = CaseState::Pending;
        }
    }
    if (source_schema < 3) {
        const std::string legacy_session_id =
            "legacy-" + manifest.suite_id;
        uint64_t observation_index = 0;
        uint64_t legacy_observations = 0;
        const auto suite_root = path.parent_path();
        for (auto &test_case : manifest.cases) {
            for (auto &result : test_case.results) {
                result.suite_id = manifest.suite_id;
                result.case_id = test_case.test_case_id;
                result.test_case_id = test_case.test_case_id;
                result.source_type = source_type_for(
                    result.source, result.simulation_profile);
                result.vidstorex_version =
                    test_case.vidstorex_version;
                result.analysis_session_id =
                    legacy_session_id;
                if (result.source_file_size == 0)
                    result.source_file_size =
                        result.downloaded_video_size;
                std::filesystem::path source_path;
                if (result.source ==
                    ResultSource::RealYouTubeRoundtrip) {
                    result.source_file_relative_name =
                        normalized_relative(
                            std::filesystem::path("imported") /
                            result.analyzed_video);
                    source_path = suite_root /
                        result.source_file_relative_name;
                } else {
                    result.source_file_relative_name =
                        result.analyzed_video;
                    if (result.simulation_profile ==
                        "master-lossless")
                        source_path = suite_root /
                            test_case.master_video_path;
                    else if (result.simulation_profile ==
                             "youtube-upload-candidate")
                        source_path = suite_root /
                            test_case.upload_candidate_path;
                }
                std::error_code exists_error;
                if (!source_path.empty() &&
                    std::filesystem::exists(
                        source_path, exists_error) &&
                    !exists_error) {
                    try {
                        result.source_file_sha256 =
                            sha256_file(source_path);
                    } catch (...) {
                    }
                    result.source_file_created_time_utc =
                        created_time_utc(source_path);
                    result.source_file_modified_time_utc =
                        modified_time_utc(source_path);
                }
                result.imported_at_utc =
                    result.source ==
                        ResultSource::RealYouTubeRoundtrip
                        ? result.source_file_modified_time_utc
                        : "";
                result.analyzed_at_utc =
                    !result.source_file_modified_time_utc.empty()
                        ? result.source_file_modified_time_utc
                        : manifest.created_at;
                if (result.sha256_match)
                    result.restored_sha256 =
                        test_case.input_sha256;
                std::ostringstream observation;
                observation << "legacy-" << std::hex
                    << stable_seed(
                           duplicate_key(
                               manifest.suite_id,
                               test_case.test_case_id,
                               result) + "|" +
                               std::to_string(
                                   observation_index++),
                           kDefaultPayloadSeed);
                result.observation_id = observation.str();
                ++legacy_observations;
            }
        }
        if (legacy_observations > 0) {
            manifest.analysis_sessions.push_back({
                .analysis_session_id = legacy_session_id,
                .label = "Migrated legacy observations",
                .created_at_utc = manifest.created_at,
                .source_folder = "",
                .observation_count = legacy_observations,
                .notes =
                    "Imported additively from manifest schema v" +
                    std::to_string(source_schema)});
            manifest.active_analysis_session_id =
                legacy_session_id;
        }
    }
    if (manifest.cases.size() > kMaximumCases)
        throw std::runtime_error(
            "Manifest exceeds the case safety limit");
    return manifest;
}

VideoTechnicalInfo analyze_video(
    const std::filesystem::path &path) {
    AVFormatContext *format = nullptr;
    try {
        throw_av(avformat_open_input(
                     &format, path.string().c_str(), nullptr, nullptr),
                 "Could not open video");
        throw_av(avformat_find_stream_info(format, nullptr),
                 "Could not analyze video");
        const int index = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        throw_av(index, "No supported video stream");
        AVStream *stream = format->streams[index];
        const AVCodecParameters *parameters = stream->codecpar;
        VideoTechnicalInfo result;
        result.container = format->iformat && format->iformat->name
            ? format->iformat->name : "";
        result.codec = avcodec_get_name(parameters->codec_id);
        if (parameters->codec_tag != 0) {
            std::array<char, AV_FOURCC_MAX_STRING_SIZE> tag{};
            result.codec_tag = av_fourcc_make_string(
                tag.data(), parameters->codec_tag);
        }
        result.profile =
            avcodec_profile_name(parameters->codec_id,
                                 parameters->profile)
                ? avcodec_profile_name(parameters->codec_id,
                                       parameters->profile)
                : "";
        result.pixel_format =
            parameters->format >= 0
                ? av_get_pix_fmt_name(
                    static_cast<AVPixelFormat>(parameters->format))
                : "";
        result.width = parameters->width;
        result.height = parameters->height;
        const AVRational sar =
            parameters->sample_aspect_ratio.num > 0 &&
            parameters->sample_aspect_ratio.den > 0
                ? parameters->sample_aspect_ratio
                : stream->sample_aspect_ratio;
        int dar_num = parameters->width;
        int dar_den = parameters->height;
        if (sar.num > 0 && sar.den > 0)
            av_reduce(
                &dar_num, &dar_den,
                static_cast<int64_t>(parameters->width) * sar.num,
                static_cast<int64_t>(parameters->height) * sar.den,
                1024 * 1024);
        if (dar_num > 0 && dar_den > 0)
            result.display_aspect_ratio =
                std::to_string(dar_num) + ":" +
                std::to_string(dar_den);
        result.time_base =
            std::to_string(stream->time_base.num) + "/" +
            std::to_string(stream->time_base.den);
        const AVRational rate =
            av_guess_frame_rate(format, stream, nullptr);
        result.fps = rate.den
            ? av_q2d(rate) : 0.0;
        result.frame_count = stream->nb_frames;
        if (stream->duration > 0)
            result.duration_seconds =
                stream->duration * av_q2d(stream->time_base);
        else if (format->duration > 0)
            result.duration_seconds =
                static_cast<double>(format->duration) / AV_TIME_BASE;
        if (result.frame_count <= 0 && result.fps > 0.0 &&
            result.duration_seconds > 0.0)
            result.frame_count = static_cast<int64_t>(
                std::llround(result.fps *
                             result.duration_seconds));
        result.file_size = std::filesystem::file_size(path);
        result.stream_bitrate =
            parameters->bit_rate > 0 ? parameters->bit_rate : 0;
        result.container_bitrate =
            format->bit_rate > 0 ? format->bit_rate : 0;
        if (result.duration_seconds > 0.0 &&
            result.file_size <=
                static_cast<uint64_t>(
                    std::numeric_limits<int64_t>::max() / 8))
            result.calculated_bitrate =
                static_cast<int64_t>(
                    std::llround(
                        static_cast<long double>(result.file_size) *
                        8.0L / result.duration_seconds));
        if (result.stream_bitrate > 0) {
            result.bitrate = result.stream_bitrate;
            result.bitrate_source = "reported_stream";
        } else if (result.container_bitrate > 0) {
            result.bitrate = result.container_bitrate;
            result.bitrate_source = "reported_container";
        } else if (result.calculated_bitrate > 0) {
            result.bitrate = result.calculated_bitrate;
            result.bitrate_source =
                "calculated_from_size_duration";
        }
        avformat_close_input(&format);
        return result;
    } catch (...) {
        if (format) avformat_close_input(&format);
        throw;
    }
}

CandidateValidation validate_upload_candidate(
    const std::filesystem::path &path,
    const ResilientVideoConfig &expected_video,
    const uint64_t minimum_frames,
    const double minimum_duration_seconds) {
    CandidateValidation result;
    AVFormatContext *format = nullptr;
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    const auto cleanup = [&] {
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (decoder) avcodec_free_context(&decoder);
        if (format) avformat_close_input(&format);
    };
    const auto fail = [&](const std::string &message) {
        if (result.error.empty()) result.error = message;
    };
    try {
        if (!expected_video.valid())
            throw std::invalid_argument(
                "invalid expected candidate video configuration");
        if (minimum_duration_seconds < 0.0 ||
            !std::isfinite(minimum_duration_seconds))
            throw std::invalid_argument(
                "invalid candidate minimum duration");
        std::error_code size_error;
        result.video.file_size =
            std::filesystem::file_size(path, size_error);
        result.file_nonempty =
            !size_error && result.video.file_size > 0;
        if (!result.file_nonempty)
            fail("Candidate file is empty");

        throw_av(avformat_open_input(
                     &format, path.string().c_str(), nullptr, nullptr),
                 "Could not open candidate container");
        result.container_opened = true;
        throw_av(avformat_find_stream_info(format, nullptr),
                 "Could not read candidate stream info");
        const int stream_index = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        throw_av(stream_index, "Candidate has no video stream");
        result.video_stream_present = true;
        AVStream *stream = format->streams[stream_index];
        const AVCodecParameters *parameters = stream->codecpar;
        result.video.container =
            format->iformat && format->iformat->name
                ? format->iformat->name : "";
        result.video.codec =
            avcodec_get_name(parameters->codec_id);
        result.codec_is_h264 =
            parameters->codec_id == AV_CODEC_ID_H264;
        if (!result.codec_is_h264)
            fail("Candidate codec is not H.264");
        result.video.width = parameters->width;
        result.video.height = parameters->height;
        result.resolution_matches =
            parameters->width == expected_video.width &&
            parameters->height == expected_video.height;
        if (!result.resolution_matches)
            fail("Candidate resolution does not match");
        const AVRational rate =
            av_guess_frame_rate(format, stream, nullptr);
        result.video.fps = rate.den ? av_q2d(rate) : 0.0;
        result.fps_matches =
            std::abs(result.video.fps - expected_video.fps) <= 0.05;
        if (!result.fps_matches)
            fail("Candidate frame rate is not approximately expected");
        if (stream->duration > 0)
            result.video.duration_seconds =
                stream->duration * av_q2d(stream->time_base);
        else if (format->duration > 0)
            result.video.duration_seconds =
                static_cast<double>(format->duration) / AV_TIME_BASE;
        result.video.bitrate =
            parameters->bit_rate > 0
                ? parameters->bit_rate : format->bit_rate;

        const AVCodec *codec =
            avcodec_find_decoder(parameters->codec_id);
        if (!codec)
            throw std::runtime_error(
                "Candidate H.264 decoder unavailable");
        decoder = avcodec_alloc_context3(codec);
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if (!decoder || !packet || !frame)
            throw std::bad_alloc();
        throw_av(avcodec_parameters_to_context(decoder, parameters),
                 "Could not configure candidate decoder");
        throw_av(avcodec_open2(decoder, codec, nullptr),
                 "Could not open candidate decoder");

        bool saw_timestamped_packet = false;
        bool all_timestamps_present = true;
        bool pts_monotonic = true;
        bool dts_monotonic = true;
        int64_t previous_pts = AV_NOPTS_VALUE;
        int64_t previous_dts = AV_NOPTS_VALUE;
        int64_t last_pts = AV_NOPTS_VALUE;
        int64_t last_duration = 0;
        auto receive_frames = [&] {
            while (true) {
                const int received =
                    avcodec_receive_frame(decoder, frame);
                if (received == AVERROR(EAGAIN) ||
                    received == AVERROR_EOF)
                    return received;
                throw_av(received,
                         "Candidate frame decode failed");
                ++result.decoded_frame_count;
                if (result.video.pixel_format.empty()) {
                    const char *name = av_get_pix_fmt_name(
                        static_cast<AVPixelFormat>(frame->format));
                    result.video.pixel_format = name ? name : "";
                }
                av_frame_unref(frame);
            }
        };

        while (true) {
            const int read = av_read_frame(format, packet);
            if (read == AVERROR_EOF) break;
            throw_av(read, "Candidate packet read failed");
            if (packet->stream_index != stream_index) {
                av_packet_unref(packet);
                continue;
            }
            if (packet->pts == AV_NOPTS_VALUE ||
                packet->dts == AV_NOPTS_VALUE) {
                all_timestamps_present = false;
                fail("Missing timestamps");
            } else {
                if (previous_pts != AV_NOPTS_VALUE &&
                    packet->pts <= previous_pts)
                    pts_monotonic = false;
                if (previous_dts != AV_NOPTS_VALUE &&
                    packet->dts <= previous_dts)
                    dts_monotonic = false;
                previous_pts = packet->pts;
                previous_dts = packet->dts;
                last_pts = packet->pts;
                last_duration = packet->duration;
                saw_timestamped_packet = true;
            }
            const int sent = avcodec_send_packet(decoder, packet);
            av_packet_unref(packet);
            throw_av(sent, "Candidate packet decode failed");
            (void) receive_frames();
        }
        throw_av(avcodec_send_packet(decoder, nullptr),
                 "Could not flush candidate decoder");
        const int drain = receive_frames();
        result.decode_completed =
            drain == AVERROR_EOF || drain == AVERROR(EAGAIN);
        result.video.frame_count = result.decoded_frame_count;
        result.pixel_format_compatible =
            result.video.pixel_format == "yuv420p" ||
            result.video.pixel_format == "yuvj420p";
        if (!result.pixel_format_compatible)
            fail("Candidate pixel format is not YUV420P compatible");
        result.frame_count_sufficient =
            static_cast<uint64_t>(
                std::max<int64_t>(0, result.decoded_frame_count)) >=
            minimum_frames;
        if (!result.frame_count_sufficient)
            fail("Insufficient frame count");
        const double duration_threshold = std::max(
            kMinimumValidatedDurationSeconds,
            minimum_duration_seconds - 0.05);
        result.duration_sufficient =
            result.video.duration_seconds >= duration_threshold;
        if (!result.duration_sufficient)
            fail("Too short or invalid MP4 duration");
        result.timestamps_present =
            saw_timestamped_packet && all_timestamps_present;
        result.pts_monotonic =
            result.timestamps_present && pts_monotonic;
        result.dts_monotonic =
            result.timestamps_present && dts_monotonic;
        if (!result.pts_monotonic ||
            !result.dts_monotonic)
            fail("Non-monotonic timestamps");
        double timestamp_end = 0.0;
        if (last_pts != AV_NOPTS_VALUE) {
            const int64_t duration =
                last_duration > 0 ? last_duration : 1;
            const int64_t start =
                stream->start_time != AV_NOPTS_VALUE
                    ? stream->start_time : 0;
            timestamp_end =
                static_cast<double>(last_pts + duration - start) *
                av_q2d(stream->time_base);
        }
        const double expected_duration =
            expected_video.fps > 0
                ? static_cast<double>(result.decoded_frame_count) /
                      expected_video.fps
                : 0.0;
        result.last_timestamp_valid =
            timestamp_end >= duration_threshold &&
            std::abs(timestamp_end - expected_duration) <= 0.10;
        if (!result.last_timestamp_valid)
            fail("Last frame timestamp does not match duration");
        if (!result.decode_completed)
            fail("Candidate decode failed");
        result.passed =
            result.container_opened &&
            result.video_stream_present &&
            result.codec_is_h264 &&
            result.pixel_format_compatible &&
            result.resolution_matches &&
            result.fps_matches &&
            result.frame_count_sufficient &&
            result.duration_sufficient &&
            result.timestamps_present &&
            result.pts_monotonic &&
            result.dts_monotonic &&
            result.last_timestamp_valid &&
            result.decode_completed &&
            result.file_nonempty;
        cleanup();
        return result;
    } catch (const std::exception &error) {
        fail(error.what());
        cleanup();
        result.passed = false;
        return result;
    }
}

AnalysisOutcome analyze_case_video_record(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    const ResultSource source,
    const std::string &simulation_profile,
    const AnalysisOptions &options) {
    const auto started = Clock::now();
    TestResult result;
    result.suite_id = manifest.suite_id;
    result.case_id = test_case.test_case_id;
    result.test_case_id = test_case.test_case_id;
    result.source = source;
    result.simulation_profile = simulation_profile;
    result.source_type =
        source_type_for(source, simulation_profile);
    result.analyzed_at_utc = iso_timestamp();
    result.imported_at_utc = options.imported_at_utc;
    result.source_file_relative_name =
        options.source_relative_name.empty()
            ? video_path.filename().string()
            : options.source_relative_name;
    result.source_file_created_time_utc =
        options.source_file_created_time_utc.empty()
            ? created_time_utc(video_path)
            : options.source_file_created_time_utc;
    result.source_file_modified_time_utc =
        options.source_file_modified_time_utc.empty()
            ? modified_time_utc(video_path)
            : options.source_file_modified_time_utc;
    result.analysis_fingerprint = kAnalysisFingerprint;
    result.vidstorex_version =
        test_case.vidstorex_version;
    try {
        result.source_file_size =
            options.source_file_size != 0
                ? options.source_file_size
                : std::filesystem::file_size(video_path);
        result.source_file_sha256 =
            options.source_file_sha256.empty()
                ? sha256_file(video_path)
                : options.source_file_sha256;
    } catch (const std::exception &error) {
        result.failure_stage = "source-file-inspection";
        result.error_message = error.what();
        result.final_status =
            FinalStatus::UnsupportedProcessedVideo;
    }
    if (!options.record_new_observation &&
        !result.source_file_sha256.empty()) {
        const std::string key = duplicate_key(
            manifest.suite_id, test_case.test_case_id, result);
        for (const auto &existing : test_case.results) {
            if (duplicate_key(
                    manifest.suite_id,
                    test_case.test_case_id,
                    existing) == key) {
                return {
                    .result = existing,
                    .recorded = false,
                    .duplicate = true,
                    .message =
                        "This video has already been analyzed for "
                        "this case. Observation " +
                        existing.observation_id + " from " +
                        existing.analyzed_at_utc + ": " +
                        to_string(existing.final_status)};
            }
        }
    }
    const auto finish = [&](TestResult completed) {
        std::string session_id = options.analysis_session_id;
        if (session_id.empty()) {
            if (!options.session_label.empty())
                session_id = create_analysis_session(
                    manifest, options.session_label,
                    options.source_folder);
            else if (!manifest.active_analysis_session_id.empty())
                session_id =
                    manifest.active_analysis_session_id;
            else
                session_id = create_analysis_session(
                    manifest,
                    options.session_label.empty()
                        ? (source ==
                                   ResultSource::RealYouTubeRoundtrip
                               ? "Manual YouTube analysis"
                               : "Local Test Lab analysis")
                        : options.session_label,
                    options.source_folder);
        } else {
            const auto found = std::find_if(
                manifest.analysis_sessions.begin(),
                manifest.analysis_sessions.end(),
                [&](const AnalysisSession &session) {
                    return session.analysis_session_id ==
                        session_id;
                });
            if (found == manifest.analysis_sessions.end()) {
                manifest.analysis_sessions.push_back({
                    .analysis_session_id = session_id,
                    .label = options.session_label,
                    .created_at_utc = iso_timestamp(),
                    .source_folder = options.source_folder});
            }
            manifest.active_analysis_session_id = session_id;
        }
        completed.analysis_session_id = session_id;
        completed.observation_id =
            create_record_id("obs");
        test_case.results.push_back(completed);
        for (auto &session : manifest.analysis_sessions)
            if (session.analysis_session_id == session_id) {
                ++session.observation_count;
                break;
            }
        return AnalysisOutcome{
            .result = std::move(completed),
            .recorded = true,
            .duplicate = false,
            .message = "Analysis recorded"};
    };
    try {
        result.analyzed_video = video_path.filename().string();
        result.video = analyze_video(video_path);
        result.downloaded_video_size = result.video.file_size;
        result.source_file_size = result.video.file_size;
    } catch (const std::exception &error) {
        result.failure_stage = "video-analysis";
        result.error_message = error.what();
        result.final_status =
            FinalStatus::UnsupportedProcessedVideo;
        result.elapsed_decode_seconds =
            std::chrono::duration<double>(
                Clock::now() - started).count();
        return finish(std::move(result));
    }

    Decoder decoder;
    std::unordered_set<uint64_t> packet_ids;
    bool found_last = false;
    uint32_t last_chunk = 0;
    try {
        VideoDecoder video(video_path.string());
        while (!video.is_eof()) {
            const uint64_t before =
                static_cast<uint64_t>(video.frames_read());
            auto packets = video.decode_next_frame();
            const uint64_t after =
                static_cast<uint64_t>(video.frames_read());
            result.telemetry.frames_read += after - before;
            if (!packets.empty())
                ++result.telemetry.frames_with_pattern;
            for (auto &raw : packets) {
                ++result.telemetry.extracted_packets;
                const auto parsed = Decoder::parse_packet(raw);
                if (!parsed ||
                    !Decoder::validate_packet_crc(*parsed)) {
                    ++result.telemetry.invalid_packets;
                    continue;
                }
                ++result.telemetry.valid_packets;
                const auto &h = parsed->header;
                const uint64_t id =
                    (static_cast<uint64_t>(h.chunk_index) << 32) |
                    h.esi;
                if (!packet_ids.insert(id).second) {
                    ++result.telemetry.duplicate_packets;
                    continue;
                }
                if (h.flags & IsRepairSymbol)
                    ++result.telemetry.repair_packets;
                else
                    ++result.telemetry.source_packets;
                result.telemetry.required_packet_threshold +=
                    packet_ids.size() == 1 ? h.k : 0;
                if (h.flags & LastChunk) {
                    found_last = true;
                    last_chunk = h.chunk_index;
                }
                if (auto decoded =
                        decoder.process_packet(*parsed, false);
                    decoded && decoded->success)
                    ++result.telemetry.recovered_chunks;
            }
        }
        result.decoded_frame_count =
            static_cast<uint64_t>(video.frames_read());
    } catch (const std::exception &error) {
        result.failure_stage = "video-decode";
        result.error_message = error.what();
    }

    const uint64_t expected_chunks = found_last
        ? static_cast<uint64_t>(last_chunk) + 1
        : calculate_chunk_count(test_case.input_size, false);
    result.telemetry.missing_chunks =
        expected_chunks > result.telemetry.recovered_chunks
            ? expected_chunks - result.telemetry.recovered_chunks : 0;
    result.packet_recovery_percentage =
        test_case.total_packet_count > 0
            ? 100.0 * static_cast<double>(
                  result.telemetry.valid_packets) /
                  static_cast<double>(test_case.total_packet_count)
            : 0.0;
    result.frame_difference =
        static_cast<int64_t>(result.decoded_frame_count) -
        static_cast<int64_t>(test_case.encoded_frame_count);

    if (result.telemetry.extracted_packets == 0) {
        result.final_status = FinalStatus::HeaderNotFound;
        result.failure_stage = "packet-extraction";
        result.telemetry.failure_reason = "No VidStoreX header found";
    } else if (result.telemetry.recovered_chunks < expected_chunks) {
        result.final_status = FinalStatus::InsufficientPackets;
        result.failure_stage = "fec-recovery";
        result.telemetry.failure_reason =
            "Not enough valid source/repair packets";
    } else {
        try {
            const std::filesystem::path restored =
                std::filesystem::temp_directory_path() /
                ("vidstorex-testlab-" + manifest.suite_id + "-" +
                 test_case.test_case_id + ".bin");
            {
                SafeOutputFile safe(restored);
                if (!decoder.write_assembled_file(
                        safe.partial_path().string(),
                        static_cast<uint32_t>(expected_chunks)))
                    throw std::runtime_error(
                        "Could not assemble recovered payload");
                safe.commit();
            }
            result.decode_completed = true;
            result.restored_sha256 = sha256_file(restored);
            result.sha256_match =
                result.restored_sha256 ==
                test_case.input_sha256;
            std::error_code ignored;
            std::filesystem::remove(restored, ignored);
            result.final_status = result.sha256_match
                ? FinalStatus::Pass : FinalStatus::CorruptOutput;
            if (!result.sha256_match)
                result.failure_stage = "sha256-verification";
        } catch (const std::exception &error) {
            result.final_status = FinalStatus::DecodeFailed;
            result.failure_stage = "output-assembly";
            result.error_message = error.what();
        }
    }
    result.elapsed_decode_seconds =
        std::chrono::duration<double>(
            Clock::now() - started).count();
    return finish(std::move(result));
}

TestResult analyze_case_video(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    const ResultSource source,
    const std::string &simulation_profile) {
    return analyze_case_video_record(
        manifest, test_case, video_path, source,
        simulation_profile).result;
}

AnalysisOutcome analyze_real_video(
    const std::filesystem::path &manifest_path,
    const std::string &case_id,
    const std::filesystem::path &video_path,
    const AnalysisOptions &options) {
    SuiteManifest manifest = read_manifest(manifest_path);
    auto test_case = std::find_if(
        manifest.cases.begin(), manifest.cases.end(),
        [&](const TestCase &item) {
            return item.test_case_id == case_id;
        });
    if (test_case == manifest.cases.end())
        throw std::invalid_argument(
            "Unknown Test Lab case ID: " + case_id);
    if (!std::filesystem::is_regular_file(video_path))
        throw std::invalid_argument(
            "Real YouTube analysis input is not a regular file");

    const uint64_t source_size =
        std::filesystem::file_size(video_path);
    const std::string source_hash = sha256_file(video_path);
    TestResult probe;
    probe.source = ResultSource::RealYouTubeRoundtrip;
    probe.source_type = "real-youtube-roundtrip";
    probe.source_file_sha256 = source_hash;
    probe.source_file_size = source_size;
    probe.analysis_fingerprint = kAnalysisFingerprint;
    const std::string key = duplicate_key(
        manifest.suite_id, case_id, probe);
    if (!options.record_new_observation) {
        for (const auto &existing : test_case->results) {
            if (duplicate_key(
                    manifest.suite_id, case_id, existing) == key) {
                return {
                    .result = existing,
                    .recorded = false,
                    .duplicate = true,
                    .message =
                        "This video has already been analyzed for "
                        "this case. Observation " +
                        existing.observation_id + " from " +
                        existing.analyzed_at_utc + ": " +
                        to_string(existing.final_status)};
            }
        }
    }

    const auto root = suite_root_from_manifest(manifest_path);
    ensure_suite_directories(root);
    std::string imported_name =
        case_id + "_" + source_hash.substr(0, 12) + "_" +
        video_path.filename().string();
    std::filesystem::path imported =
        root / "imported" / imported_name;
    if (std::filesystem::exists(imported)) {
        try {
            if (sha256_file(imported) != source_hash) {
                imported_name =
                    case_id + "_" + source_hash + "_" +
                    video_path.filename().string();
                imported = root / "imported" / imported_name;
            }
        } catch (...) {
            imported_name =
                case_id + "_" + source_hash + "_" +
                video_path.filename().string();
            imported = root / "imported" / imported_name;
        }
    }
    if (!std::filesystem::exists(imported)) {
        SafeOutputFile safe(imported);
        std::filesystem::copy_file(
            video_path, safe.partial_path(),
            std::filesystem::copy_options::overwrite_existing);
        safe.commit();
    }

    AnalysisOptions record_options = options;
    record_options.source_relative_name =
        normalized_relative(
            std::filesystem::path("imported") /
            imported.filename());
    record_options.imported_at_utc = iso_timestamp();
    record_options.source_file_size = source_size;
    record_options.source_file_sha256 = source_hash;
    record_options.source_file_created_time_utc =
        created_time_utc(video_path);
    record_options.source_file_modified_time_utc =
        modified_time_utc(video_path);
    auto outcome = analyze_case_video_record(
        manifest, *test_case, imported,
        ResultSource::RealYouTubeRoundtrip, {},
        record_options);
    if (outcome.recorded) {
        test_case->state = CaseState::Analyzed;
        (void) backup_before_migration(
            manifest_path, manifest);
        manifest.loaded_schema_version =
            kManifestSchemaVersion;
        write_manifest_atomic(manifest, manifest_path);
        write_reports(manifest, root / "reports");
    }
    return outcome;
}

uint64_t detected_duplicate_observation_count(
    const SuiteManifest &manifest) {
    std::unordered_set<std::string> observed;
    uint64_t duplicates = 0;
    for (const auto &test_case : manifest.cases) {
        for (const auto &result : test_case.results) {
            const auto key = duplicate_key(
                manifest.suite_id,
                test_case.test_case_id, result);
            if (!observed.insert(key).second)
                ++duplicates;
        }
    }
    return duplicates;
}

DeduplicationSummary deduplicate_results(
    const std::filesystem::path &manifest_path,
    const bool apply) {
    SuiteManifest manifest = read_manifest(manifest_path);
    DeduplicationSummary summary;
    std::vector<std::vector<bool>> remove;
    remove.reserve(manifest.cases.size());
    std::unordered_set<std::string> duplicate_groups;
    for (const auto &test_case : manifest.cases)
        remove.emplace_back(test_case.results.size(), false);

    for (std::size_t case_index = 0;
         case_index < manifest.cases.size(); ++case_index) {
        const auto &test_case = manifest.cases[case_index];
        std::map<std::string, std::size_t> canonical;
        for (std::size_t result_index = 0;
             result_index < test_case.results.size();
             ++result_index) {
            ++summary.observations_scanned;
            const auto &result =
                test_case.results[result_index];
            const std::string key = duplicate_key(
                manifest.suite_id,
                test_case.test_case_id, result);
            const auto [it, inserted] =
                canonical.emplace(key, result_index);
            if (inserted) continue;
            std::size_t keep = it->second;
            std::size_t discard = result_index;
            const auto &kept = test_case.results[keep];
            if (result_completeness(result) >
                    result_completeness(kept) ||
                (result_completeness(result) ==
                     result_completeness(kept) &&
                 !result.analyzed_at_utc.empty() &&
                 (kept.analyzed_at_utc.empty() ||
                  result.analyzed_at_utc <
                      kept.analyzed_at_utc))) {
                discard = keep;
                keep = result_index;
                it->second = keep;
            }
            remove[case_index][discard] = true;
            ++summary.duplicate_observations;
            duplicate_groups.insert(key);
            summary.messages.push_back(
                "Case " + test_case.test_case_id +
                ": kept observation " +
                test_case.results[keep].observation_id +
                ", excluded " +
                test_case.results[discard].observation_id);
        }
    }
    summary.duplicate_groups = duplicate_groups.size();
    summary.observations_after_apply =
        summary.observations_scanned -
        summary.duplicate_observations;
    if (!apply || summary.duplicate_observations == 0)
        return summary;

    if (manifest.loaded_schema_version <
        kManifestSchemaVersion) {
        const auto migration_backup =
            backup_manifest(manifest_path, "pre-v3-migration");
        summary.messages.push_back(
            "Schema migration backup: " +
            migration_backup.string());
    }
    summary.backup_path =
        backup_manifest(manifest_path, "pre-deduplicate");
    for (std::size_t case_index = 0;
         case_index < manifest.cases.size(); ++case_index) {
        auto &results = manifest.cases[case_index].results;
        std::vector<TestResult> retained;
        retained.reserve(results.size());
        for (std::size_t i = 0; i < results.size(); ++i)
            if (!remove[case_index][i])
                retained.push_back(std::move(results[i]));
        results = std::move(retained);
    }
    manifest.duplicate_observations_excluded +=
        summary.duplicate_observations;
    manifest.deduplication_log.insert(
        manifest.deduplication_log.end(),
        summary.messages.begin(), summary.messages.end());
    recompute_session_counts(manifest);
    manifest.loaded_schema_version = kManifestSchemaVersion;
    write_manifest_atomic(manifest, manifest_path);
    write_reports(
        manifest,
        suite_root_from_manifest(manifest_path) / "reports");
    summary.applied = true;
    return summary;
}

std::vector<BatchPreviewItem> preview_analysis_folder(
    const SuiteManifest &manifest,
    const std::filesystem::path &folder,
    const std::map<std::string, std::string> &manual_mappings) {
    if (!std::filesystem::is_directory(folder))
        throw std::invalid_argument(
            "Analysis folder does not exist");
    static const std::set<std::string> extensions{
        ".mp4", ".webm", ".mkv", ".mov", ".m4v", ".avi",
        ".mpg", ".mpeg", ".ts", ".m2ts", ".ogv"};
    std::vector<BatchPreviewItem> items;
    for (const auto &entry :
         std::filesystem::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        BatchPreviewItem item;
        item.source_path = entry.path();
        item.filename = entry.path().filename().string();
        item.file_size = entry.file_size();
        const auto mapping =
            manual_mappings.find(item.filename);
        if (mapping != manual_mappings.end())
            item.user_case_id = mapping->second;
        const std::string extension =
            lowercase(entry.path().extension().string());
        if (!extensions.contains(extension)) {
            item.status = BatchMatchStatus::Unsupported;
            item.message = "Unsupported file extension";
            items.push_back(std::move(item));
            continue;
        }
        try {
            item.video = analyze_video(entry.path());
            item.file_sha256 = sha256_file(entry.path());
        } catch (const std::exception &error) {
            item.status = BatchMatchStatus::Unsupported;
            item.message = error.what();
            items.push_back(std::move(item));
            continue;
        }
        if (!item.user_case_id.empty()) {
            const auto exists = std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const TestCase &test_case) {
                    return test_case.test_case_id ==
                        item.user_case_id;
                });
            if (exists != manifest.cases.end())
                item.detected_case_id =
                    item.user_case_id;
            else
                item.message = "Manual mapping references unknown case";
        } else {
            item.detected_case_id =
                case_id_from_filename(
                    manifest, entry.path());
        }
        if (!item.detected_case_id) {
            item.status = BatchMatchStatus::NeedsMapping;
            if (item.message.empty())
                item.message =
                    "No unique case ID found; needs mapping";
        } else {
            item.status = BatchMatchStatus::Matched;
            TestResult probe;
            probe.source = ResultSource::RealYouTubeRoundtrip;
            probe.source_type = "real-youtube-roundtrip";
            probe.source_file_sha256 = item.file_sha256;
            probe.analysis_fingerprint = kAnalysisFingerprint;
            const auto &test_case = *std::find_if(
                manifest.cases.begin(), manifest.cases.end(),
                [&](const TestCase &candidate) {
                    return candidate.test_case_id ==
                        *item.detected_case_id;
                });
            const std::string key = duplicate_key(
                manifest.suite_id, test_case.test_case_id, probe);
            item.duplicate_observation = std::any_of(
                test_case.results.begin(),
                test_case.results.end(),
                [&](const TestResult &existing) {
                    return duplicate_key(
                               manifest.suite_id,
                               test_case.test_case_id,
                               existing) == key;
                });
            if (item.duplicate_observation)
                item.message = "Already analyzed";
        }
        items.push_back(std::move(item));
    }
    std::map<std::string, std::vector<std::size_t>> by_case;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].status == BatchMatchStatus::Matched &&
            items[i].detected_case_id)
            by_case[*items[i].detected_case_id].push_back(i);
    for (const auto &[case_id, indices] : by_case) {
        if (indices.size() <= 1) continue;
        for (const auto index : indices) {
            items[index].status =
                BatchMatchStatus::DuplicateCaseConflict;
            items[index].message =
                "Multiple files map to " + case_id +
                "; needs explicit mapping";
        }
    }
    std::sort(
        items.begin(), items.end(),
        [](const BatchPreviewItem &left,
           const BatchPreviewItem &right) {
            return lowercase(left.filename) <
                lowercase(right.filename);
        });
    return items;
}

BatchAnalysisSummary analyze_folder(
    const std::filesystem::path &manifest_path,
    const std::filesystem::path &folder,
    const std::map<std::string, std::string> &manual_mappings,
    const AnalysisOptions &options,
    const ProgressCallback &progress) {
    SuiteManifest initial = read_manifest(manifest_path);
    const auto preview = preview_analysis_folder(
        initial, folder, manual_mappings);
    BatchAnalysisSummary summary;
    summary.discovered = preview.size();
    const std::string session_id =
        options.analysis_session_id.empty()
            ? create_record_id("session")
            : options.analysis_session_id;
    summary.analysis_session_id = session_id;
    std::size_t completed = 0;
    for (const auto &item : preview) {
        if (!invoke_progress(
                progress,
                {completed, preview.size(), item.filename, 0.0})) {
            summary.cancelled = true;
            break;
        }
        if (item.status == BatchMatchStatus::Unsupported) {
            ++summary.unsupported;
        } else if (item.status != BatchMatchStatus::Matched ||
                   !item.detected_case_id) {
            ++summary.needs_mapping;
        } else if (item.duplicate_observation &&
                   !options.record_new_observation) {
            ++summary.duplicates_skipped;
        } else {
            AnalysisOptions item_options = options;
            item_options.analysis_session_id = session_id;
            item_options.session_label =
                options.session_label.empty()
                    ? "Folder analysis" : options.session_label;
            item_options.source_folder =
                std::filesystem::absolute(folder).string();
            const auto outcome = analyze_real_video(
                manifest_path, *item.detected_case_id,
                item.source_path, item_options);
            if (outcome.recorded) ++summary.analyzed;
            else if (outcome.duplicate)
                ++summary.duplicates_skipped;
        }
        ++completed;
        (void) invoke_progress(
            progress,
            {completed, preview.size(), item.filename, 1.0});
    }
    return summary;
}

namespace {

void generate_case_artifacts(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &root,
    const std::function<bool(double)> &case_progress = {}) {
    const auto payload =
        resolve_suite_path(root, test_case.payload_path);
    const auto master =
        resolve_suite_path(root, test_case.master_video_path);
    const auto candidate =
        resolve_suite_path(root, test_case.upload_candidate_path);
    test_case.candidate_ready_for_youtube = false;
    test_case.candidate_duration_validation_known = false;
    test_case.candidate_validation_error.clear();

    generate_case_payload(payload, test_case);
    test_case.input_sha256 = sha256_file(payload);
    const auto stats = encode_master(
        payload, master, test_case.repair_percentage,
        test_case.video, case_progress);
    test_case.source_packet_count = stats.source_packets;
    test_case.repair_packet_count = stats.repair_packets;
    test_case.total_packet_count = stats.total_packets;
    test_case.encoded_frame_count = stats.frames;
    test_case.actual_master_frames = stats.frames;
    test_case.master_encode_seconds = stats.seconds;
    if (stats.frames < test_case.minimum_required_frames)
        throw std::runtime_error(
            "Master has fewer than the required real data frames");
    test_case.master_video_size =
        std::filesystem::file_size(master);
    test_case.master_video_sha256 = sha256_file(master);
    auto master_result = analyze_case_video(
        manifest, test_case, master,
        ResultSource::LocalSimulation, "master-lossless");
    test_case.master_duration_seconds =
        master_result.video.duration_seconds;
    test_case.master_decode_success =
        master_result.decode_completed &&
        master_result.sha256_match;
    if (!test_case.master_decode_success)
        throw std::runtime_error(
            "Lossless master failed local SHA-256 recovery");

    auto upload =
        *find_simulation_profile("yt-sim-4k-medium");
    upload.name = "youtube-upload-candidate";
    upload.width = test_case.video.width;
    upload.height = test_case.video.height;
    upload.fps = test_case.video.fps;
    upload.scale = false;
    upload.crf = 14;
    const auto transcode_started = Clock::now();
    transcode_h264(
        master, candidate, upload, manifest.suite_id,
        test_case.test_case_id,
        test_case.minimum_required_frames,
        test_case.minimum_duration_seconds);
    test_case.upload_candidate_transcode_seconds =
        std::chrono::duration<double>(
            Clock::now() - transcode_started).count();
    const auto validation = validate_upload_candidate(
        candidate, test_case.video,
        test_case.minimum_required_frames,
        test_case.minimum_duration_seconds);
    test_case.candidate_duration_validation_known = true;
    test_case.actual_candidate_frames =
        static_cast<uint64_t>(
            std::max<int64_t>(0, validation.decoded_frame_count));
    test_case.candidate_duration_seconds =
        validation.video.duration_seconds;
    test_case.candidate_timestamps_valid =
        validation.timestamps_present &&
        validation.pts_monotonic &&
        validation.dts_monotonic &&
        validation.last_timestamp_valid;
    test_case.candidate_validation_error =
        validation.error;
    test_case.upload_candidate_size =
        std::filesystem::file_size(candidate);
    test_case.upload_candidate_sha256 =
        sha256_file(candidate);
    auto candidate_result = analyze_case_video(
        manifest, test_case, candidate,
        ResultSource::LocalSimulation, upload.name);
    test_case.results.back().elapsed_transcode_seconds =
        test_case.upload_candidate_transcode_seconds;
    test_case.upload_candidate_decode_success =
        candidate_result.decode_completed;
    test_case.upload_candidate_sha256_match =
        candidate_result.sha256_match;
    test_case.candidate_ready_for_youtube =
        validation.passed &&
        test_case.upload_candidate_decode_success &&
        test_case.upload_candidate_sha256_match;
    if (!test_case.candidate_ready_for_youtube) {
        if (test_case.candidate_validation_error.empty()) {
            test_case.candidate_validation_error =
                !test_case.upload_candidate_decode_success
                    ? "Candidate decode failed"
                    : "SHA-256 mismatch";
        }
        std::error_code remove_error;
        std::filesystem::remove(candidate, remove_error);
        test_case.upload_candidate_size = 0;
        test_case.upload_candidate_sha256.clear();
        test_case.notes =
            test_case.candidate_validation_error +
            "; candidate was not retained as a final upload file.";
        throw std::runtime_error(test_case.notes);
    }
    test_case.notes =
        "Ready for YouTube - 60+ frames, 2.0+ seconds, "
        "local SHA-256 passed";
}

} // namespace

SuiteManifest create_suite(
    const std::filesystem::path &output_root,
    const std::string &preset,
    const MatrixOptions &matrix,
    const bool allow_low_disk,
    const ProgressCallback &progress) {
    const std::string suite_id = create_suite_id();
    const std::filesystem::path root =
        output_root / "youtube_test_lab" / suite_id;
    ensure_suite_directories(root);
    SuiteManifest manifest{
        .schema_version = kManifestSchemaVersion,
        .vidstorex_version = "1.4.0",
        .suite_id = suite_id,
        .created_at = iso_timestamp(),
        .preset = preset,
        .cases = build_matrix(matrix, suite_id)};
    const auto preflight = estimate_suite(manifest.cases, root);
    if (!allow_low_disk && !preflight.disk_space_sufficient)
        throw std::runtime_error(
            "Insufficient disk space for Test Lab suite");
    const auto manifest_path = root / "manifest.json";
    write_manifest_atomic(manifest, manifest_path);

    for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
        auto &c = manifest.cases[i];
        if (!invoke_progress(progress,
            {i, manifest.cases.size(), c.test_case_id, 0.0}))
            break;
        c.state = CaseState::Generating;
        write_manifest_atomic(manifest, manifest_path);
        try {
            generate_case_artifacts(
                manifest, c, root,
                [&](const double value) {
                    return invoke_progress(progress,
                        {i, manifest.cases.size(),
                         c.test_case_id, value * 0.7});
                });
            c.state = c.candidate_ready_for_youtube
                ? CaseState::WaitingForManualUpload
                : CaseState::Failed;
        } catch (const std::exception &error) {
            c.state = CaseState::Failed;
            c.notes = error.what();
            if (c.candidate_validation_error.empty())
                c.candidate_validation_error = error.what();
            c.candidate_ready_for_youtube = false;
        }
        write_manifest_atomic(manifest, manifest_path);
        (void) invoke_progress(progress,
            {i + 1, manifest.cases.size(), c.test_case_id, 1.0});
    }
    write_reports(manifest, root / "reports");
    return manifest;
}

void resume_suite(const std::filesystem::path &manifest_path,
                  const bool allow_low_disk,
                  const ProgressCallback &progress) {
    SuiteManifest manifest = read_manifest(manifest_path);
    const auto root = suite_root_from_manifest(manifest_path);
    ensure_suite_directories(root);
    const auto preflight = estimate_suite(manifest.cases, root);
    if (!allow_low_disk && !preflight.disk_space_sufficient)
        throw std::runtime_error(
            "Insufficient disk space for suite resume");
    std::size_t completed = 0;
    for (const auto &c : manifest.cases)
        if ((c.state == CaseState::WaitingForManualUpload ||
            c.state == CaseState::Analyzed)
            && c.candidate_duration_validation_known
            && c.candidate_ready_for_youtube)
            ++completed;
    for (auto &c : manifest.cases) {
        if (((c.state == CaseState::WaitingForManualUpload ||
              c.state == CaseState::Analyzed) &&
             c.candidate_duration_validation_known &&
             c.candidate_ready_for_youtube) ||
            c.state == CaseState::Skipped)
            continue;
        if (!invoke_progress(progress,
            {completed, manifest.cases.size(), c.test_case_id, 0.0}))
            break;
        c.state = CaseState::Pending;
        write_manifest_atomic(manifest, manifest_path);
        try {
            generate_case_artifacts(manifest, c, root);
            c.state = c.candidate_ready_for_youtube
                ? CaseState::WaitingForManualUpload
                : CaseState::Failed;
        } catch (const std::exception &error) {
            c.state = CaseState::Failed;
            c.notes = error.what();
            if (c.candidate_validation_error.empty())
                c.candidate_validation_error = error.what();
            c.candidate_ready_for_youtube = false;
        }
        ++completed;
        write_manifest_atomic(manifest, manifest_path);
    }
    write_reports(manifest, root / "reports");
}

void simulate_suite(const std::filesystem::path &manifest_path,
                    const SimulationProfile &profile,
                    const ProgressCallback &progress) {
    SuiteManifest manifest = read_manifest(manifest_path);
    const auto root = suite_root_from_manifest(manifest_path);
    const auto simulation_dir =
        root / "generated" / "simulations" / profile.name;
    std::filesystem::create_directories(simulation_dir);
    for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
        auto &c = manifest.cases[i];
        if (!invoke_progress(progress,
            {i, manifest.cases.size(), c.test_case_id, 0.0}))
            break;
        const auto master =
            resolve_suite_path(root, c.master_video_path);
        if (!std::filesystem::exists(master)) {
            c.notes = "Simulation skipped: master video missing";
            continue;
        }
        c.state = CaseState::Simulating;
        write_manifest_atomic(manifest, manifest_path);
        try {
            const auto output =
                simulation_dir /
                (c.test_case_id + "_" + profile.name + ".mp4");
            const auto transcode_started = Clock::now();
            transcode_h264(master, output, profile,
                           manifest.suite_id, c.test_case_id);
            const double transcode_seconds =
                std::chrono::duration<double>(
                    Clock::now() - transcode_started).count();
            const auto simulation_result = analyze_case_video(
                manifest, c, output,
                ResultSource::LocalSimulation, profile.name);
            c.results.back().elapsed_transcode_seconds =
                transcode_seconds;
            (void) simulation_result;
            c.state = CaseState::WaitingForManualUpload;
        } catch (const std::exception &error) {
            TestResult failed;
            failed.test_case_id = c.test_case_id;
            failed.source = ResultSource::LocalSimulation;
            failed.simulation_profile = profile.name;
            failed.failure_stage = "transcode";
            failed.error_message = error.what();
            failed.final_status = FinalStatus::DecodeFailed;
            c.results.push_back(std::move(failed));
            c.state = CaseState::Failed;
        }
        write_manifest_atomic(manifest, manifest_path);
        (void) invoke_progress(progress,
            {i + 1, manifest.cases.size(), c.test_case_id, 1.0});
    }
    write_reports(manifest, root / "reports");
}

void write_reports(const SuiteManifest &manifest,
                   const std::filesystem::path &reports_directory) {
    std::filesystem::create_directories(reports_directory);
    struct ObservationView {
        const TestCase *test_case = nullptr;
        const TestResult *result = nullptr;
    };
    std::vector<ObservationView> observations;
    std::set<std::string> seen;
    uint64_t currently_excluded = 0;
    for (const auto &test_case : manifest.cases) {
        for (const auto &result : test_case.results) {
            const auto key = duplicate_key(
                manifest.suite_id, test_case.test_case_id, result);
            if (!seen.insert(key).second) {
                ++currently_excluded;
                continue;
            }
            observations.push_back({&test_case, &result});
        }
    }
    std::stable_sort(
        observations.begin(), observations.end(),
        [](const ObservationView &left,
           const ObservationView &right) {
            return left.result->analyzed_at_utc <
                right.result->analyzed_at_utc;
        });
    uint64_t exact_passes = 0;
    uint64_t decode_failures = 0;
    uint64_t sha_mismatches = 0;
    uint64_t local_observations = 0;
    uint64_t real_observations = 0;
    uint64_t real_passes = 0;
    std::set<std::string> unique_cases;
    std::set<std::string> real_cases;
    std::set<std::string> real_passed_cases;
    std::set<std::string> session_ids;
    std::map<std::string, std::pair<uint64_t, uint64_t>>
        by_reliability, by_resolution, by_codec;
    const auto codec_bucket = [](const std::string &codec) {
        const auto value = lowercase(codec);
        if (value == "h264" || value == "avc") return std::string("H.264");
        if (value == "vp9") return std::string("VP9");
        if (value == "av1") return std::string("AV1");
        return std::string("Other");
    };
    const auto resolution_bucket = [](const VideoTechnicalInfo &video) {
        if (video.height == 720) return std::string("720p");
        if (video.height == 1080) return std::string("1080p");
        if (video.height == 1440) return std::string("1440p");
        if (video.height == 2160) return std::string("2160p");
        return std::string("other");
    };
    for (const auto &view : observations) {
        const auto &test_case = *view.test_case;
        const auto &result = *view.result;
        const bool passed =
            result.final_status == FinalStatus::Pass;
        unique_cases.insert(test_case.test_case_id);
        if (!result.analysis_session_id.empty())
            session_ids.insert(result.analysis_session_id);
        if (passed) ++exact_passes;
        else if (result.decode_completed) ++sha_mismatches;
        else ++decode_failures;
        if (result.source == ResultSource::LocalSimulation) {
            ++local_observations;
        } else {
            ++real_observations;
            real_cases.insert(test_case.test_case_id);
            if (passed) {
                ++real_passes;
                real_passed_cases.insert(test_case.test_case_id);
            }
        }
        auto &reliability =
            by_reliability[test_case.reliability_profile];
        ++reliability.second;
        if (passed) ++reliability.first;
        auto &resolution = by_resolution[
            resolution_bucket(result.video)];
        ++resolution.second;
        if (passed) ++resolution.first;
        auto &codec = by_codec[codec_bucket(result.video.codec)];
        ++codec.second;
        if (passed) ++codec.first;
    }
    const uint64_t duplicate_count =
        manifest.duplicate_observations_excluded +
        currently_excluded;
    std::map<std::string, std::string> session_labels;
    for (const auto &session : manifest.analysis_sessions)
        session_labels[session.analysis_session_id] =
            session.label.empty() ? session.analysis_session_id
                                  : session.label;
    const auto session_name = [&](const TestResult &result) {
        const auto it =
            session_labels.find(result.analysis_session_id);
        return it == session_labels.end()
            ? result.analysis_session_id : it->second;
    };
    const auto selected_bitrate =
        [](const VideoTechnicalInfo &video) {
            if (video.stream_bitrate > 0)
                return video.stream_bitrate;
            if (video.container_bitrate > 0)
                return video.container_bitrate;
            return video.calculated_bitrate;
        };
    const auto write_atomic_text =
        [](const std::filesystem::path &path,
           const std::function<void(std::ostream &)> &writer) {
            SafeOutputFile safe(path);
            std::ofstream out(safe.partial_path(), std::ios::binary);
            if (!out) throw std::runtime_error(
                "Could not create report");
            writer(out);
            out.flush();
            if (!out) throw std::runtime_error(
                "Could not write report");
            out.close();
            safe.commit();
        };
    write_atomic_text(reports_directory / "report.csv",
        [&](std::ostream &out) {
            out << "Observation ID,Case,Source,Session,Analyzed At,"
                   "Imported At,File Timestamp,Returned File,Container,"
                   "Codec,Profile,Codec Tag,Pixel Format,Resolution,DAR,"
                   "FPS,Time Base,Duration,Decoded Frames,Stream Bitrate,"
                   "Container Bitrate,Calculated Bitrate,Bitrate Source,"
                   "Returned Size,Packet Recovery,Restored SHA-256,"
                   "Exact Match,Failure Stage,Error,Result\n";
            for (const auto &view : observations) {
                const auto &c = *view.test_case;
                const auto &r = *view.result;
                const auto file_timestamp =
                    !r.source_file_modified_time_utc.empty()
                        ? r.source_file_modified_time_utc
                        : r.source_file_created_time_utc;
                out << csv_escape(r.observation_id) << ","
                    << csv_escape(c.test_case_id) << ","
                    << csv_escape(r.source_type.empty()
                            ? to_string(r.source) : r.source_type) << ","
                    << csv_escape(session_name(r)) << ","
                    << csv_escape(r.analyzed_at_utc) << ","
                    << csv_escape(r.imported_at_utc) << ","
                    << csv_escape(file_timestamp) << ","
                    << csv_escape(r.source_file_relative_name) << ","
                    << csv_escape(r.video.container) << ","
                    << csv_escape(r.video.codec) << ","
                    << csv_escape(r.video.profile) << ","
                    << csv_escape(r.video.codec_tag) << ","
                    << csv_escape(r.video.pixel_format) << ","
                    << r.video.width << "x" << r.video.height << ","
                    << csv_escape(r.video.display_aspect_ratio) << ","
                    << r.video.fps << ","
                    << csv_escape(r.video.time_base) << ","
                    << r.video.duration_seconds << ","
                    << r.decoded_frame_count << ","
                    << r.video.stream_bitrate << ","
                    << r.video.container_bitrate << ","
                    << r.video.calculated_bitrate << ","
                    << csv_escape(r.video.bitrate_source) << ","
                    << r.source_file_size << ","
                    << r.packet_recovery_percentage << ","
                    << csv_escape(r.restored_sha256) << ","
                    << (r.sha256_match ? "Yes" : "No") << ","
                    << csv_escape(r.failure_stage) << ","
                    << csv_escape(r.error_message) << ","
                    << csv_escape(to_string(r.final_status)) << "\n";
            }
        });
    write_atomic_text(reports_directory / "report.md",
        [&](std::ostream &out) {
            out << "# VidStoreX YouTube Test Lab Report\n\n"
                << "Suite: `" << manifest.suite_id << "`  \n"
                << "Created: " << manifest.created_at << "  \n"
                << "Preset: " << manifest.preset << "\n\n"
                << "> Local simulation is not a guaranteed copy of "
                   "YouTube processing. Real YouTube roundtrips are "
                   "reported as a separate source.\n\n"
                << "## Summary\n\n"
                << "- Unique observations: " << observations.size() << "\n"
                << "- Unique cases: " << unique_cases.size() << "\n"
                << "- SHA-256 exact passes: " << exact_passes << "\n"
                << "- SHA-256 mismatches: " << sha_mismatches << "\n"
                << "- Decode failures: " << decode_failures << "\n"
                << "- Local simulation observations: "
                << local_observations << "\n"
                << "- Real YouTube observations passed: "
                << real_passes << "/" << real_observations
                << (real_observations == 0
                        ? " (no manual results yet)" : "")
                << "\n"
                << "- Real YouTube unique cases passed: "
                << real_passed_cases.size() << "/"
                << real_cases.size() << "\n"
                << "- Duplicate observations excluded: "
                << duplicate_count << "\n"
                << "- Analysis sessions: " << session_ids.size()
                << "\n\n"
                << "## Upload candidates\n\n"
                << "| Case | Resolution | Repair | Frame capacity | "
                   "Requested | Effective | Packets (S/R/T) | "
                   "Frames (min/expected/master/candidate) | "
                   "Duration (master/candidate) | YouTube ready | "
                   "Validation |\n"
                << "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
                   ":---:|---|\n";
            for (const auto &c : manifest.cases)
                out << "| " << c.test_case_id << " | "
                    << c.video.width << "x" << c.video.height << " | "
                    << c.repair_percentage << "% | "
                    << c.frame_payload_capacity << " packets | "
                    << human_size(c.requested_input_size) << " | "
                    << human_size(c.effective_input_size)
                    << (c.payload_extended_for_duration
                            ? " (extended)" : "") << " | "
                    << c.source_packet_count << "/"
                    << c.repair_packet_count << "/"
                    << c.total_packet_count << " | "
                    << c.minimum_required_frames << "/"
                    << c.expected_encoded_frames << "/"
                    << c.actual_master_frames << "/"
                    << c.actual_candidate_frames << " | "
                    << std::fixed << std::setprecision(3)
                    << c.master_duration_seconds << "/"
                    << c.candidate_duration_seconds << " s | "
                    << (c.candidate_ready_for_youtube
                            ? "Yes" : "No") << " | "
                    << (c.candidate_validation_error.empty()
                            ? "-" : c.candidate_validation_error)
                    << " |\n";
            out << "\n## Observed decode results\n\n"
                << "| Case | Source | Session | Analyzed at | File timestamp | "
                   "Codec | Resolution | FPS | Bitrate | Returned size | "
                   "Packet recovery | SHA-256 | Result |\n"
                << "|---|---|---|---|---|---|---:|---:|---:|---:|"
                   "---:|:---:|---|\n";
            for (const auto &view : observations) {
                const auto &c = *view.test_case;
                const auto &r = *view.result;
                const auto file_timestamp =
                    !r.source_file_modified_time_utc.empty()
                        ? r.source_file_modified_time_utc
                        : r.source_file_created_time_utc;
                out << "| " << c.test_case_id << " | "
                        << (r.source_type.empty()
                                ? to_string(r.source) : r.source_type)
                        << (r.simulation_profile.empty()
                                ? "" : " (" + r.simulation_profile + ")")
                        << " | " << session_name(r)
                        << " | " << r.analyzed_at_utc
                        << " | " << file_timestamp
                        << " | " << (r.video.codec.empty()
                                ? "unavailable" : r.video.codec)
                        << " | " << r.video.width << "x"
                        << r.video.height
                        << " | " << std::fixed << std::setprecision(3)
                        << r.video.fps
                        << " | " << selected_bitrate(r.video)
                        << " (" << r.video.bitrate_source << ")"
                        << " | " << human_size(r.source_file_size)
                        << " | "
                        << std::fixed << std::setprecision(2)
                        << r.packet_recovery_percentage << "% | "
                        << (r.sha256_match ? "Yes" : "No") << " | "
                        << to_string(r.final_status) << " |\n";
            }
            out << "\n## Success rate by reliability\n\n";
            for (const auto &[name, counts] : by_reliability)
                out << "- " << name << ": " << counts.first << "/"
                    << counts.second << " observed exact passes\n";
            out << "\n## Success rate by resolution\n\n";
            for (const auto &[name, counts] : by_resolution)
                out << "- " << name << ": " << counts.first << "/"
                    << counts.second << " observed exact passes\n";
            out << "\n## Success rate by codec\n\n";
            for (const auto &name :
                 {"H.264", "VP9", "AV1", "Other"}) {
                const auto it = by_codec.find(name);
                const auto counts = it == by_codec.end()
                    ? std::pair<uint64_t, uint64_t>{0, 0}
                    : it->second;
                out << "- " << name << ": " << counts.first << "/"
                    << counts.second << " observed exact passes\n";
            }
            out << "\nThese observations are not a performance "
                   "guarantee; results vary with environment and "
                   "YouTube processing.\n";
        });
    write_atomic_text(reports_directory / "report.json",
        [&](std::ostream &out) {
            out << "{\n  \"schema_version\": "
                << kManifestSchemaVersion << ",\n"
                << "  \"suite_id\": " << q(manifest.suite_id) << ",\n"
                << "  \"summary\": {\n"
                << "    \"unique_observations\": "
                << observations.size() << ",\n"
                << "    \"unique_cases\": " << unique_cases.size()
                << ",\n"
                << "    \"exact_passes\": " << exact_passes << ",\n"
                << "    \"decode_failures\": " << decode_failures
                << ",\n"
                << "    \"sha256_mismatches\": " << sha_mismatches
                << ",\n"
                << "    \"local_simulation_observations\": "
                << local_observations << ",\n"
                << "    \"real_youtube_observations\": "
                << real_observations << ",\n"
                << "    \"real_youtube_observation_passes\": "
                << real_passes << ",\n"
                << "    \"real_youtube_unique_cases\": "
                << real_cases.size() << ",\n"
                << "    \"real_youtube_unique_case_passes\": "
                << real_passed_cases.size() << ",\n"
                << "    \"accidental_duplicates_excluded\": "
                << duplicate_count << ",\n"
                << "    \"analysis_sessions\": "
                << session_ids.size() << "\n"
                << "  },\n"
                << "  \"codec_summary\": {";
            bool first_summary = true;
            for (const auto &name :
                 {"H.264", "VP9", "AV1", "Other"}) {
                if (!first_summary) out << ",";
                const auto it = by_codec.find(name);
                const auto counts = it == by_codec.end()
                    ? std::pair<uint64_t, uint64_t>{0, 0}
                    : it->second;
                out << "\n    " << q(name) << ": {\"passed\": "
                    << counts.first << ", \"observations\": "
                    << counts.second << "}";
                first_summary = false;
            }
            out << "\n  },\n"
                << "  \"resolution_summary\": {";
            first_summary = true;
            for (const auto &name :
                 {"720p", "1080p", "1440p", "2160p", "other"}) {
                if (!first_summary) out << ",";
                const auto it = by_resolution.find(name);
                const auto counts = it == by_resolution.end()
                    ? std::pair<uint64_t, uint64_t>{0, 0}
                    : it->second;
                out << "\n    " << q(name) << ": {\"passed\": "
                    << counts.first << ", \"observations\": "
                    << counts.second << "}";
                first_summary = false;
            }
            out << "\n  },\n"
                << "  \"cases\": [";
            for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
                if (i != 0) out << ",";
                out << "\n    ";
                auto report_case = manifest.cases[i];
                report_case.results.clear();
                write_case(out, report_case, "    ");
            }
            if (!manifest.cases.empty()) out << "\n";
            out << "  ],\n"
                << "  \"results\": [";
            bool first = true;
            for (const auto &view : observations) {
                    if (!first) out << ",";
                    out << "\n    ";
                    write_result(out, *view.result, "    ");
                    first = false;
            }
            if (!first) out << "\n";
            out << "  ]\n}\n";
        });
}

std::optional<std::string> case_id_from_filename(
    const SuiteManifest &manifest,
    const std::filesystem::path &video_path) {
    const std::string name =
        lowercase(video_path.filename().string());
    std::optional<std::string> match;
    for (const auto &c : manifest.cases) {
        const std::string id = lowercase(c.test_case_id);
        std::size_t position = name.find(id);
        bool found = false;
        while (position != std::string::npos) {
            const bool left_boundary =
                position == 0 ||
                !std::isalnum(static_cast<unsigned char>(
                    name[position - 1]));
            const std::size_t end = position + id.size();
            const bool right_boundary =
                end == name.size() ||
                !std::isalnum(static_cast<unsigned char>(
                    name[end]));
            if (left_boundary && right_boundary) {
                found = true;
                break;
            }
            position = name.find(id, position + 1);
        }
        if (found) {
            if (match) return std::nullopt;
            match = c.test_case_id;
        }
    }
    return match;
}

std::string to_string(const BatchMatchStatus value) {
    switch (value) {
        case BatchMatchStatus::Matched:
            return "Matched";
        case BatchMatchStatus::NeedsMapping:
            return "Needs mapping";
        case BatchMatchStatus::DuplicateCaseConflict:
            return "Needs mapping - duplicate case conflict";
        case BatchMatchStatus::Unsupported:
            return "Unsupported";
    }
    return "Needs mapping";
}

} // namespace youtube_test_lab
