#include <gtest/gtest.h>

#include "youtube_test_lab.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace {

class TestLabTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vidstorex-testlab-unit-" +
             std::to_string(
                 std::chrono::steady_clock::now()
                     .time_since_epoch().count()) + "-" +
             std::to_string(counter++));
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
    inline static uint64_t counter = 0;
};

youtube_test_lab::SuiteManifest sample_manifest() {
    using namespace youtube_test_lab;
    SuiteManifest manifest;
    manifest.vidstorex_version = "1.3.0";
    manifest.suite_id = "suite-test";
    manifest.created_at = "2026-07-30T00:00:00Z";
    manifest.preset = "quick";
    manifest.cases = build_matrix(quick_matrix(), manifest.suite_id);
    return manifest;
}

} // namespace

TEST(YouTubeTestLabMatrix, QuickPresetHasSixCases) {
    using namespace youtube_test_lab;
    EXPECT_EQ(build_matrix(quick_matrix(), "suite").size(), 6u);
}

TEST(YouTubeTestLabDuration, ThirtyFpsTimesTwoSecondsIsSixty) {
    using namespace youtube_test_lab;
    EXPECT_EQ(minimum_frames_for_duration(2.0, 30.0), 60u);
    EXPECT_EQ(kMinimumUploadFrames, 60u);
}

TEST(YouTubeTestLabDuration, DecimalFpsUsesCeil) {
    using namespace youtube_test_lab;
    EXPECT_EQ(minimum_frames_for_duration(2.0, 29.97), 60u);
    EXPECT_EQ(minimum_frames_for_duration(2.01, 30.0), 61u);
}

TEST(YouTubeTestLabDuration, InvalidFpsIsRejected) {
    using namespace youtube_test_lab;
    EXPECT_THROW((void) minimum_frames_for_duration(2.0, 0.0),
                 std::invalid_argument);
    EXPECT_THROW((void) minimum_frames_for_duration(2.0, -1.0),
                 std::invalid_argument);
}

TEST(YouTubeTestLabDuration, OverflowIsRejected) {
    using namespace youtube_test_lab;
    EXPECT_THROW(
        (void) minimum_frames_for_duration(
            std::numeric_limits<double>::max(), 30.0),
        std::overflow_error);
}

TEST(YouTubeTestLabDuration, LessThanTwoSecondsMatrixIsRejected) {
    using namespace youtube_test_lab;
    auto matrix = quick_matrix();
    matrix.minimum_upload_duration_seconds = 1.99;
    EXPECT_THROW((void) build_matrix(matrix, "suite"),
                 std::invalid_argument);
}

TEST(YouTubeTestLabPayloadSizing, QuickCasesReachMinimumFrames) {
    using namespace youtube_test_lab;
    const auto cases = build_matrix(quick_matrix(), "suite");
    for (const auto &item : cases) {
        EXPECT_EQ(item.requested_input_size, 64u * 1024u);
        EXPECT_GT(item.effective_input_size,
                  item.requested_input_size);
        EXPECT_GE(item.expected_encoded_frames,
                  item.minimum_required_frames);
        EXPECT_TRUE(item.payload_extended_for_duration);
    }
}

TEST(YouTubeTestLabPayloadSizing,
     RepairProfilesSharePayloadWithinComparisonGroup) {
    using namespace youtube_test_lab;
    const auto cases = build_matrix(quick_matrix(), "suite");
    for (const auto [width, height] :
         std::vector<std::pair<int, int>>{
             {1920, 1080}, {3840, 2160}}) {
        std::optional<uint64_t> size;
        std::optional<uint64_t> seed;
        std::optional<uint64_t> extension_seed;
        for (const auto &item : cases) {
            if (item.video.width != width ||
                item.video.height != height)
                continue;
            if (!size) {
                size = item.effective_input_size;
                seed = item.payload_seed;
                extension_seed =
                    item.payload_extension_seed;
            } else {
                EXPECT_EQ(item.effective_input_size, *size);
                EXPECT_EQ(item.payload_seed, *seed);
                EXPECT_EQ(item.payload_extension_seed,
                          *extension_seed);
            }
        }
    }
}

TEST(YouTubeTestLabPayloadSizing,
     DifferentResolutionsHaveDifferentEffectivePayloads) {
    using namespace youtube_test_lab;
    const auto cases = build_matrix(quick_matrix(), "suite");
    uint64_t hd = 0;
    uint64_t uhd = 0;
    for (const auto &item : cases) {
        if (item.repair_percentage != 5.0) continue;
        if (item.video.width == 1920)
            hd = item.effective_input_size;
        if (item.video.width == 3840)
            uhd = item.effective_input_size;
    }
    EXPECT_GT(hd, 64u * 1024u);
    EXPECT_GT(uhd, hd);
}

