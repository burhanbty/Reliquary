#include "video_set_workflow.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
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

std::size_t json_value_start(const std::string_view json,
                             const std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto key_position = json.find(needle);
    if (key_position == std::string_view::npos)
        return std::string_view::npos;
    const auto colon = json.find(':', key_position + needle.size());
    if (colon == std::string_view::npos)
        throw std::invalid_argument("missing JSON colon");
    const auto value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string_view::npos)
        throw std::invalid_argument("missing JSON value");
    return value;
}

std::optional<std::string> json_string(const std::string_view json,
                                       const std::string_view key) {
    auto position = json_value_start(json, key);
    if (position == std::string_view::npos) return std::nullopt;
    if (json[position++] != '"')
        throw std::invalid_argument("JSON value is not a string");
    std::string result;
    while (position < json.size()) {
        const char value = json[position++];
        if (value == '"') return result;
        if (value != '\\') {
            if (static_cast<unsigned char>(value) < 0x20)
                throw std::invalid_argument("control character in JSON string");
            result.push_back(value);
            continue;
        }
        if (position >= json.size())
            throw std::invalid_argument("unterminated JSON escape");
        switch (const char escaped = json[position++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default:
                throw std::invalid_argument(
                    std::string("unsupported JSON escape: ") + escaped);
        }
    }
    throw std::invalid_argument("unterminated JSON string");
}

std::optional<uint64_t> json_u64(const std::string_view json,
                                 const std::string_view key) {
    const auto position = json_value_start(json, key);
    if (position == std::string_view::npos) return std::nullopt;
    auto end = position;
    while (end < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == position) throw std::invalid_argument("invalid JSON integer");
    uint64_t result = 0;
    const auto parsed = std::from_chars(
        json.data() + position, json.data() + end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != json.data() + end)
        throw std::invalid_argument("JSON integer overflow");
    return result;
}

std::optional<int> json_int(const std::string_view json,
                            const std::string_view key) {
    const auto position = json_value_start(json, key);
    if (position == std::string_view::npos) return std::nullopt;
    auto end = position;
    if (end < json.size() && json[end] == '-') ++end;
    while (end < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == position) throw std::invalid_argument("invalid JSON integer");
    int result = 0;
    const auto parsed = std::from_chars(
        json.data() + position, json.data() + end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != json.data() + end)
        throw std::invalid_argument("JSON integer overflow");
    return result;
}

std::optional<double> json_double(const std::string_view json,
                                  const std::string_view key) {
    const auto position = json_value_start(json, key);
    if (position == std::string_view::npos) return std::nullopt;
    const std::string tail(json.substr(position));
    char *end = nullptr;
    const double result = std::strtod(tail.c_str(), &end);
    if (end == tail.c_str() || !std::isfinite(result))
        throw std::invalid_argument("invalid JSON number");
    return result;
}

std::optional<bool> json_bool(const std::string_view json,
                              const std::string_view key) {
    const auto position = json_value_start(json, key);
    if (position == std::string_view::npos) return std::nullopt;
    if (json.substr(position, 4) == "true") return true;
    if (json.substr(position, 5) == "false") return false;
    throw std::invalid_argument("invalid JSON boolean");
}

template <typename Target>
void assign_if_present(Target &target, const std::optional<Target> &value) {
    if (value.has_value()) target = *value;
}

} // namespace

uint64_t OperationProgressModel::begin(
    const OperationType type,
    const OperationPhase phase,
    const int64_t now_ms,
    std::string primary_message,
    std::string secondary_message) {
    progress_ = {};
    progress_.operation_id = ++next_operation_id_;
    progress_.operation_type = type;
    progress_.phase = phase;
    progress_.state = OperationState::Running;
    progress_.primary_message = std::move(primary_message);
    progress_.secondary_message = std::move(secondary_message);
    progress_.can_cancel = true;
    progress_.is_busy = true;
    progress_.started_at_ms = now_ms;
    progress_.last_progress_at_ms = now_ms;
    eta_from_event_ = false;
    refresh_derived(now_ms);
    return progress_.operation_id;
}

