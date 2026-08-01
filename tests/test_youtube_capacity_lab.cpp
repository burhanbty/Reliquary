#include <gtest/gtest.h>

#include "dct_common.h"
#include "youtube_capacity_lab.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace {

using namespace youtube_capacity_lab;

ExperimentConfig config_for(
    const int block, const int bits,
    const int width = 1920, const int height = 1080) {
    ExperimentConfig config;
    config.block_width = block;
    config.block_height = block;
    config.bits_per_block = bits;
    config.resolution_width = width;
    config.resolution_height = height;
    config.modulation_version =
        bits == 1 ? kModulation1Version : kModulation2Version;
    return config;
}

std::filesystem::path unique_temp(const std::string &name) {
    static int sequence = 0;
    return std::filesystem::temp_directory_path() /
        ("vidstorex-capacity-test-" + name + "-" +
         std::to_string(++sequence));
}

CapacityCase local_case(
    const ExperimentConfig &config, const int stage,
    const std::string &profile, const bool candidate_pass = true,
    const bool master_pass = true) {
    CapacityCase test_case;
    test_case.config = config;
    test_case.config_id = config.config_id();
    test_case.case_id =
        test_case.config_id + "-" + profile;
    test_case.stage = stage;
    test_case.capacity = compute_capacity(config);
    test_case.effective_payload_bytes = 4096;
    test_case.candidate_size = 1024;
    test_case.requested_simulation_profile = profile;
    test_case.state =
        candidate_pass && master_pass
            ? CaseState::Passed : CaseState::Rejected;
    CaseResult master;
    master.source_type = "master-lossless";
    master.simulation_profile = "master-lossless";
    master.decode_completed = master_pass;
    master.sha256_match = master_pass;
    master.telemetry.packet_recovery_percent =
        master_pass ? 100.0 : 0.0;
    test_case.results.push_back(master);
    CaseResult candidate;
    candidate.source_type = "upload-candidate";
    candidate.simulation_profile = profile;
    candidate.decode_completed = candidate_pass;
    candidate.sha256_match = candidate_pass;
    candidate.metadata_valid = candidate_pass;
    candidate.telemetry.packet_recovery_percent =
        candidate_pass ? 100.0 : 80.0;
    candidate.telemetry.recovery_margin_percent =
        candidate_pass ? 2.0 : -20.0;
    candidate.telemetry.average_confidence =
        candidate_pass ? 0.9 : 0.1;
    test_case.results.push_back(candidate);
    return test_case;
}

ExperimentManifest staged_profile_manifest(
    const ExperimentConfig &config,
    const std::string &failed_profile = {}) {
    ExperimentManifest manifest;
    manifest.experiment_id = "ELIGIBILITY";
    manifest.created_at = "2026-07-30T00:00:00Z";
    manifest.preset = Preset::Staged;
    for (const std::string &profile : {
             "yt-sim-1080p-light",
             "yt-sim-1080p-medium",
             "yt-sim-1080p-heavy"})
        manifest.cases.push_back(local_case(
            config, 3, profile, profile != failed_profile));
    return manifest;
}

ExperimentManifest boundary_manifest() {
    RunOptions options;
    options.preset = Preset::Boundary1080p;
    options.maximum_cases = 7;
    ExperimentManifest manifest;
    manifest.experiment_id = "BOUNDARY";
    manifest.created_at = "2026-07-30T00:00:00Z";
    manifest.preset = Preset::Boundary1080p;
    manifest.baseline = production_baseline_config();
    manifest.maximum_cases = 7;
    manifest.maximum_shortlist_videos = 7;
    manifest.cases = build_initial_cases(options, "BOUNDARY");
    for (auto &test_case : manifest.cases) {
        test_case.state = CaseState::Passed;
        test_case.mandatory_gates_passed = true;
        auto local = local_case(
            test_case.config, 1, "yt-sim-1080p-medium");
        test_case.results = local.results;
        test_case.results.back().source_type = "local-simulation";
        test_case.results.back().simulation_profile =
            "yt-sim-1080p-medium";
        CaseResult candidate = test_case.results.back();
        candidate.source_type = "upload-candidate";
        test_case.results.insert(
            test_case.results.begin() + 1, candidate);
    }
    return manifest;
}

double boundary_gain(const CapacityCase &test_case) {
    if (test_case.config.block_width == 8 &&
        test_case.config.bits_per_block == 1)
        return 1.00;
    if (test_case.config.block_width == 6 &&
        test_case.config.bits_per_block == 1)
        return 1.77;
    if (test_case.config.block_width == 8 &&
        test_case.config.bits_per_block == 2)
        return 2.00;
    if (test_case.config.block_width == 6 &&
        test_case.config.bits_per_block == 2)
        return 3.62;
    return 4.00;
}

void add_real_observation(
    CapacityCase &test_case, const bool pass,
    const std::string &session = "Boundary initial YouTube test",
    const bool correct_resolution = true,
    const double margin = 2.0) {
    CaseResult result;
    result.source_type = "real-youtube-roundtrip";
    result.analysis_session_label = session;
    result.analyzed_file_sha256 =
        test_case.config_id + session;
    result.codec = "h264";
    result.returned_width =
        correct_resolution
            ? test_case.config.resolution_width : 1280;
    result.returned_height =
        correct_resolution
            ? test_case.config.resolution_height : 720;
    result.returned_fps = 30.0;
    result.metadata_valid = correct_resolution;
    result.decode_completed = true;
    result.sha256_match = pass;
    result.telemetry.required_packet_threshold = 100;
    result.telemetry.valid_unique_packets =
        pass ? 102 : 80;
    result.telemetry.recovery_margin_packets =
        pass ? 2 : -20;
    result.telemetry.recovery_margin_percent =
        pass ? margin : -20.0;
    result.telemetry.packet_recovery_percent =
        pass ? 100.0 : 80.0;
    test_case.results.push_back(std::move(result));
}

void observe_density(
    ExperimentManifest &manifest, const double gain,
    const bool pass) {
    for (auto &test_case : manifest.cases)
        if (std::abs(boundary_gain(test_case) - gain) < 0.001)
            add_real_observation(test_case, pass);
}

} // namespace

TEST(CapacityConfig, CanonicalIdIsStable) {
    const auto first = config_for(6, 2);
    const auto second = config_for(6, 2);
    EXPECT_EQ(first.canonical_serialization(),
              second.canonical_serialization());
    EXPECT_EQ(first.config_id(), second.config_id());
    EXPECT_EQ(first.config_id().size(), 12U);
}

TEST(CapacityConfig, FixedPointSignalSerializationIsCanonical) {
    auto config = config_for(8, 1);
    config.signal_milli = 750;
    const auto text = config.canonical_serialization();
    EXPECT_NE(text.find("signal_milli=750"), std::string::npos);
    EXPECT_EQ(text.find("0.750"), std::string::npos);
}

TEST(CapacityConfig, DifferentFieldsChangeId) {
    auto first = config_for(8, 1);
    auto second = first;
    second.signal_milli = 1250;
    EXPECT_NE(first.config_id(), second.config_id());
}

