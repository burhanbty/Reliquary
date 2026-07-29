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
    const auto result = std::filesystem::weakly_canonical(root / rel);
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
    const std::string &case_id) {
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
        encoder->max_b_frames = 2;
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
        out_stream->time_base = encoder->time_base;

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
        << indent << "  \"pixel_format\": "
        << q(video.pixel_format) << ",\n"
        << indent << "  \"width\": " << video.width << ",\n"
        << indent << "  \"height\": " << video.height << ",\n"
        << indent << "  \"fps\": " << video.fps << ",\n"
        << indent << "  \"frame_count\": " << video.frame_count << ",\n"
        << indent << "  \"duration_seconds\": "
        << video.duration_seconds << ",\n"
        << indent << "  \"bitrate\": " << video.bitrate << ",\n"
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
        << indent << "  \"payload_seed\": "
        << q(std::to_string(c.payload_seed)) << ",\n"
        << indent << "  \"input_sha256\": " << q(c.input_sha256) << ",\n"
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
        << indent << "    \"container\": " << q(c.video.container) << "\n"
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
    v.pixel_format = j.at("pixel_format").string();
    v.width = j.at("width").integer();
    v.height = j.at("height").integer();
    v.fps = j.at("fps").number();
    v.frame_count = static_cast<int64_t>(
        j.at("frame_count").number());
    v.duration_seconds = j.at("duration_seconds").number();
    v.bitrate = static_cast<int64_t>(j.at("bitrate").number());
    v.file_size = j.at("file_size").u64();
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
    c.payload_seed = std::stoull(j.at("payload_seed").string());
    c.input_sha256 = j.at("input_sha256").string();
    c.source_packet_count = j.at("source_packet_count").u64();
    c.repair_packet_count = j.at("repair_packet_count").u64();
    c.total_packet_count = j.at("total_packet_count").u64();
    c.encoded_frame_count = j.at("encoded_frame_count").u64();
    const auto &video = j.at("video");
    c.video.width = video.at("width").integer();
    c.video.height = video.at("height").integer();
    c.video.fps = video.at("fps").integer();
    c.video.codec = video.at("codec").string();
    c.video.container = video.at("container").string();
    if (!c.video.valid())
        throw std::runtime_error(
            "Invalid resilient video configuration in manifest");
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
    c.candidate_ready_for_youtube =
        j.at("candidate_ready_for_youtube").boolean();
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
        .fps = FRAME_FPS
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
        .fps = FRAME_FPS
    };
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
                .fps = options.fps};
            if (!video.valid())
                throw std::invalid_argument(
                    "Resolution must be positive and divisible by 8");
            for (const auto [type, size] : variants) {
                    if (size == 0 || size > 64 * MiB)
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
                    c.vidstorex_version = "1.3.0";
                    c.reliability_profile =
                        profile_for_repair(repair);
                    c.repair_percentage = repair;
                    c.input_data_type = type;
                    c.input_size = size;
                    c.payload_seed =
                        kDefaultPayloadSeed + sequence;
                    c.video = video;
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
            c.input_size, false,
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
                : conservative_master * 2 + c.input_size;
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

std::string create_suite_id() {
    static std::atomic<uint64_t> sequence{0};
    std::ostringstream out;
    out << compact_timestamp() << "-"
        << std::hex << std::setw(4) << std::setfill('0')
        << (sequence.fetch_add(1) & 0xffff);
    return out.str();
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
    manifest.schema_version = root.at("schema_version").integer();
    if (manifest.schema_version != kManifestSchemaVersion)
        throw std::runtime_error(
            "Unsupported manifest schema version: " +
            std::to_string(manifest.schema_version));
    manifest.vidstorex_version =
        root.at("vidstorex_version").string();
    manifest.suite_id = root.at("suite_id").string();
    manifest.created_at = root.at("created_at").string();
    manifest.preset = root.at("preset").string();
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
        result.bitrate =
            parameters->bit_rate > 0 ? parameters->bit_rate
                                     : format->bit_rate;
        result.file_size = std::filesystem::file_size(path);
        avformat_close_input(&format);
        return result;
    } catch (...) {
        if (format) avformat_close_input(&format);
        throw;
    }
}

