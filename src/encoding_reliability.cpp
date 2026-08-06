/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#include "encoding_reliability.h"

#include "configuration.h"
#include "youtube_capacity_lab.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
    constexpr uint64_t minimum_encoded_chunk_size = SYMBOL_SIZE_BYTES * 2;
    constexpr uint64_t encryption_bytes_per_chunk =
        CHUNK_SIZE_BYTES - CHUNK_SIZE_PLAIN_MAX_ENCRYPTED;

    uint64_t checked_add(const uint64_t left, const uint64_t right) {
        if (right > std::numeric_limits<uint64_t>::max() - left) {
            throw std::overflow_error("packet count overflow");
        }
        return left + right;
    }

    uint64_t checked_multiply(const uint64_t left, const uint64_t right) {
        if (left != 0 &&
            right > std::numeric_limits<uint64_t>::max() / left) {
            throw std::overflow_error("packet count overflow");
        }
        return left * right;
    }

    constexpr ReliabilityProfileDefinition resilient_profile{
        ReliabilityProfile::Local, "resilient", "Resilient",
        8, 1, 1.0, 5.0, FRAME_WIDTH, FRAME_HEIGHT,
        false, 0, 0, 0, 0};
    constexpr ReliabilityProfileDefinition balanced_profile{
        ReliabilityProfile::Balanced, "balanced", "Balanced",
        8, 1, 1.0, 20.0, FRAME_WIDTH, FRAME_HEIGHT,
        false, 0, 0, 0, 0};
    constexpr ReliabilityProfileDefinition durable_profile{
        ReliabilityProfile::Durable, "durable", "Durable",
        8, 1, 1.0, 50.0, FRAME_WIDTH, FRAME_HEIGHT,
        false, 0, 0, 0, 0};
    constexpr ReliabilityProfileDefinition high_capacity_profile{
        ReliabilityProfile::HighCapacity, "high-capacity", "High Capacity",
        4, 1, 1.0, 5.0, 1920, 1080, true, 6, 6, 0, 2};

    void accumulate_chunk_group(EncodingReliabilityEstimate &estimate,
                                const uint64_t chunk_count,
                                const uint64_t encoded_chunk_size,
                                const double repair_ratio) {
        if (chunk_count == 0) return;
        const uint64_t source_per_chunk =
            calculate_source_packet_count(encoded_chunk_size);
        const uint64_t repair_per_chunk =
            calculate_repair_packet_count(source_per_chunk, repair_ratio);
        estimate.source_packet_count = checked_add(
            estimate.source_packet_count,
            checked_multiply(chunk_count, source_per_chunk));
        estimate.repair_packet_count = checked_add(
            estimate.repair_packet_count,
            checked_multiply(chunk_count, repair_per_chunk));
    }
}

bool is_valid_repair_ratio(const double repair_ratio) noexcept {
    return std::isfinite(repair_ratio) &&
           repair_ratio >= 0.0 &&
           repair_ratio <= MAX_REPAIR_RATIO;
}

double repair_percentage_to_ratio(const double repair_percentage) {
    if (!std::isfinite(repair_percentage) ||
        repair_percentage < 0.0 ||
        repair_percentage > MAX_REPAIR_PERCENTAGE) {
        throw std::invalid_argument(
            "repair percentage must be finite and between 0 and 500");
    }
    return repair_percentage / 100.0;
}

double repair_ratio_to_percentage(const double repair_ratio) {
    if (!is_valid_repair_ratio(repair_ratio)) {
        throw std::invalid_argument(
            "repair ratio must be finite and between 0.0 and 5.0");
    }
    return repair_ratio * 100.0;
}

uint64_t calculate_source_packet_count(const uint64_t encoded_chunk_size) {
    const uint64_t padded_size =
        (std::max)(encoded_chunk_size, minimum_encoded_chunk_size);
    return padded_size / SYMBOL_SIZE_BYTES +
           (padded_size % SYMBOL_SIZE_BYTES != 0 ? 1 : 0);
}

uint64_t calculate_chunk_count(const uint64_t input_size,
                               const bool encrypted) {
    const uint64_t chunk_capacity =
        encrypted ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : CHUNK_SIZE_BYTES;
    if (chunk_capacity == 0) {
        throw std::logic_error("chunk capacity must be positive");
    }
    if (input_size == 0) return 1;
    return input_size / chunk_capacity +
           (input_size % chunk_capacity != 0 ? 1 : 0);
}

uint64_t calculate_repair_packet_count(const uint64_t source_packet_count,
                                       const double repair_ratio) {
    if (!is_valid_repair_ratio(repair_ratio)) {
        throw std::invalid_argument(
            "repair ratio must be finite and between 0.0 and 5.0");
    }
    if (source_packet_count == 0 || repair_ratio == 0.0) return 0;

    const long double raw_repair_count =
        static_cast<long double>(source_packet_count) *
        static_cast<long double>(repair_ratio);
    const long double floating_point_tolerance =
        static_cast<long double>(std::numeric_limits<double>::epsilon()) *
        (std::max)(1.0L, std::abs(raw_repair_count)) * 4.0L;
    const long double nearest_integer = std::round(raw_repair_count);
    const long double adjusted_repair_count =
        nearest_integer >= 1.0L &&
        std::abs(raw_repair_count - nearest_integer) <=
            floating_point_tolerance
            ? nearest_integer
            : raw_repair_count;
    const long double repair_count = std::ceil(adjusted_repair_count);
    if (repair_count >
        static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        throw std::overflow_error("repair packet count overflow");
    }
    return static_cast<uint64_t>(repair_count);
}

