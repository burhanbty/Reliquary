/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

enum class PerformanceStage : std::size_t {
    InputRead = 0,
    Preprocess,
    ChunkCreation,
    FecRepairGeneration,
    PacketToFrame,
    FfmpegEncode,
    MuxDiskWrite,
    VideoReadDemux,
    FrameDecode,
    PacketExtraction,
    FecChunkRecovery,
    Postprocess,
    OutputWrite,
    Count
};

class PerformanceProfiler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t stage_count =
        static_cast<std::size_t>(PerformanceStage::Count);

    PerformanceProfiler() noexcept;

    void add(PerformanceStage stage, Clock::duration elapsed) noexcept;

    void finish() noexcept;

    [[nodiscard]] double total_seconds() const noexcept;

    [[nodiscard]] double stage_seconds(PerformanceStage stage) const noexcept;

    [[nodiscard]] uint64_t stage_invocations(PerformanceStage stage) const noexcept;

private:
    Clock::time_point started_;
    std::array<std::atomic<int64_t>, stage_count> nanoseconds_{};
    std::array<std::atomic<uint64_t>, stage_count> invocations_{};
    std::atomic<int64_t> total_nanoseconds_{0};
};

class ScopedTimer {
public:
    ScopedTimer(PerformanceProfiler *profiler, PerformanceStage stage) noexcept;

    ~ScopedTimer();

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;
    ScopedTimer(ScopedTimer &&) = delete;
    ScopedTimer &operator=(ScopedTimer &&) = delete;

private:
    PerformanceProfiler *profiler_;
    PerformanceStage stage_;
    PerformanceProfiler::Clock::time_point started_;
};
