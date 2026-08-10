#include "youtube_sync_state.h"

#include "safe_output.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace youtube_sync {
namespace {

std::string escape(std::string_view value) {
    std::string out;
    for (const char c : value) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string str(const std::string &json, const std::string_view key,
                const std::size_t from = 0) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto at = json.find(marker, from);
    if (at == std::string::npos) return {};
    at = json.find(':', at + marker.size());
    at = json.find('"', at == std::string::npos ? at : at + 1);
    if (at == std::string::npos) return {};
    std::string out;
    bool escaped = false;
    for (++at; at < json.size(); ++at) {
        if (escaped) { out += json[at]; escaped = false; }
        else if (json[at] == '\\') escaped = true;
        else if (json[at] == '"') return out;
        else out += json[at];
    }
    throw std::runtime_error("invalid YouTube sync JSON");
}

uint64_t number(const std::string &json, const std::string_view key,
                const std::size_t from = 0) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto at = json.find(marker, from);
    if (at == std::string::npos) return 0;
    at = json.find(':', at + marker.size());
    if (at == std::string::npos) return 0;
    ++at;
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) ++at;
    std::size_t end = at;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    return end == at ? 0 : std::stoull(json.substr(at, end - at));
}

bool boolean(const std::string &json, const std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto at = json.find(marker);
    if (at == std::string::npos) return false;
    at = json.find(':', at + marker.size());
    return at != std::string::npos && json.substr(at + 1, 7).find("true") != std::string::npos;
}

UploadState parse_upload(const std::string_view value) {
    if (value == "pending") return UploadState::Pending;
    if (value == "session_created") return UploadState::SessionCreated;
    if (value == "uploading") return UploadState::Uploading;
    if (value == "uploaded") return UploadState::Uploaded;
    if (value == "session_expired") return UploadState::SessionExpired;
    if (value == "paused") return UploadState::Paused;
    return UploadState::Failed;
}

ProcessingState parse_processing(const std::string_view value) {
    if (value == "waiting") return ProcessingState::Waiting;
    if (value == "processing") return ProcessingState::Processing;
    if (value == "succeeded") return ProcessingState::Succeeded;
    if (value == "timed_out") return ProcessingState::TimedOut;
    return ProcessingState::Failed;
}

} // namespace

std::string_view privacy_name(const Privacy value) noexcept {
    switch (value) {
        case Privacy::Private: return "private";
        case Privacy::Unlisted: return "unlisted";
        case Privacy::Public: return "public";
    }
    return "private";
}

Privacy parse_privacy(const std::string_view value) {
    if (value == "private") return Privacy::Private;
    if (value == "unlisted") return Privacy::Unlisted;
    if (value == "public") return Privacy::Public;
    throw std::invalid_argument("invalid YouTube privacy value");
}

std::string_view upload_state_name(const UploadState value) noexcept {
    switch (value) {
        case UploadState::Pending: return "pending";
        case UploadState::SessionCreated: return "session_created";
        case UploadState::Uploading: return "uploading";
        case UploadState::Uploaded: return "uploaded";
        case UploadState::SessionExpired: return "session_expired";
        case UploadState::Paused: return "paused";
        case UploadState::Failed: return "failed";
    }
    return "failed";
}

std::string_view processing_state_name(const ProcessingState value) noexcept {
    switch (value) {
        case ProcessingState::Waiting: return "waiting";
        case ProcessingState::Processing: return "processing";
        case ProcessingState::Succeeded: return "succeeded";
        case ProcessingState::Failed: return "failed";
        case ProcessingState::TimedOut: return "timed_out";
    }
    return "failed";
}

void validate_sync_state(const SyncState &state,
                         const std::string_view expected_set_id) {
    if (state.set_id.empty()) throw std::invalid_argument("YouTube sync state has no set ID");
    if (!expected_set_id.empty() && state.set_id != expected_set_id)
        throw std::invalid_argument("YouTube sync state belongs to another Video Set");
    std::set<uint32_t> indices;
    for (const auto &part : state.parts) {
        if (!indices.insert(part.part_index).second)
            throw std::invalid_argument("YouTube sync state contains a duplicate part");
        if (part.upload_state == UploadState::Uploaded && part.youtube_video_id.empty())
            throw std::invalid_argument("uploaded YouTube part has no video ID");
    }
}

