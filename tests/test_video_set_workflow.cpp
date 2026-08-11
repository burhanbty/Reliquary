#include "video_set_workflow.h"

#include <gtest/gtest.h>

namespace {

video_set::SetPlan sample_plan(uint32_t parts = 4) {
    video_set::SetPlan plan;
    plan.original_filename = "archive.bin";
    plan.original_file_size = 4096;
    plan.profile = ReliabilityProfile::HighCapacity;
    plan.profile_name = "high-capacity";
    plan.config_id = "538F2B009FAB";
    plan.parts.resize(parts);
    for (uint32_t index = 0; index < parts; ++index) {
        auto &part = plan.parts[index];
        part.part_index = index;
        part.chunk_size = 1024;
        part.estimated_duration_seconds = 2.0;
        part.estimated_output_bytes = 2048;
    }
    return plan;
}

TEST(VideoSetWorkflow, CreateStartsAtSourceAndResilientIsDefault) {
    video_set_workflow::Controller controller;
    EXPECT_EQ(controller.view().state, video_set_workflow::State::Welcome);
    controller.choose_create();
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::SourceRequired);
    EXPECT_EQ(controller.view().selected_profile, "resilient");
    controller.select_source("archive.bin", 4096);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::ReadyToPlan);
}

TEST(VideoSetWorkflow, RecoverStartsAtReturnedVideoSelection) {
    video_set_workflow::Controller controller;
    controller.choose_recover();
    EXPECT_EQ(controller.view().path, video_set_workflow::Path::Recover);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::AwaitingReturnedVideos);
}

TEST(VideoSetWorkflow, PlanningEncodingAndLocalVerificationStayLocal) {
    video_set_workflow::Controller controller;
    controller.choose_create();
    controller.select_source("archive.bin", 4096);
    controller.begin_planning();
    EXPECT_EQ(controller.view().state, video_set_workflow::State::Planning);
    controller.apply_plan(sample_plan());
    EXPECT_EQ(controller.view().state, video_set_workflow::State::Planned);
    controller.begin_encoding();
    controller.apply_local_verification(4);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::LocallyVerified);
    EXPECT_FALSE(controller.view().final_sha_exact);
    EXPECT_NE(controller.view().primary_message.find("locally"),
              std::string::npos);
}

TEST(VideoSetWorkflow, CancelledEncodeCanResume) {
    video_set_workflow::Controller controller;
    controller.choose_create();
    controller.select_source("archive.bin", 4096);
    controller.apply_plan(sample_plan());
    controller.begin_encoding();
    controller.cancel_encoding();
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::EncodingPaused);
    EXPECT_TRUE(controller.view().can_resume);
}

TEST(VideoSetWorkflow, ScanPrecedenceSeparatesMissingCorruptAndConflict) {
    video_set_workflow::Controller controller;
    controller.choose_recover();
    video_set_workflow::ScanSummary summary;
    summary.expected_parts = 4;
    summary.returned_parts = 3;
    summary.exact_parts = 3;
    summary.missing_parts = {2};
    controller.apply_scan(summary);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::IncompleteMissingParts);
    EXPECT_FALSE(controller.view().can_recover);

    summary.missing_parts.clear();
    summary.corrupt_parts = {2};
    controller.apply_scan(summary);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::CorruptPartsDetected);

    summary.conflict_count = 1;
    controller.apply_scan(summary);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::ConflictDetected);
}

TEST(VideoSetWorkflow, CompleteScanAllowsRecoveryAndOnlyExactShaSucceeds) {
    video_set_workflow::Controller controller;
    controller.choose_recover();
    video_set_workflow::ScanSummary summary;
    summary.expected_parts = 4;
    summary.returned_parts = 4;
    summary.exact_parts = 4;
    summary.duplicate_count = 1;
    controller.apply_scan(summary);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::ReadyToRecover);
    EXPECT_TRUE(controller.view().can_recover);
    controller.begin_recovery();
    controller.apply_recovery_result("recovered/archive.bin", false);
    EXPECT_EQ(controller.view().state, video_set_workflow::State::Failed);
    EXPECT_FALSE(controller.view().final_sha_exact);

    controller.apply_scan(summary);
    controller.begin_recovery();
    controller.apply_recovery_result("recovered/archive.bin", true);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::RecoveredExact);
    EXPECT_TRUE(controller.view().final_sha_exact);
}

