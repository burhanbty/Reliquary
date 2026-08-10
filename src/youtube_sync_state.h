#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace youtube_sync {

enum class Privacy { Private, Unlisted, Public };
enum class UploadState { Pending, SessionCreated, Uploading, Uploaded,
                         SessionExpired, Paused, Failed };
enum class ProcessingState { Waiting, Processing, Succeeded, Failed,
                             TimedOut };

struct PartState {
    uint32_t part_index = 0;
    std::string part_id;
    std::string video_path;
    UploadState upload_state = UploadState::Pending;
    std::string upload_session_uri;
    uint64_t uploaded_bytes = 0;
    std::string youtube_video_id;
    Privacy requested_privacy = Privacy::Unlisted;
    Privacy actual_privacy = Privacy::Private;
    std::string playlist_item_id;
    ProcessingState processing_state = ProcessingState::Waiting;
    uint64_t processing_parts_done = 0;
    uint64_t processing_parts_total = 0;
    std::string returned_download_state = "pending";
};

struct SyncState {
    int version = 1;
    std::string set_id;
    Privacy requested_privacy = Privacy::Unlisted;
    Privacy actual_privacy = Privacy::Private;
    std::string playlist_id;
    std::string playlist_url;
    bool playlist_created = false;
    std::vector<PartState> parts;
    std::string last_updated;
};

[[nodiscard]] std::string_view privacy_name(Privacy value) noexcept;
[[nodiscard]] Privacy parse_privacy(std::string_view value);
[[nodiscard]] std::string_view upload_state_name(UploadState value) noexcept;
[[nodiscard]] std::string_view processing_state_name(ProcessingState value) noexcept;
void validate_sync_state(const SyncState &state,
                         std::string_view expected_set_id = {});
void write_sync_state_atomic(const std::filesystem::path &path,
                             const SyncState &state);
[[nodiscard]] SyncState read_sync_state(const std::filesystem::path &path,
                                        std::string_view expected_set_id = {});

} // namespace youtube_sync
