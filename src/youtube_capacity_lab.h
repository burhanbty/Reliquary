#pragma once

#include "configuration.h"
#include "youtube_test_lab.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace youtube_capacity_lab {

inline constexpr int kManifestSchemaVersion = 5;
inline constexpr std::size_t kDefaultMaximumCases = 64;
inline constexpr std::size_t kAbsoluteMaximumCases = 192;
inline constexpr uint64_t kDefaultMaximumDiskBytes =
    20ULL * 1024 * 1024 * 1024;
inline constexpr uint64_t kMinimumFreeDiskBytes =
    512ULL * 1024 * 1024;
inline constexpr uint64_t kMinimumFrames = 60;
inline constexpr int kFps = 30;
inline constexpr const char *kTransformVersion = "dct-separable-v1";
inline constexpr const char *kModulation1Version =
    "production-sign-ac01-v1";
inline constexpr const char *kModulation2Version =
    "gray4level-ac01-v1";
inline constexpr const char *kThresholdVersion =
    "nearest-level-v1";

enum class Preset {
    Smoke, Staged, Boundary1080p, OneBitVerification1080p, Custom
};
enum class CaseState {
    Pending,
    Running,
    Passed,
    Rejected,
    Cancelled,
    Shortlisted,
    Unavailable,
    ResolutionUnsupported,
    Failed
};

struct ExperimentConfig {
    int block_width = 8;
    int block_height = 8;
    int bits_per_block = 1;
    // Stored as fixed-point thousandths to keep serialization canonical.
    int signal_milli = 1000;
    int repair_basis_points = 500;
    int resolution_width = 1920;
    int resolution_height = 1080;
    int fps = kFps;
    int packet_symbol_size = static_cast<int>(SYMBOL_SIZE_BYTES);
    std::string transform_version = kTransformVersion;
    std::string modulation_version = kModulation1Version;
    std::string decoder_threshold_version = kThresholdVersion;
    bool interleaving = false;
    std::string created_with_version = "1.4.0";

    [[nodiscard]] bool valid(std::string *reason = nullptr) const;
    [[nodiscard]] std::string canonical_serialization() const;
    [[nodiscard]] std::string config_id() const;
    [[nodiscard]] double signal_multiplier() const noexcept {
        return static_cast<double>(signal_milli) / 1000.0;
    }
    [[nodiscard]] double repair_percent() const noexcept {
        return static_cast<double>(repair_basis_points) / 100.0;
    }
};

struct BlockGeometry {
    int frame_width = 0;
    int frame_height = 0;
    int block_width = 0;
    int block_height = 0;
    int blocks_per_row = 0;
    int blocks_per_column = 0;
    uint64_t total_blocks = 0;
    uint64_t header_sync_blocks = 0;
    uint64_t data_blocks = 0;
    uint64_t raw_bits_per_frame = 0;
    uint64_t raw_bytes_per_frame = 0;
    uint64_t packets_per_frame = 0;
    uint64_t packet_header_bytes_per_frame = 0;
    uint64_t source_payload_bytes_per_frame = 0;
    int unused_right_pixels = 0;
    int unused_bottom_pixels = 0;
};

struct SymbolDecision {
    uint8_t symbol = 0;
    double confidence = 0.0;
    double nearest_level_distance = 0.0;
    double coefficient = 0.0;
};

struct CapacityMetrics {
    BlockGeometry geometry;
    uint64_t useful_bits_per_frame = 0;
    uint64_t source_payload_bytes_per_frame = 0;
    uint64_t minimum_payload_bytes = 0;
    uint64_t expected_source_packets = 0;
    uint64_t expected_repair_packets = 0;
    uint64_t expected_total_packets = 0;
    uint64_t expected_frames = 0;
    double expected_duration_seconds = 0.0;
    double useful_payload_bytes_per_second = 0.0;
    uint64_t estimated_master_bytes = 0;
    uint64_t estimated_candidate_bytes = 0;
    uint64_t estimated_required_disk_bytes = 0;
    uint64_t estimated_peak_memory_bytes = 0;
    double raw_capacity_gain = 1.0;
    double useful_payload_gain = 1.0;
    double frame_reduction = 0.0;
    double duration_reduction = 0.0;
};

struct RecoveryTelemetry {
    uint64_t frames_read = 0;
    uint64_t symbols_compared = 0;
    uint64_t symbol_errors = 0;
    uint64_t bits_compared = 0;
    uint64_t bit_errors = 0;
    uint64_t extracted_packets = 0;
    uint64_t valid_unique_packets = 0;
    uint64_t duplicate_packets = 0;
    uint64_t crc_invalid_packets = 0;
    uint64_t missing_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    uint64_t required_packet_threshold = 0;
    int64_t recovery_margin_packets = 0;
    double recovery_margin_percent = 0.0;
    double packet_recovery_percent = 0.0;
    double raw_ber = 0.0;
    double raw_ser = 0.0;
    double average_confidence = 0.0;
    double minimum_confidence = 0.0;
};