TEST(VideoSetWorkflow, ResumeManifestInfersAwaitingUploadOrPausedEncoding) {
    auto plan = sample_plan();
    for (auto &part : plan.parts) part.local_decode_state = "Exact";
    video_set_workflow::Controller controller;
    controller.resume_from_manifest(plan);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::AwaitingUpload);

    plan.parts.back().local_decode_state = "Pending";
    controller.resume_from_manifest(plan);
    EXPECT_EQ(controller.view().state,
              video_set_workflow::State::EncodingPaused);
}

TEST(VideoSetWorkflow, PlanSummaryUsesBackendPlanValues) {
    const auto summary = video_set_workflow::summarize_plan(sample_plan(3));
    EXPECT_EQ(summary.part_count, 3u);
    EXPECT_DOUBLE_EQ(summary.maximum_part_duration_seconds, 2.0);
    EXPECT_DOUBLE_EQ(summary.total_duration_seconds, 6.0);
    EXPECT_EQ(summary.total_estimated_output_bytes, 6144u);
    EXPECT_EQ(summary.temporary_disk_bytes, 3072u);
    EXPECT_EQ(summary.recovery_disk_bytes, 4096u);
}

TEST(VideoSetWorkflow, YtDlpArgumentsAreShellFreeAndPreserveUrlAndTemplate) {
    const std::string url =
        "https://www.youtube.com/playlist?list=PL123&feature=shared";
    const auto args = video_set_workflow::ytdlp_arguments(
        url, std::filesystem::path("C:/Returned Videos"));
    ASSERT_EQ(args.size(), 7u);
    EXPECT_EQ(args[3], "bv*[height=1080]/bv*[height<=1080]");
    EXPECT_NE(args[5].find("%(title)s [%(id)s].%(ext)s"),
              std::string::npos);
    EXPECT_EQ(args.back(), url);
    for (const auto &argument : args) {
        EXPECT_EQ(argument.find("--cookies"), std::string::npos);
        EXPECT_EQ(argument.find("powershell"), std::string::npos);
        EXPECT_EQ(argument.find("cmd.exe"), std::string::npos);
    }
}

