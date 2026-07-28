// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>

#include <gtest/gtest.h>

#include "../include/media_storage.h"
#include "configuration.h"
#include "encoding_preflight.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
    class PreflightFiles {
    public:
        PreflightFiles() {
            static std::atomic<uint64_t> sequence{0};
            directory = std::filesystem::temp_directory_path() /
                        ("vidstorex-preflight-test-" +
                         std::to_string(sequence.fetch_add(1)));
            std::filesystem::create_directories(directory);
            input = directory / "input.bin";
            output = directory / "output.mkv";
            json = directory / "estimate.json";
        }

        ~PreflightFiles() {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }

        void write_input(const std::size_t size) const {
            std::ofstream stream(input, std::ios::binary);
            std::vector<char> bytes(size);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] =
                    static_cast<char>((i * 131 + 17) & 0xff);
            }
            stream.write(
                bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        void write_output_sentinel() const {
            std::ofstream stream(output, std::ios::binary);
            stream << "do-not-touch";
        }

        [[nodiscard]] std::string read_output() const {
            std::ifstream stream(output, std::ios::binary);
            return {
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()};
        }

        std::filesystem::path directory;
        std::filesystem::path input;
        std::filesystem::path output;
        std::filesystem::path json;
    };

    ms_encode_options_t api_options(const PreflightFiles &files) {
        ms_encode_options_t options{};
        // The path strings are assigned by each test after this helper.
        options.repair_ratio = 0.05;
        options.repair_ratio_is_set = 1;
        return options;
    }

    class EnvironmentOverride {
    public:
        EnvironmentOverride(
            const char *name, const char *value)
            : name_(name) {
#if defined(_WIN32)
            char *existing = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&existing, &length, name) == 0 &&
                existing) {
                old_value_ = existing;
                had_value_ = true;
                std::free(existing);
            }
            _putenv_s(name, value);
#else
            if (const char *existing = std::getenv(name)) {
                old_value_ = existing;
                had_value_ = true;
            }
            setenv(name, value, 1);
#endif
        }

        ~EnvironmentOverride() {
#if defined(_WIN32)
            _putenv_s(
                name_.c_str(),
                had_value_ ? old_value_.c_str() : "");
#else
            if (had_value_) setenv(
                name_.c_str(), old_value_.c_str(), 1);
            else unsetenv(name_.c_str());
#endif
        }

        EnvironmentOverride(const EnvironmentOverride &) = delete;
        EnvironmentOverride &operator=(
            const EnvironmentOverride &) = delete;

    private:
        std::string name_;
        std::string old_value_;
        bool had_value_ = false;
    };

    std::size_t partial_file_count(
        const PreflightFiles &files) {
        std::size_t count = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(files.directory)) {
            if (entry.path().filename().string().find(
                    ".vidstorex-part-") != std::string::npos) {
                ++count;
            }
        }
        return count;
    }
}

TEST(EncodingPreflight, ChunkCountUsesEncodeCapacities) {
    EXPECT_EQ(calculate_chunk_count(0, false), 1u);
    EXPECT_EQ(calculate_chunk_count(CHUNK_SIZE_BYTES, false), 1u);
    EXPECT_EQ(calculate_chunk_count(CHUNK_SIZE_BYTES + 1, false), 2u);
    EXPECT_EQ(
        calculate_chunk_count(CHUNK_SIZE_PLAIN_MAX_ENCRYPTED, true),
        1u);
    EXPECT_EQ(
        calculate_chunk_count(CHUNK_SIZE_PLAIN_MAX_ENCRYPTED + 1, true),
        2u);
}