struct CaseResult {
    std::string config_id;
    std::string boundary_case_id;
    std::string payload_instance_id;
    std::string source_type;
    std::string simulation_profile;
    std::string analysis_session_label;
    std::string imported_at;
    std::string analyzed_at;
    std::string analyzed_file_sha256;
    std::string codec;
    std::string pixel_format;
    int returned_width = 0;
    int returned_height = 0;
    double returned_fps = 0.0;
    int64_t bitrate = 0;
    uint64_t file_size = 0;
    double returned_duration = 0.0;
    double encode_seconds = 0.0;
    double transcode_seconds = 0.0;
    double decode_seconds = 0.0;
    bool metadata_valid = false;
    bool decode_completed = false;
    bool sha256_match = false;
    RecoveryTelemetry telemetry;
    std::string restored_sha256;
    std::string error;
};

struct CapacityCase {
    std::string case_id;
    std::string boundary_case_id;
    std::string config_id;
    int stage = 1;
    ExperimentConfig config;
    CapacityMetrics capacity;
    double boundary_density_gain = 0.0;
    uint64_t requested_payload_bytes = 0;
    uint64_t effective_payload_bytes = 0;
    uint64_t payload_seed = 0;
    std::string deterministic_stream_id;
    std::string payload_family_id;
    std::string payload_instance_id;
    std::string role;
    std::string payload_mode;
    std::string source_case_id;
    std::string source_config_id;
    std::string source_payload_sha256;
    bool source_payload_reused = false;
    bool source_payload_regenerated = false;
    std::string source_payload_validation;
    uint64_t payload_prefix_bytes = 0;
    double payload_entropy_estimate = 0.0;
    std::string source_sha256;
    std::string payload_path;
    std::string master_path;
    std::string candidate_path;
    std::string restored_path;
    std::string requested_simulation_profile = "yt-sim-1080p-medium";
    uint64_t master_size = 0;
    uint64_t candidate_size = 0;
    CaseState state = CaseState::Pending;
    bool mandatory_gates_passed = false;
    bool dominated = false;
    bool pareto = false;
    bool shortlisted = false;
    bool eligible_for_shortlist = false;
    bool incomplete = false;
    std::string category;
    std::string rejection_reason;
    std::string shortlist_reason;
    std::string shortlist_exclusion_reason;
    std::string failed_mandatory_profile;
    std::string local_gate_status;
    std::string local_evidence_status;
    std::string real_youtube_status;
    std::string overall_evidence_status;
    std::string last_real_analysis_event;
    bool production_codec_path = false;
    bool simulation_warning = false;
    std::vector<CaseResult> results;
};

struct HistoricalEvidence {
    std::string source_experiment_id;
    std::string source_case_id;
    std::string config_id;
    std::string payload_instance_id;
    std::string source_payload_sha256;
    std::string session_label;
    std::string returned_file_sha256;
    std::string observation_fingerprint;
    double packet_recovery_percent = 0.0;
    double recovery_margin_percent = 0.0;
    bool exact = false;
};

struct ExperimentManifest {
    int schema_version = kManifestSchemaVersion;
    std::string experiment_id;
    std::string created_at;
    std::string vidstorex_version = "1.4.0";
    Preset preset = Preset::Smoke;
    ExperimentConfig baseline;
    std::size_t maximum_cases = kDefaultMaximumCases;
    std::size_t maximum_shortlist_videos = 8;
    uint64_t maximum_disk_bytes = kDefaultMaximumDiskBytes;
    bool cancelled = false;
    bool include_simulation_failures = false;
    std::string source_experiment_id;
    std::string source_manifest_path;
    std::string source_manifest_sha256;
    std::string source_payload_validation;
    std::vector<HistoricalEvidence> historical_evidence;
    std::vector<std::string> mandatory_stage1_profiles{
        "yt-sim-1080p-medium"};
    std::vector<std::string> mandatory_stage3_profiles{
        "yt-sim-1080p-light",
        "yt-sim-1080p-medium",
        "yt-sim-1080p-heavy"};
    std::vector<CapacityCase> cases;
};

struct EligibilityDecision {
    std::string config_id;
    bool eligible = false;
    bool rejected = false;
    bool incomplete = false;
    std::string failed_mandatory_profile;
    std::string reason;
    std::optional<std::size_t> representative_case;
};

