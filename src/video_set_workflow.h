#pragma once

#include "video_set.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video_set_workflow {

enum class Path { None, Create, Recover };

enum class OperationType {
    None,
    Plan,
    Encode,
    Download,
    Scan,
    Recover,
    FinalHash,
    Finalize,
    OAuth,
    PlaylistCreate,
    Upload,
    YouTubeProcessing,
    ProcessedDownload,
    InstantRecovery
};

enum class OperationPhase {
    Idle,
    Preparing,
    DiscoveringFiles,
    ReadingMetadata,
    HashingSource,
    CalculatingPlan,
    EncodingPart,
    DecodingVideo,
    VerifyingPart,
    WritingChunk,
    CheckingFullFile,
    Finalizing,
    Completed,
    Cancelled,
    Failed,
    Unknown
};

enum class OperationState {
    Idle,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Failed
};

enum class OperationDomain {
    None,
    Create,
    Recover
};

enum class PresentationPage {
    None,
    CreateSource,
    CreateMode,
    CreatePlan,
    CreateProgress,
    RecoverDownload,
    RecoverSetup,
    RecoverProgress
};

enum class State {
    Welcome,
    SourceRequired,
    ReadyToPlan,
    Planning,
    Planned,
    Encoding,
    EncodingPaused,
    LocallyVerified,
    AwaitingUpload,
    AwaitingReturnedVideos,
    DownloadingReturnedVideos,
    ScanningReturnedVideos,
    IncompleteMissingParts,
    ConflictDetected,
    CorruptPartsDetected,
    ReadyToRecover,
    Recovering,
    RecoveredExact,
    Failed
};

struct ScanSummary {
    uint32_t expected_parts = 0;
    uint32_t returned_parts = 0;
    uint32_t exact_parts = 0;
    std::vector<uint32_t> missing_parts;
    std::vector<uint32_t> corrupt_parts;
    uint32_t duplicate_count = 0;
    uint32_t conflict_count = 0;
};

struct ViewState {
    Path path = Path::None;
    State state = State::Welcome;
    int current_step = 0;
    std::string set_id;
    std::string source_filename;
    uint64_t source_size = 0;
    std::string source_sha256;
    std::string selected_profile = "resilient";
    std::string config_id;
    uint32_t part_count = 0;
    uint32_t local_verified_count = 0;
    ScanSummary scan;
    std::string final_output_path;
    bool final_sha_exact = false;
    std::string primary_message;
    std::string suggested_action;
    bool can_continue = false;
    bool can_recover = false;
    bool can_resume = false;
};

struct PlanSummary {
    uint32_t part_count = 0;
    double maximum_part_duration_seconds = 0.0;
    double total_duration_seconds = 0.0;
    uint64_t total_estimated_output_bytes = 0;
    uint64_t temporary_disk_bytes = 0;
    uint64_t recovery_disk_bytes = 0;
};

struct RecentSet {
    std::string manifest_path;
    std::string display_name;
    std::string status;
    uint32_t part_count = 0;
    int64_t last_opened_epoch_seconds = 0;
};

struct DownloadProgress {
    std::optional<double> percent;
    std::optional<uint32_t> current_item;
    std::optional<uint32_t> total_items;
    std::optional<uint64_t> downloaded_bytes;
    std::optional<uint64_t> total_bytes;
    std::optional<double> speed_bytes_per_second;
    std::optional<uint32_t> eta_seconds;
    std::string destination_filename;
};

struct OperationEvent {
    uint64_t operation_id = 0;
    OperationType operation_type = OperationType::None;
    OperationPhase phase = OperationPhase::Unknown;
    OperationState state = OperationState::Running;
    std::string primary_message;
    std::string secondary_message;
    std::string current_item_name;
    std::optional<uint64_t> current_index;
    std::optional<uint64_t> total_items;
    std::optional<uint64_t> completed_items;
    std::optional<uint64_t> progress_current;
    std::optional<uint64_t> progress_total;
    std::optional<double> estimated_remaining_seconds;
    bool progress_is_determinate = false;
    bool can_retry = false;
    std::string technical_detail;
    int backend_exit_code = 0;
    std::string suggested_action;
    std::string status;
    std::string sha256;
    std::string output_path;
    std::string file_name;
    std::optional<uint64_t> file_size;
    std::string profile_name;
    std::optional<uint64_t> part_count;
    ScanSummary scan;
    bool has_scan_summary = false;
};

