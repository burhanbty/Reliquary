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