TEST(YouTubeTestLabPayloadSizing, SufficientPayloadIsUnchanged) {
    using namespace youtube_test_lab;
    const ResilientVideoConfig video{
        .width = 640, .height = 480, .fps = 30};
    const uint64_t requested = 1024u * 1024u;
    EXPECT_EQ(minimum_payload_size_for_frames(
                  requested, video, 5.0, 60),
              requested);
}

TEST(YouTubeTestLabMatrix, FullPresetHasThirtySixCases) {
    using namespace youtube_test_lab;
    EXPECT_EQ(build_matrix(full_matrix(), "suite").size(), 36u);
}

TEST(YouTubeTestLabMatrix, CaseIdsAreUnique) {
    using namespace youtube_test_lab;
    const auto cases = build_matrix(full_matrix(), "suite");
    std::set<std::string> ids;
    for (const auto &item : cases)
        EXPECT_TRUE(ids.insert(item.test_case_id).second);
}

TEST(YouTubeTestLabMatrix, DuplicateInputsAreRejected) {
    using namespace youtube_test_lab;
    auto matrix = quick_matrix();
    matrix.input_variants.push_back(matrix.input_variants.front());
    EXPECT_THROW((void) build_matrix(matrix, "suite"),
                 std::invalid_argument);
}

TEST(YouTubeTestLabMatrix, InvalidResolutionIsRejected) {
    using namespace youtube_test_lab;
    auto matrix = quick_matrix();
    matrix.resolutions = {{1919, 1080}};
    EXPECT_THROW((void) build_matrix(matrix, "suite"),
                 std::invalid_argument);
}

TEST(YouTubeTestLabMatrix, OversizePayloadIsRejected) {
    using namespace youtube_test_lab;
    auto matrix = quick_matrix();
    matrix.input_variants = {{DataType::Random, 65ULL * 1024 * 1024}};
    EXPECT_THROW((void) build_matrix(matrix, "suite"),
                 std::invalid_argument);
}

TEST(YouTubeTestLabMatrix, ExcessiveMatrixIsRejected) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    for (int i = 0; i < 257; ++i)
        matrix.repair_percentages.push_back(
            static_cast<double>(i % 501));
    matrix.input_variants = {{DataType::Random, 1024}};
    matrix.resolutions = {{1920, 1080}};
    EXPECT_THROW((void) build_matrix(matrix, "suite"),
                 std::invalid_argument);
}

TEST_F(TestLabTest, PreflightReportsCountsAndDiskRequirement) {
    using namespace youtube_test_lab;
    const auto cases = build_matrix(quick_matrix(), "suite");
    const auto estimate = estimate_suite(cases, root);
    EXPECT_EQ(estimate.case_count, 6u);
    EXPECT_GT(estimate.estimated_total_frames, 0u);
    EXPECT_GT(estimate.required_disk_bytes,
              estimate.estimated_output_bytes);
    EXPECT_TRUE(estimate.available_disk_bytes.has_value());
}

TEST_F(TestLabTest, RandomPayloadIsDeterministic) {
    using namespace youtube_test_lab;
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    generate_payload(first, DataType::Random, 64 * 1024, 42);
    generate_payload(second, DataType::Random, 64 * 1024, 42);
    EXPECT_EQ(sha256_file(first), sha256_file(second));
}

TEST_F(TestLabTest, DifferentRandomSeedChangesHash) {
    using namespace youtube_test_lab;
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    generate_payload(first, DataType::Random, 64 * 1024, 42);
    generate_payload(second, DataType::Random, 64 * 1024, 43);
    EXPECT_NE(sha256_file(first), sha256_file(second));
}

TEST_F(TestLabTest, CompressiblePayloadIsDeterministic) {
    using namespace youtube_test_lab;
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    generate_payload(first, DataType::Compressible, 256 * 1024, 7);
    generate_payload(second, DataType::Compressible, 256 * 1024, 7);
    EXPECT_EQ(sha256_file(first), sha256_file(second));
}

TEST_F(TestLabTest, PayloadSizesAreExact) {
    using namespace youtube_test_lab;
    for (const uint64_t size : {
             64ULL * 1024, 256ULL * 1024, 1024ULL * 1024}) {
        const auto path = root / (std::to_string(size) + ".bin");
        generate_payload(path, DataType::Random, size, 9);
        EXPECT_EQ(std::filesystem::file_size(path), size);
    }
}

TEST_F(TestLabTest, ExtendedPayloadIsDeterministicAndExact) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {5.0, 20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto cases = build_matrix(matrix, "deterministic-suite");
    ASSERT_EQ(cases.size(), 2u);
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    generate_case_payload(first, cases[0]);
    generate_case_payload(second, cases[1]);
    EXPECT_EQ(std::filesystem::file_size(first),
              cases[0].effective_input_size);
    EXPECT_EQ(sha256_file(first), sha256_file(second));
}