TEST(EncodingPreflight, DeterministicEstimateDoesNotTouchOutput) {
    const PreflightFiles files;
    files.write_input(65536);
    files.write_output_sentinel();
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    EXPECT_EQ(
        estimate.struct_size,
        sizeof(ms_encoding_estimate_t));
    EXPECT_EQ(
        estimate.struct_version,
        MS_ENCODING_ESTIMATE_VERSION);
    EXPECT_EQ(files.read_output(), "do-not-touch");
    EXPECT_EQ(estimate.input_size_bytes, 65536u);
    EXPECT_EQ(
        estimate.source_packet_count + estimate.repair_packet_count,
        estimate.total_packet_count);
    EXPECT_GT(estimate.estimated_frame_count, 0u);
    EXPECT_FALSE(estimate.output_size_estimate_available);
    EXPECT_FALSE(estimate.required_disk_space_known);
    EXPECT_EQ(estimate.disk_space_sufficient, -1);
    EXPECT_TRUE(estimate.can_start_encoding);
}

TEST(EncodingPreflight, RejectsUnsupportedEstimateStructVersion) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    estimate.struct_version = MS_ENCODING_ESTIMATE_VERSION + 1;
    options.preflight_estimate = &estimate;
    EXPECT_EQ(ms_encode(&options, nullptr), MS_ERR_INVALID_ARGS);
}

TEST(EncodingPreflight, ProbeProducesMeasuredRangeAndSafetyPolicy) {
    const PreflightFiles files;
    files.write_input(65536);
    files.write_output_sentinel();
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    EXPECT_EQ(files.read_output(), "do-not-touch");
    ASSERT_TRUE(estimate.output_size_estimate_available);
    EXPECT_LE(
        estimate.estimated_output_min_bytes,
        estimate.estimated_output_bytes);
    EXPECT_LE(
        estimate.estimated_output_bytes,
        estimate.estimated_output_max_bytes);
    EXPECT_GT(estimate.probe_frame_count, 0u);
    EXPECT_GT(estimate.probe_duration_seconds, 0.0);
    EXPECT_TRUE(estimate.required_disk_space_known);
    EXPECT_GE(
        estimate.safety_margin_bytes, 1024ull * 1024ull * 1024ull);
    EXPECT_EQ(
        estimate.required_disk_bytes,
        estimate.estimated_output_max_bytes +
            estimate.safety_margin_bytes);
    EXPECT_NE(estimate.disk_space_sufficient, -1);
}

TEST(EncodingPreflight, ActualEncodeUsesIdenticalPacketAndFrameCounts) {
    const PreflightFiles files;
    files.write_input(65536);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    options.preflight_estimate = &estimate;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    EXPECT_EQ(result.total_chunks, estimate.chunk_count);
    EXPECT_EQ(result.source_packets, estimate.source_packet_count);
    EXPECT_EQ(result.repair_packets, estimate.repair_packet_count);
    EXPECT_EQ(result.total_packets, estimate.total_packet_count);
    EXPECT_EQ(result.total_frames, estimate.estimated_frame_count);
}

TEST(EncodingPreflight, EncryptedCountsMatchActualEncode) {
    const PreflightFiles files;
    files.write_input(32768);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    const std::string password = "preflight-test-password";
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();
    options.encrypt = 1;
    options.password = password.c_str();
    options.password_len = password.size();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    options.preflight_estimate = &estimate;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    EXPECT_EQ(result.total_chunks, estimate.chunk_count);
    EXPECT_EQ(result.source_packets, estimate.source_packet_count);
    EXPECT_EQ(result.repair_packets, estimate.repair_packet_count);
    EXPECT_EQ(result.total_packets, estimate.total_packet_count);
    EXPECT_EQ(result.total_frames, estimate.estimated_frame_count);
}

TEST(EncodingPreflight, ProbeIsBoundedAndUsesRepresentativeSegments) {
    const PreflightFiles files;
    files.write_input(4 * 1024 * 1024);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    ASSERT_TRUE(estimate.output_size_estimate_available);
    EXPECT_GT(estimate.probe_frame_count, 0u);
    EXPECT_LE(estimate.probe_frame_count, 90u);
    EXPECT_NE(
        std::string(estimate.estimation_method).find(
            "start/middle/end"),
        std::string::npos);
}

