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
    config.block_width = config.block_height = 5;
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
        std::tuple{1920, 1080, 4, 480},
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
    testing::Values(4, 6, 8));

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
