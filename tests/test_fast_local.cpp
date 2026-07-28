#include "fast_local_codec.h"
#include "fast_local_format.h"
#include "media_storage.h"
#include "video_decoder.h"
#include "video_encoder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("vidstorex-fast-local-test-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path file(
        const std::string &name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path &path,
                 const std::vector<std::byte> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output);
}

std::vector<std::byte> read_bytes(
    const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    EXPECT_TRUE(input);
    return bytes;
}

std::vector<std::byte> random_bytes(const std::size_t size) {
    std::mt19937 generator(0x51f45u);
    std::uniform_int_distribution<unsigned> distribution(0, 255);
    std::vector<std::byte> bytes(size);
    for (auto &value : bytes) {
        value = static_cast<std::byte>(distribution(generator));
    }
    return bytes;
}

ms_encoding_estimate_t estimate_without_probe(
    const std::filesystem::path &input,
    const std::filesystem::path &video,
    const bool encrypted = false,
    const std::string &password = {}) {
    const std::string input_text = input.string();
    const std::string video_text = video.string();
    ms_encode_options_t options{};
    options.input_path = input_text.c_str();
    options.output_path = video_text.c_str();
    options.encrypt = encrypted ? 1 : 0;
    options.password = password.c_str();
    options.password_len = password.size();
    options.encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
    ms_encoding_estimate_t estimate{};
    EXPECT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    EXPECT_TRUE(estimate.can_start_encoding);
    return estimate;
}

ms_result_t encode_api(
    const std::filesystem::path &input,
    const std::filesystem::path &video,
    const bool encrypted = false,
    const std::string &password = {}) {
    const std::string input_text = input.string();
    const std::string video_text = video.string();
    auto estimate = estimate_without_probe(
        input, video, encrypted, password);
    ms_encode_options_t options{};
    options.input_path = input_text.c_str();
    options.output_path = video_text.c_str();
    options.encrypt = encrypted ? 1 : 0;
    options.password = password.c_str();
    options.password_len = password.size();
    options.encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
    options.preflight_estimate = &estimate;
    ms_result_t result{};
    EXPECT_EQ(ms_encode(&options, &result), MS_OK);
    return result;
}

ms_status_t decode_api(
    const std::filesystem::path &video,
    const std::filesystem::path &output,
    const std::string &password = {},
    ms_result_t *result = nullptr) {
    const std::string video_text = video.string();
    const std::string output_text = output.string();
    ms_decode_options_t options{};
    options.input_path = video_text.c_str();
    options.output_path = output_text.c_str();
    options.password = password.c_str();
    options.password_len = password.size();
    return ms_decode(&options, result);
}

void expect_roundtrip(const std::vector<std::byte> &bytes,
                      const bool encrypted = false) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    const auto video = temporary.file("output.mkv");
    const auto restored = temporary.file("restored.bin");
    const std::string password = encrypted ? "test-password" : "";
    write_bytes(input, bytes);
    const auto encoded = encode_api(input, video, encrypted, password);
    EXPECT_EQ(encoded.encoding_mode, MS_ENCODING_MODE_FAST_LOCAL);
    EXPECT_EQ(encoded.total_frames,
              fast_local_frame_count(bytes.size(), encrypted));
    ms_result_t decoded{};
    ASSERT_EQ(decode_api(video, restored, password, &decoded), MS_OK);
    EXPECT_EQ(decoded.encoding_mode, MS_ENCODING_MODE_FAST_LOCAL);
    EXPECT_EQ(read_bytes(restored), bytes);
}
}

TEST(FastLocalFormat, FileHeaderRoundTripAndLittleEndian) {
    FastLocalFileHeader header;
    header.flags = FastLocalEncrypted;
    header.original_size = 0x0102030405060708ull;
    header.total_frames = 3;
    header.plain_frame_capacity = fast_local_plain_capacity(true);
    header.file_id[0] = std::byte{0x7a};
    header.original_sha256.bytes[31] = std::byte{0x55};
    const auto bytes = serialize_fast_local_file_header(header);

    EXPECT_EQ(bytes[32], std::byte{0x08});
    EXPECT_EQ(bytes[39], std::byte{0x01});
    const auto decoded = deserialize_fast_local_file_header(bytes);
    EXPECT_EQ(decoded.original_size, header.original_size);
    EXPECT_EQ(decoded.total_frames, header.total_frames);
    EXPECT_EQ(decoded.file_id, header.file_id);
    EXPECT_EQ(decoded.original_sha256, header.original_sha256);
}

