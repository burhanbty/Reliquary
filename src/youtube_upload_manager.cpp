#include "youtube_upload_manager.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace youtube_sync {

bool valid_chunk_size(const uint64_t chunk_bytes) noexcept {
    return chunk_bytes != 0 && chunk_bytes % (256ull * 1024) == 0;
}

UploadChunk next_upload_chunk(const uint64_t offset, const uint64_t total,
                              const uint64_t chunk_bytes) {
    if (!valid_chunk_size(chunk_bytes))
        throw std::invalid_argument("upload chunk size must be a multiple of 256 KiB");
    if (offset >= total) throw std::invalid_argument("upload offset is outside the file");
    return {offset, (std::min)(total - 1, offset + chunk_bytes - 1), total};
}

std::optional<uint64_t> committed_bytes_from_range(
    const std::string_view range) {
    const auto dash = range.rfind('-');
    if (dash == std::string_view::npos) return std::nullopt;
    std::size_t end = dash + 1;
    while (end < range.size() && std::isdigit(static_cast<unsigned char>(range[end]))) ++end;
    if (end == dash + 1) return std::nullopt;
    try { return std::stoull(std::string(range.substr(dash + 1, end - dash - 1))) + 1; }
    catch (...) { return std::nullopt; }
}

UploadResponseDecision decide_upload_response(
    const int status, const std::string_view range,
    const std::string_view body,
    const std::optional<uint32_t> retry_after) {
    UploadResponseDecision result;
    result.retry_after_seconds = retry_after;
    if (status == 200 || status == 201) {
        result.action = UploadResponseDecision::Action::Completed;
        const std::string json(body);
        const auto marker = json.find("\"id\"");
        if (marker != std::string::npos) {
            const auto colon = json.find(':', marker);
            const auto quote = json.find('"', colon + 1);
            const auto end = json.find('"', quote + 1);
            if (quote != std::string::npos && end != std::string::npos)
                result.video_id = json.substr(quote + 1, end - quote - 1);
        }
        return result;
    }
    if (status == 308) {
        result.action = UploadResponseDecision::Action::NextChunk;
        result.next_offset = committed_bytes_from_range(range).value_or(0);
        return result;
    }
    if (status == 404) {
        result.action = UploadResponseDecision::Action::SessionExpired;
        return result;
    }
    if (status == 0 || status == 500 || status == 502 || status == 503 || status == 504) {
        result.action = UploadResponseDecision::Action::QueryStatus;
        return result;
    }
    result.action = UploadResponseDecision::Action::PermanentFailure;
    return result;
}

uint32_t exponential_backoff_seconds(const uint32_t attempt,
                                     const uint32_t cap) noexcept {
    if (attempt >= 31) return cap;
    return (std::min)(cap, 1u << attempt);
}

HttpRequest resumable_session_request(const EndpointSet &endpoints,
                                      const std::string_view access_token,
                                      const VideoMetadata &metadata,
                                      const uint64_t content_length,
                                      const std::string_view mime_type) {
    return {"POST", endpoints.upload_base +
            "/videos?uploadType=resumable&part=snippet,status",
            {{"Authorization", "Bearer " + std::string(access_token)},
             {"Content-Type", "application/json; charset=UTF-8"},
             {"X-Upload-Content-Length", std::to_string(content_length)},
             {"X-Upload-Content-Type", std::string(mime_type)}},
            video_metadata_json(metadata)};
}

HttpRequest upload_status_request(const std::string_view session_uri,
                                  const std::string_view access_token,
                                  const uint64_t total_length) {
    return {"PUT", std::string(session_uri),
            {{"Authorization", "Bearer " + std::string(access_token)},
             {"Content-Length", "0"},
             {"Content-Range", "bytes */" + std::to_string(total_length)}}, {}};
}

} // namespace youtube_sync