struct ValidationIssue {
    std::string code;
    std::string config_id;
    std::string detail;
};

struct ValidationReport {
    std::size_t total_configs = 0;
    std::size_t eligible_configs = 0;
    std::size_t rejected_configs = 0;
    std::size_t incomplete_configs = 0;
    std::size_t pareto_configs = 0;
    std::size_t shortlisted_configs = 0;
    std::vector<ValidationIssue> issues;
};

struct ShortlistRegenerationReport {
    std::size_t eligible_configs = 0;
    std::size_t selected_configs = 0;
    std::vector<std::string> rejected_configs;
    std::vector<std::string> removed_files;
    std::vector<std::string> created_files;
    std::filesystem::path manifest_backup;
    std::filesystem::path previous_shortlist_archive;
};

struct RunOptions {
    Preset preset = Preset::Smoke;
    std::filesystem::path output_root;
    std::vector<int> block_sizes{8, 6, 4};
    std::vector<int> bits_per_block{1, 2};
    std::vector<int> signal_milli{750, 1000, 1250, 1500};
    std::vector<int> repair_basis_points{0, 100, 200, 500};
    std::vector<std::pair<int, int>> resolutions{{1920, 1080}};
    std::vector<std::string> simulations{"yt-sim-1080p-medium"};
    std::size_t maximum_cases = kDefaultMaximumCases;
    uint64_t maximum_disk_bytes = kDefaultMaximumDiskBytes;
    std::size_t maximum_shortlist_videos = 8;
    bool allow_low_disk = false;
    bool estimate_only = false;
    bool include_simulation_failures = false;
    std::filesystem::path source_manifest;
};

struct Preflight {
    uint64_t raw_combination_count = 0;
    uint64_t staged_maximum_cases = 0;
    uint64_t estimated_transcodes = 0;
    uint64_t estimated_total_frames = 0;
    uint64_t estimated_output_bytes = 0;
    uint64_t safety_margin_bytes = 0;
    uint64_t required_disk_bytes = 0;
    std::optional<uint64_t> available_disk_bytes;
    double estimated_seconds = 0.0;
    bool disk_space_sufficient = false;
};

struct Progress {
    std::size_t completed_cases = 0;
    std::size_t total_cases = 0;
    int stage = 0;
    std::string active_config_id;
    uint64_t disk_used_bytes = 0;
};

enum class DensityEvidence { Untested, Pass, Fail };

struct BoundaryDensityResult {
    double gain = 0.0;
    std::size_t profiles_tested = 0;
    std::size_t exact_passes = 0;
    std::size_t failures = 0;
    double best_margin_percent = 0.0;
    DensityEvidence evidence = DensityEvidence::Untested;
};

struct BoundaryInference {
    std::string baseline_status = "Not uploaded/tested";
    std::vector<BoundaryDensityResult> densities;
    std::optional<double> highest_exact_pass;
    std::optional<double> lowest_failure_above;
    std::string status = "Insufficient observations";
    std::string bracket = "Insufficient observations";
    bool non_monotonic = false;
    bool retest_required = true;
    std::string safe_candidate_config_id;
    std::string next_experiment;
};

enum class GeometryEvidence {
    Untested, InitialPass, VerifiedPass, MixedResult, Fail
};

struct CaseObservationResult {
    std::string case_id;
    std::string config_id;
    std::string payload_instance_id;
    std::size_t current_observation_count = 0;
    std::size_t exact_pass_count = 0;
    std::size_t failure_count = 0;
    std::size_t duplicate_count = 0;
    double best_recovery = 0.0;
    double best_margin = 0.0;
    std::string current_status = "untested";
    std::vector<std::string> failure_reasons;
};

struct GeometryDensityResult {
    int block_size = 0;
    double gain = 0.0;
    std::size_t historical_passes = 0;
    std::size_t historical_failures = 0;
    std::size_t current_passes = 0;
    std::size_t failures = 0;
    std::size_t unique_configs = 0;
    std::size_t unique_cases = 0;
    std::size_t unique_payload_instances = 0;
    std::size_t observation_count = 0;
    std::size_t exact_pass_count = 0;
    double best_margin_percent = 0.0;
    GeometryEvidence evidence = GeometryEvidence::Untested;
};

struct OneBitInference {
    std::string production_control = "Not uploaded/tested";
    std::vector<CaseObservationResult> cases;
    std::vector<GeometryDensityResult> densities;
    std::string four_x_state = "Untested";
    std::optional<double> highest_initial_exact_density;
    std::optional<double> highest_verified_exact_density;
    std::optional<double> lowest_failure_above;
    std::string status = "Insufficient observations";
    std::string boundary_bracket = "Insufficient observations";
    std::string safe_candidate;
    std::string balanced_candidate;
    std::string experimental_candidate;
    bool non_monotonic = false;
    bool retest_required = true;
    std::string recommended_next_experiment;
};

