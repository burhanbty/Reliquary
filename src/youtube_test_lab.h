#pragma once

#include "video_encoder.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace youtube_test_lab {

inline constexpr int kManifestSchemaVersion = 3;
inline constexpr int kOldestSupportedManifestSchemaVersion = 1;
inline constexpr std::size_t kMaximumCases = 256;
inline constexpr double kMinimumUploadDurationSeconds = 2.0;
inline constexpr double kMinimumValidatedDurationSeconds = 1.95;
inline constexpr int kDefaultUploadFps = 30;
inline constexpr uint64_t kMinimumUploadFrames = 60;
inline constexpr uint64_t kMaximumPayloadBytes = 64ULL * 1024 * 1024;
inline constexpr const char *kPayloadExtensionVersion =
    "duration-extension-v1";
inline constexpr const char *kAnalysisFingerprint =
    "vidstorex-testlab-decoder-v3";
inline constexpr uint64_t kDefaultPayloadSeed =
    0x56535859544c4142ULL; // "VSXYTLAB"

enum class DataType { Compressible, Random, ExistingFile };
enum class CaseState {
    Pending,
    Generating,
    Generated,
    Simulating,
    WaitingForManualUpload,
    Imported,
    Analyzed,
    Failed,
    Skipped
};
enum class ResultSource { LocalSimulation, RealYouTubeRoundtrip };
enum class FinalStatus {
    Pass,
    RecoverableIncomplete,
    DecodeFailed,
    HeaderNotFound,
    InsufficientPackets,
    CorruptOutput,
    WrongTestCase,
    UnsupportedProcessedVideo
};

struct VideoTechnicalInfo {
    std::string container;
    std::string codec;
    std::string profile;
    std::string codec_tag;
    std::string pixel_format;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t frame_count = 0;
    double duration_seconds = 0.0;
    int64_t bitrate = 0;
    int64_t stream_bitrate = 0;
    int64_t container_bitrate = 0;
    int64_t calculated_bitrate = 0;
    std::string bitrate_source = "unavailable";
    std::string display_aspect_ratio;
    std::string time_base;
    uint64_t file_size = 0;
};

struct CandidateValidation {
    VideoTechnicalInfo video;
    int64_t decoded_frame_count = 0;
    bool container_opened = false;
    bool video_stream_present = false;
    bool codec_is_h264 = false;
    bool pixel_format_compatible = false;
    bool resolution_matches = false;
    bool fps_matches = false;
    bool frame_count_sufficient = false;
    bool duration_sufficient = false;
    bool timestamps_present = false;
    bool pts_monotonic = false;
    bool dts_monotonic = false;
    bool last_timestamp_valid = false;
    bool decode_completed = false;
    bool file_nonempty = false;
    bool passed = false;
    std::string error;
};

struct PacketRecoveryTelemetry {
    uint64_t frames_read = 0;
    uint64_t frames_with_pattern = 0;
    uint64_t extracted_packets = 0;
    uint64_t valid_packets = 0;
    uint64_t invalid_packets = 0;
    uint64_t duplicate_packets = 0;
    uint64_t source_packets = 0;
    uint64_t repair_packets = 0;
    uint64_t recovered_chunks = 0;
    uint64_t missing_chunks = 0;
    uint64_t required_packet_threshold = 0;
    std::string failure_reason;
};

struct TestResult {
    std::string observation_id;
    std::string analysis_session_id;
    std::string suite_id;
    std::string case_id;
    std::string source_type;
    std::string analyzed_at_utc;
    std::string imported_at_utc;
    std::string source_file_relative_name;
    uint64_t source_file_size = 0;
    std::string source_file_sha256;
    std::string source_file_created_time_utc;
    std::string source_file_modified_time_utc;
    std::string analysis_fingerprint = kAnalysisFingerprint;
    std::string restored_sha256;
    std::string vidstorex_version;
    std::string test_case_id;
    std::string analyzed_video;
    ResultSource source = ResultSource::LocalSimulation;
    std::string simulation_profile;
    VideoTechnicalInfo video;
    uint64_t downloaded_video_size = 0;
    uint64_t decoded_frame_count = 0;
    PacketRecoveryTelemetry telemetry;
    double packet_recovery_percentage = 0.0;
    int64_t frame_difference = 0;
    bool decode_completed = false;
    bool sha256_match = false;
    std::string failure_stage;
    std::string error_message;
    double elapsed_transcode_seconds = 0.0;
    double elapsed_decode_seconds = 0.0;
    FinalStatus final_status = FinalStatus::DecodeFailed;
};

struct AnalysisSession {
    std::string analysis_session_id;
    std::string label;
    std::string created_at_utc;
    std::string source_folder;
    uint64_t observation_count = 0;
    std::string notes;
};