TEST(CapacityConfig, ProductionBaselineIsUnchanged) {
    const auto baseline = production_baseline_config();
    EXPECT_EQ(baseline.block_width, 8);
    EXPECT_EQ(baseline.bits_per_block, 1);
    EXPECT_EQ(baseline.signal_milli, 1000);
    EXPECT_EQ(baseline.repair_basis_points, 500);
    EXPECT_DOUBLE_EQ(COEFFICIENT_STRENGTH, 500.0);
}

TEST(CapacityConfig, InvalidBlockBitsSignalRepairAndResolutionReject) {
    std::string reason;
    auto config = config_for(8, 1);
    config.block_width = config.block_height = 7;
    EXPECT_FALSE(config.valid(&reason));
    config = config_for(8, 3);
    EXPECT_FALSE(config.valid(&reason));
    config = config_for(8, 1);
    config.signal_milli = 999;
    EXPECT_FALSE(config.valid(&reason));
    config = config_for(8, 1);
    config.repair_basis_points = 300;
    EXPECT_FALSE(config.valid(&reason));
    config = config_for(8, 1, 1280, 720);
    EXPECT_FALSE(config.valid(&reason));
}

class CapacityGeometryTest :
    public testing::TestWithParam<std::tuple<int, int, int, int>> {};

TEST_P(CapacityGeometryTest, ComputesDeterministicFullBlocks) {
    const auto [width, height, block, expected_columns] = GetParam();
    const auto geometry =
        compute_geometry(config_for(block, 1, width, height));
    EXPECT_EQ(geometry.blocks_per_row, expected_columns);
    EXPECT_EQ(geometry.blocks_per_column, height / block);
    EXPECT_EQ(geometry.total_blocks,
              static_cast<uint64_t>(width / block) * (height / block));
    EXPECT_EQ(geometry.raw_bits_per_frame, geometry.total_blocks);
    EXPECT_EQ(geometry.unused_right_pixels, width % block);
    EXPECT_EQ(geometry.unused_bottom_pixels, height % block);
    EXPECT_GT(geometry.packets_per_frame, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    ResolutionsAndBlocks, CapacityGeometryTest,
    testing::Values(
        std::tuple{1920, 1080, 8, 240},
        std::tuple{1920, 1080, 6, 320},
        std::tuple{1920, 1080, 5, 384},
        std::tuple{1920, 1080, 4, 480},
        std::tuple{1920, 1080, 3, 640},
        std::tuple{3840, 2160, 8, 480},
        std::tuple{3840, 2160, 6, 640},
        std::tuple{3840, 2160, 4, 960}));

TEST(CapacityGeometry, SixBySixUsesNoPartialBottomBlock) {
    const auto geometry =
        compute_geometry(config_for(6, 2));
    EXPECT_EQ(geometry.unused_bottom_pixels, 0);
    EXPECT_EQ(geometry.unused_right_pixels, 0);
    EXPECT_EQ(geometry.raw_bits_per_frame,
              geometry.total_blocks * 2);
}

TEST(CapacityGeometry, TwoBitsDoublesRawCapacity) {
    const auto one = compute_geometry(config_for(4, 1));
    const auto two = compute_geometry(config_for(4, 2));
    EXPECT_EQ(two.raw_bits_per_frame,
              one.raw_bits_per_frame * 2);
    EXPECT_GT(two.packets_per_frame, one.packets_per_frame);
}

class CapacityTransformTest :
    public testing::TestWithParam<int> {};

TEST_P(CapacityTransformTest, ForwardInverseRoundTrip) {
    const int n = GetParam();
    std::vector<double> pixels(
        static_cast<std::size_t>(n * n));
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = 20.0 + (i * 17) % 211;
    std::vector<double> coefficients;
    std::vector<double> restored;
    forward_dct(pixels, n, coefficients);
    inverse_dct(coefficients, n, restored);
    ASSERT_EQ(restored.size(), pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i)
        EXPECT_NEAR(restored[i], pixels[i], 1e-9);
}

INSTANTIATE_TEST_SUITE_P(
    SupportedSizes, CapacityTransformTest,
    testing::Values(3, 4, 5, 6, 8));

TEST(CapacityTransform, BasisIsCached) {
    EXPECT_EQ(&dct_basis(6), &dct_basis(6));
    EXPECT_NE(&dct_basis(4), &dct_basis(8));
}

TEST(CapacityTransform, ProductionEightByEightPatternRegression) {
    const auto config = config_for(8, 1);
    const auto zero = make_symbol_block(config, 0);
    const auto one = make_symbol_block(config, 1);
    const auto &production = get_precomputed_blocks();
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            EXPECT_EQ(zero[static_cast<std::size_t>(y * 8 + x)],
                      production.patterns[0][y][x]);
            EXPECT_EQ(one[static_cast<std::size_t>(y * 8 + x)],
                      production.patterns[1][y][x]);
        }
}

TEST(CapacityModulation1, EverySignalLevelRoundTrips) {
    for (const int signal : {750, 1000, 1250, 1500}) {
        auto config = config_for(8, 1);
        config.signal_milli = signal;
        for (uint8_t symbol = 0; symbol < 2; ++symbol) {
            const auto block = make_symbol_block(config, symbol);
            const auto decision =
                decode_symbol(block.data(), 8, config);
            EXPECT_EQ(decision.symbol, symbol);
            EXPECT_GT(decision.confidence, 0.9);
        }
    }
}

TEST(CapacityModulation1, StrongSignalReportsClamp) {
    auto config = config_for(4, 1);
    config.signal_milli = 1500;
    bool clamped = false;
    (void) make_symbol_block(config, 0, &clamped);
    EXPECT_TRUE(clamped);
}

TEST(CapacityModulation2, GrayCodeOrderIs001110) {
    EXPECT_EQ(gray_symbol_for_bits(0b00), 0);
    EXPECT_EQ(gray_symbol_for_bits(0b01), 1);
    EXPECT_EQ(gray_symbol_for_bits(0b11), 2);
    EXPECT_EQ(gray_symbol_for_bits(0b10), 3);
    for (uint8_t bits = 0; bits < 4; ++bits)
        EXPECT_EQ(bits_for_gray_symbol(
                      gray_symbol_for_bits(bits)),
                  bits);
}

TEST(CapacityModulation2, FourSymbolsRoundTripWithConfidence) {
    auto config = config_for(6, 2);
    config.signal_milli = 1250;
    for (uint8_t symbol = 0; symbol < 4; ++symbol) {
        const auto block = make_symbol_block(config, symbol);
        const auto decision =
            decode_symbol(block.data(), 6, config);
        EXPECT_EQ(decision.symbol, symbol);
        EXPECT_GT(decision.confidence, 0.85);
        EXPECT_LT(decision.nearest_level_distance, 10.0);
    }
}

TEST(CapacityModulation2, NeighborLevelGrayErrorChangesOneBit) {
    for (uint8_t symbol = 0; symbol < 3; ++symbol) {
        const uint8_t first = bits_for_gray_symbol(symbol);
        const uint8_t second = bits_for_gray_symbol(symbol + 1);
        const unsigned difference = first ^ second;
        EXPECT_TRUE(difference == 1 || difference == 2);
    }
}