bool OperationProgressModel::apply(const OperationEvent &event,
                                   const int64_t now_ms) {
    if (event.operation_id == 0 ||
        event.operation_id != progress_.operation_id)
        return false;
    if (progress_.state != OperationState::Running &&
        progress_.state != OperationState::Cancelling)
        return false;

    if (event.operation_type != OperationType::None)
        progress_.operation_type = event.operation_type;
    progress_.phase = event.phase;
    progress_.state = event.state;
    if (!event.primary_message.empty())
        progress_.primary_message = event.primary_message;
    if (!event.secondary_message.empty())
        progress_.secondary_message = event.secondary_message;
    if (!event.current_item_name.empty())
        progress_.current_item_name = event.current_item_name;
    assign_if_present(progress_.current_index, event.current_index);
    assign_if_present(progress_.total_items, event.total_items);
    assign_if_present(progress_.completed_items, event.completed_items);
    assign_if_present(progress_.progress_current, event.progress_current);
    assign_if_present(progress_.progress_total, event.progress_total);
    progress_.progress_is_determinate = event.progress_is_determinate &&
        event.progress_total.value_or(progress_.progress_total) != 0;
    if (event.estimated_remaining_seconds.has_value()) {
        progress_.estimated_remaining_seconds =
            event.estimated_remaining_seconds;
        eta_from_event_ = true;
    } else {
        eta_from_event_ = false;
    }
    progress_.can_retry = event.can_retry;
    if (!event.technical_detail.empty())
        progress_.technical_detail = event.technical_detail;
    progress_.backend_exit_code = event.backend_exit_code;
    if (!event.suggested_action.empty())
        progress_.suggested_action = event.suggested_action;
    if (!event.status.empty()) progress_.status = event.status;
    if (!event.sha256.empty()) progress_.sha256 = event.sha256;
    if (!event.output_path.empty())
        progress_.output_path = event.output_path;
    if (!event.file_name.empty()) progress_.file_name = event.file_name;
    assign_if_present(progress_.file_size, event.file_size);
    if (!event.profile_name.empty())
        progress_.profile_name = event.profile_name;
    assign_if_present(progress_.part_count, event.part_count);
    if (event.has_scan_summary) {
        progress_.scan = event.scan;
        progress_.has_scan_summary = true;
    }
    progress_.last_progress_at_ms = now_ms;
    refresh_derived(now_ms);
    return true;
}

bool OperationProgressModel::tick(const int64_t now_ms) {
    if (progress_.state == OperationState::Idle) return false;
    refresh_derived(now_ms);
    return true;
}

bool OperationProgressModel::request_cancel(const int64_t now_ms) {
    if (progress_.state == OperationState::Cancelling) return true;
    if (progress_.state != OperationState::Running) return false;
    progress_.state = OperationState::Cancelling;
    progress_.primary_message = "Cancelling...";
    progress_.secondary_message =
        "Waiting for the current safe boundary. Completed work will be kept.";
    progress_.can_cancel = false;
    progress_.is_busy = true;
    refresh_derived(now_ms);
    return true;
}

bool OperationProgressModel::complete(const uint64_t operation_id,
                                      const int64_t now_ms,
                                      std::string message) {
    if (operation_id != progress_.operation_id || operation_id == 0)
        return false;
    progress_.state = OperationState::Completed;
    progress_.phase = OperationPhase::Completed;
    if (!message.empty()) progress_.primary_message = std::move(message);
    progress_.can_cancel = false;
    progress_.can_continue = true;
    progress_.is_busy = false;
    progress_.taking_longer_than_usual = false;
    refresh_derived(now_ms);
    return true;
}

bool OperationProgressModel::cancel(const uint64_t operation_id,
                                    const int64_t now_ms,
                                    std::string message) {
    if (operation_id != progress_.operation_id || operation_id == 0)
        return false;
    progress_.state = OperationState::Cancelled;
    progress_.phase = OperationPhase::Cancelled;
    progress_.primary_message = message.empty()
        ? "Operation cancelled. You can continue later."
        : std::move(message);
    progress_.can_cancel = false;
    progress_.can_retry = true;
    progress_.is_busy = false;
    progress_.taking_longer_than_usual = false;
    refresh_derived(now_ms);
    return true;
}

