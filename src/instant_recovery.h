#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace instant_recovery {

enum class Phase {
    ValidatingPlaylist,
    PreparingJob,
    Downloading,
    Scanning,
    SelectingSet,
    ReadyToRecover,
    Recovering,
    CheckingFinalSha,
    RecoveredExact,
    NeedsAttention,
    Failed,
    Cancelled
};

struct SetCandidate {
    std::string set_id;
    std::string source_filename;
    uint32_t expected_parts = 0;
    uint32_t exact_parts = 0;
    uint32_t duplicate_count = 0;
    uint32_t corrupt_count = 0;
    uint32_t conflict_count = 0;
};

enum class SelectionStatus { Selected, NoneRecoverable, MultipleRecoverable };

struct Selection {
    SelectionStatus status = SelectionStatus::NoneRecoverable;
    std::optional<std::string> set_id;
    std::string message;
};

struct JobState {
    int version = 1;
    std::string job_id;
    std::string playlist_url;
    std::string output_directory;
    std::string selected_set_id;
    std::string final_output_path;
    std::string final_sha256;
    Phase phase = Phase::PreparingJob;
    bool explicit_auto_recover = true;
    bool final_sha_exact = false;
    int64_t updated_at_epoch_seconds = 0;
};

[[nodiscard]] std::string_view phase_name(Phase phase) noexcept;
[[nodiscard]] Phase parse_phase(std::string_view value);
[[nodiscard]] Selection select_single_complete_set(
    const std::vector<SetCandidate> &candidates);
[[nodiscard]] bool may_auto_recover(const JobState &job,
                                    bool output_exists,
                                    const Selection &selection) noexcept;
[[nodiscard]] std::string make_job_id();
[[nodiscard]] std::filesystem::path default_jobs_root();
void initialize_job_directories(const std::filesystem::path &job_root);
void write_job_state_atomic(const std::filesystem::path &path,
                            const JobState &state);
[[nodiscard]] JobState read_job_state(const std::filesystem::path &path);

} // namespace instant_recovery