TEST(CapacitySearch, SmokeHasTwelveUniqueConfigs) {
    const auto configs = smoke_configs();
    ASSERT_EQ(configs.size(), 12U);
    std::set<std::string> ids;
    for (const auto &config : configs)
        ids.insert(config.config_id());
    EXPECT_EQ(ids.size(), configs.size());
}

TEST(CapacitySearch, StageOneHasTwentyFourConfigs) {
    const auto configs = stage1_configs();
    ASSERT_EQ(configs.size(), 24U);
    for (const auto &config : configs) {
        EXPECT_EQ(config.resolution_width, 1920);
        EXPECT_EQ(config.repair_basis_points, 200);
    }
}

TEST(CapacityFairness, RepairFamilyUsesSamePayloadAndSeed) {
    RunOptions options;
    options.preset = Preset::Custom;
    options.block_sizes = {6};
    options.bits_per_block = {2};
    options.signal_milli = {1250};
    options.repair_basis_points = {0, 100, 200, 500};
    options.resolutions = {{1920, 1080}};
    options.maximum_cases = 4;
    const auto cases = build_initial_cases(options, "fairness");
    ASSERT_EQ(cases.size(), 4U);
    for (const auto &test_case : cases) {
        EXPECT_EQ(test_case.effective_payload_bytes,
                  cases.front().effective_payload_bytes);
        EXPECT_EQ(test_case.payload_seed,
                  cases.front().payload_seed);
        EXPECT_GE(test_case.capacity.expected_frames, 60U);
    }
}

TEST(CapacityMetrics, GainsAreComparedWithBaseline) {
    const auto baseline =
        compute_capacity(production_baseline_config());
    const auto dense =
        compute_capacity(config_for(4, 2));
    EXPECT_DOUBLE_EQ(baseline.useful_payload_gain, 1.0);
    EXPECT_GT(dense.raw_capacity_gain, 7.9);
    EXPECT_GT(dense.useful_payload_gain, 7.0);
    EXPECT_GT(dense.useful_payload_bytes_per_second,
              baseline.useful_payload_bytes_per_second);
}

TEST(CapacityPareto, DominatedCandidateIsMarked) {
    CapacityCase strong;
    strong.config = config_for(4, 2);
    strong.config_id = strong.config.config_id();
    strong.capacity = compute_capacity(strong.config);
    strong.effective_payload_bytes = 1000;
    strong.candidate_size = 1000;
    strong.mandatory_gates_passed = true;
    CaseResult strong_result;
    strong_result.source_type = "upload-candidate";
    strong_result.telemetry.recovery_margin_percent = 10;
    strong_result.telemetry.average_confidence = 0.9;
    strong.results.push_back(strong_result);
    CapacityCase weak = strong;
    weak.config = config_for(8, 1);
    weak.config_id = weak.config.config_id();
    weak.capacity = compute_capacity(weak.config);
    weak.candidate_size = 2000;
    weak.results.back().telemetry.recovery_margin_percent = 5;
    std::vector<CapacityCase> cases{strong, weak};
    update_pareto_and_categories(cases);
    EXPECT_TRUE(cases[0].pareto);
    EXPECT_TRUE(cases[1].dominated);
}

TEST(CapacityManifest, RoundTripPreservesConfigAndResults) {
    const auto root = unique_temp("manifest");
    std::filesystem::create_directories(root);
    ExperimentManifest manifest;
    manifest.experiment_id = "CAPACITY-ROUNDTRIP";
    manifest.created_at = "2026-07-30T00:00:00Z";
    manifest.preset = Preset::Smoke;
    CapacityCase test_case;
    test_case.config = config_for(6, 2);
    test_case.config_id = test_case.config.config_id();
    test_case.case_id = "case-1";
    test_case.capacity = compute_capacity(test_case.config);
    test_case.payload_seed = 0xfedcba9876543210ULL;
    test_case.state = CaseState::Passed;
    test_case.mandatory_gates_passed = true;
    CaseResult result;
    result.source_type = "upload-candidate";
    result.sha256_match = true;
    result.telemetry.raw_ber = 0.001;
    test_case.results.push_back(result);
    manifest.cases.push_back(test_case);
    const auto path = root / "manifest.json";
    write_manifest_atomic(manifest, path);
    const auto restored = read_manifest(path);
    ASSERT_EQ(restored.cases.size(), 1U);
    EXPECT_EQ(restored.cases[0].config_id, test_case.config_id);
    EXPECT_EQ(restored.cases[0].payload_seed,
              test_case.payload_seed);
    ASSERT_EQ(restored.cases[0].results.size(), 1U);
    EXPECT_TRUE(restored.cases[0].results[0].sha256_match);
    EXPECT_FALSE(std::filesystem::exists(
        path.string() + ".partial"));
    std::filesystem::remove_all(root);
}

TEST(CapacityManifest, RecomputesBerSerFromAuthoritativeCounters) {
    const auto root = unique_temp("manifest-ber-migration");
    std::filesystem::create_directories(root);
    ExperimentManifest manifest;
    manifest.experiment_id = "CAPACITY-BER-MIGRATION";
    manifest.created_at = "2026-07-30T00:00:00Z";
    auto test_case = local_case(
        config_for(4, 2), 1,
        "yt-sim-1080p-medium");
    auto &result = test_case.results.back();
    result.telemetry.bits_compared = 100000000;
    result.telemetry.bit_errors = 1;
    result.telemetry.symbols_compared = 50000000;
    result.telemetry.symbol_errors = 1;
    // Simulate a schema-v4 manifest previously rewritten after losing the
    // scientific-notation exponent.
    result.telemetry.raw_ber = 10.0;
    result.telemetry.raw_ser = 20.0;
    manifest.cases.push_back(test_case);
    const auto path = root / "manifest.json";
    write_manifest_atomic(manifest, path);
    const auto restored = read_manifest(path);
    ASSERT_EQ(restored.cases.size(), 1U);
    ASSERT_EQ(restored.cases.front().results.size(), 2U);
    EXPECT_DOUBLE_EQ(
        restored.cases.front().results.back()
            .telemetry.raw_ber,
        1.0 / 100000000.0);
    EXPECT_DOUBLE_EQ(
        restored.cases.front().results.back()
            .telemetry.raw_ser,
        1.0 / 50000000.0);
    std::filesystem::remove_all(root);
}

