/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class GuiPreflightStatus {
    WaitingForInput,
    WaitingForOutputPath,
    WaitingForValidSettings,
    InputChanged,
    Estimating,
    Ready,
    InsufficientDiskSpace,
    DiskSpaceUnknown,
    OutputSizeEstimateUnavailable,
    EstimateFailed,
};

enum class GuiEncodeEligibility {
    RefreshRequired,
    Ready,
    Blocked,
    BlockedInsufficientDisk,
    ConfirmDiskUnknown,
    ConfirmOutputSizeUnavailable,
};

struct GuiPreflightFingerprint {
    std::string normalized_input_path;
    uint64_t input_size = 0;
    int64_t input_last_write_time = 0;
    std::string normalized_output_path;
    int reliability_profile = 0;
    double repair_ratio = 0.0;
    bool encrypted = false;

    bool operator==(const GuiPreflightFingerprint &) const = default;
};

struct GuiPreflightSnapshot {
    bool output_size_estimate_available = false;
    bool disk_space_known = false;
    int disk_space_sufficient = -1;
    bool can_start_encoding = false;
    bool low_disk_override_permitted = false;
    std::string warning;
    std::string error;
};

/**
 * Widget-independent state machine used by the Qt GUI. It owns request
 * generations, accepted fingerprints and the narrowly scoped low-disk
 * override policy.
 */
class GuiPreflightModel {
public:
    [[nodiscard]] std::optional<uint64_t> request(
        const GuiPreflightFingerprint &fingerprint, bool force = false);

    void waitFor(GuiPreflightStatus status);

    [[nodiscard]] bool accept(
        uint64_t generation,
        const GuiPreflightFingerprint &fingerprint,
        const GuiPreflightSnapshot &snapshot);

    [[nodiscard]] bool fail(
        uint64_t generation,
        const GuiPreflightFingerprint &fingerprint);

    [[nodiscard]] bool isCurrent(
        const GuiPreflightFingerprint &fingerprint) const;

    [[nodiscard]] GuiEncodeEligibility eligibility(
        const GuiPreflightFingerprint &fingerprint) const;

    [[nodiscard]] bool setLowDiskOverride(bool enabled);

    void beginShutdown();

    [[nodiscard]] uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] GuiPreflightStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] bool lowDiskOverride() const noexcept {
        return low_disk_override_;
    }

private:
    uint64_t generation_ = 0;
    GuiPreflightStatus status_ = GuiPreflightStatus::WaitingForInput;
    std::optional<GuiPreflightFingerprint> requested_;
    std::optional<GuiPreflightFingerprint> accepted_;
    std::optional<GuiPreflightSnapshot> snapshot_;
    bool low_disk_override_ = false;
    bool shutting_down_ = false;
};

[[nodiscard]] const char *gui_preflight_status_text(
    GuiPreflightStatus status) noexcept;
