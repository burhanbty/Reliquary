#include <gtest/gtest.h>

#include "youtube_test_lab.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace {

class TestLabTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vidstorex-testlab-unit-" +
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

    const auto suite_root =
        root / "youtube_test_lab" / generated.suite_id;
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
