// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>

#include <gtest/gtest.h>

#include "media_storage.h"
#include "performance_profiler.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(PerformanceProfiler, ScopedTimerRecordsDurationAndInvocation) {
    PerformanceProfiler profiler;
    {
        ScopedTimer timer(&profiler, PerformanceStage::InputRead);
        std::this_thread::sleep_for(1ms);
    }
    profiler.finish();

    EXPECT_EQ(profiler.stage_invocations(PerformanceStage::InputRead), 1u);
    EXPECT_GT(profiler.stage_seconds(PerformanceStage::InputRead), 0.0);
    EXPECT_GE(profiler.total_seconds(),
              profiler.stage_seconds(PerformanceStage::InputRead));
}

TEST(PerformanceProfiler, AccumulatesFromMultipleThreads) {
    PerformanceProfiler profiler;
    constexpr int thread_count = 8;
    constexpr int samples_per_thread = 1000;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&profiler] {
            for (int sample = 0; sample < samples_per_thread; ++sample) {
                ScopedTimer timer(
                    &profiler, PerformanceStage::FecRepairGeneration);
            }
        });
    }
    for (auto &thread: threads) thread.join();

    EXPECT_EQ(
        profiler.stage_invocations(PerformanceStage::FecRepairGeneration),
        static_cast<uint64_t>(thread_count * samples_per_thread));
}

TEST(PerformanceReport, FormatsHumanReadableSummary) {
    ms_result_t result{};
    result.operation = MS_OPERATION_ENCODE;
    result.input_size = 1024;
    result.output_size = 4096;
    result.total_chunks = 2;
    result.total_packets = 12;
    result.source_packets = 10;
    result.repair_packets = 2;
    result.total_frames = 3;
    result.total_seconds = 2.0;
    result.average_frames_per_second = 1.5;
    result.throughput_mib_per_second = 0.5;
    result.output_input_ratio = 4.0;
    result.selected_repair_percentage = 5.0;
    result.selected_repair_ratio = 0.05;
    result.repair_source_ratio = 0.2;
    result.stage_timings[MS_PERF_FFMPEG_ENCODE] = {1.0, 50.0, 3};

    const size_t required =
        ms_format_performance_report(&result, nullptr, 0);
    ASSERT_GT(required, 1u);
    std::vector<char> report(required);
    EXPECT_EQ(ms_format_performance_report(
                  &result, report.data(), report.size()),
              required);

    const std::string text(report.data());
    EXPECT_NE(text.find("Performance report (encode)"), std::string::npos);
    EXPECT_NE(text.find("source 10, repair 2"), std::string::npos);
    EXPECT_NE(text.find("Repair percentage: 5.00%"), std::string::npos);
    EXPECT_NE(text.find("FFmpeg video encoding"), std::string::npos);
    EXPECT_NE(text.find("Total wall time"), std::string::npos);
}

TEST(PerformanceReport, WritesMachineReadableJson) {
    ms_result_t result{};
    result.operation = MS_OPERATION_ENCODE;
    result.input_size = 4096;
    result.output_size = 1024;
    result.total_packets = 12;
    result.source_packets = 10;
    result.repair_packets = 2;
    result.selected_repair_percentage = 5.0;
    result.selected_repair_ratio = 0.05;
    result.repair_source_ratio = 0.2;
    result.total_seconds = 1.25;
    result.stage_timings[MS_PERF_FFMPEG_ENCODE] = {0.5, 40.0, 4};

    const auto path = std::filesystem::temp_directory_path() /
                      "vidstorex_benchmark_report_test.json";
    ASSERT_EQ(ms_write_benchmark_json(&result, path.string().c_str()), MS_OK);

    std::ifstream input(path, std::ios::binary);
    const std::string json(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    EXPECT_NE(json.find("\"schema_version\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"operation\": \"encode\""), std::string::npos);
    EXPECT_NE(json.find("\"ffmpeg_encode\""), std::string::npos);
    EXPECT_NE(json.find("\"source\": 10"), std::string::npos);
    EXPECT_NE(json.find("\"repair_percentage\": 5.000000000"),
              std::string::npos);
    EXPECT_NE(json.find("\"repair_ratio\": 0.050000000"),
              std::string::npos);
}
