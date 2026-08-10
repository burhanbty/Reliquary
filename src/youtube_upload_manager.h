#pragma once

#include "youtube_api_client.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace youtube_sync {

inline constexpr uint64_t kDefaultUploadChunkBytes = 8ull * 1024 * 1024;

struct UploadChunk {
    uint64_t first = 0;
    uint64_t last = 0;
    uint64_t total = 0;
    [[nodiscard]] uint64_t size() const noexcept { return last - first + 1; }
};

struct UploadResponseDecision {
    enum class Action { NextChunk, QueryStatus, Completed, SessionExpired,
                        PermanentFailure } action = Action::PermanentFailure;
    uint64_t next_offset = 0;
    std::optional<uint32_t> retry_after_seconds;
    std::string video_id;
};

[[nodiscard]] bool valid_chunk_size(uint64_t chunk_bytes) noexcept;
[[nodiscard]] UploadChunk next_upload_chunk(uint64_t offset,
                                            uint64_t total,
                                            uint64_t chunk_bytes =
                                                kDefaultUploadChunkBytes);
[[nodiscard]] std::optional<uint64_t> committed_bytes_from_range(
    std::string_view range_header);
[[nodiscard]] UploadResponseDecision decide_upload_response(
    int status, std::string_view range_header, std::string_view response_body,
    std::optional<uint32_t> retry_after_seconds = std::nullopt);
[[nodiscard]] uint32_t exponential_backoff_seconds(uint32_t attempt,
                                                   uint32_t cap = 64) noexcept;
[[nodiscard]] HttpRequest resumable_session_request(
    const EndpointSet &endpoints, std::string_view access_token,
    const VideoMetadata &metadata, uint64_t content_length,
    std::string_view mime_type = "video/*");
[[nodiscard]] HttpRequest upload_status_request(
    std::string_view session_uri, std::string_view access_token,
    uint64_t total_length);

} // namespace youtube_sync
