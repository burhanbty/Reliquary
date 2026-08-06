#include "video_set.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>

namespace {

class VideoSetTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
            ("vidstorex-video-set-test-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path make_file(const std::string &name,
                                    const std::size_t bytes) const {
        const auto path = root / name;
        std::ofstream out(path, std::ios::binary);
        for (std::size_t i = 0; i < bytes; ++i)
            out.put(static_cast<char>((i * 131 + 17) & 0xff));
        return path;
    }
    video_set::SetPlan make_plan(const std::filesystem::path &source,
                                 const uint64_t chunk = 1024) const {
        video_set::PlanOptions options;
        video_set::Id128 id{};
        id[15] = std::byte{0x42};
        options.deterministic_set_id = id;
        options.maximum_actual_video_size_bytes = 0;
        options.target_duration_seconds = 600;
        options.forced_chunk_payload_bytes = chunk;
        auto plan = video_set::plan_file(source, options);
        video_set::populate_chunk_hashes(source, plan);
        return plan;
    }
    std::filesystem::path root;
};

TEST_F(VideoSetTest, EnvelopeV1RoundTripUsesLittleEndianAndChecksum) {
    const auto source = make_file("sample.bin", 2049);
    const auto plan = make_plan(source);
    const auto expected = video_set::envelope_for_part(plan, plan.parts[1]);
    const auto bytes = video_set::serialize_envelope(expected);
    ASSERT_GE(bytes.size(), 208u);
    EXPECT_EQ(std::to_integer<unsigned>(bytes[8]), 1u);
    EXPECT_EQ(std::to_integer<unsigned>(bytes[9]), 0u);
    const auto parsed = video_set::parse_envelope(bytes);
    ASSERT_EQ(parsed.kind, video_set::ParseKind::Valid) << parsed.error;
    EXPECT_EQ(parsed.envelope.set_id, expected.set_id);
    EXPECT_EQ(parsed.envelope.part_index, 1u);
    EXPECT_EQ(parsed.envelope.chunk_offset, 1024u);
    EXPECT_EQ(parsed.envelope.chunk_sha256, expected.chunk_sha256);
    EXPECT_EQ(parsed.envelope.descriptor_hash, expected.descriptor_hash);
}

TEST_F(VideoSetTest, OldPayloadIsNotMistakenForVideoSet) {
    const std::array<std::byte, 16> old{};
    EXPECT_EQ(video_set::parse_envelope(old).kind,
              video_set::ParseKind::NotVideoSet);
}

TEST_F(VideoSetTest, TruncatedAndCorruptHeadersAreRejected) {
    const auto source = make_file("sample.bin", 3);
    const auto plan = make_plan(source);
    auto bytes = video_set::serialize_envelope(
        video_set::envelope_for_part(plan, plan.parts[0]));
    EXPECT_EQ(video_set::parse_envelope(
        std::span(bytes).first(20)).kind, video_set::ParseKind::Invalid);
    bytes[80] ^= std::byte{1};
    const auto corrupt = video_set::parse_envelope(bytes);
    EXPECT_EQ(corrupt.kind, video_set::ParseKind::Invalid);
    EXPECT_NE(corrupt.error.find("checksum"), std::string::npos);
}

TEST_F(VideoSetTest, FutureVersionAndInvalidHeaderLengthAreRejected) {
    const auto source = make_file("sample.bin", 3);
    const auto plan = make_plan(source);
    auto bytes = video_set::serialize_envelope(
        video_set::envelope_for_part(plan, plan.parts[0]));
    bytes[8] = std::byte{2};
    EXPECT_EQ(video_set::parse_envelope(bytes).kind,
              video_set::ParseKind::Invalid);
    bytes = video_set::serialize_envelope(
        video_set::envelope_for_part(plan, plan.parts[0]));
    bytes[10] = std::byte{1}; bytes[11] = std::byte{0};
    EXPECT_EQ(video_set::parse_envelope(bytes).kind,
              video_set::ParseKind::Invalid);
}

TEST_F(VideoSetTest, EnvelopeRejectsInvalidPartAndOverflowingRange) {
    const auto source = make_file("sample.bin", 3);
    const auto plan = make_plan(source);
    auto envelope = video_set::envelope_for_part(plan, plan.parts[0]);
    envelope.part_count = 0;
    EXPECT_THROW((void) video_set::serialize_envelope(envelope), std::invalid_argument);
    envelope.part_count = 1;
    envelope.part_index = 1;
    EXPECT_THROW((void) video_set::serialize_envelope(envelope), std::invalid_argument);
    envelope.part_index = 0;
    envelope.chunk_offset = std::numeric_limits<uint64_t>::max();
    EXPECT_THROW((void) video_set::serialize_envelope(envelope), std::invalid_argument);
}