TEST_F(TestLabTest, DifferentComparisonGroupChangesExtendedHash) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {5.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}, {800, 600}};
    const auto cases = build_matrix(matrix, "deterministic-suite");
    ASSERT_EQ(cases.size(), 2u);
    const auto first = root / "first.bin";
    const auto second = root / "second.bin";
    generate_case_payload(first, cases[0]);
    generate_case_payload(second, cases[1]);
    EXPECT_NE(sha256_file(first), sha256_file(second));
}

TEST_F(TestLabTest, CompressibleExtendedPayloadDiffersFromRandom) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {5.0};
    matrix.input_variants = {
        {DataType::Compressible, 8 * 1024},
        {DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto cases = build_matrix(matrix, "payload-kind-suite");
    ASSERT_EQ(cases.size(), 2u);
    const auto compressible = root / "compressible.bin";
    const auto random = root / "random.bin";
    generate_case_payload(compressible, cases[0]);
    generate_case_payload(random, cases[1]);
    EXPECT_NE(sha256_file(compressible), sha256_file(random));
}

TEST_F(TestLabTest, ManifestRoundTripPreservesFields) {
    using namespace youtube_test_lab;
    const auto path = root / "manifest.json";
    const auto original = sample_manifest();
    write_manifest_atomic(original, path);
    const auto restored = read_manifest(path);
    EXPECT_EQ(restored.schema_version, kManifestSchemaVersion);
    EXPECT_EQ(restored.suite_id, original.suite_id);
    ASSERT_EQ(restored.cases.size(), original.cases.size());
    EXPECT_EQ(restored.cases.front().payload_seed,
              original.cases.front().payload_seed);
    EXPECT_EQ(restored.cases.front().video.width, 1920);
    EXPECT_EQ(restored.cases.front().requested_input_size,
              original.cases.front().requested_input_size);
    EXPECT_EQ(restored.cases.front().effective_input_size,
              original.cases.front().effective_input_size);
    EXPECT_EQ(restored.cases.front().minimum_required_frames, 60u);
}

TEST_F(TestLabTest, SchemaOneManifestLoadsAsValidationUnknown) {
    using namespace youtube_test_lab;
    const auto current = root / "current.json";
    const auto legacy = root / "legacy.json";
    write_manifest_atomic(sample_manifest(), current);
    std::ifstream input(current);
    std::ofstream output(legacy);
    const std::vector<std::string> additive_fields{
        "\"requested_input_size\"",
        "\"effective_input_size\"",
        "\"minimum_duration_seconds\"",
        "\"minimum_required_frames\"",
        "\"expected_encoded_frames\"",
        "\"actual_master_frames\"",
        "\"actual_candidate_frames\"",
        "\"master_duration_seconds\"",
        "\"candidate_duration_seconds\"",
        "\"payload_extended_for_duration\"",
        "\"payload_extension_seed\"",
        "\"payload_extension_version\"",
        "\"frame_payload_capacity\"",
        "\"candidate_duration_validation_known\"",
        "\"candidate_timestamps_valid\"",
        "\"candidate_validation_error\"",
        "\"explicit_frame_duration\""};
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("\"schema_version\": 3") !=
            std::string::npos)
            line.replace(
                line.find(": 3"), 3, ": 1");
        bool skip = false;
        for (const auto &field : additive_fields)
            if (line.find(field) != std::string::npos) {
                skip = true;
                break;
            }
        if (skip) continue;
        if (line.find("\"container\":") != std::string::npos &&
            !line.empty() && line.back() == ',')
            line.pop_back();
        output << line << "\n";
    }
    output.close();
    const auto restored = read_manifest(legacy);
    EXPECT_EQ(restored.schema_version, kManifestSchemaVersion);
    ASSERT_FALSE(restored.cases.empty());
    const auto &item = restored.cases.front();
    EXPECT_FALSE(item.candidate_duration_validation_known);
    EXPECT_FALSE(item.candidate_ready_for_youtube);
    EXPECT_NE(item.candidate_validation_error.find("unknown"),
              std::string::npos);
    EXPECT_NE(item.upload_candidate_path.find("_duration-v2"),
              std::string::npos);
}

