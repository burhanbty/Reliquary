/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#pragma once

#include "encoder.h"
#include "encoding_reliability.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

struct EncodingPreflightEstimate {
    uint64_t input_size_bytes = 0;
    int64_t input_last_write_time_ticks = 0;
    uint64_t input_path_fingerprint = 0;
    double repair_percentage = 0.0;
    double repair_ratio = 0.0;
    uint64_t chunk_count = 0;
    uint64_t source_packet_count = 0;
    uint64_t repair_packet_count = 0;
    uint64_t total_packet_count = 0;
    uint64_t estimated_frame_count = 0;
    double estimated_video_duration_seconds = 0.0;

    std::optional<uint64_t> estimated_output_bytes;
    std::optional<uint64_t> estimated_output_min_bytes;
    std::optional<uint64_t> estimated_output_max_bytes;
    bool output_size_estimate_available = false;

    std::optional<uint64_t> available_disk_bytes;
    bool disk_space_known = false;
    std::optional<uint64_t> safety_margin_bytes;
    std::optional<uint64_t> required_disk_bytes;
    std::optional<bool> disk_space_sufficient;
    bool can_start_encoding = false;
    bool low_disk_override_permitted = false;

    std::string estimation_method = "deterministic-packet-frame-counts";
    uint64_t probe_frame_count = 0;
    double probe_duration_seconds = 0.0;
    double preflight_duration_seconds = 0.0;
    std::string warning;
    std::string error;
    std::string debug_detail;
};

struct EncodingPreflightRequest {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    bool encrypted = false;
    std::span<const std::byte> password;
    HashAlgorithm hash_algorithm = HashAlgorithm::CRC32;
    EncodingReliabilityOptions reliability;
    bool enable_probe = true;
    uint64_t maximum_probe_frames = 90;
};

struct EncodingStartValidation {
    bool metadata_matches = false;
    bool disk_space_known = false;
    std::optional<uint64_t> available_disk_bytes;
    std::optional<bool> disk_space_sufficient;
    bool can_start_encoding = false;
    std::string warning;
    std::string error;
};

struct EncodingDiskRequirement {
    uint64_t safety_margin_bytes = 0;
    uint64_t required_disk_bytes = 0;
};

/**
 * Calculate deterministic encode counts, perform a bounded representative
 * FFV1 probe, and check the target filesystem without touching output_path.
 */
[[nodiscard]] EncodingPreflightEstimate estimate_encoding_preflight(
    const EncodingPreflightRequest &request);

/**
 * Recheck input identity/metadata, output validity and target-disk space
 * immediately before opening the encoder. No probe is repeated.
 */
[[nodiscard]] EncodingStartValidation validate_encoding_preflight_for_start(
    const EncodingPreflightRequest &request,
    const EncodingPreflightEstimate &estimate,
    bool allow_low_disk);

/**
 * Central safety policy. The final output is the only same-filesystem partial
 * allocation used by the safe-replace encoder path.
 */
[[nodiscard]] EncodingDiskRequirement calculate_encoding_disk_requirement(
    uint64_t estimated_output_max_bytes);
