/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

inline constexpr double DEFAULT_REPAIR_RATIO = 0.05;
inline constexpr double MAX_REPAIR_RATIO = 5.0;
inline constexpr double DEFAULT_REPAIR_PERCENTAGE = 5.0;
inline constexpr double MAX_REPAIR_PERCENTAGE = 500.0;

struct EncodingReliabilityOptions {
    double repair_ratio = DEFAULT_REPAIR_RATIO;
};

enum class ReliabilityProfile {
    Local,
    Balanced,
    Durable,
};

struct EncodingReliabilityEstimate {
    uint64_t chunk_count = 0;
    uint64_t source_packet_count = 0;
    uint64_t repair_packet_count = 0;
    uint64_t total_packet_count = 0;
    uint64_t frame_count = 0;
    double video_duration_seconds = 0.0;
};

[[nodiscard]] bool is_valid_repair_ratio(double repair_ratio) noexcept;

[[nodiscard]] double repair_percentage_to_ratio(double repair_percentage);

[[nodiscard]] double repair_ratio_to_percentage(double repair_ratio);

[[nodiscard]] uint64_t calculate_source_packet_count(uint64_t encoded_chunk_size);

[[nodiscard]] uint64_t calculate_chunk_count(
    uint64_t input_size, bool encrypted);

[[nodiscard]] uint64_t calculate_repair_packet_count(
    uint64_t source_packet_count, double repair_ratio);

[[nodiscard]] EncodingReliabilityOptions reliability_options_for_profile(
    ReliabilityProfile profile);

[[nodiscard]] ReliabilityProfile parse_reliability_profile(
    std::string_view profile_name);

[[nodiscard]] double parse_repair_percentage(std::string_view text);

[[nodiscard]] EncodingReliabilityOptions resolve_reliability_options(
    std::optional<ReliabilityProfile> profile,
    std::optional<double> repair_percentage);

[[nodiscard]] EncodingReliabilityEstimate estimate_encoding_reliability(
    uint64_t input_size,
    bool encrypted,
    const EncodingReliabilityOptions &options,
    uint64_t packets_per_frame,
    uint32_t frames_per_second);