TEST_F(VideoSetTest, PlannerCoversSourceExactlyWithoutGapOrOverlap) {
    const auto source = make_file("large.bin", 4097);
    const auto plan = make_plan(source, 1024);
    ASSERT_EQ(plan.parts.size(), 5u);
    uint64_t next = 0;
    for (const auto &part : plan.parts) {
        EXPECT_EQ(part.chunk_offset, next);
        next += part.chunk_size;
    }
    EXPECT_EQ(next, 4097u);
    EXPECT_EQ(plan.parts.back().chunk_size, 1u);
}

TEST_F(VideoSetTest, PlannerSupportsOneByteAndOnePartSet) {
    const auto source = make_file("one.bin", 1);
    const auto plan = make_plan(source, 1024);
    ASSERT_EQ(plan.parts.size(), 1u);
    EXPECT_EQ(plan.parts[0].chunk_size, 1u);
}

TEST_F(VideoSetTest, PlannerSupportsEmptyMetadataOnlyPart) {
    const auto source = make_file("empty.bin", 0);
    const auto plan = make_plan(source, 1024);
    ASSERT_EQ(plan.parts.size(), 1u);
    EXPECT_EQ(plan.parts[0].chunk_offset, 0u);
    EXPECT_EQ(plan.parts[0].chunk_size, 0u);
}

TEST_F(VideoSetTest, InjectedSetAndPartIdsAreDeterministic) {
    const auto source = make_file("stable.bin", 2048);
    const auto first = make_plan(source);
    const auto second = make_plan(source);
    EXPECT_EQ(first.set_id, second.set_id);
    ASSERT_EQ(first.parts.size(), second.parts.size());
    EXPECT_EQ(first.descriptor_hash, second.descriptor_hash);
    EXPECT_EQ(first.parts[0].part_id, second.parts[0].part_id);
}

TEST_F(VideoSetTest, LogicalPayloadStreamsExactRangeAndVerifies) {
    const auto source = make_file("range.bin", 2500);
    const auto plan = make_plan(source, 1000);
    const auto payload = root / "logical.payload";
    video_set::write_logical_payload(source, plan, plan.parts[1], payload);
    video_set::PartEnvelopeV1 envelope;
    std::string error;
    ASSERT_TRUE(video_set::verify_logical_payload(payload, &envelope, &error)) << error;
    EXPECT_EQ(envelope.chunk_offset, 1000u);
    EXPECT_EQ(envelope.chunk_size, 1000u);
}

TEST_F(VideoSetTest, ManifestRoundTripPreservesCoreAndUnknownFieldsAreTolerated) {
    const auto source = make_file("manifest.bin", 2500);
    auto plan = make_plan(source, 1000);
    plan.parts[0].local_encode_state = "Locally verified";
    plan.parts[0].local_decode_state = "Exact";
    const auto path = root / "set_manifest.json";
    video_set::write_manifest_atomic(path, plan);
    std::ifstream initial(path);
    auto text = std::string(std::istreambuf_iterator<char>(initial), {});
    text.insert(text.find('{') + 1, "\n  \"future_optional_field\": {\"x\": true},");
    std::ofstream(path, std::ios::trunc) << text;
    const auto loaded = video_set::read_manifest(path);
    EXPECT_EQ(loaded.set_id, plan.set_id);
    EXPECT_EQ(loaded.descriptor_hash, plan.descriptor_hash);
    EXPECT_EQ(loaded.parts.size(), plan.parts.size());
    EXPECT_EQ(loaded.parts[0].local_decode_state, "Exact");
}

TEST_F(VideoSetTest, ManifestRejectsFutureMajorAndDuplicatePartIndex) {
    const auto source = make_file("manifest.bin", 2500);
    const auto plan = make_plan(source, 1000);
    auto text = video_set::manifest_json(plan);
    auto version = text.find("\"version\": 1");
    text.replace(version, std::string("\"version\": 1").size(), "\"version\": 2");
    const auto future = root / "future.json";
    std::ofstream(future) << text;
    EXPECT_THROW((void) video_set::read_manifest(future), std::runtime_error);

    text = video_set::manifest_json(plan);
    const auto second = text.find("\"part_index\": 1");
    ASSERT_NE(second, std::string::npos);
    text.replace(second, std::string("\"part_index\": 1").size(), "\"part_index\": 0");
    const auto duplicate = root / "duplicate.json";
    std::ofstream(duplicate) << text;
    EXPECT_THROW((void) video_set::read_manifest(duplicate), std::runtime_error);
}

TEST_F(VideoSetTest, ManifestIsAtomicAndBackupProtectsPreviousVersion) {
    const auto source = make_file("manifest.bin", 10);
    auto plan = make_plan(source);
    const auto path = root / "set_manifest.json";
    video_set::write_manifest_atomic(path, plan);
    plan.aggregate_state = "Encoding";
    video_set::write_manifest_atomic(path, plan);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path.string() + ".bak"));
    EXPECT_EQ(video_set::read_manifest(path).aggregate_state, "Encoding");
}