struct OperationProgress {
    uint64_t operation_id = 0;
    OperationType operation_type = OperationType::None;
    OperationPhase phase = OperationPhase::Idle;
    OperationState state = OperationState::Idle;
    std::string primary_message;
    std::string secondary_message;
    std::string current_item_name;
    uint64_t current_index = 0;
    uint64_t total_items = 0;
    uint64_t completed_items = 0;
    uint64_t progress_current = 0;
    uint64_t progress_total = 0;
    std::optional<double> progress_percent;
    bool progress_is_determinate = false;
    double elapsed_seconds = 0.0;
    std::optional<double> estimated_remaining_seconds;
    bool can_cancel = false;
    bool can_retry = false;
    bool can_continue = false;
    bool is_busy = false;
    bool taking_longer_than_usual = false;
    int64_t started_at_ms = 0;
    int64_t finished_at_ms = 0;
    int64_t last_progress_at_ms = 0;
    std::string technical_detail;
    int backend_exit_code = 0;
    std::string suggested_action;
    std::string status;
    std::string sha256;
    std::string output_path;
    std::string file_name;
    uint64_t file_size = 0;
    std::string profile_name;
    uint64_t part_count = 0;
    ScanSummary scan;
    bool has_scan_summary = false;
};

class OperationProgressModel {
public:
    [[nodiscard]] const OperationProgress &view() const noexcept {
        return progress_;
    }

    uint64_t begin(OperationType type,
                   OperationPhase phase,
                   int64_t now_ms,
                   std::string primary_message,
                   std::string secondary_message = {});
    [[nodiscard]] bool apply(const OperationEvent &event,
                             int64_t now_ms);
    [[nodiscard]] bool tick(int64_t now_ms);
    [[nodiscard]] bool request_cancel(int64_t now_ms);
    [[nodiscard]] bool complete(uint64_t operation_id,
                                int64_t now_ms,
                                std::string message = {});
    [[nodiscard]] bool cancel(uint64_t operation_id,
                              int64_t now_ms,
                              std::string message = {});
    [[nodiscard]] bool fail(uint64_t operation_id,
                            int64_t now_ms,
                            int exit_code,
                            std::string message,
                            std::string suggested_action = {});
    void reset();

private:
    void refresh_derived(int64_t now_ms);
    uint64_t next_operation_id_ = 0;
    bool eta_from_event_ = false;
    OperationProgress progress_;
};

class Controller {
public:
    Controller();

    [[nodiscard]] const ViewState &view() const noexcept { return view_; }

    void reset();
    void choose_create();
    void choose_recover();
    void select_source(std::string filename, uint64_t size);
    void select_profile(std::string profile, std::string config_id);
    void invalidate_plan();
    void begin_planning();
    void apply_plan(const video_set::SetPlan &plan);
    void begin_encoding();
    void cancel_encoding();
    void apply_local_verification(uint32_t exact_parts);
    void show_upload_guide();
    void acknowledge_upload();
    void begin_download();
    void cancel_download();
    void begin_scan();
    void apply_scan(ScanSummary summary);
    void begin_recovery();
    void cancel_recovery();
    void apply_recovery_result(std::string output_path,
                               bool full_sha_exact);
    void fail(std::string message, std::string suggested_action);
    void resume_from_manifest(const video_set::SetPlan &plan);

private:
    void refresh_message();
    ViewState view_;
};

[[nodiscard]] std::string_view state_name(State state) noexcept;
[[nodiscard]] std::string_view operation_type_name(
    OperationType type) noexcept;
[[nodiscard]] std::string_view operation_phase_name(
    OperationPhase phase) noexcept;
[[nodiscard]] OperationType parse_operation_type(
    std::string_view value) noexcept;
[[nodiscard]] OperationPhase parse_operation_phase(
    std::string_view value) noexcept;
[[nodiscard]] OperationDomain operation_domain(
    OperationType type) noexcept;
[[nodiscard]] bool should_present_operation(
    PresentationPage page,
    const OperationProgress &operation,
    bool current_plan_matches) noexcept;
[[nodiscard]] std::optional<OperationEvent> parse_progress_jsonl(
    std::string_view line) noexcept;
[[nodiscard]] PlanSummary summarize_plan(const video_set::SetPlan &plan);
[[nodiscard]] bool is_youtube_playlist_url(std::string_view url) noexcept;
[[nodiscard]] std::vector<std::string> ytdlp_arguments(
    std::string_view playlist_url,
    const std::filesystem::path &returned_directory);
[[nodiscard]] std::string select_ytdlp_executable(
    std::string path_candidate,
    std::vector<std::string> tool_candidates,
    std::string saved_candidate,
    const std::function<bool(std::string_view)> &is_executable);
[[nodiscard]] DownloadProgress parse_ytdlp_progress(
    std::string_view output);
[[nodiscard]] std::vector<RecentSet> add_recent_set(
    std::vector<RecentSet> recent,
    RecentSet entry,
    std::size_t maximum = 5);

inline constexpr std::string_view kYtDlpFormatSelector =
    "bv*[height=1080]/bv*[height<=1080]";

} // namespace video_set_workflow