TEST(VideoSetWorkflow, PlaylistValidationRejectsInjectionAndNonPlaylistUrls) {
    EXPECT_TRUE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/playlist?list=PL123"));
    EXPECT_TRUE(video_set_workflow::is_youtube_playlist_url(
        "https://music.youtube.com/playlist?feature=share&list=PL123"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/watch?v=abc"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/playlist?list=PL123\ncalc.exe"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://example.com/?next=youtube.com/playlist&list=PL123"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://notyoutube.com/playlist?list=PL123"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/playlist?list="));
}

TEST(VideoSetWorkflow, YtDlpDetectionUsesPathToolsThenSavedSelection) {
    const auto executable = [](const std::string_view value) {
        return value == "C:/Program Files/yt-dlp.exe" ||
               value == "C:/Saved Tools/yt-dlp.exe";
    };
    EXPECT_EQ(video_set_workflow::select_ytdlp_executable(
                  "C:/Program Files/yt-dlp.exe",
                  {"C:/App/tools/yt-dlp.exe"},
                  "C:/Saved Tools/yt-dlp.exe", executable),
              "C:/Program Files/yt-dlp.exe");
    EXPECT_EQ(video_set_workflow::select_ytdlp_executable(
                  {}, {"C:/Missing/yt-dlp.exe"},
                  "C:/Saved Tools/yt-dlp.exe", executable),
              "C:/Saved Tools/yt-dlp.exe");
}

TEST(VideoSetWorkflow, YtDlpProgressParsingIsOptionalAndNonFatal) {
    const auto parsed = video_set_workflow::parse_ytdlp_progress(
        "[download] Downloading item 2 of 4\n"
        "[download] Destination: returned/part 2.mkv\n"
        "[download] 37.5% of 20MiB at 2.5MiB/s ETA 00:05");
    ASSERT_TRUE(parsed.percent.has_value());
    EXPECT_DOUBLE_EQ(*parsed.percent, 37.5);
    EXPECT_EQ(parsed.current_item, 2u);
    EXPECT_EQ(parsed.total_items, 4u);
    ASSERT_TRUE(parsed.total_bytes.has_value());
    EXPECT_EQ(*parsed.total_bytes, 20u * 1024u * 1024u);
    ASSERT_TRUE(parsed.downloaded_bytes.has_value());
    EXPECT_EQ(*parsed.downloaded_bytes, 7864320u);
    ASSERT_TRUE(parsed.speed_bytes_per_second.has_value());
    EXPECT_DOUBLE_EQ(*parsed.speed_bytes_per_second, 2621440.0);
    EXPECT_EQ(parsed.eta_seconds, 5u);
    EXPECT_EQ(parsed.destination_filename, "returned/part 2.mkv");
    const auto unknown = video_set_workflow::parse_ytdlp_progress(
        "yt-dlp output changed but the process can continue");
    EXPECT_FALSE(unknown.percent.has_value());
    EXPECT_FALSE(unknown.current_item.has_value());
}

TEST(VideoSetWorkflow, OperationProgressStartsIndeterminateAndRejectsStaleEvents) {
    using namespace video_set_workflow;
    OperationProgressModel model;
    EXPECT_EQ(model.view().state, OperationState::Idle);
    const auto first = model.begin(OperationType::Scan,
        OperationPhase::DiscoveringFiles, 1000, "Scanning", "Discovering");
    EXPECT_TRUE(model.view().is_busy);
    EXPECT_FALSE(model.view().progress_percent.has_value());
    const auto second = model.begin(OperationType::Recover,
        OperationPhase::Preparing, 2000, "Recovering");
    EXPECT_GT(second, first);
    OperationEvent stale;
    stale.operation_id = first;
    stale.operation_type = OperationType::Scan;
    stale.phase = OperationPhase::Completed;
    EXPECT_FALSE(model.apply(stale, 2100));
    EXPECT_EQ(model.view().operation_id, second);
    EXPECT_EQ(model.view().operation_type, OperationType::Recover);
}

TEST(VideoSetWorkflow, OperationProgressIsDeterminateOnlyWithAnHonestTotal) {
    using namespace video_set_workflow;
    OperationProgressModel model;
    const auto id = model.begin(OperationType::Scan,
        OperationPhase::ReadingMetadata, 1000, "Scanning");
    OperationEvent event;
    event.operation_id = id;
    event.operation_type = OperationType::Scan;
    event.phase = OperationPhase::ReadingMetadata;
    event.progress_is_determinate = true;
    event.progress_current = 10;
    event.progress_total = 0;
    EXPECT_TRUE(model.apply(event, 2000));
    EXPECT_FALSE(model.view().progress_percent.has_value());
    event.progress_current = 12;
    event.progress_total = 10;
    EXPECT_TRUE(model.apply(event, 3000));
    ASSERT_TRUE(model.view().progress_percent.has_value());
    EXPECT_DOUBLE_EQ(*model.view().progress_percent, 100.0);
    EXPECT_EQ(model.view().progress_current, 10u);
}

TEST(VideoSetWorkflow, OperationProgressWatchdogCancelAndTerminalStatesAreSafe) {
    using namespace video_set_workflow;
    OperationProgressModel model;
    const auto id = model.begin(OperationType::Encode,
        OperationPhase::EncodingPart, 1000, "Encoding");
    EXPECT_TRUE(model.tick(31000));
    EXPECT_TRUE(model.view().taking_longer_than_usual);
    EXPECT_EQ(model.view().state, OperationState::Running);
    EXPECT_TRUE(model.request_cancel(32000));
    EXPECT_TRUE(model.request_cancel(33000));
    EXPECT_EQ(model.view().state, OperationState::Cancelling);
    EXPECT_EQ(model.view().primary_message, "Cancelling...");
    EXPECT_TRUE(model.cancel(id, 34000));
    EXPECT_TRUE(model.view().can_retry);
    EXPECT_FALSE(model.view().is_busy);
    EXPECT_FALSE(model.apply(OperationEvent{.operation_id = id}, 35000));
}

TEST(VideoSetWorkflow, StructuredProgressParsingIsSafeAndForwardCompatible) {
    using namespace video_set_workflow;
    const auto parsed = parse_progress_jsonl(
        R"({"type":"progress","operation_id":7,"operation":"scan","phase":"reading_metadata","message":"Dosyalar taranıyor","current_item":"parça ü.mkv","current":2,"total":4,"completed":1,"progress_current":2,"progress_total":4,"determinate":true,"candidates":4,"checked":2,"verified":1,"corrupt":1,"unknown_future_field":"ok"})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->operation_id, 7u);
    EXPECT_EQ(parsed->operation_type, OperationType::Scan);
    EXPECT_EQ(parsed->phase, OperationPhase::ReadingMetadata);
    EXPECT_EQ(parsed->current_item_name, "parça ü.mkv");
    EXPECT_EQ(parsed->progress_total, 4u);
    EXPECT_TRUE(parsed->has_scan_summary);
    EXPECT_EQ(parsed->scan.exact_parts, 1u);
    EXPECT_EQ(parsed->scan.corrupt_parts.size(), 1u);
    EXPECT_FALSE(parse_progress_jsonl("not json").has_value());
    EXPECT_FALSE(parse_progress_jsonl(
        R"({"type":"progress","operation_id":0,"operation":"scan"})").has_value());
}

TEST(VideoSetWorkflow, StructuredResultCarriesRecoveryProof) {
    using namespace video_set_workflow;
    const auto parsed = parse_progress_jsonl(
        R"({"type":"result","operation_id":9,"operation":"recover","status":"recovered_exact","sha256":"2FAD","output_path":"C:/out/archive.bin","file_name":"archive.bin","file_size":1048576,"profile_name":"high-capacity","part_count":4})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->state, OperationState::Completed);
    EXPECT_EQ(parsed->phase, OperationPhase::Completed);
    EXPECT_EQ(parsed->status, "recovered_exact");
    EXPECT_EQ(parsed->sha256, "2FAD");
    EXPECT_EQ(parsed->output_path, "C:/out/archive.bin");
    EXPECT_EQ(parsed->file_name, "archive.bin");
    EXPECT_EQ(parsed->file_size, 1048576u);
    EXPECT_EQ(parsed->profile_name, "high-capacity");
    EXPECT_EQ(parsed->part_count, 4u);
}

TEST(VideoSetWorkflow, StructuredErrorCarriesRetryGuidanceAndExitCode) {
    using namespace video_set_workflow;
    const auto parsed = parse_progress_jsonl(
        R"({"type":"error","operation_id":11,"operation":"encode","phase":"failed","message":"disk full","suggested_action":"Free disk space and retry.","can_retry":true,"exit_code":5})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->state, OperationState::Failed);
    EXPECT_EQ(parsed->phase, OperationPhase::Failed);
    EXPECT_TRUE(parsed->can_retry);
    EXPECT_EQ(parsed->backend_exit_code, 5);
    EXPECT_EQ(parsed->suggested_action, "Free disk space and retry.");
}

TEST(VideoSetWorkflow, RecentSetsDeduplicateAndRemainBounded) {
    std::vector<video_set_workflow::RecentSet> recent;
    for (int index = 0; index < 7; ++index) {
        recent = video_set_workflow::add_recent_set(
            std::move(recent),
            {"manifest-" + std::to_string(index),
             "set-" + std::to_string(index), "Planned", 4, index});
    }
    ASSERT_EQ(recent.size(), 5u);
    EXPECT_EQ(recent.front().manifest_path, "manifest-6");
    recent = video_set_workflow::add_recent_set(
        std::move(recent),
        {"manifest-4", "updated", "Recovered exact", 4, 99});
    ASSERT_EQ(recent.size(), 5u);
    EXPECT_EQ(recent.front().display_name, "updated");
}

TEST(VideoSetWorkflow, HighCapacityMetadataRemainsOptInAndDistinct) {
    video_set_workflow::Controller controller;
    controller.choose_create();
    EXPECT_EQ(controller.view().selected_profile, "resilient");
    controller.select_profile("high-capacity", "538F2B009FAB");
    EXPECT_EQ(controller.view().selected_profile, "high-capacity");
    EXPECT_EQ(controller.view().config_id, "538F2B009FAB");
    EXPECT_EQ(video_set::kRealYoutubeValidation.validation_parts, 4u);
    EXPECT_EQ(reliability_profile_definition(
                  ReliabilityProfile::HighCapacity).validation_cases, 6);
}

} // namespace
