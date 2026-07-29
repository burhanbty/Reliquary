#pragma once

#include "video_encoder.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace youtube_test_lab {

inline constexpr int kManifestSchemaVersion = 1;
inline constexpr std::size_t kMaximumCases = 256;
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
    std::string pixel_format;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    int64_t frame_count = 0;
    double duration_seconds = 0.0;
    int64_t bitrate = 0;
    uint64_t file_size = 0;
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

struct TestCase {
    std::string test_suite_id;
    std::string test_case_id;
    std::string created_at;
    std::string vidstorex_version;
    std::string encoding_mode = "resilient";
    std::string reliability_profile;
    double repair_percentage = 0.0;
    DataType input_data_type = DataType::Random;
    uint64_t input_size = 0;
    uint64_t payload_seed = kDefaultPayloadSeed;
    std::string input_sha256;
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
    std::vector<TestCase> cases;
};

struct MatrixOptions {
    std::vector<double> repair_percentages;
    std::vector<uint64_t> input_sizes;
    std::vector<DataType> data_types;
    std::vector<std::pair<DataType, uint64_t>> input_variants;
    std::vector<std::pair<int, int>> resolutions;
    int fps = FRAME_FPS;
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
[[nodiscard]] std::vector<TestCase> build_matrix(
    const MatrixOptions &options, const std::string &suite_id);
[[nodiscard]] SuitePreflight estimate_suite(
    const std::vector<TestCase> &cases,
    const std::filesystem::path &output_root);
[[nodiscard]] std::vector<SimulationProfile> simulation_profiles();
[[nodiscard]] std::optional<SimulationProfile> find_simulation_profile(
    const std::string &name);

[[nodiscard]] std::string create_suite_id();
[[nodiscard]] std::string sha256_file(
    const std::filesystem::path &path);
void generate_payload(const std::filesystem::path &path, DataType type,
                      uint64_t size, uint64_t seed);

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
[[nodiscard]] TestResult analyze_case_video(
    SuiteManifest &manifest, TestCase &test_case,
    const std::filesystem::path &video_path,
    ResultSource source,
    const std::string &simulation_profile = {});
void simulate_suite(const std::filesystem::path &manifest_path,
                    const SimulationProfile &profile,
                    const ProgressCallback &progress = {});

void write_reports(const SuiteManifest &manifest,
                   const std::filesystem::path &reports_directory);
[[nodiscard]] std::optional<std::string> case_id_from_filename(
    const SuiteManifest &manifest,
    const std::filesystem::path &video_path);

[[nodiscard]] std::string to_string(DataType value);
[[nodiscard]] std::string to_string(CaseState value);
[[nodiscard]] std::string to_string(ResultSource value);
[[nodiscard]] std::string to_string(FinalStatus value);

} // namespace youtube_test_lab