TEST_F(TestLabTest, SchemaTwoManifestLoadsAndMigratesToV3) {
    using namespace youtube_test_lab;
    const auto current = root / "current.json";
    const auto legacy = root / "schema-v2.json";
    write_manifest_atomic(sample_manifest(), current);
    std::ifstream input(current);
    std::ofstream output(legacy);
    std::string line;
    while (std::getline(input, line)) {
        if (line.find("\"schema_version\": 3") !=
            std::string::npos)
            line.replace(line.find(": 3"), 3, ": 2");
        output << line << "\n";
    }
    output.close();
    const auto restored = read_manifest(legacy);
    EXPECT_EQ(restored.schema_version, kManifestSchemaVersion);
    EXPECT_EQ(restored.loaded_schema_version, 2);
    ASSERT_EQ(restored.cases.size(), 6u);
}

TEST_F(TestLabTest, ManifestUsesRelativePaths) {
    using namespace youtube_test_lab;
    const auto path = root / "manifest.json";
    write_manifest_atomic(sample_manifest(), path);
    const auto restored = read_manifest(path);
    for (const auto &item : restored.cases) {
        EXPECT_FALSE(
            std::filesystem::path(item.payload_path).is_absolute());
        EXPECT_FALSE(
            std::filesystem::path(item.master_video_path).is_absolute());
    }
}

TEST_F(TestLabTest, CorruptManifestIsRejected) {
    const auto path = root / "manifest.json";
    std::ofstream(path) << "{\"schema_version\":";
    EXPECT_THROW((void) youtube_test_lab::read_manifest(path),
                 std::runtime_error);
}

TEST_F(TestLabTest, UnsupportedSchemaIsRejected) {
    const auto path = root / "manifest.json";
    std::ofstream(path)
        << "{\"schema_version\":999,\"vidstorex_version\":\"x\","
           "\"suite_id\":\"x\",\"created_at\":\"x\","
           "\"preset\":\"x\",\"cases\":[]}";
    EXPECT_THROW((void) youtube_test_lab::read_manifest(path),
                 std::runtime_error);
}

TEST_F(TestLabTest, AtomicManifestLeavesNoPartialFile) {
    using namespace youtube_test_lab;
    const auto path = root / "manifest.json";
    write_manifest_atomic(sample_manifest(), path);
    EXPECT_TRUE(std::filesystem::exists(path));
    for (const auto &entry :
         std::filesystem::directory_iterator(root))
        EXPECT_EQ(entry.path().filename(), "manifest.json");
}

TEST_F(TestLabTest, ReportsProduceJsonCsvAndMarkdown) {
    using namespace youtube_test_lab;
    const auto manifest = sample_manifest();
    const auto reports = root / "reports";
    write_reports(manifest, reports);
    EXPECT_TRUE(std::filesystem::exists(reports / "report.json"));
    EXPECT_TRUE(std::filesystem::exists(reports / "report.csv"));
    EXPECT_TRUE(std::filesystem::exists(reports / "report.md"));
    std::ifstream markdown(reports / "report.md");
    const std::string text{
        std::istreambuf_iterator<char>(markdown),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("Requested"), std::string::npos);
    EXPECT_NE(text.find("Effective"), std::string::npos);
    EXPECT_NE(text.find("YouTube ready"), std::string::npos);
}

TEST_F(TestLabTest, EmptyCandidateIsRejected) {
    using namespace youtube_test_lab;
    const auto empty = root / "zero.mp4";
    std::ofstream(empty, std::ios::binary).close();
    const ResilientVideoConfig expected{
        .width = 1920, .height = 1080, .fps = 30};
    const auto validation = validate_upload_candidate(
        empty, expected, 60, 2.0);
    EXPECT_FALSE(validation.passed);
    EXPECT_FALSE(validation.file_nonempty);
    EXPECT_FALSE(validation.error.empty());
}

TEST(YouTubeTestLabSimulation, AllNamedProfilesAreAvailable) {
    using namespace youtube_test_lab;
    EXPECT_TRUE(find_simulation_profile(
        "yt-sim-1080p-light").has_value());
    EXPECT_TRUE(find_simulation_profile(
        "yt-sim-1080p-medium").has_value());
    EXPECT_TRUE(find_simulation_profile(
        "yt-sim-1080p-heavy").has_value());
    EXPECT_TRUE(find_simulation_profile(
        "yt-sim-720p-downscale").has_value());
    EXPECT_TRUE(find_simulation_profile(
        "yt-sim-4k-medium").has_value());
}

TEST(YouTubeTestLabSimulation, UnknownProfileIsRejected) {
    EXPECT_FALSE(youtube_test_lab::find_simulation_profile(
        "not-a-profile").has_value());
}

TEST(YouTubeTestLabVideoConfig, DefaultsPreserveProductionSettings) {
    const ResilientVideoConfig config;
    EXPECT_EQ(config.width, FRAME_WIDTH);
    EXPECT_EQ(config.height, FRAME_HEIGHT);
    EXPECT_EQ(config.fps, FRAME_FPS);
    EXPECT_EQ(config.codec, VIDEO_CODEC);
    EXPECT_EQ(config.container, VIDEO_CONTAINER);
}

