#pragma once

#include "youtube_sync_state.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace youtube_sync {

struct EndpointSet {
    std::string api_base = "https://www.googleapis.com/youtube/v3";
    std::string upload_base = "https://www.googleapis.com/upload/youtube/v3";
};

struct HttpRequest {
    std::string method;
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct VideoMetadata {
    std::string set_id;
    uint32_t part_index = 0;
    uint32_t part_count = 0;
    std::string source_basename;
    Privacy privacy = Privacy::Unlisted;
    bool privacy_friendly_titles = true;
};

struct ApiError {
    std::string category;
    std::string user_message;
    std::string technical_detail;
    bool retryable = false;
};

[[nodiscard]] std::string privacy_friendly_title(const VideoMetadata &metadata);
[[nodiscard]] std::string video_metadata_json(const VideoMetadata &metadata);
[[nodiscard]] HttpRequest create_playlist_request(
    const EndpointSet &endpoints, std::string_view access_token,
    std::string_view title, Privacy privacy);
[[nodiscard]] HttpRequest add_playlist_item_request(
    const EndpointSet &endpoints, std::string_view access_token,
    std::string_view playlist_id, std::string_view video_id,
    uint32_t position);
[[nodiscard]] HttpRequest processing_status_request(
    const EndpointSet &endpoints, std::string_view access_token,
    const std::vector<std::string> &video_ids);
[[nodiscard]] ApiError classify_api_error(int http_status,
                                          std::string_view response_body,
                                          std::string_view operation);
[[nodiscard]] bool upload_quota_warning(uint32_t part_count) noexcept;

} // namespace youtube_sync