TEST(CapacityReports, JsonCsvMarkdownKeepLocalAndRealDistinct) {
    const auto root = unique_temp("reports");
    ExperimentManifest manifest;
    manifest.experiment_id = "CAPACITY-REPORT";
    manifest.created_at = "2026-07-30T00:00:00Z";
    write_reports(manifest, root);
    EXPECT_TRUE(std::filesystem::exists(
        root / "capacity_summary.json"));
    EXPECT_TRUE(std::filesystem::exists(
        root / "capacity_results.csv"));
    {
        std::ifstream input(root / "capacity_results.csv");
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        EXPECT_NE(
            text.find("failed_mandatory_profile"),
            std::string::npos);
        EXPECT_NE(
            text.find("shortlist_filename"),
            std::string::npos);
    }
    {
        std::ifstream input(root / "capacity_summary.json");
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        EXPECT_NE(
            text.find("\"config_decisions\""),
            std::string::npos);
        EXPECT_NE(
            text.find("\"shortlist_exclusions\""),
            std::string::npos);
    }
    const auto markdown = root / "capacity_report.md";
    ASSERT_TRUE(std::filesystem::exists(markdown));
    {
        std::ifstream input(markdown);
        const std::string text{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        EXPECT_NE(text.find("not real YouTube evidence"),
                  std::string::npos);
        EXPECT_NE(text.find("Insufficient observations"),
                  std::string::npos);
    }
    std::filesystem::remove_all(root);
}

TEST(CapacityShortlist, UsesUniqueConfigsAndResetsOldSelections) {
    CapacityCase first;
    first.config = config_for(4, 2);
    first.config_id = first.config.config_id();
    first.capacity = compute_capacity(first.config);
    first.mandatory_gates_passed = true;
    first.state = CaseState::Shortlisted;
    first.shortlisted = true;
    CaseResult first_result;
    first_result.source_type = "upload-candidate";
    first_result.sha256_match = true;
    first_result.telemetry.recovery_margin_percent = 5;
    first.results.push_back(first_result);

    CapacityCase duplicate = first;
    duplicate.case_id = "same-config-other-profile";
    duplicate.state = CaseState::Passed;
    duplicate.shortlisted = false;
    duplicate.results.back().telemetry.recovery_margin_percent = 4;

    CapacityCase second = first;
    second.config = config_for(8, 1);
    second.config_id = second.config.config_id();
    second.capacity = compute_capacity(second.config);
    second.state = CaseState::Passed;
    second.shortlisted = false;
    second.results.back().telemetry.recovery_margin_percent = 10;

    std::vector<CapacityCase> cases{first, duplicate, second};
    const auto selected = select_shortlist(cases, 8);
    std::set<std::string> selected_ids;
    for (const auto index : selected)
        selected_ids.insert(cases[index].config_id);
    EXPECT_EQ(selected.size(), selected_ids.size());
    EXPECT_LE(selected.size(), 2U);
    EXPECT_EQ(
        std::count_if(
            cases.begin(), cases.end(),
            [](const CapacityCase &value) {
                return value.state == CaseState::Shortlisted;
            }),
        static_cast<int>(selected.size()));
}

TEST(CapacityEligibility, AllMandatoryProfilesPass) {
    auto manifest =
        staged_profile_manifest(config_for(4, 2));
    const auto decision =
        evaluate_shortlist_eligibility(
            manifest, manifest.cases.front().config_id);
    EXPECT_TRUE(decision.eligible);
    EXPECT_FALSE(decision.rejected);
    EXPECT_FALSE(decision.incomplete);
    EXPECT_EQ(
        manifest.cases[*decision.representative_case]
            .requested_simulation_profile,
        "yt-sim-1080p-medium");
}

TEST(CapacityEligibility, EveryMandatoryProfileCanBlockConfig) {
    for (const std::string &profile : {
             "yt-sim-1080p-light",
             "yt-sim-1080p-medium",
             "yt-sim-1080p-heavy"}) {
        auto manifest =
            staged_profile_manifest(
                config_for(4, 2), profile);
        const auto decision =
            evaluate_shortlist_eligibility(
                manifest,
                manifest.cases.front().config_id);
        EXPECT_FALSE(decision.eligible);
        EXPECT_TRUE(decision.rejected);
        EXPECT_EQ(
            decision.failed_mandatory_profile,
            profile);
        EXPECT_NE(
            decision.reason.find("SHA-256 mismatch"),
            std::string::npos);
    }
}

TEST(CapacityEligibility, DownscaleFailureIsNonGating) {
    auto manifest =
        staged_profile_manifest(config_for(4, 2));
    auto downscale = local_case(
        manifest.cases.front().config, 3,
        "yt-sim-720p-downscale", false);
    downscale.state = CaseState::ResolutionUnsupported;
    manifest.cases.push_back(std::move(downscale));
    EXPECT_TRUE(
        evaluate_shortlist_eligibility(
            manifest, manifest.cases.front().config_id)
            .eligible);
}

TEST(CapacityEligibility, MasterFailureAndIncompleteAreIneligible) {
    auto master_failed =
        staged_profile_manifest(config_for(4, 2));
    master_failed.cases.front() = local_case(
        master_failed.cases.front().config, 3,
        "yt-sim-1080p-light", true, false);
    auto decision = evaluate_shortlist_eligibility(
        master_failed,
        master_failed.cases.front().config_id);
    EXPECT_TRUE(decision.rejected);
    EXPECT_FALSE(decision.eligible);

    auto incomplete =
        staged_profile_manifest(config_for(4, 2));
    incomplete.cases.pop_back();
    decision = evaluate_shortlist_eligibility(
        incomplete, incomplete.cases.front().config_id);
    EXPECT_TRUE(decision.incomplete);
    EXPECT_FALSE(decision.eligible);
}

TEST(CapacityEligibility, RejectedConfigCannotEnterParetoOrShortlist) {
    auto manifest =
        staged_profile_manifest(
            config_for(4, 2),
            "yt-sim-1080p-heavy");
    auto eligible = local_case(
        config_for(8, 1), 1,
        "yt-sim-1080p-medium");
    manifest.cases.push_back(std::move(eligible));
    const auto rejected_id =
        manifest.cases.front().config_id;
    const auto selected =
        select_shortlist(manifest, 8);
    EXPECT_TRUE(std::none_of(
        selected.begin(), selected.end(),
        [&](const std::size_t index) {
            return manifest.cases[index].config_id ==
                rejected_id;
        }));
    for (const auto &test_case : manifest.cases)
        if (test_case.config_id == rejected_id) {
            EXPECT_FALSE(test_case.shortlisted);
            EXPECT_FALSE(test_case.pareto);
            EXPECT_FALSE(test_case.eligible_for_shortlist);
        }
}

TEST(CapacityShortlist, RegenerationRepairsConflictAtomically) {
    const auto root = unique_temp("shortlist-regeneration");
    std::filesystem::create_directories(
        root / "simulations");
    std::filesystem::create_directories(
        root / "youtube_shortlist");
    {
        std::ofstream stale(
            root / "youtube_shortlist" / "stale.mp4",
            std::ios::binary);
        stale << "old shortlist";
    }
    auto manifest =
        staged_profile_manifest(
            config_for(4, 2),
            "yt-sim-1080p-heavy");
    manifest.cases[1].shortlisted = true;
    manifest.cases[1].state = CaseState::Shortlisted;
    manifest.cases[1].pareto = true;
    for (auto &test_case : manifest.cases) {
        test_case.candidate_path =
            "simulations/" + test_case.case_id + ".mp4";
        std::ofstream candidate(
            root / test_case.candidate_path,
            std::ios::binary);
        candidate << "candidate";
    }
    auto good = local_case(
        config_for(8, 1), 1,
        "yt-sim-1080p-medium");
    good.candidate_path =
        "simulations/" + good.case_id + ".mp4";
    {
        std::ofstream candidate(
            root / good.candidate_path,
            std::ios::binary);
        candidate << "eligible candidate";
    }
    manifest.cases.push_back(good);
    const auto path = root / "manifest.json";
    write_manifest_atomic(manifest, path);
    std::ifstream before_input(path, std::ios::binary);
    const std::string before{
        std::istreambuf_iterator<char>(before_input),
        std::istreambuf_iterator<char>()};
    before_input.close();

    const auto before_validation =
        validate_experiment(path);
    EXPECT_FALSE(before_validation.issues.empty());
    std::ifstream after_validate_input(path, std::ios::binary);
    const std::string after_validate{
        std::istreambuf_iterator<char>(after_validate_input),
        std::istreambuf_iterator<char>()};
    after_validate_input.close();
    EXPECT_EQ(before, after_validate);

    const auto regenerated =
        generate_shortlist(path, 8);
    EXPECT_TRUE(std::filesystem::exists(
        regenerated.manifest_backup));
    EXPECT_TRUE(std::filesystem::exists(
        regenerated.previous_shortlist_archive /
        "stale.mp4"));
    EXPECT_EQ(regenerated.selected_configs, 1U);
    EXPECT_NE(
        std::find(
            regenerated.removed_files.begin(),
            regenerated.removed_files.end(),
            "stale.mp4"),
        regenerated.removed_files.end());
    std::size_t videos = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator(
             root / "youtube_shortlist"))
        videos += entry.is_regular_file() &&
            entry.path().extension() == ".mp4";
    EXPECT_EQ(videos, 1U);
    EXPECT_TRUE(validate_experiment(path).issues.empty());
    EXPECT_TRUE(std::filesystem::exists(
        root / good.candidate_path));

    const auto repeated =
        generate_shortlist(path, 8);
    EXPECT_EQ(repeated.selected_configs, 1U);
    EXPECT_TRUE(repeated.removed_files.empty());
    EXPECT_TRUE(validate_experiment(path).issues.empty());
    std::filesystem::remove_all(root);
}