struct TestCase {
    std::string test_suite_id;
    std::string test_case_id;
    std::string created_at;
    std::string vidstorex_version;
    std::string encoding_mode = "resilient";
    std::string reliability_profile;
    double repair_percentage = 0.0;
    DataType input_data_type = DataType::Random;
    // input_size is retained as a schema-v1 compatibility alias for the
    // effective payload size.
    uint64_t input_size = 0;
    uint64_t requested_input_size = 0;
    uint64_t effective_input_size = 0;
    double minimum_duration_seconds = kMinimumUploadDurationSeconds;
    uint64_t minimum_required_frames = kMinimumUploadFrames;
    uint64_t expected_encoded_frames = 0;
    uint64_t actual_master_frames = 0;
    uint64_t actual_candidate_frames = 0;
    double master_duration_seconds = 0.0;
    double candidate_duration_seconds = 0.0;
    bool payload_extended_for_duration = false;
    uint64_t payload_extension_seed = 0;
    std::string payload_extension_version = kPayloadExtensionVersion;
    uint64_t payload_seed = kDefaultPayloadSeed;
    std::string input_sha256;
    uint64_t frame_payload_capacity = 0;
    uint64_t source_packet_count = 0;
    uint64_t repair_packet_count = 0;
    uint64_t total_packet_count = 0;
    uint64_t encoded_frame_count = 0;
    ResilientVideoConfig video;
    int block_size = 8;
    int bits_per_block = BITS_PER_BLOCK;
    double coefficient_strength = COEFFICIENT_STRENGTH;
    std::string payload_path;
    std::string master_video_path;
    std::string master_video_sha256;
    uint64_t master_video_size = 0;
    double master_encode_seconds = 0.0;
    std::string upload_candidate_path;
    std::string upload_candidate_sha256;
    uint64_t upload_candidate_size = 0;
    double upload_candidate_transcode_seconds = 0.0;
    std::string expected_output_filename;
    bool master_decode_success = false;
    bool upload_candidate_decode_success = false;
    bool upload_candidate_sha256_match = false;
    bool candidate_duration_validation_known = false;
    bool candidate_timestamps_valid = false;
    std::string candidate_validation_error;
    bool candidate_ready_for_youtube = false;
    CaseState state = CaseState::Pending;
    std::string notes;
    std::vector<TestResult> results;
};

struct SuiteManifest {
    int schema_version = kManifestSchemaVersion;
    std::string vidstorex_version;
    std::string suite_id;
    std::string created_at;
    std::string preset;
    std::string active_analysis_session_id;
    uint64_t duplicate_observations_excluded = 0;
    std::vector<std::string> deduplication_log;
    std::vector<AnalysisSession> analysis_sessions;
    std::vector<TestCase> cases;
    // Runtime-only migration provenance; not serialized.
    int loaded_schema_version = kManifestSchemaVersion;
};

struct AnalysisOptions {
    std::string analysis_session_id;
    std::string session_label;
    std::string source_folder;
    std::string source_relative_name;
    std::string imported_at_utc;
    uint64_t source_file_size = 0;
    std::string source_file_sha256;
    std::string source_file_created_time_utc;
    std::string source_file_modified_time_utc;
    bool record_new_observation = false;
};

struct AnalysisOutcome {
    TestResult result;
    bool recorded = false;
    bool duplicate = false;
    std::string message;
};

enum class BatchMatchStatus {
    Matched,
    NeedsMapping,
    DuplicateCaseConflict,
    Unsupported
};

struct BatchPreviewItem {
    std::filesystem::path source_path;
    std::string filename;
    std::optional<std::string> detected_case_id;
    std::string user_case_id;
    VideoTechnicalInfo video;
    uint64_t file_size = 0;
    std::string file_sha256;
    BatchMatchStatus status = BatchMatchStatus::NeedsMapping;
    bool duplicate_observation = false;
    std::string message;
};

struct BatchAnalysisSummary {
    std::string analysis_session_id;
    uint64_t discovered = 0;
    uint64_t analyzed = 0;
    uint64_t duplicates_skipped = 0;
    uint64_t needs_mapping = 0;
    uint64_t unsupported = 0;
    bool cancelled = false;
};

struct DeduplicationSummary {
    uint64_t observations_scanned = 0;
    uint64_t duplicate_observations = 0;
    uint64_t duplicate_groups = 0;
    uint64_t observations_after_apply = 0;
    bool applied = false;
    std::filesystem::path backup_path;
    std::vector<std::string> messages;
};

struct MatrixOptions {
    std::vector<double> repair_percentages;
    std::vector<uint64_t> input_sizes;
    std::vector<DataType> data_types;
    std::vector<std::pair<DataType, uint64_t>> input_variants;
    std::vector<std::pair<int, int>> resolutions;
    int fps = FRAME_FPS;
    double minimum_upload_duration_seconds =
        kMinimumUploadDurationSeconds;
};

struct SuitePreflight {
    uint64_t case_count = 0;
    uint64_t estimated_total_frames = 0;
    double estimated_total_duration_seconds = 0.0;
    uint64_t estimated_output_bytes = 0;
    uint64_t safety_margin_bytes = 0;
    uint64_t required_disk_bytes = 0;
    std::optional<uint64_t> available_disk_bytes;
    bool disk_space_sufficient = false;
};