void write_sync_state_atomic(const std::filesystem::path &path,
                             const SyncState &state) {
    validate_sync_state(state);
    std::filesystem::create_directories(path.parent_path());
    SafeOutputFile safe(path);
    std::ofstream out(safe.partial_path(), std::ios::binary);
    out << "{\n  \"schema\": \"vidstorex.youtube_sync\",\n  \"version\": 1,\n"
        << "  \"set_id\": \"" << escape(state.set_id) << "\",\n"
        << "  \"playlist\": {\"requested_privacy\": \""
        << privacy_name(state.requested_privacy) << "\", \"actual_privacy\": \""
        << privacy_name(state.actual_privacy) << "\", \"playlist_id\": \""
        << escape(state.playlist_id) << "\", \"playlist_url\": \""
        << escape(state.playlist_url) << "\", \"created\": "
        << (state.playlist_created ? "true" : "false") << "},\n  \"parts\": [\n";
    for (std::size_t i = 0; i < state.parts.size(); ++i) {
        const auto &p = state.parts[i];
        out << "    {\"part_index\":" << p.part_index << ",\"part_id\":\""
            << escape(p.part_id) << "\",\"video_path\":\"" << escape(p.video_path)
            << "\",\"upload_state\":\"" << upload_state_name(p.upload_state)
            << "\",\"upload_session_uri\":\"" << escape(p.upload_session_uri)
            << "\",\"uploaded_bytes\":" << p.uploaded_bytes
            << ",\"youtube_video_id\":\"" << escape(p.youtube_video_id)
            << "\",\"requested_privacy\":\"" << privacy_name(p.requested_privacy)
            << "\",\"actual_privacy\":\"" << privacy_name(p.actual_privacy)
            << "\",\"playlist_item_id\":\"" << escape(p.playlist_item_id)
            << "\",\"processing_state\":\"" << processing_state_name(p.processing_state)
            << "\",\"processing_parts_done\":" << p.processing_parts_done
            << ",\"processing_parts_total\":" << p.processing_parts_total
            << ",\"returned_download_state\":\"" << escape(p.returned_download_state) << "\"}"
            << (i + 1 == state.parts.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"last_updated\": \"" << escape(state.last_updated) << "\"\n}\n";
    out.close();
    if (!out) throw std::runtime_error("could not write YouTube sync state");
    safe.commit();
}

SyncState read_sync_state(const std::filesystem::path &path,
                          const std::string_view expected_set_id) {
    std::ifstream in(path, std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(in)), {});
    if (json.find("\"schema\": \"vidstorex.youtube_sync\"") == std::string::npos ||
        json.find("\"version\": 1") == std::string::npos)
        throw std::runtime_error("unsupported YouTube sync state");
    SyncState state;
    state.set_id = str(json, "set_id");
    state.requested_privacy = parse_privacy(str(json, "requested_privacy"));
    const auto actual_at = json.find("\"actual_privacy\"");
    state.actual_privacy = parse_privacy(str(json, "actual_privacy", actual_at));
    state.playlist_id = str(json, "playlist_id");
    state.playlist_url = str(json, "playlist_url");
    state.playlist_created = boolean(json, "created");
    state.last_updated = str(json, "last_updated");
    const auto parts_at = json.find("\"parts\"");
    auto cursor = parts_at;
    while ((cursor = json.find("\"part_index\"", cursor)) != std::string::npos) {
        PartState part;
        part.part_index = static_cast<uint32_t>(number(json, "part_index", cursor));
        part.part_id = str(json, "part_id", cursor);
        part.video_path = str(json, "video_path", cursor);
        part.upload_state = parse_upload(str(json, "upload_state", cursor));
        part.upload_session_uri = str(json, "upload_session_uri", cursor);
        part.uploaded_bytes = number(json, "uploaded_bytes", cursor);
        part.youtube_video_id = str(json, "youtube_video_id", cursor);
        part.requested_privacy = parse_privacy(str(json, "requested_privacy", cursor));
        part.actual_privacy = parse_privacy(str(json, "actual_privacy", cursor));
        part.playlist_item_id = str(json, "playlist_item_id", cursor);
        part.processing_state = parse_processing(str(json, "processing_state", cursor));
        part.processing_parts_done = number(json, "processing_parts_done", cursor);
        part.processing_parts_total = number(json, "processing_parts_total", cursor);
        part.returned_download_state = str(json, "returned_download_state", cursor);
        state.parts.push_back(std::move(part));
        ++cursor;
    }
    validate_sync_state(state, expected_set_id);
    return state;
}

} // namespace youtube_sync
