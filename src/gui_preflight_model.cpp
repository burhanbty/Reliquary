/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#include "gui_preflight_model.h"

std::optional<uint64_t> GuiPreflightModel::request(
    const GuiPreflightFingerprint &fingerprint, const bool force) {
    if (shutting_down_) return std::nullopt;

    if (!force) {
        if (status_ == GuiPreflightStatus::Estimating &&
            requested_ && *requested_ == fingerprint) {
            return std::nullopt;
        }
        if (accepted_ && *accepted_ == fingerprint &&
            status_ != GuiPreflightStatus::EstimateFailed) {
            return std::nullopt;
        }
    }

    const bool settings_changed =
        !requested_.has_value() || *requested_ != fingerprint;
    if (settings_changed) low_disk_override_ = false;

    ++generation_;
    requested_ = fingerprint;
    accepted_.reset();
    snapshot_.reset();
    status_ = GuiPreflightStatus::Estimating;
    return generation_;
}

void GuiPreflightModel::waitFor(const GuiPreflightStatus status) {
    if (shutting_down_) return;
    ++generation_;
    requested_.reset();
    accepted_.reset();
    snapshot_.reset();
    low_disk_override_ = false;
    status_ = status;
}

bool GuiPreflightModel::accept(
    const uint64_t generation,
    const GuiPreflightFingerprint &fingerprint,
    const GuiPreflightSnapshot &snapshot) {
    if (shutting_down_ || generation != generation_ ||
        !requested_ || *requested_ != fingerprint) {
        return false;
    }

    accepted_ = fingerprint;
    snapshot_ = snapshot;
    if (!snapshot.error.empty()) {
        status_ = GuiPreflightStatus::EstimateFailed;
    } else if (snapshot.disk_space_known &&
               snapshot.disk_space_sufficient == 0) {
        status_ = GuiPreflightStatus::InsufficientDiskSpace;
    } else if (!snapshot.output_size_estimate_available) {
        status_ = GuiPreflightStatus::OutputSizeEstimateUnavailable;
    } else if (!snapshot.disk_space_known) {
        status_ = GuiPreflightStatus::DiskSpaceUnknown;
    } else if (snapshot.can_start_encoding) {
        status_ = GuiPreflightStatus::Ready;
    } else {
        status_ = GuiPreflightStatus::EstimateFailed;
    }
    return true;
}

bool GuiPreflightModel::fail(
    const uint64_t generation,
    const GuiPreflightFingerprint &fingerprint) {
    if (shutting_down_ || generation != generation_ ||
        !requested_ || *requested_ != fingerprint) {
        return false;
    }
    accepted_ = fingerprint;
    snapshot_.reset();
    low_disk_override_ = false;
    status_ = GuiPreflightStatus::EstimateFailed;
    return true;
}

bool GuiPreflightModel::isCurrent(
    const GuiPreflightFingerprint &fingerprint) const {
    return !shutting_down_ && accepted_ &&
           *accepted_ == fingerprint && snapshot_.has_value();
}

GuiEncodeEligibility GuiPreflightModel::eligibility(
    const GuiPreflightFingerprint &fingerprint) const {
    if (!isCurrent(fingerprint)) {
        return GuiEncodeEligibility::RefreshRequired;
    }
    if (status_ == GuiPreflightStatus::InsufficientDiskSpace) {
        return low_disk_override_
                   ? GuiEncodeEligibility::Ready
                   : GuiEncodeEligibility::BlockedInsufficientDisk;
    }
    if (status_ == GuiPreflightStatus::DiskSpaceUnknown) {
        return GuiEncodeEligibility::ConfirmDiskUnknown;
    }
    if (status_ ==
        GuiPreflightStatus::OutputSizeEstimateUnavailable) {
        return GuiEncodeEligibility::ConfirmOutputSizeUnavailable;
    }
    if (status_ == GuiPreflightStatus::Ready) {
        return GuiEncodeEligibility::Ready;
    }
    return GuiEncodeEligibility::Blocked;
}

bool GuiPreflightModel::setLowDiskOverride(const bool enabled) {
    if (status_ != GuiPreflightStatus::InsufficientDiskSpace ||
        !snapshot_ || !snapshot_->disk_space_known ||
        snapshot_->disk_space_sufficient != 0 ||
        !snapshot_->low_disk_override_permitted) {
        low_disk_override_ = false;
        return false;
    }
    low_disk_override_ = enabled;
    return true;
}

void GuiPreflightModel::beginShutdown() {
    shutting_down_ = true;
    ++generation_;
    requested_.reset();
    accepted_.reset();
    snapshot_.reset();
    low_disk_override_ = false;
}

const char *gui_preflight_status_text(
    const GuiPreflightStatus status) noexcept {
    switch (status) {
        case GuiPreflightStatus::WaitingForInput:
            return "Waiting for input";
        case GuiPreflightStatus::WaitingForOutputPath:
            return "Waiting for output path";
        case GuiPreflightStatus::WaitingForValidSettings:
            return "Waiting for valid settings";
        case GuiPreflightStatus::InputChanged:
            return "Input changed, re-estimating";
        case GuiPreflightStatus::Estimating:
            return "Estimating...";
        case GuiPreflightStatus::Ready:
            return "Ready";
        case GuiPreflightStatus::InsufficientDiskSpace:
            return "Insufficient disk space";
        case GuiPreflightStatus::DiskSpaceUnknown:
            return "Disk space unknown";
        case GuiPreflightStatus::OutputSizeEstimateUnavailable:
            return "Output-size estimate unavailable";
        case GuiPreflightStatus::EstimateFailed:
            return "Estimate failed";
    }
    return "Estimate failed";
}