struct SimulationProfile {
    std::string name;
    std::string codec = "libx264";
    int crf = 18;
    std::string preset = "medium";
    std::string pixel_format = "yuv420p";
    int width = 0;
    int height = 0;
    int fps = FRAME_FPS;
    bool scale = false;
    int gop = 60;
};

struct Progress {
    std::size_t completed_cases = 0;
    std::size_t total_cases = 0;
    std::string active_case;
    double case_progress = 0.0;
};

using ProgressCallback = std::function<bool(const Progress &)>;

[[nodiscard]] MatrixOptions quick_matrix();
[[nodiscard]] MatrixOptions full_matrix();
[[nodiscard]] uint64_t minimum_frames_for_duration(
    double duration_seconds, double fps);
[[nodiscard]] uint64_t minimum_payload_size_for_frames(
    uint64_t requested_size, const ResilientVideoConfig &video,
    double repair_percentage, uint64_t minimum_frames);
[[nodiscard]] std::vector<TestCase> build_matrix(
    const MatrixOptions &options, const std::string &suite_id);
[[nodiscard]] SuitePreflight estimate_suite(
    const std::vector<TestCase> &cases,
    const std::filesystem::path &output_root);
[[nodiscard]] std::vector<SimulationProfile> simulation_profiles();
[[nodiscard]] std::optional<SimulationProfile> find_simulation_profile(
    const std::string &name);
void transcode_simulation_video(
    const std::filesystem::path &input,
    const std::filesystem::path &output,
    const SimulationProfile &profile,
    const std::string &suite_id,
    const std::string &case_id);

[[nodiscard]] std::string create_suite_id();
[[nodiscard]] std::string sha256_file(
    const std::filesystem::path &path);
void generate_payload(const std::filesystem::path &path, DataType type,
                      uint64_t size, uint64_t seed);
void generate_case_payload(const std::filesystem::path &path,
                           const TestCase &test_case);

void write_manifest_atomic(const SuiteManifest &manifest,
                           const std::filesystem::path &path);
[[nodiscard]] SuiteManifest read_manifest(
    const std::filesystem::path &path);

[[nodiscard]] SuiteManifest create_suite(
    const std::filesystem::path &output_root,
    const std::string &preset,
    const MatrixOptions &matrix,
    bool allow_low_disk = false,
    const ProgressCallback &progress = {});
void resume_suite(const std::filesystem::path &manifest_path,
                  bool allow_low_disk = false,
                  const ProgressCallback &progress = {});

[[nodiscard]] VideoTechnicalInfo analyze_video(
    const std::filesystem::path &path);
[[nodiscard]] CandidateValidation validate_upload_candidate(
    const std::filesystem::path &path,
    const ResilientVideoConfig &expected_video,
    uint64_t minimum_frames,
    double minimum_duration_seconds);
[[nodiscard]] TestResult analyze_case_video(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    ResultSource source,
    const std::string &simulation_profile = {});
[[nodiscard]] AnalysisOutcome analyze_case_video_record(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    ResultSource source, const std::string &simulation_profile,
    const AnalysisOptions &options = {});
[[nodiscard]] AnalysisOutcome analyze_real_video(
    const std::filesystem::path &manifest_path,
    const std::string &case_id,
    const std::filesystem::path &video_path,
    const AnalysisOptions &options = {});
[[nodiscard]] std::vector<BatchPreviewItem> preview_analysis_folder(
    const SuiteManifest &manifest,
    const std::filesystem::path &folder,
    const std::map<std::string, std::string> &manual_mappings = {});
[[nodiscard]] BatchAnalysisSummary analyze_folder(
    const std::filesystem::path &manifest_path,
    const std::filesystem::path &folder,
    const std::map<std::string, std::string> &manual_mappings = {},
    const AnalysisOptions &options = {},
    const ProgressCallback &progress = {});
[[nodiscard]] DeduplicationSummary deduplicate_results(
    const std::filesystem::path &manifest_path, bool apply);
[[nodiscard]] uint64_t detected_duplicate_observation_count(
    const SuiteManifest &manifest);
[[nodiscard]] std::string create_analysis_session(
    SuiteManifest &manifest, const std::string &label,
    const std::string &source_folder = {},
    const std::string &notes = {});
void simulate_suite(const std::filesystem::path &manifest_path,
                    const SimulationProfile &profile,
                    const ProgressCallback &progress = {});

void write_reports(const SuiteManifest &manifest,
                   const std::filesystem::path &reports_directory);
[[nodiscard]] std::optional<std::string> case_id_from_filename(
    const SuiteManifest &manifest,
    const std::filesystem::path &video_path);
[[nodiscard]] std::string to_string(BatchMatchStatus value);

[[nodiscard]] std::string to_string(DataType value);
[[nodiscard]] std::string to_string(CaseState value);
[[nodiscard]] std::string to_string(ResultSource value);
[[nodiscard]] std::string to_string(FinalStatus value);

} // namespace youtube_test_lab