bool OperationProgressModel::fail(const uint64_t operation_id,
                                  const int64_t now_ms,
                                  const int exit_code,
                                  std::string message,
                                  std::string suggested_action) {
    if (operation_id != progress_.operation_id || operation_id == 0)
        return false;
    progress_.state = OperationState::Failed;
    progress_.phase = OperationPhase::Failed;
    progress_.primary_message = std::move(message);
    progress_.suggested_action = std::move(suggested_action);
    progress_.backend_exit_code = exit_code;
    progress_.can_cancel = false;
    progress_.can_retry = true;
    progress_.is_busy = false;
    progress_.taking_longer_than_usual = false;
    refresh_derived(now_ms);
    return true;
}

void OperationProgressModel::reset() {
    progress_ = {};
    eta_from_event_ = false;
}

void OperationProgressModel::refresh_derived(const int64_t now_ms) {
    if (progress_.started_at_ms > 0 && now_ms >= progress_.started_at_ms)
        progress_.elapsed_seconds =
            static_cast<double>(now_ms - progress_.started_at_ms) / 1000.0;
    progress_.is_busy = progress_.state == OperationState::Running ||
        progress_.state == OperationState::Cancelling;
    progress_.can_cancel = progress_.state == OperationState::Running;
    progress_.taking_longer_than_usual = progress_.is_busy &&
        progress_.last_progress_at_ms > 0 &&
        now_ms - progress_.last_progress_at_ms >= 30000;

    if (progress_.progress_is_determinate &&
        progress_.progress_total != 0) {
        progress_.progress_current = (std::min)(
            progress_.progress_current, progress_.progress_total);
        progress_.progress_percent = (std::clamp)(
            static_cast<double>(progress_.progress_current) * 100.0 /
                static_cast<double>(progress_.progress_total),
            0.0, 100.0);
        if (!eta_from_event_ && progress_.progress_current != 0 &&
            progress_.progress_current < progress_.progress_total &&
            progress_.elapsed_seconds > 0.0) {
            progress_.estimated_remaining_seconds =
                progress_.elapsed_seconds /
                static_cast<double>(progress_.progress_current) *
                static_cast<double>(progress_.progress_total -
                                    progress_.progress_current);
        }
    } else {
        progress_.progress_percent.reset();
        if (!eta_from_event_)
            progress_.estimated_remaining_seconds.reset();
    }
}

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
    view_.scan = {};
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
            view_.suggested_action = "When download finishes, choose Check Videos.";
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
            view_.primary_message =
                "One or more parts are corrupt or could not be verified.";
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

std::string_view operation_type_name(const OperationType type) noexcept {
    switch (type) {
        case OperationType::None: return "none";
        case OperationType::Plan: return "plan";
        case OperationType::Encode: return "encode";
        case OperationType::Download: return "download";
        case OperationType::Scan: return "scan";
        case OperationType::Recover: return "recover";
        case OperationType::FinalHash: return "final_hash";
        case OperationType::Finalize: return "finalize";
        case OperationType::OAuth: return "oauth";
        case OperationType::PlaylistCreate: return "playlist_create";
        case OperationType::Upload: return "upload";
        case OperationType::YouTubeProcessing: return "youtube_processing";
        case OperationType::ProcessedDownload: return "processed_download";
        case OperationType::InstantRecovery: return "instant_recovery";
    }
    return "none";
}

std::string_view operation_phase_name(const OperationPhase phase) noexcept {
    switch (phase) {
        case OperationPhase::Idle: return "idle";
        case OperationPhase::Preparing: return "preparing";
        case OperationPhase::DiscoveringFiles: return "discovering_files";
        case OperationPhase::ReadingMetadata: return "reading_metadata";
        case OperationPhase::HashingSource: return "hashing_source";
        case OperationPhase::CalculatingPlan: return "calculating_plan";
        case OperationPhase::EncodingPart: return "encoding_part";
        case OperationPhase::DecodingVideo: return "decoding_video";
        case OperationPhase::VerifyingPart: return "verifying_part";
        case OperationPhase::WritingChunk: return "writing_chunk";
        case OperationPhase::CheckingFullFile: return "checking_full_file";
        case OperationPhase::Finalizing: return "finalizing";
        case OperationPhase::Completed: return "completed";
        case OperationPhase::Cancelled: return "cancelled";
        case OperationPhase::Failed: return "failed";
        case OperationPhase::Unknown: return "unknown";
    }
    return "unknown";
}