struct RepairComparison {
    int block_size = 0;
    int bits_per_block = 0;
    std::string repair2_config_id;
    std::string repair5_config_id;
    double recovery_delta_percent = 0.0;
    double margin_delta_percent = 0.0;
    int64_t candidate_size_delta = 0;
    double duration_delta_seconds = 0.0;
    std::string sha_effect;
    std::string inference;
};

using ProgressCallback = std::function<bool(const Progress &)>;

[[nodiscard]] ExperimentConfig production_baseline_config();
[[nodiscard]] BlockGeometry compute_geometry(
    const ExperimentConfig &config);
[[nodiscard]] CapacityMetrics compute_capacity(
    const ExperimentConfig &config,
    const ExperimentConfig &baseline = production_baseline_config());
[[nodiscard]] const std::vector<double> &dct_basis(int block_size);
void forward_dct(const std::vector<double> &pixels, int block_size,
                 std::vector<double> &coefficients);
void inverse_dct(const std::vector<double> &coefficients, int block_size,
                 std::vector<double> &pixels);
[[nodiscard]] std::vector<uint8_t> make_symbol_block(
    const ExperimentConfig &config, uint8_t symbol,
    bool *clamped = nullptr);
[[nodiscard]] SymbolDecision decode_symbol(
    const uint8_t *pixels, int stride,
    const ExperimentConfig &config);
[[nodiscard]] uint8_t gray_symbol_for_bits(uint8_t two_bits);
[[nodiscard]] uint8_t bits_for_gray_symbol(uint8_t symbol);

[[nodiscard]] std::vector<ExperimentConfig> smoke_configs();
[[nodiscard]] std::vector<ExperimentConfig> stage1_configs();
[[nodiscard]] std::vector<ExperimentConfig> boundary_1080p_configs();
[[nodiscard]] std::vector<ExperimentConfig> onebit_verification_configs();
[[nodiscard]] std::vector<CapacityCase> build_initial_cases(
    const RunOptions &options, const std::string &experiment_id);
[[nodiscard]] Preflight estimate(const RunOptions &options);
void update_pareto_and_categories(std::vector<CapacityCase> &cases);
[[nodiscard]] std::vector<std::size_t> select_shortlist(
    std::vector<CapacityCase> &cases, std::size_t maximum_videos);
[[nodiscard]] EligibilityDecision evaluate_shortlist_eligibility(
    const ExperimentManifest &manifest, const std::string &config_id);
void recompute_experiment_decisions(ExperimentManifest &manifest);
[[nodiscard]] std::vector<std::size_t> select_shortlist(
    ExperimentManifest &manifest, std::size_t maximum_videos);

[[nodiscard]] ExperimentManifest run(
    const RunOptions &options, const ProgressCallback &progress = {});
void resume(const std::filesystem::path &manifest_path,
            const ProgressCallback &progress = {});
[[nodiscard]] ShortlistRegenerationReport generate_shortlist(
    const std::filesystem::path &manifest_path,
    std::size_t maximum_videos = 8);
[[nodiscard]] ValidationReport validate_experiment(
    const std::filesystem::path &manifest_path);
void analyze_folder(const std::filesystem::path &manifest_path,
                    const std::filesystem::path &folder,
                    const std::string &session_label = {});
[[nodiscard]] BoundaryInference infer_boundary(
    const ExperimentManifest &manifest);
[[nodiscard]] OneBitInference infer_onebit_geometry(
    const ExperimentManifest &manifest);
[[nodiscard]] std::vector<CaseObservationResult>
infer_onebit_case_observations(const ExperimentManifest &manifest);
void verify_source_payloads(const std::filesystem::path &manifest_path);
[[nodiscard]] std::vector<RepairComparison> compare_boundary_repairs(
    const ExperimentManifest &manifest);
[[nodiscard]] std::string local_evidence_status(
    const CapacityCase &test_case);
[[nodiscard]] std::string real_youtube_status(
    const CapacityCase &test_case);
[[nodiscard]] std::string overall_evidence_status(
    const CapacityCase &test_case);

void write_manifest_atomic(const ExperimentManifest &manifest,
                           const std::filesystem::path &path);
[[nodiscard]] ExperimentManifest read_manifest(
    const std::filesystem::path &path);
void write_reports(const ExperimentManifest &manifest,
                   const std::filesystem::path &reports_directory);
[[nodiscard]] std::string to_string(Preset value);
[[nodiscard]] std::string to_string(CaseState value);

} // namespace youtube_capacity_lab