TEST(EncodingPreflight, RefusesToOverwriteTheInputFile) {
    const PreflightFiles files;
    files.write_input(1024);
    const std::string input = files.input.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = input.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    EXPECT_FALSE(estimate.can_start_encoding);
    EXPECT_NE(
        std::string(estimate.error).find("same file"),
        std::string::npos);
    EXPECT_EQ(std::filesystem::file_size(files.input), 1024u);
}

TEST(EncodingPreflight, MissingOutputParentIsReportedButDiskIsQueried) {
    const PreflightFiles files;
    files.write_input(1024);
    const std::string input = files.input.string();
    const std::string output =
        (files.directory / "missing" / "nested" / "output.mkv").string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    EXPECT_TRUE(estimate.disk_space_known);
    EXPECT_FALSE(estimate.can_start_encoding);
    EXPECT_NE(
        std::string(estimate.error).find(
            "output parent directory does not exist"),
        std::string::npos);
}

TEST(EncodingPreflight, JsonUsesNullForUnavailableValues) {
    const PreflightFiles files;
    files.write_input(1024);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    const std::string json = files.json.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    ASSERT_EQ(
        ms_write_encoding_estimate_json(&estimate, json.c_str()),
        MS_OK);

    std::ifstream stream(files.json, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(
        contents.find("\"estimated_output_bytes\": null"),
        std::string::npos);
    EXPECT_NE(
        contents.find("\"required_disk_bytes\": null"),
        std::string::npos);
    EXPECT_NE(
        contents.find("\"disk_space_sufficient\": null"),
        std::string::npos);
}

TEST(EncodingPreflight, StaleEstimateRejectsChangedInputSize) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    {
        std::ofstream stream(
            files.input, std::ios::binary | std::ios::app);
        stream.put('x');
    }
    options.preflight_estimate = &estimate;

    EXPECT_EQ(
        ms_encode(&options, nullptr), MS_ERR_PREFLIGHT_STALE);
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(EncodingPreflight, StaleEstimateRejectsChangedWriteTime) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    const auto old_time =
        std::filesystem::last_write_time(files.input);
    std::filesystem::last_write_time(
        files.input, old_time + std::chrono::seconds(2));
    options.preflight_estimate = &estimate;

    EXPECT_EQ(
        ms_encode(&options, nullptr), MS_ERR_PREFLIGHT_STALE);
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(EncodingPreflight, KnownLowDiskBlocksWithoutOverride) {
    const EnvironmentOverride disk(
        "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES", "1");
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    ASSERT_EQ(estimate.disk_space_sufficient, 0);
    ASSERT_TRUE(estimate.low_disk_override_permitted);
    options.preflight_estimate = &estimate;

    EXPECT_EQ(
        ms_encode(&options, nullptr),
        MS_ERR_INSUFFICIENT_DISK);
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(EncodingPreflight, LowDiskOverrideOnlyOverridesDiskBlocker) {
    const EnvironmentOverride disk(
        "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES", "1");
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    options.preflight_estimate = &estimate;
    options.allow_low_disk = 1;
    EXPECT_EQ(ms_encode(&options, nullptr), MS_OK);
    EXPECT_TRUE(std::filesystem::is_regular_file(files.output));

    options.output_path = input.c_str();
    EXPECT_NE(ms_encode(&options, nullptr), MS_OK);
}

TEST(EncodingPreflight, UnknownDiskWarnsButDoesNotBlock) {
    const EnvironmentOverride disk(
        "VIDSTOREX_TEST_DISK_UNKNOWN", "1");
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    EXPECT_FALSE(estimate.disk_space_known);
    EXPECT_EQ(estimate.disk_space_sufficient, -1);
    EXPECT_TRUE(estimate.can_start_encoding);
    EXPECT_NE(
        std::string(estimate.warning).find(
            "could not be determined"),
        std::string::npos);
}

TEST(EncodingPreflight, DiskSpaceIsRecheckedBeforeEncode) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    ASSERT_NE(estimate.disk_space_sufficient, 0);
    options.preflight_estimate = &estimate;
    {
        const EnvironmentOverride disk(
            "VIDSTOREX_TEST_AVAILABLE_DISK_BYTES", "1");
        EXPECT_EQ(
            ms_encode(&options, nullptr),
            MS_ERR_INSUFFICIENT_DISK);
    }
    EXPECT_FALSE(std::filesystem::exists(files.output));
}

TEST(EncodingPreflight, ValidationCalculatesErrorsAndRange) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    estimate.estimated_output_bytes = 1;
    estimate.estimated_output_min_bytes = 1;
    estimate.estimated_output_max_bytes = 1;
    estimate.safety_margin_bytes =
        1024ull * 1024ull * 1024ull;
    estimate.required_disk_bytes =
        estimate.safety_margin_bytes + 1;
    options.preflight_estimate = &estimate;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    ASSERT_TRUE(result.estimate_validation_available);
    EXPECT_EQ(
        result.estimate_absolute_error_bytes,
        result.actual_output_bytes - 1);
    EXPECT_DOUBLE_EQ(
        result.estimate_relative_error_percent,
        static_cast<double>(
            result.estimate_absolute_error_bytes) * 100.0);
    EXPECT_EQ(result.actual_inside_estimated_range, 0);
}