TEST(YouTubeTestLabVideoConfig, ResolutionChangesFrameCapacity) {
    const ResilientVideoConfig hd{
        .width = 1920, .height = 1080};
    const ResilientVideoConfig uhd{
        .width = 3840, .height = 2160};
    EXPECT_LT(VideoEncoder::packets_per_frame(hd),
              VideoEncoder::packets_per_frame(uhd));
}

TEST(YouTubeTestLabFilename, DetectsCaseId) {
    using namespace youtube_test_lab;
    const auto manifest = sample_manifest();
    const auto id = manifest.cases.front().test_case_id;
    const auto detected = case_id_from_filename(
        manifest, "returned_" + id + ".mp4");
    ASSERT_TRUE(detected.has_value());
    EXPECT_EQ(*detected, id);
}

TEST(YouTubeTestLabFilename,
     DetectsCaseIdWithDownloaderPrefixSuffixAndCaseDifference) {
    using namespace youtube_test_lab;
    const auto manifest = sample_manifest();
    const auto id = manifest.cases.front().test_case_id;
    std::string upper = id;
    for (auto &character : upper)
        character = static_cast<char>(std::toupper(
            static_cast<unsigned char>(character)));
    const auto detected = case_id_from_filename(
        manifest, "Downloader_VSX_YT_" + upper + "_copy.webm");
    ASSERT_TRUE(detected.has_value());
    EXPECT_EQ(*detected, id);
}

TEST(YouTubeTestLabFilename, MissingCaseIdNeedsManualSelection) {
    const auto manifest = sample_manifest();
    EXPECT_FALSE(youtube_test_lab::case_id_from_filename(
        manifest, "youtube-download.mp4").has_value());
}

TEST_F(TestLabTest, EndToEndLocalSimulationAndCleanup) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    matrix.fps = 30;
    const auto generated = create_suite(
        root, "integration", matrix, true);
    ASSERT_EQ(generated.cases.size(), 1u);
    EXPECT_TRUE(generated.cases.front().master_decode_success);
    EXPECT_TRUE(generated.cases.front().candidate_ready_for_youtube);
    EXPECT_EQ(generated.cases.front().minimum_required_frames, 60u);
    EXPECT_GE(generated.cases.front().actual_master_frames, 60u);
    EXPECT_GE(generated.cases.front().actual_candidate_frames, 60u);
    EXPECT_GE(generated.cases.front().master_duration_seconds, 1.95);
    EXPECT_GE(generated.cases.front().candidate_duration_seconds, 1.95);
    EXPECT_TRUE(
        generated.cases.front().candidate_timestamps_valid);
    EXPECT_TRUE(
        generated.cases.front().candidate_validation_error.empty());

    const auto suite_root =
        root / "youtube_test_lab" / generated.suite_id;
    const auto candidate_path =
        suite_root /
        generated.cases.front().upload_candidate_path;
    const auto validation = validate_upload_candidate(
        candidate_path, generated.cases.front().video, 60, 2.0);
    EXPECT_TRUE(validation.passed) << validation.error;
    EXPECT_EQ(validation.video.codec, "h264");
    EXPECT_EQ(validation.video.pixel_format, "yuv420p");
    EXPECT_TRUE(validation.pts_monotonic);
    EXPECT_TRUE(validation.dts_monotonic);
    EXPECT_TRUE(validation.last_timestamp_valid);
    const auto truncated = suite_root / "truncated.mp4";
    {
        std::ifstream source(candidate_path, std::ios::binary);
        std::array<char, 256> prefix{};
        source.read(prefix.data(), prefix.size());
        std::ofstream damaged(truncated, std::ios::binary);
        damaged.write(prefix.data(), source.gcount());
    }
    const auto truncated_validation =
        validate_upload_candidate(
            truncated, generated.cases.front().video, 60, 2.0);
    EXPECT_FALSE(truncated_validation.passed);
    EXPECT_FALSE(truncated_validation.error.empty());
    const auto manifest_path = suite_root / "manifest.json";
    SimulationProfile profile{
        "yt-sim-unit", "libx264", 18, "fast", "yuv420p",
        640, 480, 30, true, 30};
    simulate_suite(manifest_path, profile);
    const auto restored = read_manifest(manifest_path);
    ASSERT_FALSE(restored.cases.front().results.empty());
    const auto &result = restored.cases.front().results.back();
    EXPECT_EQ(result.simulation_profile, "yt-sim-unit");
    EXPECT_EQ(result.final_status, FinalStatus::Pass);
    EXPECT_TRUE(result.sha256_match);
    EXPECT_GT(result.telemetry.valid_packets, 0u);

    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(suite_root)) {
        if (entry.is_regular_file())
            EXPECT_EQ(entry.path().filename().string().find(
                          ".vidstorex-part-"),
                      std::string::npos);
    }
}