TEST(FastLocalFormat, RejectsInvalidMagicVersionChecksumAndTruncation) {
    FastLocalFileHeader header;
    header.total_frames = 1;
    auto bytes = serialize_fast_local_file_header(header);
    auto changed = bytes;
    changed[0] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(
                     deserialize_fast_local_file_header(changed)),
                 std::runtime_error);
    changed = bytes;
    changed[8] = std::byte{2};
    EXPECT_THROW(static_cast<void>(
                     deserialize_fast_local_file_header(changed)),
                 std::runtime_error);
    changed = bytes;
    changed[32] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(
                     deserialize_fast_local_file_header(changed)),
                 std::runtime_error);
    EXPECT_THROW(
        static_cast<void>(deserialize_fast_local_file_header(
            std::span<const std::byte>(bytes).first(127))),
        std::runtime_error);
}

TEST(FastLocalFormat, FrameHeaderRoundTripAndChecksums) {
    FastLocalFrameHeader header;
    header.frame_index = 2;
    header.total_frames = 4;
    header.payload_length = 123;
    header.plain_length = 103;
    header.payload_checksum = 0x11223344;
    auto bytes = serialize_fast_local_frame_header(header);
    const auto decoded = deserialize_fast_local_frame_header(bytes);
    EXPECT_EQ(decoded.frame_index, 2u);
    EXPECT_EQ(decoded.total_frames, 4u);
    EXPECT_EQ(decoded.payload_length, 123u);
    bytes[12] ^= std::byte{1};
    EXPECT_THROW(static_cast<void>(
                     deserialize_fast_local_frame_header(bytes)),
                 std::runtime_error);
}

TEST(FastLocalFormat, BoundaryFrameCountsAndPadding) {
    const uint64_t capacity = fast_local_plain_capacity(false);
    EXPECT_EQ(fast_local_frame_count(0, false), 1u);
    EXPECT_EQ(fast_local_frame_count(1, false), 1u);
    EXPECT_EQ(fast_local_frame_count(capacity, false), 1u);
    EXPECT_EQ(fast_local_frame_count(capacity + 1, false), 2u);
    EXPECT_EQ(fast_local_padding_bytes(capacity, false), 0u);
    EXPECT_EQ(fast_local_padding_bytes(capacity + 1, false),
              capacity - 1);
    EXPECT_EQ(fast_local_plain_capacity(true) +
                  FAST_LOCAL_CRYPTO_OVERHEAD,
              FAST_LOCAL_FRAME_PAYLOAD_CAPACITY);
    EXPECT_THROW(
        static_cast<void>(fast_local_frame_count(
            std::numeric_limits<uint64_t>::max(), false)),
        std::overflow_error);
}

TEST(FastLocalRoundTrip, EmptyAndOneByte) {
    expect_roundtrip({});
    expect_roundtrip({std::byte{0xa5}});
}

TEST(FastLocalRoundTrip, SmallText) {
    const std::string text =
        "Fast Local preserves every byte through FFV1/GRAY8.";
    expect_roundtrip(std::vector<std::byte>(
        reinterpret_cast<const std::byte *>(text.data()),
        reinterpret_cast<const std::byte *>(text.data() + text.size())));
}

TEST(FastLocalRoundTrip, OneMiBCompressibleAndRandom) {
    expect_roundtrip(
        std::vector<std::byte>(1024 * 1024, std::byte{0x41}));
    expect_roundtrip(random_bytes(1024 * 1024));
}

TEST(FastLocalRoundTrip, TenMiBCompressibleAndRandom) {
    expect_roundtrip(
        std::vector<std::byte>(10 * 1024 * 1024, std::byte{0}));
    expect_roundtrip(random_bytes(10 * 1024 * 1024));
}

