/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "performance_profiler.h"

namespace {
    constexpr double nanoseconds_per_second = 1'000'000'000.0;
}

PerformanceProfiler::PerformanceProfiler() noexcept
    : started_(Clock::now()) {
}

void PerformanceProfiler::add(const PerformanceStage stage,
                              const Clock::duration elapsed) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= stage_count) return;

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    nanoseconds_[index].fetch_add(ns, std::memory_order_relaxed);
    invocations_[index].fetch_add(1, std::memory_order_relaxed);
}

void PerformanceProfiler::finish() noexcept {
    int64_t expected = 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - started_).count();
    total_nanoseconds_.compare_exchange_strong(
        expected, elapsed, std::memory_order_relaxed);
}

double PerformanceProfiler::total_seconds() const noexcept {
    int64_t ns = total_nanoseconds_.load(std::memory_order_relaxed);
    if (ns == 0) {
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started_).count();
    }
    return static_cast<double>(ns) / nanoseconds_per_second;
}

double PerformanceProfiler::stage_seconds(const PerformanceStage stage) const noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= stage_count) return 0.0;
    return static_cast<double>(
        nanoseconds_[index].load(std::memory_order_relaxed)) / nanoseconds_per_second;
}

uint64_t PerformanceProfiler::stage_invocations(const PerformanceStage stage) const noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= stage_count) return 0;
    return invocations_[index].load(std::memory_order_relaxed);
}

ScopedTimer::ScopedTimer(PerformanceProfiler *profiler,
                         const PerformanceStage stage) noexcept
    : profiler_(profiler), stage_(stage),
      started_(profiler ? PerformanceProfiler::Clock::now()
                        : PerformanceProfiler::Clock::time_point{}) {
}

ScopedTimer::~ScopedTimer() {
    if (profiler_) {
        profiler_->add(stage_, PerformanceProfiler::Clock::now() - started_);
    }
}