TEST_F(TestLabTest,
       RealAnalysisPreventsDuplicatesAndSupportsTimedObservations) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto generated = create_suite(
        root, "observation-test", matrix, true);
    const auto suite_root =
        root / "youtube_test_lab" / generated.suite_id;
    const auto manifest_path = suite_root / "manifest.json";
    const auto candidate =
        suite_root /
        generated.cases.front().upload_candidate_path;
    const auto case_id = generated.cases.front().test_case_id;

    AnalysisOptions initial;
    initial.session_label = "Initial upload";
    const auto first = analyze_real_video(
        manifest_path, case_id, candidate, initial);
    ASSERT_TRUE(first.recorded);
    EXPECT_FALSE(first.duplicate);
    EXPECT_FALSE(first.result.observation_id.empty());
    EXPECT_FALSE(first.result.analysis_session_id.empty());
    EXPECT_EQ(first.result.source_type,
              "real-youtube-roundtrip");
    EXPECT_FALSE(first.result.source_file_sha256.empty());
    EXPECT_GT(first.result.source_file_size, 0u);
    EXPECT_EQ(first.result.video.codec, "h264");
    EXPECT_GT(first.result.video.width, 0);
    EXPECT_GT(first.result.video.fps, 0.0);
    EXPECT_GT(first.result.video.duration_seconds, 0.0);
    EXPECT_FALSE(first.result.video.bitrate_source.empty());
    EXPECT_FALSE(first.result.imported_at_utc.empty());
    EXPECT_FALSE(first.result.analyzed_at_utc.empty());
    EXPECT_FALSE(
        first.result.source_file_modified_time_utc.empty());

    const auto before_duplicate = read_manifest(manifest_path);
    const auto duplicate = analyze_real_video(
        manifest_path, case_id, candidate, {});
    EXPECT_TRUE(duplicate.duplicate);
    EXPECT_FALSE(duplicate.recorded);
    EXPECT_NE(duplicate.message.find(
                  "already been analyzed"),
              std::string::npos);
    const auto after_duplicate = read_manifest(manifest_path);
    EXPECT_EQ(
        after_duplicate.cases.front().results.size(),
        before_duplicate.cases.front().results.size());

    AnalysisOptions timed;
    timed.session_label = "7-day retest";
    timed.record_new_observation = true;
    const auto repeated = analyze_real_video(
        manifest_path, case_id, candidate, timed);
    EXPECT_TRUE(repeated.recorded);
    EXPECT_NE(repeated.result.observation_id,
              first.result.observation_id);
    EXPECT_NE(repeated.result.analysis_session_id,
              first.result.analysis_session_id);

    const auto changed = root / "changed_" /
        ("prefix_" + case_id + "_changed.mp4");
    std::filesystem::create_directories(changed.parent_path());
    std::filesystem::copy_file(candidate, changed);
    std::ofstream(changed, std::ios::binary | std::ios::app).put('\0');
    const auto different_hash = analyze_real_video(
        manifest_path, case_id, changed, {});
    EXPECT_TRUE(different_hash.recorded);
    EXPECT_NE(different_hash.result.source_file_sha256,
              first.result.source_file_sha256);

    const auto final_manifest = read_manifest(manifest_path);
    bool has_local = false;
    bool has_real = false;
    for (const auto &result :
         final_manifest.cases.front().results) {
        has_local = has_local ||
            result.source_type == "local-simulation" ||
            result.source_type == "master-lossless" ||
            result.source_type == "youtube-upload-candidate";
        has_real = has_real ||
            result.source_type == "real-youtube-roundtrip";
    }
    EXPECT_TRUE(has_local);
    EXPECT_TRUE(has_real);
}

