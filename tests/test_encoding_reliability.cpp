// This file is part of yt-media-storage, a tool for encoding media.
// Copyright (C) 2026 Brandon Li <https://brandonli.me/>

#include <gtest/gtest.h>

#include "encoding_reliability.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

TEST(RepairCalculation, ZeroPercentProducesNoRepairPackets) {
    EXPECT_EQ(calculate_repair_packet_count(100, 0.0), 0u);
}

TEST(RepairCalculation, FivePercentProducesFiveRepairPackets) {
    EXPECT_EQ(calculate_repair_packet_count(100, 0.05), 5u);
}

TEST(RepairCalculation, TwentyPercentProducesTwentyRepairPackets) {
    EXPECT_EQ(calculate_repair_packet_count(100, 0.20), 20u);
}

TEST(RepairCalculation, FiftyPercentProducesFiftyRepairPackets) {
    EXPECT_EQ(calculate_repair_packet_count(100, 0.50), 50u);
}

TEST(RepairCalculation, FiveHundredPercentProducesFiveHundredRepairPackets) {
    EXPECT_EQ(calculate_repair_packet_count(100, 5.0), 500u);
}

TEST(RepairCalculation, SmallSourceCountUsesCeil) {
    EXPECT_EQ(calculate_repair_packet_count(1, 0.05), 1u);
}

TEST(RepairCalculation, DecimalPercentageUsesCeil) {
    const double ratio = repair_percentage_to_ratio(12.5);
    EXPECT_DOUBLE_EQ(ratio, 0.125);
    EXPECT_EQ(calculate_repair_packet_count(100, ratio), 13u);
}

TEST(RepairCalculation, RejectsNegativePercentage) {
    EXPECT_THROW(
        (void) repair_percentage_to_ratio(-0.01),
        std::invalid_argument);
}

TEST(RepairCalculation, RejectsNanAndInfinity) {
    EXPECT_THROW(
        (void) repair_percentage_to_ratio(
            std::numeric_limits<double>::quiet_NaN()),
        std::invalid_argument);
    EXPECT_THROW(
        (void) repair_percentage_to_ratio(
            std::numeric_limits<double>::infinity()),
        std::invalid_argument);
}

TEST(RepairCalculation, RejectsPercentageAboveMaximum) {
    EXPECT_THROW(
        (void) repair_percentage_to_ratio(
            MAX_REPAIR_PERCENTAGE + 0.01),
        std::invalid_argument);
}

TEST(RepairCalculation, LargeCountsAreCheckedForOverflow) {
    const uint64_t large = std::numeric_limits<uint64_t>::max() / 10;
    EXPECT_NO_THROW(
        (void) calculate_repair_packet_count(large, 0.5));
    EXPECT_THROW(
        (void) calculate_repair_packet_count(
            std::numeric_limits<uint64_t>::max(), 5.0),
        std::overflow_error);
}

TEST(ReliabilityProfiles, LocalIsFivePercent) {
    EXPECT_DOUBLE_EQ(
        reliability_options_for_profile(
            ReliabilityProfile::Local).repair_ratio,
        0.05);
}

TEST(ReliabilityProfiles, BalancedIsTwentyPercent) {
    EXPECT_DOUBLE_EQ(
        reliability_options_for_profile(
            ReliabilityProfile::Balanced).repair_ratio,
        0.20);
}

TEST(ReliabilityProfiles, DurableIsFiftyPercent) {
    EXPECT_DOUBLE_EQ(
        reliability_options_for_profile(
            ReliabilityProfile::Durable).repair_ratio,
        0.50);
}

TEST(ReliabilityProfiles, ExplicitPercentageOverridesProfile) {
    const auto options = resolve_reliability_options(
        ReliabilityProfile::Durable, 7.5);
    EXPECT_DOUBLE_EQ(options.repair_ratio, 0.075);
}

TEST(CLIRepairOptions, FivePercentConvertsToPointZeroFive) {
    const double percentage = parse_repair_percentage("5");
    const auto options = resolve_reliability_options(
        std::nullopt, percentage);
    EXPECT_DOUBLE_EQ(options.repair_ratio, 0.05);
}

TEST(CLIRepairOptions, InvalidValuesProduceClearValidationErrors) {
    EXPECT_THROW(
        (void) parse_repair_percentage("-1"), std::invalid_argument);
    EXPECT_THROW(
        (void) parse_repair_percentage("nan"), std::invalid_argument);
    EXPECT_THROW(
        (void) parse_repair_percentage("inf"), std::invalid_argument);
    EXPECT_THROW(
        (void) parse_repair_percentage("501"), std::invalid_argument);
    EXPECT_THROW(
        (void) parse_repair_percentage("5percent"), std::invalid_argument);
}

TEST(CLIRepairOptions, DefaultIsFivePercent) {
    const auto options = resolve_reliability_options(
        std::nullopt, std::nullopt);
    EXPECT_DOUBLE_EQ(options.repair_ratio, DEFAULT_REPAIR_RATIO);
}

TEST(ReliabilityEstimate, RegressionFromFiveHundredToFivePercent) {
    constexpr uint64_t input_size = 68'185'385;
    constexpr uint64_t packets_per_frame = 52;
    constexpr uint32_t frames_per_second = 30;

    const auto old_estimate = estimate_encoding_reliability(
        input_size, false, {5.0}, packets_per_frame, frames_per_second);
    const auto new_estimate = estimate_encoding_reliability(
        input_size, false, {0.05}, packets_per_frame, frames_per_second);

    EXPECT_EQ(old_estimate.source_packet_count, 266'350u);
    EXPECT_EQ(old_estimate.repair_packet_count, 1'331'750u);
    EXPECT_EQ(old_estimate.total_packet_count, 1'598'100u);
    EXPECT_EQ(old_estimate.frame_count, 30'733u);

    // Repair is ceiled per chunk, matching Encoder::encode_chunk exactly.
    EXPECT_EQ(new_estimate.source_packet_count, 266'350u);
    EXPECT_EQ(new_estimate.repair_packet_count, 13'331u);
    EXPECT_EQ(new_estimate.total_packet_count, 279'681u);
    EXPECT_EQ(new_estimate.frame_count, 5'379u);
    EXPECT_LT(
        static_cast<double>(new_estimate.frame_count) /
            static_cast<double>(old_estimate.frame_count),
        0.176);
}