TestResult analyze_case_video(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    const ResultSource source,
    const std::string &simulation_profile) {
    const auto started = Clock::now();
    TestResult result;
    result.test_case_id = test_case.test_case_id;
    result.source = source;
    result.simulation_profile = simulation_profile;
    const auto suite_root = std::filesystem::current_path();
    try {
        result.analyzed_video = video_path.filename().string();
        result.video = analyze_video(video_path);
        result.downloaded_video_size = result.video.file_size;
    } catch (const std::exception &error) {
        result.failure_stage = "video-analysis";
        result.error_message = error.what();
        result.final_status =
            FinalStatus::UnsupportedProcessedVideo;
        result.elapsed_decode_seconds =
            std::chrono::duration<double>(
                Clock::now() - started).count();
        test_case.results.push_back(result);
        return result;
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
            result.sha256_match =
                sha256_file(restored) == test_case.input_sha256;
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
    test_case.results.push_back(result);
    return result;
}

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
        .vidstorex_version = "1.3.0",
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

    const auto candidate_profile =
        *find_simulation_profile("yt-sim-4k-medium");
    for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
        auto &c = manifest.cases[i];
        if (!invoke_progress(progress,
            {i, manifest.cases.size(), c.test_case_id, 0.0}))
            break;
        c.state = CaseState::Generating;
        write_manifest_atomic(manifest, manifest_path);
        try {
            const auto payload =
                resolve_suite_path(root, c.payload_path);
            const auto master =
                resolve_suite_path(root, c.master_video_path);
            const auto candidate =
                resolve_suite_path(root, c.upload_candidate_path);
            generate_payload(payload, c.input_data_type,
                             c.input_size, c.payload_seed);
            c.input_sha256 = sha256_file(payload);
            const auto stats = encode_master(
                payload, master, c.repair_percentage, c.video,
                [&](const double value) {
                    return invoke_progress(progress,
                        {i, manifest.cases.size(),
                         c.test_case_id, value * 0.7});
                });
            c.source_packet_count = stats.source_packets;
            c.repair_packet_count = stats.repair_packets;
            c.total_packet_count = stats.total_packets;
            c.encoded_frame_count = stats.frames;
            c.master_encode_seconds = stats.seconds;
            c.master_video_size =
                std::filesystem::file_size(master);
            c.master_video_sha256 = sha256_file(master);
            auto master_result = analyze_case_video(
                manifest, c, master,
                ResultSource::LocalSimulation, "master-lossless");
            c.master_decode_success =
                master_result.decode_completed &&
                master_result.sha256_match;

            SimulationProfile upload = candidate_profile;
            upload.name = "youtube-upload-candidate";
            upload.width = c.video.width;
            upload.height = c.video.height;
            upload.scale = false;
            upload.crf = 14;
            const auto transcode_started = Clock::now();
            transcode_h264(master, candidate, upload,
                           manifest.suite_id, c.test_case_id);
            c.upload_candidate_transcode_seconds =
                std::chrono::duration<double>(
                    Clock::now() - transcode_started).count();
            c.upload_candidate_size =
                std::filesystem::file_size(candidate);
            c.upload_candidate_sha256 = sha256_file(candidate);
            auto candidate_result = analyze_case_video(
                manifest, c, candidate,
                ResultSource::LocalSimulation, upload.name);
            c.results.back().elapsed_transcode_seconds =
                c.upload_candidate_transcode_seconds;
            c.upload_candidate_decode_success =
                candidate_result.decode_completed;
            c.upload_candidate_sha256_match =
                candidate_result.sha256_match;
            c.candidate_ready_for_youtube =
                c.upload_candidate_decode_success &&
                c.upload_candidate_sha256_match;
            c.notes.clear();
            c.state = c.candidate_ready_for_youtube
                ? CaseState::WaitingForManualUpload
                : CaseState::Failed;
            if (!c.candidate_ready_for_youtube)
                c.notes =
                    "Upload candidate failed local exact recovery; "
                    "do not upload as a ready candidate.";
        } catch (const std::exception &error) {
            c.state = CaseState::Failed;
            c.notes = error.what();
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
        if (c.state == CaseState::WaitingForManualUpload ||
            c.state == CaseState::Analyzed)
            ++completed;
    for (auto &c : manifest.cases) {
        if (c.state == CaseState::WaitingForManualUpload ||
            c.state == CaseState::Analyzed ||
            c.state == CaseState::Skipped)
            continue;
        if (!invoke_progress(progress,
            {completed, manifest.cases.size(), c.test_case_id, 0.0}))
            break;
        c.state = CaseState::Pending;
        write_manifest_atomic(manifest, manifest_path);
        try {
            const auto payload =
                resolve_suite_path(root, c.payload_path);
            const auto master =
                resolve_suite_path(root, c.master_video_path);
            const auto candidate =
                resolve_suite_path(root, c.upload_candidate_path);
            generate_payload(payload, c.input_data_type,
                             c.input_size, c.payload_seed);
            c.input_sha256 = sha256_file(payload);
            const auto stats = encode_master(
                payload, master, c.repair_percentage, c.video);
            c.source_packet_count = stats.source_packets;
            c.repair_packet_count = stats.repair_packets;
            c.total_packet_count = stats.total_packets;
            c.encoded_frame_count = stats.frames;
            c.master_encode_seconds = stats.seconds;
            c.master_video_size =
                std::filesystem::file_size(master);
            c.master_video_sha256 = sha256_file(master);
            auto master_result = analyze_case_video(
                manifest, c, master,
                ResultSource::LocalSimulation, "master-lossless");
            c.master_decode_success =
                master_result.decode_completed &&
                master_result.sha256_match;
            auto upload =
                *find_simulation_profile("yt-sim-4k-medium");
            upload.name = "youtube-upload-candidate";
            upload.width = c.video.width;
            upload.height = c.video.height;
            upload.scale = false;
            upload.crf = 14;
            const auto transcode_started = Clock::now();
            transcode_h264(master, candidate, upload,
                           manifest.suite_id, c.test_case_id);
            c.upload_candidate_transcode_seconds =
                std::chrono::duration<double>(
                    Clock::now() - transcode_started).count();
            c.upload_candidate_size =
                std::filesystem::file_size(candidate);
            c.upload_candidate_sha256 = sha256_file(candidate);
            const auto candidate_result = analyze_case_video(
                manifest, c, candidate,
                ResultSource::LocalSimulation, upload.name);
            c.results.back().elapsed_transcode_seconds =
                c.upload_candidate_transcode_seconds;
            c.upload_candidate_decode_success =
                candidate_result.decode_completed;
            c.upload_candidate_sha256_match =
                candidate_result.sha256_match;
            c.candidate_ready_for_youtube =
                candidate_result.decode_completed &&
                candidate_result.sha256_match;
            c.notes.clear();
            c.state = c.candidate_ready_for_youtube
                ? CaseState::WaitingForManualUpload
                : CaseState::Failed;
        } catch (const std::exception &error) {
            c.state = CaseState::Failed;
            c.notes = error.what();
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
            out << "Case,Source,Simulation Profile,Resolution,Repair,"
                   "Input,Returned Video,Valid Packets,Packet Recovery,"
                   "Encode Seconds,Transcode Seconds,Decode Seconds,"
                   "Decode,SHA-256,Result\n";
            for (const auto &c : manifest.cases)
                for (const auto &r : c.results)
                    out << csv_escape(c.test_case_id) << ","
                        << csv_escape(to_string(r.source)) << ","
                        << csv_escape(r.simulation_profile) << ","
                        << c.video.width << "x" << c.video.height << ","
                        << c.repair_percentage << ","
                        << c.input_size << ","
                        << r.downloaded_video_size << ","
                        << r.telemetry.valid_packets << ","
                        << r.packet_recovery_percentage << ","
                        << c.master_encode_seconds << ","
                        << r.elapsed_transcode_seconds << ","
                        << r.elapsed_decode_seconds << ","
                        << (r.decode_completed ? "Yes" : "No") << ","
                        << (r.sha256_match ? "Yes" : "No") << ","
                        << csv_escape(to_string(r.final_status)) << "\n";
        });
    write_atomic_text(reports_directory / "report.md",
        [&](std::ostream &out) {
            uint64_t total = 0, pass = 0, decoded_bad_hash = 0,
                     failed = 0;
            uint64_t local_total = 0, local_pass = 0,
                     real_total = 0, real_pass = 0;
            std::map<std::string, std::pair<uint64_t, uint64_t>>
                by_reliability, by_resolution;
            for (const auto &c : manifest.cases)
                for (const auto &r : c.results) {
                    ++total;
                    const bool ok = r.final_status == FinalStatus::Pass;
                    if (ok) ++pass;
                    else if (r.decode_completed) ++decoded_bad_hash;
                    else ++failed;
                    if (r.source == ResultSource::LocalSimulation) {
                        ++local_total;
                        if (ok) ++local_pass;
                    } else {
                        ++real_total;
                        if (ok) ++real_pass;
                    }
                    auto &rel = by_reliability[c.reliability_profile];
                    ++rel.second;
                    if (ok) ++rel.first;
                    auto &res = by_resolution[resolution_token(
                        c.video.width, c.video.height)];
                    ++res.second;
                    if (ok) ++res.first;
                }
            out << "# VidStoreX YouTube Test Lab Report\n\n"
                << "Suite: `" << manifest.suite_id << "`  \n"
                << "Created: " << manifest.created_at << "  \n"
                << "Preset: " << manifest.preset << "\n\n"
                << "> Local simulation is not a guaranteed copy of "
                   "YouTube processing. Real YouTube roundtrips are "
                   "reported as a separate source.\n\n"
                << "## Summary\n\n"
                << "- Results: " << total << "\n"
                << "- SHA-256 exact passes: " << pass << "\n"
                << "- Decoded but SHA-256 mismatch: "
                << decoded_bad_hash << "\n"
                << "- Decode failures: " << failed << "\n\n"
                << "- Local simulation exact passes: "
                << local_pass << "/" << local_total << "\n"
                << "- Real YouTube roundtrip exact passes: "
                << real_pass << "/" << real_total
                << (real_total == 0
                        ? " (no manual results yet)" : "")
                << "\n\n"
                << "| Case | Source | Resolution | Repair | Input | "
                   "Returned Video | Packet Recovery | SHA-256 | Result |\n"
                << "|---|---|---:|---:|---:|---:|---:|:---:|---|\n";
            for (const auto &c : manifest.cases)
                for (const auto &r : c.results)
                    out << "| " << c.test_case_id << " | "
                        << to_string(r.source)
                        << (r.simulation_profile.empty()
                                ? "" : " (" + r.simulation_profile + ")")
                        << " | " << c.video.width << "x"
                        << c.video.height << " | "
                        << c.repair_percentage << "% | "
                        << human_size(c.input_size) << " | "
                        << human_size(r.downloaded_video_size) << " | "
                        << std::fixed << std::setprecision(2)
                        << r.packet_recovery_percentage << "% | "
                        << (r.sha256_match ? "Yes" : "No") << " | "
                        << to_string(r.final_status) << " |\n";
            out << "\n## Success rate by reliability\n\n";
            for (const auto &[name, counts] : by_reliability)
                out << "- " << name << ": " << counts.first << "/"
                    << counts.second << " observed exact passes\n";
            out << "\n## Success rate by resolution\n\n";
            for (const auto &[name, counts] : by_resolution)
                out << "- " << name << ": " << counts.first << "/"
                    << counts.second << " observed exact passes\n";
            out << "\nThese observations are not a performance "
                   "guarantee; results vary with environment and "
                   "YouTube processing.\n";
        });
    write_atomic_text(reports_directory / "report.json",
        [&](std::ostream &out) {
            out << "{\n  \"schema_version\": "
                << kManifestSchemaVersion << ",\n"
                << "  \"suite_id\": " << q(manifest.suite_id) << ",\n"
                << "  \"results\": [";
            bool first = true;
            for (const auto &c : manifest.cases)
                for (const auto &r : c.results) {
                    if (!first) out << ",";
                    out << "\n    ";
                    write_result(out, r, "    ");
                    first = false;
                }
            if (!first) out << "\n";
            out << "  ]\n}\n";
        });
}

std::optional<std::string> case_id_from_filename(
    const SuiteManifest &manifest,
    const std::filesystem::path &video_path) {
    const std::string name = video_path.filename().string();
    std::optional<std::string> match;
    for (const auto &c : manifest.cases) {
        if (name.find(c.test_case_id) != std::string::npos) {
            if (match) return std::nullopt;
            match = c.test_case_id;
        }
    }
    return match;
}

} // namespace youtube_test_lab