TEST_F(TestLabTest,
       DeduplicateDryRunApplyBackupAndIdempotency) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto generated = create_suite(
        root, "dedup-test", matrix, true);
    const auto suite_root =
        root / "youtube_test_lab" / generated.suite_id;
    const auto manifest_path = suite_root / "manifest.json";
    const auto candidate =
        suite_root /
        generated.cases.front().upload_candidate_path;
    const auto case_id = generated.cases.front().test_case_id;
    ASSERT_TRUE(analyze_real_video(
        manifest_path, case_id, candidate, {}).recorded);
    AnalysisOptions forced;
    forced.record_new_observation = true;
    ASSERT_TRUE(analyze_real_video(
        manifest_path, case_id, candidate, forced).recorded);
    const auto before = read_manifest(manifest_path);
    const auto imported_count_before = std::distance(
        std::filesystem::directory_iterator(
            suite_root / "imported"),
        std::filesystem::directory_iterator{});

    const auto dry_run =
        deduplicate_results(manifest_path, false);
    EXPECT_FALSE(dry_run.applied);
    EXPECT_EQ(dry_run.duplicate_observations, 1u);
    EXPECT_EQ(read_manifest(manifest_path)
                  .cases.front().results.size(),
              before.cases.front().results.size());

    const auto applied =
        deduplicate_results(manifest_path, true);
    EXPECT_TRUE(applied.applied);
    EXPECT_EQ(applied.duplicate_observations, 1u);
    EXPECT_TRUE(std::filesystem::exists(applied.backup_path));
    const auto after = read_manifest(manifest_path);
    EXPECT_EQ(after.cases.front().results.size() + 1,
              before.cases.front().results.size());
    EXPECT_EQ(after.duplicate_observations_excluded, 1u);
    EXPECT_EQ(
        std::distance(
            std::filesystem::directory_iterator(
                suite_root / "imported"),
            std::filesystem::directory_iterator{}),
        imported_count_before);
    const auto second =
        deduplicate_results(manifest_path, true);
    EXPECT_FALSE(second.applied);
    EXPECT_EQ(second.duplicate_observations, 0u);
}

TEST_F(TestLabTest, BatchPreviewMatchesSixCasesAndFlagsConflicts) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto generated = create_suite(
        root, "batch-video-source", matrix, true);
    const auto generated_root =
        root / "youtube_test_lab" / generated.suite_id;
    const auto candidate =
        generated_root /
        generated.cases.front().upload_candidate_path;
    const auto manifest = sample_manifest();
    const auto folder = root / "batch";
    std::filesystem::create_directories(folder);
    for (std::size_t i = 0; i < manifest.cases.size(); ++i) {
        std::string id = manifest.cases[i].test_case_id;
        if (i == 1)
            for (auto &character : id)
                character = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(character)));
        const auto filename =
            i == 0 ? "VSX_YT_" + id + "_download.mp4"
                   : "prefix_" + id + "_suffix.mp4";
        std::filesystem::copy_file(candidate, folder / filename);
    }
    auto preview = preview_analysis_folder(manifest, folder);
    ASSERT_EQ(preview.size(), 6u);
    for (const auto &item : preview) {
        EXPECT_EQ(item.status, BatchMatchStatus::Matched);
        EXPECT_TRUE(item.detected_case_id.has_value());
        EXPECT_EQ(item.video.codec, "h264");
        EXPECT_GT(item.file_size, 0u);
    }

    const auto first_id = manifest.cases.front().test_case_id;
    std::filesystem::copy_file(
        candidate, folder / ("another_" + first_id + ".mp4"));
    preview = preview_analysis_folder(manifest, folder);
    int conflicts = 0;
    for (const auto &item : preview)
        if (item.status ==
            BatchMatchStatus::DuplicateCaseConflict)
            ++conflicts;
    EXPECT_EQ(conflicts, 2);
    std::ofstream(folder / "bad_yt003.webm",
                  std::ios::binary) << "not-video";
    preview = preview_analysis_folder(manifest, folder);
    EXPECT_TRUE(std::any_of(
        preview.begin(), preview.end(),
        [](const BatchPreviewItem &item) {
            return item.status == BatchMatchStatus::Unsupported;
        }));
}

TEST_F(TestLabTest,
       BatchCancelResumeAndDuplicateSkipPreserveCompletedState) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 8 * 1024}};
    matrix.resolutions = {{640, 480}};
    const auto generated = create_suite(
        root, "batch-resume", matrix, true);
    const auto suite_root =
        root / "youtube_test_lab" / generated.suite_id;
    const auto manifest_path = suite_root / "manifest.json";
    const auto case_id = generated.cases.front().test_case_id;
    const auto candidate =
        suite_root /
        generated.cases.front().upload_candidate_path;
    const auto folder = root / "batch-resume-input";
    std::filesystem::create_directories(folder);
    std::filesystem::copy_file(
        candidate, folder / ("download_" + case_id + ".mp4"));

    const auto before = read_manifest(manifest_path)
        .cases.front().results.size();
    const auto cancelled = analyze_folder(
        manifest_path, folder, {}, {},
        [](const Progress &) { return false; });
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_EQ(read_manifest(manifest_path)
                  .cases.front().results.size(),
              before);

    const auto resumed =
        analyze_folder(manifest_path, folder);
    EXPECT_FALSE(resumed.cancelled);
    EXPECT_EQ(resumed.analyzed, 1u);
    const auto after = read_manifest(manifest_path)
        .cases.front().results.size();
    EXPECT_EQ(after, before + 1);

    const auto repeated =
        analyze_folder(manifest_path, folder);
    EXPECT_EQ(repeated.analyzed, 0u);
    EXPECT_EQ(repeated.duplicates_skipped, 1u);
    EXPECT_EQ(read_manifest(manifest_path)
                  .cases.front().results.size(),
              after);
}

