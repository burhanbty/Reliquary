/*
 * This file is part of yt-media-storage, a tool for encoding media.
 * Copyright (C) 2026 Brandon Li <https://brandonli.me/>
 */

#include "gui_preflight_model.h"

#include <gtest/gtest.h>

namespace {
GuiPreflightFingerprint fingerprint() {
    return {
        "C:/input.bin", 1024, 100,
        "C:/output.mkv", 0, 0.05, false,
    };
}

GuiPreflightSnapshot ready_snapshot() {
    return {
        true, true, 1, true, false, {}, {},
    };
}
}  // namespace

TEST(GuiPreflightModel, InputAndOutputSelectionCreatesRequest) {
    GuiPreflightModel model;
    const auto generation = model.request(fingerprint());
    ASSERT_TRUE(generation.has_value());
    EXPECT_EQ(*generation, 1U);
    EXPECT_EQ(model.status(), GuiPreflightStatus::Estimating);
}

TEST(GuiPreflightModel, SameOptionsDoNotCreateDuplicateRequest) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    ASSERT_TRUE(model.request(value));
    EXPECT_FALSE(model.request(value));
}

TEST(GuiPreflightModel, OnlyCurrentGenerationIsAccepted) {
    GuiPreflightModel model;
    auto first = fingerprint();
    const uint64_t firstGeneration = *model.request(first);
    auto second = first;
    second.normalized_input_path = "C:/new-input.bin";
    const uint64_t secondGeneration = *model.request(second);

    EXPECT_FALSE(model.accept(
        firstGeneration, first, ready_snapshot()));
    EXPECT_TRUE(model.accept(
        secondGeneration, second, ready_snapshot()));
    EXPECT_TRUE(model.isCurrent(second));
}

TEST(GuiPreflightModel, InputSizeChangeMakesEstimateStale) {
    GuiPreflightModel model;
    auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    ++value.input_size;
    EXPECT_FALSE(model.isCurrent(value));
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::RefreshRequired);
}

TEST(GuiPreflightModel, LastWriteTimeChangeMakesEstimateStale) {
    GuiPreflightModel model;
    auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    ++value.input_last_write_time;
    EXPECT_FALSE(model.isCurrent(value));
}

TEST(GuiPreflightModel, OutputPathChangeMakesEstimateStale) {
    GuiPreflightModel model;
    auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    value.normalized_output_path = "D:/other.mkv";
    EXPECT_FALSE(model.isCurrent(value));
}

TEST(GuiPreflightModel, ReliabilityChangeMakesEstimateStale) {
    GuiPreflightModel model;
    auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    value.repair_ratio = 0.2;
    value.reliability_profile = 1;
    EXPECT_FALSE(model.isCurrent(value));
}

TEST(GuiPreflightModel, EncodingModeChangeMakesEstimateStale) {
    GuiPreflightModel model;
    auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    value.encoding_mode = 1;
    EXPECT_FALSE(model.isCurrent(value));
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::RefreshRequired);
}

TEST(GuiPreflightModel, ReadyEstimateAllowsEncode) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(
        generation, value, ready_snapshot()));
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::Ready);
}

TEST(GuiPreflightModel, KnownLowDiskBlocksEncode) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    auto snapshot = ready_snapshot();
    snapshot.disk_space_sufficient = 0;
    snapshot.can_start_encoding = false;
    snapshot.low_disk_override_permitted = true;
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(generation, value, snapshot));
    EXPECT_EQ(model.status(),
              GuiPreflightStatus::InsufficientDiskSpace);
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::BlockedInsufficientDisk);
}

TEST(GuiPreflightModel, LowDiskOverrideOnlyRemovesDiskBlocker) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    auto snapshot = ready_snapshot();
    snapshot.disk_space_sufficient = 0;
    snapshot.can_start_encoding = false;
    snapshot.low_disk_override_permitted = true;
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(generation, value, snapshot));
    ASSERT_TRUE(model.setLowDiskOverride(true));
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::Ready);

    GuiPreflightModel failed;
    auto invalid = ready_snapshot();
    invalid.error = "input equals output";
    const uint64_t failedGeneration = *failed.request(value);
    ASSERT_TRUE(failed.accept(
        failedGeneration, value, invalid));
    EXPECT_FALSE(failed.setLowDiskOverride(true));
    EXPECT_EQ(failed.eligibility(value),
              GuiEncodeEligibility::Blocked);
}

TEST(GuiPreflightModel, SettingsChangeResetsLowDiskOverride) {
    GuiPreflightModel model;
    auto value = fingerprint();
    auto snapshot = ready_snapshot();
    snapshot.disk_space_sufficient = 0;
    snapshot.can_start_encoding = false;
    snapshot.low_disk_override_permitted = true;
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(generation, value, snapshot));
    ASSERT_TRUE(model.setLowDiskOverride(true));

    value.normalized_output_path = "D:/new.mkv";
    ASSERT_TRUE(model.request(value));
    EXPECT_FALSE(model.lowDiskOverride());
}

TEST(GuiPreflightModel, UnknownDiskRequiresConfirmation) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    auto snapshot = ready_snapshot();
    snapshot.disk_space_known = false;
    snapshot.disk_space_sufficient = -1;
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(generation, value, snapshot));
    EXPECT_EQ(model.status(),
              GuiPreflightStatus::DiskSpaceUnknown);
    EXPECT_EQ(model.eligibility(value),
              GuiEncodeEligibility::ConfirmDiskUnknown);
}

TEST(GuiPreflightModel, UnavailableOutputEstimateRequiresConfirmation) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    auto snapshot = ready_snapshot();
    snapshot.output_size_estimate_available = false;
    snapshot.disk_space_known = false;
    snapshot.disk_space_sufficient = -1;
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.accept(generation, value, snapshot));
    EXPECT_EQ(
        model.status(),
        GuiPreflightStatus::OutputSizeEstimateUnavailable);
    EXPECT_EQ(
        model.eligibility(value),
        GuiEncodeEligibility::ConfirmOutputSizeUnavailable);
}

TEST(GuiPreflightModel, ProbeFailureSafelyBlocksEncode) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    ASSERT_TRUE(model.fail(generation, value));
    EXPECT_EQ(model.status(),
              GuiPreflightStatus::EstimateFailed);
    EXPECT_NE(model.eligibility(value),
              GuiEncodeEligibility::Ready);
}

TEST(GuiPreflightModel, ShutdownIgnoresWorkerResult) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    model.beginShutdown();
    EXPECT_FALSE(model.accept(
        generation, value, ready_snapshot()));
}

TEST(GuiPreflightModel, WaitingStateInvalidatesActiveResult) {
    GuiPreflightModel model;
    const auto value = fingerprint();
    const uint64_t generation = *model.request(value);
    model.waitFor(GuiPreflightStatus::WaitingForOutputPath);
    EXPECT_FALSE(model.accept(
        generation, value, ready_snapshot()));
    EXPECT_EQ(model.status(),
              GuiPreflightStatus::WaitingForOutputPath);
}