TEST(CapacityPreflight, SmokeIsBoundedAndReportsDisk) {
    RunOptions options;
    options.output_root = std::filesystem::temp_directory_path();
    const auto result = estimate(options);
    EXPECT_EQ(result.raw_combination_count, 192U);
    EXPECT_EQ(result.staged_maximum_cases, 12U);
    EXPECT_EQ(result.estimated_transcodes, 12U);
    EXPECT_GT(result.required_disk_bytes,
              result.estimated_output_bytes);
}

TEST(CapacityPreflight, FullMatrixRequiresExplicitCaseLimit) {
    RunOptions options;
    options.preset = Preset::Custom;
    options.maximum_cases = 64;
    EXPECT_THROW((void) build_initial_cases(options, "too-many"),
                 std::invalid_argument);
}

TEST(BoundaryPreset, HasExactlySevenDeterministic1080pCases) {
    RunOptions options;
    options.preset = Preset::Boundary1080p;
    options.maximum_cases = 7;
    const auto cases =
        build_initial_cases(options, "BOUNDARY");
    ASSERT_EQ(cases.size(), 7U);
    const std::array<std::string, 7> ids{
        "B00", "B01", "B02", "B03",
        "B04", "B05", "B06"};
    const std::array<double, 7> gains{
        1.00, 1.77, 1.77, 2.00, 2.00, 3.62, 4.00};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(cases[i].boundary_case_id, ids[i]);
        EXPECT_EQ(cases[i].case_id, ids[i]);
        EXPECT_EQ(cases[i].config.resolution_width, 1920);
        EXPECT_EQ(cases[i].config.resolution_height, 1080);
        EXPECT_EQ(cases[i].config.signal_milli, 1000);
        EXPECT_EQ(cases[i].config_id,
                  cases[i].config.config_id());
        EXPECT_DOUBLE_EQ(
            cases[i].boundary_density_gain, gains[i]);
    }
}

TEST(BoundaryPreset, MatrixExcludes4kAndFourByFourTwoBit) {
    const auto configs = boundary_1080p_configs();
    ASSERT_EQ(configs.size(), 7U);
    for (const auto &config : configs) {
        EXPECT_EQ(config.resolution_height, 1080);
        EXPECT_FALSE(config.block_width == 4 &&
                     config.bits_per_block == 2);
    }
}

TEST(BoundaryPreset, OrderedMatrixMatchesSpecification) {
    const auto configs = boundary_1080p_configs();
    const std::array<std::tuple<int, int, int>, 7> expected{
        std::tuple{8, 1, 500},
        std::tuple{6, 1, 200},
        std::tuple{6, 1, 500},
        std::tuple{8, 2, 200},
        std::tuple{8, 2, 500},
        std::tuple{6, 2, 500},
        std::tuple{4, 1, 500}};
    ASSERT_EQ(configs.size(), expected.size());
    for (std::size_t i = 0; i < configs.size(); ++i) {
        EXPECT_EQ(configs[i].block_width,
                  std::get<0>(expected[i]));
        EXPECT_EQ(configs[i].bits_per_block,
                  std::get<1>(expected[i]));
        EXPECT_EQ(configs[i].repair_basis_points,
                  std::get<2>(expected[i]));
    }
}

TEST(BoundaryPayload, RepairFamiliesSharePayloadIdentity) {
    auto manifest = boundary_manifest();
    EXPECT_EQ(manifest.cases[1].payload_family_id,
              manifest.cases[2].payload_family_id);
    EXPECT_EQ(manifest.cases[1].deterministic_stream_id,
              manifest.cases[2].deterministic_stream_id);
    EXPECT_EQ(manifest.cases[1].payload_seed,
              manifest.cases[2].payload_seed);
    EXPECT_EQ(manifest.cases[1].effective_payload_bytes,
              manifest.cases[2].effective_payload_bytes);
    EXPECT_EQ(manifest.cases[3].payload_family_id,
              manifest.cases[4].payload_family_id);
    EXPECT_EQ(manifest.cases[3].payload_seed,
              manifest.cases[4].payload_seed);
}

TEST(BoundaryPayload, EveryCaseUsesAtLeastSixtyRealFrames) {
    const auto manifest = boundary_manifest();
    for (const auto &test_case : manifest.cases) {
        EXPECT_GE(test_case.capacity.expected_frames, 60U);
        EXPECT_GE(test_case.capacity.expected_duration_seconds, 2.0);
        EXPECT_GE(test_case.effective_payload_bytes,
                  test_case.capacity.minimum_payload_bytes);
    }
}

TEST(BoundaryBaseline, UsesExactProductionConfiguration) {
    const auto manifest = boundary_manifest();
    ASSERT_FALSE(manifest.cases.empty());
    EXPECT_TRUE(manifest.cases.front().production_codec_path);
    EXPECT_EQ(
        manifest.cases.front().config.canonical_serialization(),
        production_baseline_config().canonical_serialization());
}

TEST(BoundaryInference, UntestedDoesNotInventBoundary) {
    const auto result = infer_boundary(boundary_manifest());
    EXPECT_EQ(result.status, "Insufficient observations");
    EXPECT_EQ(result.bracket, "Insufficient observations");
    EXPECT_EQ(result.baseline_status, "Not uploaded/tested");
}