EncodingReliabilityOptions reliability_options_for_profile(
    const ReliabilityProfile profile) {
    return {repair_percentage_to_ratio(
        reliability_profile_definition(profile).repair_percentage)};
}

const ReliabilityProfileDefinition &reliability_profile_definition(
    const ReliabilityProfile profile) {
    switch (profile) {
        case ReliabilityProfile::Local: return resilient_profile;
        case ReliabilityProfile::Balanced: return balanced_profile;
        case ReliabilityProfile::Durable: return durable_profile;
        case ReliabilityProfile::HighCapacity: return high_capacity_profile;
    }
    throw std::invalid_argument("unknown reliability profile");
}

ReliabilityProfile reliability_profile_from_id(const int profile_id) noexcept {
    switch (profile_id) {
        case 0: return ReliabilityProfile::Local;
        case 1: return ReliabilityProfile::Balanced;
        case 2: return ReliabilityProfile::Durable;
        case 3: return ReliabilityProfile::HighCapacity;
        default: return ReliabilityProfile::Local;
    }
}

std::string reliability_profile_config_id(
    const ReliabilityProfile profile) {
    const auto &definition = reliability_profile_definition(profile);
    youtube_capacity_lab::ExperimentConfig config;
    config.block_width = definition.block_size;
    config.block_height = definition.block_size;
    config.bits_per_block = definition.bits_per_symbol;
    config.signal_milli = static_cast<int>(
        std::lround(definition.signal_strength * 1000.0));
    config.repair_basis_points = static_cast<int>(
        std::lround(definition.repair_percentage * 100.0));
    config.resolution_width = definition.width;
    config.resolution_height = definition.height;
    return config.config_id();
}

ReliabilityProfile parse_reliability_profile(
    const std::string_view profile_name) {
    if (profile_name == "local") return ReliabilityProfile::Local;
    if (profile_name == "resilient") return ReliabilityProfile::Local;
    if (profile_name == "balanced") return ReliabilityProfile::Balanced;
    if (profile_name == "durable") return ReliabilityProfile::Durable;
    if (profile_name == "high-capacity" ||
        profile_name == "high_capacity" ||
        profile_name == "highcapacity")
        return ReliabilityProfile::HighCapacity;
    throw std::invalid_argument(
        "reliability profile must be high-capacity, resilient, local, "
        "balanced, or durable");
}

double parse_repair_percentage(const std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("repair percentage is missing");
    }

    const std::string value(text);
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno == ERANGE || end == value.c_str() || *end != '\0') {
        throw std::invalid_argument(
            "repair percentage must be a valid number");
    }
    (void) repair_percentage_to_ratio(parsed);
    return parsed;
}

EncodingReliabilityOptions resolve_reliability_options(
    const std::optional<ReliabilityProfile> profile,
    const std::optional<double> repair_percentage) {
    EncodingReliabilityOptions options =
        profile.has_value()
            ? reliability_options_for_profile(*profile)
            : EncodingReliabilityOptions{};
    if (repair_percentage.has_value()) {
        options.repair_ratio =
            repair_percentage_to_ratio(*repair_percentage);
    }
    return options;
}

EncodingReliabilityEstimate estimate_encoding_reliability(
    const uint64_t input_size,
    const bool encrypted,
    const EncodingReliabilityOptions &options,
    const uint64_t packets_per_frame,
    const uint32_t frames_per_second) {
    if (!is_valid_repair_ratio(options.repair_ratio)) {
        throw std::invalid_argument("invalid repair ratio");
    }
    if (packets_per_frame == 0 || frames_per_second == 0) {
        throw std::invalid_argument(
            "packet capacity and frame rate must be positive");
    }

    const uint64_t plain_chunk_capacity =
        encrypted ? CHUNK_SIZE_PLAIN_MAX_ENCRYPTED : CHUNK_SIZE_BYTES;
    EncodingReliabilityEstimate estimate;
    estimate.chunk_count = calculate_chunk_count(input_size, encrypted);

    if (input_size == 0) {
        const uint64_t encoded_size =
            encrypted ? encryption_bytes_per_chunk : 0;
        accumulate_chunk_group(
            estimate, 1, encoded_size, options.repair_ratio);
    } else {
        const uint64_t full_chunk_count = input_size / plain_chunk_capacity;
        const uint64_t remainder = input_size % plain_chunk_capacity;

        const uint64_t full_encoded_size =
            plain_chunk_capacity +
            (encrypted ? encryption_bytes_per_chunk : 0);
        accumulate_chunk_group(
            estimate, full_chunk_count, full_encoded_size,
            options.repair_ratio);
        if (remainder != 0) {
            const uint64_t last_encoded_size =
                remainder + (encrypted ? encryption_bytes_per_chunk : 0);
            accumulate_chunk_group(
                estimate, 1, last_encoded_size, options.repair_ratio);
        }
    }

    estimate.total_packet_count = checked_add(
        estimate.source_packet_count, estimate.repair_packet_count);
    estimate.frame_count =
        estimate.total_packet_count / packets_per_frame +
        (estimate.total_packet_count % packets_per_frame != 0 ? 1 : 0);
    estimate.video_duration_seconds =
        static_cast<double>(estimate.frame_count) /
        static_cast<double>(frames_per_second);
    return estimate;
}
