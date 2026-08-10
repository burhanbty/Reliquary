#pragma once

#include "youtube_sync_state.h"

#include <cstdint>
#include <string>

namespace youtube_sync {

enum class SyncPhase { NotConfigured, Disconnected, Ready, CreatingPlaylist,
                       Uploading, Paused, Processing, ReadyToDownload,
                       PrivateRestriction, Completed, NeedsAttention };

struct SyncView {
    SyncPhase phase = SyncPhase::NotConfigured;
    uint32_t uploaded_parts = 0;
    uint32_t processed_parts = 0;
    uint32_t total_parts = 0;
    std::string message;
    bool can_resume = false;
    bool can_use_manual_fallback = true;
};

[[nodiscard]] uint32_t processing_poll_delay_seconds(
    uint32_t attempt) noexcept;
[[nodiscard]] bool processing_poll_timed_out(
    int64_t started_at_ms, int64_t now_ms,
    int64_t timeout_ms = 60ll * 60ll * 1000ll) noexcept;

class YouTubeSyncController {
public:
    void configure(bool configured, bool connected);
    void load(const SyncState &state);
    void begin_playlist_creation();
    void begin_upload();
    void pause();
    void begin_processing();
    void refresh(const SyncState &state);
    void fail(std::string message);
    [[nodiscard]] const SyncView &view() const noexcept { return view_; }

private:
    SyncView view_;
};

} // namespace youtube_sync