TEST(BoundaryInference, BaselineFailureInvalidatesControl) {
    auto manifest = boundary_manifest();
    add_real_observation(manifest.cases.front(), false);
    const auto result = infer_boundary(manifest);
    EXPECT_EQ(result.status, "Invalid control result");
    EXPECT_EQ(result.bracket, "Invalid control result");
}

TEST(BoundaryInference, BaselineOnlyPassBracketsAt177Failure) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    observe_density(manifest, 1.77, false);
    const auto result = infer_boundary(manifest);
    EXPECT_EQ(result.bracket,
              "1.00x <= boundary < 1.77x");
}

TEST(BoundaryInference, Pass177Fail200CreatesBracket) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    observe_density(manifest, 1.77, true);
    observe_density(manifest, 2.00, false);
    EXPECT_EQ(infer_boundary(manifest).bracket,
              "1.77x <= boundary < 2.00x");
}

TEST(BoundaryInference, Pass200Fail362CreatesBracket) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    observe_density(manifest, 1.77, true);
    observe_density(manifest, 2.00, true);
    observe_density(manifest, 3.62, false);
    EXPECT_EQ(infer_boundary(manifest).bracket,
              "2.00x <= boundary < 3.62x");
}

TEST(BoundaryInference, Pass362Fail400CreatesBracket) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    observe_density(manifest, 1.77, true);
    observe_density(manifest, 2.00, true);
    observe_density(manifest, 3.62, true);
    observe_density(manifest, 4.00, false);
    EXPECT_EQ(infer_boundary(manifest).bracket,
              "3.62x <= boundary < 4.00x");
}

TEST(BoundaryInference, Pass400RequiresHigherSweep) {
    auto manifest = boundary_manifest();
    for (const double gain :
         {1.00, 1.77, 2.00, 3.62, 4.00})
        observe_density(manifest, gain, true);
    const auto result = infer_boundary(manifest);
    EXPECT_EQ(result.status, "At least 4.00x");
    EXPECT_NE(result.next_experiment.find("above 4.00x"),
              std::string::npos);
}

TEST(BoundaryInference, NonMonotonicIsInconclusive) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    observe_density(manifest, 1.77, false);
    observe_density(manifest, 2.00, true);
    const auto result = infer_boundary(manifest);
    EXPECT_TRUE(result.non_monotonic);
    EXPECT_EQ(result.status, "Non-monotonic / inconclusive");
}

TEST(BoundaryEvidence, WrongResolutionFailsRealGate) {
    auto manifest = boundary_manifest();
    add_real_observation(
        manifest.cases.front(), true,
        "Boundary initial YouTube test", false);
    EXPECT_EQ(real_youtube_status(manifest.cases.front()),
              "Real YouTube invalid resolution");
    EXPECT_EQ(infer_boundary(manifest).status,
              "Invalid control result");
}

TEST(BoundaryEvidence, LocalPassAndRealFailRemainSeparate) {
    auto manifest = boundary_manifest();
    add_real_observation(manifest.cases[1], false);
    EXPECT_EQ(local_evidence_status(manifest.cases[1]),
              "Local simulation pass");
    EXPECT_EQ(real_youtube_status(manifest.cases[1]),
              "Real YouTube SHA mismatch");
    EXPECT_EQ(overall_evidence_status(manifest.cases[1]),
              "Local candidate; Real YouTube failed");
}

TEST(BoundaryEvidence, InitialAndRepeatedExactPassDiffer) {
    auto manifest = boundary_manifest();
    auto &test_case = manifest.cases[2];
    add_real_observation(
        test_case, true, "Boundary initial YouTube test");
    EXPECT_EQ(real_youtube_status(test_case),
              "Real YouTube initial exact pass");
    add_real_observation(
        test_case, true, "Boundary 24-hour retest");
    EXPECT_EQ(real_youtube_status(test_case),
              "Real YouTube repeated exact pass");
}

TEST(BoundaryEvidence, SafeCandidateNeedsRepairFiveAndTwoSessions) {
    auto manifest = boundary_manifest();
    observe_density(manifest, 1.00, true);
    auto &repair5 = manifest.cases[2];
    add_real_observation(
        repair5, true, "Boundary initial YouTube test", true, 2.0);
    add_real_observation(
        repair5, true, "Boundary 24-hour retest", true, 2.0);
    const auto result = infer_boundary(manifest);
    EXPECT_EQ(result.safe_candidate_config_id,
              repair5.config_id);
    EXPECT_FALSE(result.retest_required);
}

TEST(BoundaryRepair, ComparisonUsesMatchedPayloadFamilies) {
    auto manifest = boundary_manifest();
    manifest.cases[1].candidate_size = 1000;
    manifest.cases[2].candidate_size = 1200;
    add_real_observation(manifest.cases[1], false);
    add_real_observation(manifest.cases[2], true);
    const auto comparisons =
        compare_boundary_repairs(manifest);
    ASSERT_EQ(comparisons.size(), 2U);
    EXPECT_EQ(comparisons.front().candidate_size_delta, 200);
    EXPECT_GT(comparisons.front().margin_delta_percent, 0.0);
}

TEST(BoundaryReports, JsonCsvAndMarkdownAgreeWithoutEvidence) {
    const auto root = unique_temp("boundary-reports");
    std::filesystem::create_directories(root);
    const auto manifest = boundary_manifest();
    write_reports(manifest, root);
    for (const auto &name : {
             "boundary_report.md", "boundary_results.csv",
             "boundary_summary.json"})
        EXPECT_TRUE(std::filesystem::exists(root / name));
    std::ifstream markdown(root / "boundary_report.md");
    const std::string text{
        std::istreambuf_iterator<char>(markdown),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("Insufficient observations"),
              std::string::npos);
    EXPECT_NE(text.find("## H. Next experiment recommendation"),
              std::string::npos);
    markdown.close();
    std::filesystem::remove_all(root);
}

TEST(BoundaryManifest, RoundtripPreservesBoundaryState) {
    const auto root = unique_temp("boundary-manifest");
    std::filesystem::create_directories(root);
    auto manifest = boundary_manifest();
    manifest.include_simulation_failures = true;
    const auto path = root / "manifest.json";
    write_manifest_atomic(manifest, path);
    const auto loaded = read_manifest(path);
    EXPECT_EQ(loaded.preset, Preset::Boundary1080p);
    EXPECT_TRUE(loaded.include_simulation_failures);
    ASSERT_EQ(loaded.cases.size(), 7U);
    EXPECT_EQ(loaded.cases.front().boundary_case_id, "B00");
    EXPECT_TRUE(loaded.cases.front().production_codec_path);
    EXPECT_FALSE(
        loaded.cases.front().deterministic_stream_id.empty());
    std::filesystem::remove_all(root);
}

TEST(BoundaryPreflight, IsBoundedToSevenCases) {
    RunOptions options;
    options.preset = Preset::Boundary1080p;
    options.output_root =
        std::filesystem::temp_directory_path();
    const auto result = estimate(options);
    EXPECT_EQ(result.staged_maximum_cases, 7U);
    EXPECT_EQ(result.estimated_transcodes, 21U);
}