TEST(EncodingPreflight, ZeroEstimateHasNoRelativeError) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    estimate.estimated_output_bytes = 0;
    estimate.estimated_output_min_bytes = 0;
    estimate.estimated_output_max_bytes = 0;
    estimate.safety_margin_bytes =
        1024ull * 1024ull * 1024ull;
    estimate.required_disk_bytes =
        estimate.safety_margin_bytes;
    options.preflight_estimate = &estimate;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    EXPECT_TRUE(result.estimate_validation_available);
    EXPECT_FALSE(result.estimate_relative_error_available);
    EXPECT_EQ(result.actual_inside_estimated_range, 0);
}

TEST(EncodingPreflight, UnavailableEstimateHasNoValidationMetrics) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 0, &estimate), MS_OK);
    options.preflight_estimate = &estimate;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    EXPECT_FALSE(result.estimate_validation_available);
    EXPECT_FALSE(result.estimate_relative_error_available);
    EXPECT_EQ(result.actual_inside_estimated_range, -1);
}

TEST(EncodingPreflight, BenchmarkAndTextReportsIncludeValidation) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    const std::string json = files.json.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    options.preflight_estimate = &estimate;
    options.preflight_duration_seconds =
        estimate.preflight_duration_seconds;

    ms_result_t result{};
    ASSERT_EQ(ms_encode(&options, &result), MS_OK);
    ASSERT_EQ(
        ms_write_benchmark_json(&result, json.c_str()), MS_OK);

    std::ifstream stream(files.json, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(
        contents.find("\"estimate_validation\""),
        std::string::npos);
    EXPECT_NE(
        contents.find("\"actual_output_bytes\""),
        std::string::npos);
    EXPECT_NE(
        contents.find("\"preflight_duration_seconds\""),
        std::string::npos);

    const std::size_t required =
        ms_format_performance_report(&result, nullptr, 0);
    std::vector<char> report(required);
    ms_format_performance_report(
        &result, report.data(), report.size());
    EXPECT_NE(
        std::string(report.data()).find("Estimate validation:"),
        std::string::npos);
}

TEST(EncodingPreflight, DiskRequirementRejectsOverflow) {
    EXPECT_THROW(
        (void) calculate_encoding_disk_requirement(
            std::numeric_limits<uint64_t>::max()),
        std::overflow_error);
    const auto requirement =
        calculate_encoding_disk_requirement(1000);
    EXPECT_EQ(
        requirement.safety_margin_bytes,
        1024ull * 1024ull * 1024ull);
    EXPECT_EQ(
        requirement.required_disk_bytes,
        1000ull + 1024ull * 1024ull * 1024ull);
}

