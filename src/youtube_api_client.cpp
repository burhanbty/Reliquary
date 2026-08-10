#include "youtube_api_client.h"

#include "youtube_auth.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace youtube_sync {
namespace {

std::string escape(std::string_view value) {
    std::string result;
    for (const char c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

std::string url_encode(std::string_view value) {
    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out << static_cast<char>(c);
        else out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}

std::string sanitized_basename(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](const unsigned char c) {
        return c < 0x20 || c == '/' || c == '\\';
    }), value.end());
    if (value.size() > 80) value.resize(80);
    return value;
}

} // namespace

std::string privacy_friendly_title(const VideoMetadata &metadata) {
    const auto prefix = metadata.set_id.substr(0, 8);
    std::ostringstream out;
    out << "VidStoreX - Set " << prefix << " - Part "
        << std::setw(2) << std::setfill('0') << metadata.part_index + 1 << '/'
        << std::setw(2) << std::setfill('0') << metadata.part_count;
    if (!metadata.privacy_friendly_titles && !metadata.source_basename.empty())
        out << " - " << sanitized_basename(metadata.source_basename);
    return out.str();
}

std::string video_metadata_json(const VideoMetadata &metadata) {
    std::ostringstream description;
    description << "VidStoreX Video Set\nSet: " << metadata.set_id.substr(0, 8)
                << "\nPart: " << metadata.part_index + 1 << '/'
                << metadata.part_count;
    return "{\"snippet\":{\"title\":\"" +
        escape(privacy_friendly_title(metadata)) +
        "\",\"description\":\"" + escape(description.str()) +
        "\",\"categoryId\":\"22\"},\"status\":{\"privacyStatus\":\"" +
        std::string(privacy_name(metadata.privacy)) +
        "\",\"selfDeclaredMadeForKids\":false}}";
}

HttpRequest create_playlist_request(const EndpointSet &endpoints,
                                    const std::string_view access_token,
                                    const std::string_view title,
                                    const Privacy privacy) {
    return {"POST", endpoints.api_base + "/playlists?part=snippet,status",
            {{"Authorization", "Bearer " + std::string(access_token)},
             {"Content-Type", "application/json; charset=UTF-8"}},
            "{\"snippet\":{\"title\":\"" + escape(title) +
            "\"},\"status\":{\"privacyStatus\":\"" +
            std::string(privacy_name(privacy)) + "\"}}"};
}

HttpRequest add_playlist_item_request(const EndpointSet &endpoints,
                                      const std::string_view access_token,
                                      const std::string_view playlist_id,
                                      const std::string_view video_id,
                                      const uint32_t position) {
    return {"POST", endpoints.api_base + "/playlistItems?part=snippet",
            {{"Authorization", "Bearer " + std::string(access_token)},
             {"Content-Type", "application/json; charset=UTF-8"}},
            "{\"snippet\":{\"playlistId\":\"" + escape(playlist_id) +
            "\",\"position\":" + std::to_string(position) +
            ",\"resourceId\":{\"kind\":\"youtube#video\",\"videoId\":\"" +
            escape(video_id) + "\"}}}"};
}

HttpRequest processing_status_request(const EndpointSet &endpoints,
                                      const std::string_view access_token,
                                      const std::vector<std::string> &video_ids) {
    std::string joined;
    for (const auto &id : video_ids) {
        if (!joined.empty()) joined += ',';
        joined += id;
    }
    return {"GET", endpoints.api_base +
            "/videos?part=processingDetails,status,contentDetails&id=" +
            url_encode(joined),
            {{"Authorization", "Bearer " + std::string(access_token)}}, {}};
}

ApiError classify_api_error(const int status, const std::string_view body,
                            const std::string_view operation) {
    const std::string text(body);
    ApiError result;
    result.technical_detail = "HTTP " + std::to_string(status) + " during " +
        std::string(operation) + ": " + redact_sensitive(text);
    if (status == 401) {
        result.category = "AuthRequired";
        result.user_message = "YouTube authorization is required.";
    } else if (status == 403 &&
               (text.find("quotaExceeded") != std::string::npos ||
                text.find("uploadLimitExceeded") != std::string::npos)) {
        result.category = "UploadQuotaReached";
        result.user_message = "YouTube API upload quota was reached.";
    } else if (status == 403) {
        result.category = "AuthDenied";
        result.user_message = "YouTube denied this operation.";
    } else if (status == 404 && operation == "upload_status") {
        result.category = "UploadSessionExpired";
        result.user_message = "The YouTube upload session expired. Review before retrying to avoid a duplicate.";
    } else if (status == 500 || status == 502 || status == 503 || status == 504) {
        result.category = "UploadFailed";
        result.user_message = "YouTube is temporarily unavailable. VidStoreX will retry safely.";
        result.retryable = true;
    } else {
        result.category = operation == "playlist_create" ? "PlaylistCreateFailed" :
            operation == "playlist_insert" ? "PlaylistInsertFailed" : "UploadFailed";
        result.user_message = "The YouTube operation could not be completed.";
    }
    return result;
}

bool upload_quota_warning(const uint32_t part_count) noexcept {
    return part_count > 100;
}

} // namespace youtube_sync
