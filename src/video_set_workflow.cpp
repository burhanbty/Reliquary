#include "video_set_workflow.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <stdexcept>

namespace video_set_workflow {
namespace {

int step_for(const ViewState &view) {
    if (view.path == Path::Recover) {
        switch (view.state) {
            case State::Welcome: return 0;
            case State::AwaitingReturnedVideos:
            case State::ScanningReturnedVideos:
            case State::IncompleteMissingParts:
            case State::ConflictDetected:
            case State::CorruptPartsDetected:
            case State::ReadyToRecover: return 7;
            case State::Recovering: return 8;
            case State::RecoveredExact: return 9;
            default: return 7;
        }
    }
    switch (view.state) {
        case State::Welcome: return 0;
        case State::SourceRequired: return 1;
        case State::ReadyToPlan: return 2;
        case State::Planning:
        case State::Planned: return 3;
        case State::Encoding:
        case State::EncodingPaused:
        case State::LocallyVerified: return 4;
        case State::AwaitingUpload: return 5;
        case State::AwaitingReturnedVideos:
        case State::DownloadingReturnedVideos: return 6;
        case State::ScanningReturnedVideos:
        case State::IncompleteMissingParts:
        case State::ConflictDetected:
        case State::CorruptPartsDetected:
        case State::ReadyToRecover: return 7;
        case State::Recovering: return 8;
        case State::RecoveredExact: return 9;
        case State::Failed: return 0;
    }
    return 0;
}

} // namespace

Controller::Controller() { refresh_message(); }

void Controller::reset() {
    view_ = {};
    refresh_message();
}

void Controller::choose_create() {
    view_ = {};
    view_.path = Path::Create;
    view_.state = State::SourceRequired;
    refresh_message();
}

void Controller::choose_recover() {
    view_ = {};
    view_.path = Path::Recover;
    view_.state = State::AwaitingReturnedVideos;
    refresh_message();
}

void Controller::select_source(std::string filename, const uint64_t size) {
    if (view_.path != Path::Create)
        throw std::logic_error("source selection requires Create workflow");
    view_.source_filename = std::move(filename);
    view_.source_size = size;
    view_.state = view_.source_filename.empty()
        ? State::SourceRequired : State::ReadyToPlan;
    refresh_message();
}

void Controller::select_profile(std::string profile, std::string config_id) {
    if (profile != "resilient" && profile != "high-capacity")
        throw std::invalid_argument("guided workflow profile is unsupported");
    view_.selected_profile = std::move(profile);
    view_.config_id = std::move(config_id);
    invalidate_plan();
}

void Controller::invalidate_plan() {
    view_.part_count = 0;
    view_.local_verified_count = 0;
    if (view_.path == Path::Create)
        view_.state = view_.source_filename.empty()
            ? State::SourceRequired : State::ReadyToPlan;
    refresh_message();
}

void Controller::begin_planning() {
    if (view_.path != Path::Create || view_.source_filename.empty())
        throw std::logic_error("planning requires a selected source");
    view_.state = State::Planning;
    refresh_message();
}

void Controller::apply_plan(const video_set::SetPlan &plan) {
    view_.set_id = video_set::id_hex(plan.set_id);
    view_.source_filename = plan.original_filename;
    view_.source_size = plan.original_file_size;
    view_.source_sha256 = plan.original_file_sha256.hexValue();
    view_.selected_profile = plan.profile_name;
    view_.config_id = plan.config_id;
    view_.part_count = static_cast<uint32_t>(plan.parts.size());
    view_.state = State::Planned;
    refresh_message();
}

void Controller::begin_encoding() {
    if (view_.state != State::Planned &&
        view_.state != State::EncodingPaused)
        throw std::logic_error("encoding requires a current plan");
    view_.state = State::Encoding;
    refresh_message();
}

void Controller::cancel_encoding() {
    if (view_.state == State::Encoding)
        view_.state = State::EncodingPaused;
    refresh_message();
}

void Controller::apply_local_verification(const uint32_t exact_parts) {
    view_.local_verified_count = exact_parts;
    if (view_.part_count == 0 || exact_parts != view_.part_count)
        throw std::invalid_argument("local verification is incomplete");
    view_.state = State::LocallyVerified;
    refresh_message();
}

void Controller::show_upload_guide() {
    if (view_.state != State::LocallyVerified)
        throw std::logic_error("upload guidance requires locally verified videos");
    view_.state = State::AwaitingUpload;
    refresh_message();
}

void Controller::acknowledge_upload() {
    if (view_.state != State::LocallyVerified &&
        view_.state != State::AwaitingUpload)
        throw std::logic_error("upload guidance requires locally verified videos");
    view_.state = State::AwaitingReturnedVideos;
    refresh_message();
}

void Controller::begin_download() {
    if (view_.state != State::AwaitingReturnedVideos)
        throw std::logic_error("download requires returned-video step");
    view_.state = State::DownloadingReturnedVideos;
    refresh_message();
}

void Controller::cancel_download() {
    if (view_.state == State::DownloadingReturnedVideos)
        view_.state = State::AwaitingReturnedVideos;
    refresh_message();
}

void Controller::begin_scan() {
    view_.state = State::ScanningReturnedVideos;
    refresh_message();
}

void Controller::apply_scan(ScanSummary summary) {
    view_.scan = std::move(summary);
    view_.part_count = view_.scan.expected_parts;
    if (view_.scan.conflict_count != 0)
        view_.state = State::ConflictDetected;
    else if (!view_.scan.corrupt_parts.empty())
        view_.state = State::CorruptPartsDetected;
    else if (!view_.scan.missing_parts.empty() ||
             view_.scan.returned_parts < view_.scan.expected_parts)
        view_.state = State::IncompleteMissingParts;
    else
        view_.state = State::ReadyToRecover;
    refresh_message();
}

void Controller::begin_recovery() {
    if (view_.state != State::ReadyToRecover)
        throw std::logic_error("recovery requires a complete verified set");
    view_.state = State::Recovering;
    refresh_message();
}

void Controller::cancel_recovery() {
    if (view_.state == State::Recovering)
        view_.state = State::ReadyToRecover;
    view_.can_resume = true;
    refresh_message();
}

void Controller::apply_recovery_result(std::string output_path,
                                       const bool full_sha_exact) {
    view_.final_output_path = std::move(output_path);
    view_.final_sha_exact = full_sha_exact;
    view_.state = full_sha_exact ? State::RecoveredExact : State::Failed;
    refresh_message();
}

void Controller::fail(std::string message, std::string suggested_action) {
    view_.state = State::Failed;
    view_.primary_message = std::move(message);
    view_.suggested_action = std::move(suggested_action);
    view_.current_step = step_for(view_);
    view_.can_continue = false;
    view_.can_recover = false;
}

void Controller::resume_from_manifest(const video_set::SetPlan &plan) {
    view_ = {};
    view_.path = Path::Create;
    apply_plan(plan);
    view_.local_verified_count = static_cast<uint32_t>(std::count_if(
        plan.parts.begin(), plan.parts.end(), [](const auto &part) {
            return part.local_decode_state == "Exact";
        }));
    if (plan.aggregate_state == "Recovered exact") {
        view_.state = State::RecoveredExact;
        view_.final_sha_exact = true;
        view_.final_output_path = plan.final_output_path;
    } else if (view_.part_count != 0 &&
               view_.local_verified_count == view_.part_count) {
        view_.state = State::AwaitingUpload;
    } else if (view_.local_verified_count != 0 ||
               plan.aggregate_state == "Encoding") {
        view_.state = State::EncodingPaused;
        view_.can_resume = true;
    }
    refresh_message();
}

void Controller::refresh_message() {
    view_.current_step = step_for(view_);
    view_.can_continue = false;
    view_.can_recover = false;
    view_.can_resume = view_.state == State::EncodingPaused;
    switch (view_.state) {
        case State::Welcome:
            view_.primary_message = "Store one file across multiple videos, or recover a file from downloaded videos.";
            view_.suggested_action = "Choose Create or Recover.";
            break;
        case State::SourceRequired:
            view_.primary_message = "Choose the file you want to turn into videos.";
            view_.suggested_action = "Choose a readable file and an output folder.";
            break;
        case State::ReadyToPlan:
            view_.primary_message = "Choose how you want to balance reliability and video count.";
            view_.suggested_action = "Continue to calculate the plan.";
            view_.can_continue = true;
            break;
        case State::Planning:
            view_.primary_message = "Calculating a safe Video Set plan...";
            view_.suggested_action = "The source file is being read without modification.";
            break;
        case State::Planned:
            view_.primary_message = "Your file will be divided into " +
                std::to_string(view_.part_count) + " videos.";
            view_.suggested_action = "Review the estimate, then create the videos.";
            view_.can_continue = true;
            break;
        case State::Encoding:
            view_.primary_message = "Creating and checking each video...";
            view_.suggested_action = "You can cancel safely and continue later.";
            break;
        case State::EncodingPaused:
            view_.primary_message = "Video creation was paused safely.";
            view_.suggested_action = "You can continue this Video Set later.";
            break;
        case State::LocallyVerified:
            view_.primary_message = "All " + std::to_string(view_.part_count) +
                " videos were created and verified locally.";
            view_.suggested_action = "Next, upload every video as Unlisted.";
            view_.can_continue = true;
            break;
        case State::AwaitingUpload:
            view_.primary_message = "Upload every video as Unlisted and wait for 1080p processing to finish.";
            view_.suggested_action = "Put the videos in one playlist, then continue.";
            view_.can_continue = true;
            break;
        case State::AwaitingReturnedVideos:
            view_.primary_message = "Paste the playlist link to download YouTube's processed copies.";
            view_.suggested_action = "You can also choose downloaded videos manually.";
            break;
        case State::DownloadingReturnedVideos:
            view_.primary_message = "Downloading returned videos...";
            view_.suggested_action = "The folder will be scanned automatically when download finishes.";
            break;
        case State::ScanningReturnedVideos:
            view_.primary_message = "Checking embedded Video Set information...";
            view_.suggested_action = "Filenames and playlist order are not used as identity.";
            break;
        case State::IncompleteMissingParts:
            view_.primary_message = std::to_string(view_.scan.missing_parts.size()) +
                " of " + std::to_string(view_.scan.expected_parts) + " parts are missing.";
            view_.suggested_action = "Download the missing parts, then scan the folder again.";
            break;
        case State::ConflictDetected:
            view_.primary_message = "Different videos claim to be the same part.";
            view_.suggested_action = "Remove the conflicting copy and scan again.";
            break;
        case State::CorruptPartsDetected:
            view_.primary_message = "One or more parts could not be verified.";
            view_.suggested_action = "Download the affected videos again, then rescan.";
            break;
        case State::ReadyToRecover:
            view_.primary_message = "All " + std::to_string(view_.scan.returned_parts) +
                " of " + std::to_string(view_.scan.expected_parts) +
                " parts were found and verified.";
            view_.suggested_action = "Your file is ready to recover.";
            view_.can_recover = true;
            break;
        case State::Recovering:
            view_.primary_message = "Recovering and checking the original file...";
            view_.suggested_action = "Success is shown only after the full SHA-256 matches.";
            break;
        case State::RecoveredExact:
            view_.primary_message = "Your file was recovered exactly.";
            view_.suggested_action = "The full-file SHA-256 matches the original.";
            view_.can_continue = true;
            break;
        case State::Failed:
            if (view_.primary_message.empty())
                view_.primary_message = "The operation could not be completed.";
            if (view_.suggested_action.empty())
                view_.suggested_action = "Open technical details, correct the problem, and retry.";
            break;
    }
}

std::string_view state_name(const State state) noexcept {
    switch (state) {
        case State::Welcome: return "Welcome";
        case State::SourceRequired: return "SourceRequired";
        case State::ReadyToPlan: return "ReadyToPlan";
        case State::Planning: return "Planning";
        case State::Planned: return "Planned";
        case State::Encoding: return "Encoding";
        case State::EncodingPaused: return "EncodingPaused";
        case State::LocallyVerified: return "LocallyVerified";
        case State::AwaitingUpload: return "AwaitingUpload";
        case State::AwaitingReturnedVideos: return "AwaitingReturnedVideos";
        case State::DownloadingReturnedVideos: return "DownloadingReturnedVideos";
        case State::ScanningReturnedVideos: return "ScanningReturnedVideos";
        case State::IncompleteMissingParts: return "IncompleteMissingParts";
        case State::ConflictDetected: return "ConflictDetected";
        case State::CorruptPartsDetected: return "CorruptPartsDetected";
        case State::ReadyToRecover: return "ReadyToRecover";
        case State::Recovering: return "Recovering";
        case State::RecoveredExact: return "RecoveredExact";
        case State::Failed: return "Failed";
    }
    return "Failed";
}

PlanSummary summarize_plan(const video_set::SetPlan &plan) {
    PlanSummary result;
    result.part_count = static_cast<uint32_t>(plan.parts.size());
    result.recovery_disk_bytes = plan.original_file_size;
    for (const auto &part : plan.parts) {
        result.maximum_part_duration_seconds =
            (std::max)(result.maximum_part_duration_seconds,
                       part.estimated_duration_seconds);
        result.total_duration_seconds += part.estimated_duration_seconds;
        result.total_estimated_output_bytes += part.estimated_output_bytes;
        result.temporary_disk_bytes = (std::max)(
            result.temporary_disk_bytes,
            part.chunk_size + part.estimated_output_bytes);
    }
    return result;
}

bool is_youtube_playlist_url(const std::string_view url) noexcept {
    if (url.find_first_of("\r\n\t ") != std::string_view::npos)
        return false;
    std::size_t scheme_end = 0;
    if (url.starts_with("https://")) scheme_end = 8;
    else if (url.starts_with("http://")) scheme_end = 7;
    else return false;

    const auto authority_end = url.find_first_of("/?#", scheme_end);
    if (authority_end == scheme_end) return false;
    std::string host(url.substr(
        scheme_end, authority_end == std::string_view::npos
            ? url.size() - scheme_end : authority_end - scheme_end));
    if (host.find_first_of("@:") != std::string::npos) return false;
    std::transform(host.begin(), host.end(), host.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    const bool youtube_host = host == "youtube.com" ||
        host == "youtu.be" ||
        (host.size() > 12 && host.ends_with(".youtube.com"));
    if (!youtube_host) return false;

    const auto query_start = url.find('?', scheme_end);
    if (query_start == std::string_view::npos) return false;
    const auto fragment_start = url.find('#', query_start + 1);
    const auto query = url.substr(
        query_start + 1, fragment_start == std::string_view::npos
            ? url.size() - query_start - 1
            : fragment_start - query_start - 1);
    std::size_t offset = 0;
    while (offset <= query.size()) {
        const auto separator = query.find('&', offset);
        const auto parameter = query.substr(
            offset, separator == std::string_view::npos
                ? query.size() - offset : separator - offset);
        if (parameter.starts_with("list=") && parameter.size() > 5)
            return true;
        if (separator == std::string_view::npos) break;
        offset = separator + 1;
    }
    return false;
}

std::vector<std::string> ytdlp_arguments(
    const std::string_view playlist_url,
    const std::filesystem::path &returned_directory) {
    if (!is_youtube_playlist_url(playlist_url))
        throw std::invalid_argument("enter a valid YouTube playlist URL");
    const auto output = returned_directory /
        std::filesystem::path("%(title)s [%(id)s].%(ext)s");
    return {"--newline", "--no-part", "--format",
            std::string(kYtDlpFormatSelector), "--output",
            output.string(), std::string(playlist_url)};
}

std::string select_ytdlp_executable(
    std::string path_candidate,
    std::vector<std::string> tool_candidates,
    std::string saved_candidate,
    const std::function<bool(std::string_view)> &is_executable) {
    if (!path_candidate.empty() && is_executable(path_candidate))
        return path_candidate;
    for (auto &candidate : tool_candidates)
        if (!candidate.empty() && is_executable(candidate))
            return candidate;
    if (!saved_candidate.empty() && is_executable(saved_candidate))
        return saved_candidate;
    return {};
}

DownloadProgress parse_ytdlp_progress(const std::string_view output) {
    DownloadProgress result;
    const std::string text(output);
    std::smatch match;
    if (std::regex_search(text, match,
            std::regex(R"(([0-9]+(?:\.[0-9]+)?)%)")))
        result.percent = std::stod(match[1].str());
    if (std::regex_search(text, match,
            std::regex(R"(Downloading item\s+(\d+)\s+of\s+(\d+))",
                       std::regex_constants::icase))) {
        result.current_item = static_cast<uint32_t>(
            std::stoul(match[1].str()));
        result.total_items = static_cast<uint32_t>(
            std::stoul(match[2].str()));
    }
    return result;
}

std::vector<RecentSet> add_recent_set(std::vector<RecentSet> recent,
                                      RecentSet entry,
                                      const std::size_t maximum) {
    recent.erase(std::remove_if(recent.begin(), recent.end(),
        [&](const RecentSet &item) {
            return item.manifest_path == entry.manifest_path ||
                   item.manifest_path.empty();
        }), recent.end());
    if (!entry.manifest_path.empty()) recent.insert(recent.begin(), std::move(entry));
    if (recent.size() > maximum) recent.resize(maximum);
    return recent;
}

} // namespace video_set_workflow