TEST(EncodingPreflight, FailedEncodePreservesExistingOutputAndCleansPartial) {
    const PreflightFiles files;
    files.write_input(4096);
    files.write_output_sentinel();
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();
    options.progress = [](uint64_t, uint64_t, void *) {
        return 1;
    };

    EXPECT_EQ(
        ms_encode(&options, nullptr), MS_ERR_ENCODE_FAILED);
    EXPECT_EQ(files.read_output(), "do-not-touch");
    EXPECT_EQ(partial_file_count(files), 0u);
}

TEST(EncodingPreflight, SuccessfulEncodeSafelyReplacesExistingOutput) {
    const PreflightFiles files;
    files.write_input(4096);
    files.write_output_sentinel();
    const std::string input = files.input.string();
    const std::string output = files.output.string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ASSERT_EQ(ms_encode(&options, nullptr), MS_OK);
    EXPECT_NE(files.read_output(), "do-not-touch");
    EXPECT_GT(std::filesystem::file_size(files.output), 0u);
    EXPECT_EQ(partial_file_count(files), 0u);
}

TEST(EncodingPreflight, ProbeFailureKeepsDeterministicEstimateAndCleansTemp) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const std::string output =
        (files.directory / "output.unsupported-mux").string();
    auto options = api_options(files);
    options.input_path = input.c_str();
    options.output_path = output.c_str();

    ms_encoding_estimate_t estimate{};
    ASSERT_EQ(ms_estimate_encode(&options, 1, &estimate), MS_OK);
    EXPECT_GT(estimate.total_packet_count, 0u);
    EXPECT_GT(estimate.estimated_frame_count, 0u);
    EXPECT_FALSE(estimate.output_size_estimate_available);
    EXPECT_FALSE(estimate.required_disk_space_known);
    EXPECT_TRUE(estimate.can_start_encoding);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(EncodingPreflight, ConcurrentProbesDoNotCollideOrTouchTarget) {
    const PreflightFiles files;
    files.write_input(65536);
    files.write_output_sentinel();
    const std::string input = files.input.string();
    const std::string output = files.output.string();

    const auto run = [&] {
        auto options = api_options(files);
        options.input_path = input.c_str();
        options.output_path = output.c_str();
        ms_encoding_estimate_t estimate{};
        return std::pair{
            ms_estimate_encode(&options, 1, &estimate),
            estimate};
    };
    auto first = std::async(std::launch::async, run);
    auto second = std::async(std::launch::async, run);
    const auto first_result = first.get();
    const auto second_result = second.get();

    EXPECT_EQ(first_result.first, MS_OK);
    EXPECT_EQ(second_result.first, MS_OK);
    EXPECT_TRUE(
        first_result.second.output_size_estimate_available);
    EXPECT_TRUE(
        second_result.second.output_size_estimate_available);
    EXPECT_EQ(files.read_output(), "do-not-touch");
}

TEST(EncodingPreflight, ReliabilityProfilesAndHighRepairRoundtrip) {
    const PreflightFiles files;
    files.write_input(4096);
    const std::string input = files.input.string();
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<char>{
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
    };
    const auto original = read(files.input);

    for (const double repair_ratio :
         {0.05, 0.20, 0.50, 5.0}) {
        const auto suffix =
            std::to_string(static_cast<int>(repair_ratio * 100));
        const auto encoded =
            files.directory / ("profile-" + suffix + ".mkv");
        const auto decoded =
            files.directory / ("profile-" + suffix + ".bin");
        const std::string encoded_text = encoded.string();
        const std::string decoded_text = decoded.string();

        auto encode_options = api_options(files);
        encode_options.input_path = input.c_str();
        encode_options.output_path = encoded_text.c_str();
        encode_options.repair_ratio = repair_ratio;
        encode_options.repair_ratio_is_set = 1;
        ASSERT_EQ(ms_encode(&encode_options, nullptr), MS_OK);

        ms_decode_options_t decode_options{};
        decode_options.input_path = encoded_text.c_str();
        decode_options.output_path = decoded_text.c_str();
        ASSERT_EQ(ms_decode(&decode_options, nullptr), MS_OK);
        EXPECT_EQ(read(decoded), original);
    }
}