TEST(CapacityRealEvidence,
     ConfigLevelReportDoesNotHideRealFailureOnAnotherProfile) {
    const auto root = unique_temp("capacity-real-evidence");
    std::filesystem::create_directories(root);
    auto config = config_for(8, 1);
    auto manifest = staged_profile_manifest(config);
    add_real_observation(manifest.cases.back(), false);
    write_reports(manifest, root);
    std::ifstream report(root / "capacity_report.md");
    const std::string text{
        std::istreambuf_iterator<char>(report),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(
        text.find("Local candidate; Real YouTube failed"),
        std::string::npos);
    report.close();
    std::filesystem::remove_all(root);
}

TEST(OneBitPreset, HasExactlySixCasesInVerificationOrder) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    const auto cases = build_initial_cases(options, "ONEBIT");
    const std::array<std::string, 6> ids{
        "R00", "R01", "G04", "R02", "R03", "G05"};
    const std::array<int, 6> blocks{8, 6, 5, 4, 4, 3};
    ASSERT_EQ(cases.size(), 6U);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(cases[i].case_id, ids[i]);
        EXPECT_EQ(cases[i].config.block_width, blocks[i]);
        EXPECT_EQ(cases[i].config.resolution_width, 1920);
        EXPECT_EQ(cases[i].config.resolution_height, 1080);
        EXPECT_EQ(cases[i].config.bits_per_block, 1);
        EXPECT_EQ(cases[i].config.signal_milli, 1000);
        EXPECT_EQ(cases[i].config.repair_basis_points, 500);
        EXPECT_EQ(1920 % blocks[i], 0);
        EXPECT_EQ(1080 % blocks[i], 0);
        EXPECT_NEAR(cases[i].boundary_density_gain,
                    64.0 / (blocks[i] * blocks[i]), 1e-9);
    }
    EXPECT_EQ(cases[3].config_id, cases[4].config_id);
    EXPECT_NE(cases[3].payload_instance_id,
              cases[4].payload_instance_id);
    EXPECT_NE(cases[3].payload_seed, cases[4].payload_seed);
    EXPECT_EQ(cases[2].deterministic_stream_id,
              cases[5].deterministic_stream_id);
}

TEST(OneBitPreflight, IsBoundedToSixCasesAndThreeSimulations) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.output_root = std::filesystem::temp_directory_path();
    const auto result = estimate(options);
    EXPECT_EQ(result.staged_maximum_cases, 6U);
    EXPECT_EQ(result.estimated_transcodes, 18U);
}

TEST(OneBitInference, VerifiedFourXAndBracket) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    for (auto &c : manifest.cases)
        add_real_observation(c, c.case_id != "G05",
                             "1-bit verification retest");
    HistoricalEvidence b06;
    b06.source_case_id = "B06";
    b06.config_id = manifest.cases[3].config_id;
    b06.payload_instance_id = manifest.cases[3].payload_instance_id;
    b06.session_label = "Boundary initial YouTube test";
    b06.returned_file_sha256 = "historical-b06";
    b06.exact = true;
    b06.recovery_margin_percent = 5.0;
    manifest.historical_evidence.push_back(b06);
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_EQ(inferred.four_x_state,
              "Verified across session and payload");
    const auto four = std::find_if(inferred.densities.begin(),
        inferred.densities.end(), [](const GeometryDensityResult &density) {
            return density.block_size == 4;
        });
    ASSERT_NE(four, inferred.densities.end());
    EXPECT_EQ(four->unique_configs, 1U);
    EXPECT_EQ(four->unique_cases, 2U);
    EXPECT_EQ(four->unique_payload_instances, 2U);
    EXPECT_EQ(four->current_passes, 2U);
    EXPECT_EQ(four->failures, 0U);
    EXPECT_EQ(inferred.status, "Bracketed");
    EXPECT_NE(inferred.boundary_bracket.find("4.00x"),
              std::string::npos);
    EXPECT_FALSE(inferred.safe_candidate.empty());
}

TEST(OneBitInference, SameConfigDifferentCasesRemainIndependentPasses) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    auto &r02 = manifest.cases[3];
    auto &r03 = manifest.cases[4];
    add_real_observation(r02, true, "retest", true, 5.0);
    add_real_observation(r03, true, "retest", true, 5.0);
    r02.results.back().metadata_valid = false;
    r02.results.back().error = "Non-monotonic timestamps";
    r03.results.back().metadata_valid = false;
    r03.results.back().error = "Non-monotonic timestamps";

    const auto cases = infer_onebit_case_observations(manifest);
    ASSERT_EQ(cases.size(), 6U);
    EXPECT_EQ(cases[3].config_id, cases[4].config_id);
    EXPECT_NE(cases[3].payload_instance_id, cases[4].payload_instance_id);
    EXPECT_EQ(cases[3].exact_pass_count, 1U);
    EXPECT_EQ(cases[4].exact_pass_count, 1U);
    EXPECT_EQ(cases[3].failure_count, 0U);
    EXPECT_EQ(cases[4].failure_count, 0U);
    EXPECT_EQ(cases[3].current_status, "pass");
    EXPECT_EQ(cases[4].current_status, "pass");
}

TEST(OneBitInference, SameConfigPassAndFailureProduceMixedGeometry) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    add_real_observation(manifest.cases[0], true);
    add_real_observation(manifest.cases[3], true);
    add_real_observation(manifest.cases[4], false);
    const auto inferred = infer_onebit_geometry(manifest);
    const auto four = std::find_if(inferred.densities.begin(),
        inferred.densities.end(), [](const GeometryDensityResult &density) {
            return density.block_size == 4;
        });
    ASSERT_NE(four, inferred.densities.end());
    EXPECT_EQ(four->current_passes, 1U);
    EXPECT_EQ(four->failures, 1U);
    EXPECT_EQ(four->evidence, GeometryEvidence::MixedResult);
}

TEST(OneBitInference, MissingIndependentPayloadRemainsPending) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    add_real_observation(manifest.cases[0], true);
    add_real_observation(manifest.cases[3], true, "retest", true, 5.0);
    HistoricalEvidence history;
    history.source_case_id = "historical-four";
    history.config_id = manifest.cases[3].config_id;
    history.payload_instance_id = manifest.cases[3].payload_instance_id;
    history.recovery_margin_percent = 5.0;
    history.exact = true;
    manifest.historical_evidence.push_back(history);
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_EQ(inferred.four_x_state,
              "Repeated-session evidence; independent payload pending");
    EXPECT_TRUE(inferred.safe_candidate.empty());
}

TEST(OneBitInference, ControlFailureInvalidatesAllDecisions) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    for (auto &c : manifest.cases)
        add_real_observation(c, c.case_id != "R00");
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_EQ(inferred.status, "Invalid production control");
    EXPECT_TRUE(inferred.safe_candidate.empty());
}

