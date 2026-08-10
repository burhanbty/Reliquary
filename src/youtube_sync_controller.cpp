#include "youtube_sync_controller.h"

#include <algorithm>

namespace youtube_sync {

uint32_t processing_poll_delay_seconds(const uint32_t attempt) noexcept {
    return attempt < 3 ? 15u : 30u;
}

bool processing_poll_timed_out(const int64_t started_at_ms,
                               const int64_t now_ms,
                               const int64_t timeout_ms) noexcept {
    return started_at_ms > 0 && now_ms >= started_at_ms &&
        now_ms - started_at_ms >= timeout_ms;
}

void YouTubeSyncController::configure(const bool configured,
                                      const bool connected) {
    view_ = {};
    if (!configured) {
        view_.phase = SyncPhase::NotConfigured;
        view_.message = "YouTube Sync is not configured for this build.";
    } else if (!connected) {
        view_.phase = SyncPhase::Disconnected;
        view_.message = "Connect YouTube to use optional automatic sync.";
    } else {
        view_.phase = SyncPhase::Ready;
        view_.message = "YouTube Sync is ready.";
    }
}

void YouTubeSyncController::load(const SyncState &state) { refresh(state); }

void YouTubeSyncController::begin_playlist_creation() {
    view_.phase = SyncPhase::CreatingPlaylist;
    view_.message = "Creating the Video Set playlist.";
}

void YouTubeSyncController::begin_upload() {
    view_.phase = SyncPhase::Uploading;
    view_.message = "Uploading Video Set parts.";
}

void YouTubeSyncController::pause() {
    view_.phase = SyncPhase::Paused;
    view_.message = "Upload paused. Already uploaded videos remain on YouTube.";
    view_.can_resume = true;
}

void YouTubeSyncController::begin_processing() {
    view_.phase = SyncPhase::Processing;
    view_.message = "YouTube is processing the uploaded videos.";
}

void YouTubeSyncController::refresh(const SyncState &state) {
    view_.total_parts = static_cast<uint32_t>(state.parts.size());
    view_.uploaded_parts = static_cast<uint32_t>(std::count_if(
        state.parts.begin(), state.parts.end(), [](const PartState &part) {
            return part.upload_state == UploadState::Uploaded;
        }));
    view_.processed_parts = static_cast<uint32_t>(std::count_if(
        state.parts.begin(), state.parts.end(), [](const PartState &part) {
            return part.processing_state == ProcessingState::Succeeded;
        }));
    if (state.actual_privacy == Privacy::Private && view_.uploaded_parts > 0) {
        view_.phase = SyncPhase::PrivateRestriction;
        view_.message = "Videos were uploaded successfully, but they are Private. Automatic download of YouTube's processed copies is unavailable in this configuration.";
    } else if (view_.total_parts != 0 &&
               view_.processed_parts == view_.total_parts) {
        view_.phase = SyncPhase::ReadyToDownload;
        view_.message = "YouTube processed copies are ready to download and verify.";
    } else if (view_.uploaded_parts == view_.total_parts && view_.total_parts != 0) {
        begin_processing();
    } else if (view_.uploaded_parts != 0) {
        begin_upload();
        view_.can_resume = true;
    } else {
        view_.phase = SyncPhase::Ready;
        view_.message = "YouTube Sync is ready.";
    }
}

void YouTubeSyncController::fail(std::string message) {
    view_.phase = SyncPhase::NeedsAttention;
    view_.message = std::move(message);
    view_.can_resume = true;
}

} // namespace youtube_sync