TEST_F(VideoSetTest, FilenameSanitizationBlocksTraversalAndWindowsDevices) {
    EXPECT_EQ(video_set::sanitize_filename("../../secret.bin"), "secret.bin");
    EXPECT_EQ(video_set::sanitize_filename("C:\\temp\\CON.txt"), "_CON.txt");
    EXPECT_EQ(video_set::sanitize_filename(".."), "recovered-file.bin");
}

TEST_F(VideoSetTest, NonAsciiFilenameSurvivesEnvelopeMetadata) {
    const std::string utf8_name = "ar\xC5\x9Fiv.bin";
    const auto source = root / std::filesystem::u8path(utf8_name);
    std::ofstream(source, std::ios::binary).put('x');
    const auto plan = make_plan(source);
    EXPECT_EQ(plan.original_filename, utf8_name);
    const auto bytes = video_set::serialize_envelope(
        video_set::envelope_for_part(plan, plan.parts[0]));
    const auto parsed = video_set::parse_envelope(bytes);
    ASSERT_EQ(parsed.kind, video_set::ParseKind::Valid) << parsed.error;
    EXPECT_EQ(parsed.envelope.original_filename, utf8_name);
}

TEST_F(VideoSetTest, ReportsAndManualHelperContainNoSecretsOrAbsoluteSourcePath) {
    const auto source = make_file("manual.bin", 10);
    const auto plan = make_plan(source);
    const auto set_root = root / "set";
    video_set::write_reports(set_root, plan);
    video_set::write_manual_workflow_files(set_root, plan, 5);
    ASSERT_TRUE(std::filesystem::exists(set_root / "tools" / "download_returned_playlist.ps1"));
    std::ifstream manifest_stream(set_root / "upload_checklist.md");
    const std::string checklist((std::istreambuf_iterator<char>(manifest_stream)), {});
    EXPECT_EQ(checklist.find(root.string()), std::string::npos);
    EXPECT_NE(checklist.find("Unlisted"), std::string::npos);
    std::ifstream helper_stream(set_root / "tools" / "download_returned_playlist.ps1");
    const std::string helper((std::istreambuf_iterator<char>(helper_stream)), {});
    EXPECT_NE(helper.find("%(title)s"), std::string::npos);
    EXPECT_NE(helper.find("yt-dlp was not found"), std::string::npos);
    EXPECT_NE(helper.find("download_returned_playlist.log"), std::string::npos);
}

TEST_F(VideoSetTest, HighCapacityConfigIdAndResilientDefaultRemainStable) {
    EXPECT_EQ(video_set::PlanOptions{}.profile, ReliabilityProfile::Local);
    EXPECT_EQ(reliability_profile_config_id(ReliabilityProfile::HighCapacity),
              "538F2B009FAB");
    const auto &resilient = reliability_profile_definition(ReliabilityProfile::Local);
    EXPECT_EQ(resilient.block_size, 8);
    EXPECT_EQ(resilient.bits_per_symbol, 1);
    EXPECT_DOUBLE_EQ(resilient.repair_percentage, 5.0);
}

TEST_F(VideoSetTest, RealYoutubeValidationMetadataIsExactAndDistinctFromProfileProof) {
    const auto &validation = video_set::kRealYoutubeValidation;
    EXPECT_EQ(validation.validation_type, "real YouTube roundtrip");
    EXPECT_EQ(validation.validation_parts, 4u);
    EXPECT_EQ(validation.exact_parts, 4u);
    EXPECT_TRUE(validation.full_file_sha_exact);
    EXPECT_EQ(validation.source_size_bytes, 33554432u);
    EXPECT_EQ(validation.profile, "high-capacity");
    EXPECT_EQ(validation.config_id, "538F2B009FAB");
    EXPECT_EQ(validation.upload_width, 1920u);
    EXPECT_EQ(validation.upload_height, 1080u);
    EXPECT_EQ(validation.validation_scope, "tested four-part workflow");
    EXPECT_NE(validation.gui_statement.find("Real YouTube"),
              std::string_view::npos);
    EXPECT_NE(validation.gui_statement.find("4/4 parts"),
              std::string_view::npos);
    EXPECT_NE(validation.gui_statement.find("full-file SHA exact"),
              std::string_view::npos);

    const auto &profile = reliability_profile_definition(
        ReliabilityProfile::HighCapacity);
    EXPECT_EQ(profile.validation_cases, 6);
    EXPECT_EQ(profile.exact_passes, 6);
    EXPECT_EQ(profile.upload_sessions, 2);
    EXPECT_EQ(reliability_profile_config_id(
                  ReliabilityProfile::HighCapacity),
              validation.config_id);
    EXPECT_EQ(static_cast<int>(ReliabilityProfile::Local), 0);
}

} // namespace