TEST(OneBitReports, WritesAllRequiredArtifacts) {
    const auto root = unique_temp("onebit-reports");
    ExperimentManifest manifest;
    manifest.experiment_id = "ONEBIT";
    manifest.preset = Preset::OneBitVerification1080p;
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    write_reports(manifest, root);
    for (const auto *name : {"onebit_report.md", "onebit_summary.json",
             "onebit_cases.csv", "onebit_observations.csv",
             "onebit_geometry.csv", "onebit_evidence.json"})
        EXPECT_TRUE(std::filesystem::exists(root / name));
    std::filesystem::remove_all(root);
}

TEST(OneBitInference, IndependentFailureMakesFourXMixedAndUnsafe) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    for (auto &c : manifest.cases)
        add_real_observation(c, c.case_id != "R03" && c.case_id != "G05");
    HistoricalEvidence history;
    history.source_case_id = "B06";
    history.config_id = manifest.cases[3].config_id;
    history.exact = true;
    manifest.historical_evidence.push_back(history);
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_EQ(inferred.four_x_state, "Mixed result");
    EXPECT_NE(inferred.safe_candidate, manifest.cases[3].config_id);
}

TEST(OneBitInference, ThreeByThreePassRequiresFinerExperiment) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    for (auto &c : manifest.cases) add_real_observation(c, true);
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_EQ(inferred.status,
              "At least 7.11x; finer geometry experiment required");
    EXPECT_EQ(inferred.experimental_candidate,
              manifest.cases.back().config_id);
    EXPECT_TRUE(inferred.safe_candidate.empty());
}

TEST(OneBitInference, LowerDensityFailureThenHigherPassIsNonMonotonic) {
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.cases = build_initial_cases(options, "ONEBIT");
    for (auto &c : manifest.cases)
        add_real_observation(c, c.case_id != "G04" && c.case_id != "G05");
    const auto inferred = infer_onebit_geometry(manifest);
    EXPECT_TRUE(inferred.non_monotonic);
    EXPECT_EQ(inferred.status, "Non-monotonic / inconclusive");
}

TEST(OneBitManifest, RoundtripPreservesProvenanceAndPayloadInstances) {
    const auto root = unique_temp("onebit-manifest");
    std::filesystem::create_directories(root);
    RunOptions options;
    options.preset = Preset::OneBitVerification1080p;
    options.maximum_cases = 6;
    ExperimentManifest manifest;
    manifest.experiment_id = "ONEBIT";
    manifest.preset = Preset::OneBitVerification1080p;
    manifest.source_experiment_id = "BOUNDARY";
    manifest.source_manifest_sha256 = "abc";
    manifest.cases = build_initial_cases(options, "ONEBIT");
    add_real_observation(manifest.cases[3], true);
    add_real_observation(manifest.cases[4], true);
    manifest.cases[3].results.back().boundary_case_id = "R02";
    manifest.cases[3].results.back().payload_instance_id =
        manifest.cases[3].payload_instance_id;
    manifest.cases[4].results.back().boundary_case_id = "R03";
    manifest.cases[4].results.back().payload_instance_id =
        manifest.cases[4].payload_instance_id;
    HistoricalEvidence evidence;
    evidence.source_case_id = "B06";
    evidence.config_id = manifest.cases[3].config_id;
    evidence.exact = true;
    manifest.historical_evidence.push_back(evidence);
    const auto path = root / "manifest.json";
    write_manifest_atomic(manifest, path);
    const auto loaded = read_manifest(path);
    EXPECT_EQ(loaded.schema_version, 5);
    EXPECT_EQ(loaded.source_experiment_id, "BOUNDARY");
    ASSERT_EQ(loaded.historical_evidence.size(), 1U);
    EXPECT_EQ(loaded.cases[3].config_id, loaded.cases[4].config_id);
    EXPECT_NE(loaded.cases[3].payload_instance_id,
              loaded.cases[4].payload_instance_id);
    ASSERT_EQ(loaded.cases[3].results.size(), 1U);
    ASSERT_EQ(loaded.cases[4].results.size(), 1U);
    EXPECT_EQ(loaded.cases[3].results[0].boundary_case_id, "R02");
    EXPECT_EQ(loaded.cases[4].results[0].boundary_case_id, "R03");
    EXPECT_NE(loaded.cases[3].results[0].payload_instance_id,
              loaded.cases[4].results[0].payload_instance_id);
    std::filesystem::remove_all(root);
}

TEST(OneBitManifestReplay, SanitizedMultiPayloadEvidenceBracketsFourToSeven) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
        "fixtures" / "onebit_multi_payload_manifest.json";
    const auto manifest = read_manifest(fixture);
    const auto inferred = infer_onebit_geometry(manifest);
    ASSERT_EQ(inferred.cases.size(), 6U);
    EXPECT_EQ(inferred.cases[3].case_id, "R02");
    EXPECT_EQ(inferred.cases[4].case_id, "R03");
    EXPECT_EQ(inferred.cases[3].exact_pass_count, 1U);
    EXPECT_EQ(inferred.cases[4].exact_pass_count, 1U);
    EXPECT_EQ(inferred.four_x_state,
              "Verified across session and payload");
    EXPECT_EQ(inferred.status, "Bracketed");
    EXPECT_EQ(inferred.boundary_bracket,
              "4.00x <= 1-bit geometry boundary < 7.11x");
    EXPECT_EQ(inferred.safe_candidate, "538F2B009FAB");
    const auto four = std::find_if(inferred.densities.begin(),
        inferred.densities.end(), [](const GeometryDensityResult &density) {
            return density.block_size == 4;
        });
    ASSERT_NE(four, inferred.densities.end());
    EXPECT_EQ(four->historical_passes, 1U);
    EXPECT_EQ(four->current_passes, 2U);
    EXPECT_EQ(four->failures, 0U);
}

TEST(OneBitReports, MarkdownJsonAndCsvShareCentralInference) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
        "fixtures" / "onebit_multi_payload_manifest.json";
    const auto root = unique_temp("onebit-consistency");
    const auto manifest = read_manifest(fixture);
    write_reports(manifest, root);
    const auto read_all = [](const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    };
    const auto markdown = read_all(root / "onebit_report.md");
    const auto summary = read_all(root / "onebit_summary.json");
    const auto geometry = read_all(root / "onebit_geometry.csv");
    const auto cases = read_all(root / "onebit_cases.csv");
    EXPECT_NE(markdown.find(
        "4.00x <= 1-bit geometry boundary < 7.11x"), std::string::npos);
    EXPECT_NE(markdown.find(
        "Combined state: Verified across session and payload"),
        std::string::npos);
    EXPECT_NE(summary.find(
        "\"four_x_state\": \"Verified across session and payload\""),
        std::string::npos);
    EXPECT_NE(summary.find(
        "\"safe_candidate\": \"538F2B009FAB\""), std::string::npos);
    EXPECT_NE(geometry.find(
        "Verified across session and payload"), std::string::npos);
    EXPECT_NE(geometry.find(
        "4.00x <= 1-bit geometry boundary < 7.11x"), std::string::npos);
    EXPECT_NE(cases.find("R02,538F2B009FAB,same0001"),
              std::string::npos);
    EXPECT_NE(cases.find("R03,538F2B009FAB,indp0001"),
              std::string::npos);
    std::filesystem::remove_all(root);
}