TEST(FastLocalRoundTrip, EncryptedAndWrongPasswordPreservesTarget) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    const auto video = temporary.file("encrypted.mkv");
    const auto output = temporary.file("existing.bin");
    const auto bytes = random_bytes(1024 * 1024);
    const std::vector<std::byte> existing{
        std::byte{'k'}, std::byte{'e'}, std::byte{'e'}, std::byte{'p'}};
    write_bytes(input, bytes);
    write_bytes(output, existing);
    encode_api(input, video, true, "correct-password");

    EXPECT_EQ(decode_api(video, output, "wrong-password"),
              MS_ERR_CRYPTO);
    EXPECT_EQ(read_bytes(output), existing);
    EXPECT_EQ(decode_api(video, output, "correct-password"), MS_OK);
    EXPECT_EQ(read_bytes(output), bytes);
}

TEST(FastLocalDetection, DetectsFastAndRejectsCorruptedPayload) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    const auto video = temporary.file("valid.mkv");
    const auto corrupt_video = temporary.file("corrupt.mkv");
    const auto output = temporary.file("output.bin");
    write_bytes(input, random_bytes(4096));
    encode_api(input, video);
    ASSERT_TRUE(fast_local_has_magic(video));

    VideoDecoder decoder(video.string());
    std::vector<std::byte> frame;
    ASSERT_TRUE(decoder.decode_next_gray8_frame(frame));
    frame[FAST_LOCAL_RESERVED_PREFIX + 17] ^= std::byte{1};
    VideoEncoder encoder(corrupt_video.string());
    encoder.encode_gray8_frame(frame);
    encoder.finalize();
    EXPECT_EQ(decode_api(corrupt_video, output), MS_ERR_CORRUPT);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(FastLocalCorruption, TruncatedVideoPreservesExistingOutput) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    const auto video = temporary.file("valid.mkv");
    const auto truncated = temporary.file("truncated.mkv");
    const auto output = temporary.file("existing.bin");
    write_bytes(input, random_bytes(10 * 1024 * 1024));
    write_bytes(output, {std::byte{0x42}});
    encode_api(input, video);
    auto video_bytes = read_bytes(video);
    video_bytes.resize(video_bytes.size() / 2);
    write_bytes(truncated, video_bytes);

    EXPECT_NE(decode_api(truncated, output), MS_OK);
    EXPECT_EQ(read_bytes(output),
              std::vector<std::byte>{std::byte{0x42}});
}

TEST(FastLocalPreflight, DeterministicFrameCountAndMkvRequirement) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    write_bytes(input, random_bytes(10 * 1024 * 1024));
    const auto estimate = estimate_without_probe(
        input, temporary.file("output.mkv"));
    EXPECT_EQ(estimate.encoding_mode, MS_ENCODING_MODE_FAST_LOCAL);
    EXPECT_EQ(estimate.estimated_frame_count, 2u);
    EXPECT_EQ(estimate.frame_payload_capacity,
              fast_local_plain_capacity(false));

    const std::string input_text = input.string();
    const std::string output_text =
        temporary.file("invalid.mp4").string();
    ms_encode_options_t options{};
    options.input_path = input_text.c_str();
    options.output_path = output_text.c_str();
    options.encoding_mode = MS_ENCODING_MODE_FAST_LOCAL;
    ms_encoding_estimate_t invalid{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &invalid), MS_OK);
    EXPECT_FALSE(invalid.can_start_encoding);
    EXPECT_NE(std::string(invalid.error).find(".mkv"),
              std::string::npos);
}

TEST(FastLocalProfiler, BenchmarkJsonContainsModeAndLayout) {
    TemporaryDirectory temporary;
    const auto input = temporary.file("input.bin");
    const auto video = temporary.file("output.mkv");
    const auto json = temporary.file("report.json");
    write_bytes(input, random_bytes(1024));
    const auto result = encode_api(input, video);
    const std::string json_text = json.string();
    ASSERT_EQ(ms_write_benchmark_json(&result, json_text.c_str()), MS_OK);
    const auto contents = read_bytes(json);
    const std::string text(
        reinterpret_cast<const char *>(contents.data()), contents.size());
    EXPECT_NE(text.find("\"encoding_mode\": \"fast-local\""),
              std::string::npos);
    EXPECT_NE(text.find("\"frame_payload_capacity\""),
              std::string::npos);
}