TEST_F(TestLabTest, ReportsExcludeAccidentalDuplicates) {
    using namespace youtube_test_lab;
    auto manifest = sample_manifest();
    TestResult result;
    result.observation_id = "obs-1";
    result.analysis_session_id = "session-1";
    result.suite_id = manifest.suite_id;
    result.case_id = manifest.cases.front().test_case_id;
    result.test_case_id = result.case_id;
    result.source = ResultSource::RealYouTubeRoundtrip;
    result.source_type = "real-youtube-roundtrip";
    result.source_file_sha256 = std::string(64, 'a');
    result.analysis_fingerprint = kAnalysisFingerprint;
    result.analyzed_at_utc = "2026-07-30T01:00:00Z";
    result.video.codec = "vp9";
    result.video.height = 1080;
    result.final_status = FinalStatus::Pass;
    result.decode_completed = true;
    result.sha256_match = true;
    manifest.cases.front().results = {result, result};
    const auto reports = root / "unique-reports";
    write_reports(manifest, reports);
    std::ifstream json(reports / "report.json");
    const std::string text{
        std::istreambuf_iterator<char>(json),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("\"unique_observations\": 1"),
              std::string::npos);
    EXPECT_NE(text.find(
                  "\"accidental_duplicates_excluded\": 1"),
              std::string::npos);
    EXPECT_NE(text.find("\"VP9\": {\"passed\": 1"),
              std::string::npos);
}

TEST_F(TestLabTest, Quick1080pRepairProfilesProduceReadyCandidates) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {5.0, 20.0, 50.0};
    matrix.input_variants = {{DataType::Random, 64 * 1024}};
    matrix.resolutions = {{1920, 1080}};
    const auto generated = create_suite(
        root, "quick-1080p-duration", matrix, true);
    ASSERT_EQ(generated.cases.size(), 3u);
    std::optional<uint64_t> effective;
    std::optional<std::string> hash;
    for (const auto &item : generated.cases) {
        EXPECT_GE(item.actual_master_frames, 60u);
        EXPECT_GE(item.actual_candidate_frames, 60u);
        EXPECT_GE(item.candidate_duration_seconds, 1.95);
        EXPECT_TRUE(item.candidate_timestamps_valid);
        EXPECT_TRUE(item.upload_candidate_sha256_match);
        EXPECT_TRUE(item.candidate_ready_for_youtube);
        if (!effective) {
            effective = item.effective_input_size;
            hash = item.input_sha256;
        } else {
            EXPECT_EQ(item.effective_input_size, *effective);
            EXPECT_EQ(item.input_sha256, *hash);
        }
    }
}

TEST_F(TestLabTest, FourKBalancedProducesReadyCandidate) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {20.0};
    matrix.input_variants = {{DataType::Random, 64 * 1024}};
    matrix.resolutions = {{3840, 2160}};
    const auto generated = create_suite(
        root, "quick-4k-duration", matrix, true);
    ASSERT_EQ(generated.cases.size(), 1u);
    const auto &item = generated.cases.front();
    EXPECT_GT(item.effective_input_size,
              item.requested_input_size);
    EXPECT_GE(item.actual_master_frames, 60u);
    EXPECT_GE(item.actual_candidate_frames, 60u);
    EXPECT_GE(item.candidate_duration_seconds, 1.95);
    EXPECT_TRUE(item.candidate_timestamps_valid);
    EXPECT_TRUE(item.upload_candidate_sha256_match);
    EXPECT_TRUE(item.candidate_ready_for_youtube);
}

TEST_F(TestLabTest, CancellationPreservesPendingCasesAndNoPartials) {
    using namespace youtube_test_lab;
    MatrixOptions matrix;
    matrix.repair_percentages = {5.0};
    matrix.input_variants = {{DataType::Random, 1024}};
    matrix.resolutions = {{640, 480}};
    const auto manifest = create_suite(
        root, "cancel-test", matrix, true,
        [](const Progress &) { return false; });
    ASSERT_EQ(manifest.cases.size(), 1u);
    EXPECT_EQ(manifest.cases.front().state, CaseState::Pending);
    const auto suite_root =
        root / "youtube_test_lab" / manifest.suite_id;
    EXPECT_TRUE(std::filesystem::exists(
        suite_root / "manifest.json"));
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(suite_root)) {
        if (entry.is_regular_file())
            EXPECT_EQ(entry.path().filename().string().find(
                          ".vidstorex-part-"),
                      std::string::npos);
    }
}
