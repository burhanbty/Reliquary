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