OperationType parse_operation_type(const std::string_view value) noexcept {
    if (value == "plan") return OperationType::Plan;
    if (value == "encode") return OperationType::Encode;
    if (value == "download") return OperationType::Download;
    if (value == "scan") return OperationType::Scan;
    if (value == "recover") return OperationType::Recover;
    if (value == "final_hash") return OperationType::FinalHash;
    if (value == "finalize") return OperationType::Finalize;
    if (value == "oauth") return OperationType::OAuth;
    if (value == "playlist_create") return OperationType::PlaylistCreate;
    if (value == "upload") return OperationType::Upload;
    if (value == "youtube_processing") return OperationType::YouTubeProcessing;
    if (value == "processed_download") return OperationType::ProcessedDownload;
    if (value == "instant_recovery") return OperationType::InstantRecovery;
    return OperationType::None;
}

OperationPhase parse_operation_phase(const std::string_view value) noexcept {
    if (value == "idle") return OperationPhase::Idle;
    if (value == "preparing") return OperationPhase::Preparing;
    if (value == "discovering_files") return OperationPhase::DiscoveringFiles;
    if (value == "reading_metadata") return OperationPhase::ReadingMetadata;
    if (value == "hashing_source") return OperationPhase::HashingSource;
    if (value == "calculating_plan") return OperationPhase::CalculatingPlan;
    if (value == "encoding_part") return OperationPhase::EncodingPart;
    if (value == "decoding_video") return OperationPhase::DecodingVideo;
    if (value == "verifying_part") return OperationPhase::VerifyingPart;
    if (value == "writing_chunk") return OperationPhase::WritingChunk;
    if (value == "checking_full_file") return OperationPhase::CheckingFullFile;
    if (value == "finalizing") return OperationPhase::Finalizing;
    if (value == "completed") return OperationPhase::Completed;
    if (value == "cancelled") return OperationPhase::Cancelled;
    if (value == "failed") return OperationPhase::Failed;
    return OperationPhase::Unknown;
}

std::optional<OperationEvent> parse_progress_jsonl(
    const std::string_view line) noexcept {
    try {
        const auto first = line.find_first_not_of(" \t\r\n");
        const auto last = line.find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos || line[first] != '{' ||
            last == std::string_view::npos || line[last] != '}')
            return std::nullopt;
        const auto type = json_string(line, "type");
        const auto operation_id = json_u64(line, "operation_id");
        const auto operation = json_string(line, "operation");
        if (!type || !operation_id || !operation || *operation_id == 0)
            return std::nullopt;
        if (*type != "progress" && *type != "result" && *type != "error")
            return std::nullopt;

        OperationEvent event;
        event.operation_id = *operation_id;
        event.operation_type = parse_operation_type(*operation);
        if (event.operation_type == OperationType::None)
            return std::nullopt;
        event.state = *type == "result" ? OperationState::Completed :
            (*type == "error" ? OperationState::Failed :
                                OperationState::Running);
        if (const auto phase = json_string(line, "phase"))
            event.phase = parse_operation_phase(*phase);
        else if (event.state == OperationState::Completed)
            event.phase = OperationPhase::Completed;
        else if (event.state == OperationState::Failed)
            event.phase = OperationPhase::Failed;

        assign_if_present(event.primary_message, json_string(line, "message"));
        assign_if_present(event.secondary_message,
                          json_string(line, "secondary_message"));
        assign_if_present(event.current_item_name,
                          json_string(line, "current_item"));
        event.current_index = json_u64(line, "current");
        event.total_items = json_u64(line, "total");
        event.completed_items = json_u64(line, "completed");
        event.progress_current = json_u64(line, "progress_current");
        event.progress_total = json_u64(line, "progress_total");
        event.estimated_remaining_seconds =
            json_double(line, "estimated_remaining_seconds");
        event.progress_is_determinate =
            json_bool(line, "determinate").value_or(false);
        event.can_retry = json_bool(line, "can_retry").value_or(false);
        assign_if_present(event.technical_detail,
                          json_string(line, "technical_detail"));
        event.backend_exit_code = json_int(line, "exit_code").value_or(0);
        assign_if_present(event.suggested_action,
                          json_string(line, "suggested_action"));
        assign_if_present(event.status, json_string(line, "status"));
        assign_if_present(event.sha256, json_string(line, "sha256"));
        assign_if_present(event.output_path,
                           json_string(line, "output_path"));
        assign_if_present(event.file_name, json_string(line, "file_name"));
        event.file_size = json_u64(line, "file_size");
        assign_if_present(event.profile_name,
                          json_string(line, "profile_name"));
        event.part_count = json_u64(line, "part_count");

        const auto candidates = json_u64(line, "candidates");
        const auto checked = json_u64(line, "checked");
        const auto verified = json_u64(line, "verified");
        const auto expected = json_u64(line, "expected_parts");
        const auto returned = json_u64(line, "returned_parts");
        const auto missing = json_u64(line, "missing");
        const auto corrupt = json_u64(line, "corrupt");
        const auto duplicates = json_u64(line, "duplicates");
        const auto conflicts = json_u64(line, "conflicts");
        event.has_scan_summary = candidates || checked || verified ||
            expected || returned || missing || corrupt || duplicates ||
            conflicts;
        if (event.has_scan_summary) {
            event.scan.expected_parts = static_cast<uint32_t>(
                expected.value_or(event.total_items.value_or(0)));
            event.scan.returned_parts = static_cast<uint32_t>(
                returned.value_or(verified.value_or(0)));
            event.scan.exact_parts = static_cast<uint32_t>(
                verified.value_or(event.scan.returned_parts));
            event.scan.duplicate_count = static_cast<uint32_t>(
                duplicates.value_or(0));
            event.scan.conflict_count = static_cast<uint32_t>(
                conflicts.value_or(0));
            const auto missing_count = static_cast<uint32_t>(
                missing.value_or(0));
            const auto corrupt_count = static_cast<uint32_t>(
                corrupt.value_or(0));
            event.scan.missing_parts.resize(missing_count);
            event.scan.corrupt_parts.resize(corrupt_count);
        }
        return event;
    } catch (...) {
        return std::nullopt;
    }
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
    const auto parse_size = [](const std::string &number,
                               const std::string &unit) -> uint64_t {
        const double value = std::stod(number);
        const auto normalized = [&] {
            std::string lowered = unit;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](const unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            return lowered;
        }();
        double multiplier = 1.0;
        if (normalized == "kib") multiplier = 1024.0;
        else if (normalized == "mib") multiplier = 1024.0 * 1024.0;
        else if (normalized == "gib")
            multiplier = 1024.0 * 1024.0 * 1024.0;
        return static_cast<uint64_t>(value * multiplier);
    };
    if (std::regex_search(text, match,
            std::regex(R"(([0-9]+(?:\.[0-9]+)?)(KiB|MiB|GiB)\s+of\s+~?\s*([0-9]+(?:\.[0-9]+)?)(KiB|MiB|GiB))",
                       std::regex_constants::icase))) {
        result.downloaded_bytes = parse_size(match[1].str(), match[2].str());
        result.total_bytes = parse_size(match[3].str(), match[4].str());
    } else if (result.percent && std::regex_search(text, match,
            std::regex(R"(of\s+~?\s*([0-9]+(?:\.[0-9]+)?)(KiB|MiB|GiB))",
                       std::regex_constants::icase))) {
        result.total_bytes = parse_size(match[1].str(), match[2].str());
        result.downloaded_bytes = static_cast<uint64_t>(
            static_cast<double>(*result.total_bytes) * *result.percent / 100.0);
    }
    if (std::regex_search(text, match,
            std::regex(R"(at\s+([0-9]+(?:\.[0-9]+)?)(KiB|MiB|GiB)/s)",
                       std::regex_constants::icase)))
        result.speed_bytes_per_second = static_cast<double>(
            parse_size(match[1].str(), match[2].str()));
    if (std::regex_search(text, match,
            std::regex(R"(ETA\s+(?:(\d+):)?(\d+):(\d+))",
                       std::regex_constants::icase))) {
        const uint32_t hours = match[1].matched
            ? static_cast<uint32_t>(std::stoul(match[1].str())) : 0;
        result.eta_seconds = hours * 3600u +
            static_cast<uint32_t>(std::stoul(match[2].str())) * 60u +
            static_cast<uint32_t>(std::stoul(match[3].str()));
    }
    if (std::regex_search(text, match,
            std::regex(R"(\[download\]\s+Destination:\s+(.+?)(?:\r?\n|$))",
                       std::regex_constants::icase)))
        result.destination_filename = match[1].str();
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
