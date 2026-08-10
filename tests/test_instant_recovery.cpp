#include "instant_recovery.h"
#include "video_set_workflow.h"

#include <gtest/gtest.h>

#include <filesystem>

TEST(InstantRecovery, ValidatesPlaylistUrlsWithoutShellParsing) {
    EXPECT_TRUE(video_set_workflow::is_youtube_playlist_url(
        "https://www.youtube.com/playlist?list=PL123&feature=share"));
    EXPECT_TRUE(video_set_workflow::is_youtube_playlist_url(
        "https://m.youtube.com/playlist?feature=share&list=PL_abc-123"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://example.com/playlist?list=PL123"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/watch?v=abc"));
    EXPECT_FALSE(video_set_workflow::is_youtube_playlist_url(
        "https://youtube.com/playlist?list=x\n--exec"));
}

TEST(InstantRecovery, DownloaderKeepsAmpersandInOneArgument) {
    const std::string url =
        "https://youtube.com/playlist?list=PL123&feature=share";
    const auto args = video_set_workflow::ytdlp_arguments(url, "returned");
    ASSERT_FALSE(args.empty());
    EXPECT_EQ(args.back(), url);
    EXPECT_EQ(args[3], video_set_workflow::kYtDlpFormatSelector);
}

TEST(InstantRecovery, AutoSelectsOnlyOneCompleteSet) {
    using namespace instant_recovery;
    const auto selected = select_single_complete_set({
        {.set_id="A", .expected_parts=4, .exact_parts=4, .duplicate_count=1},
        {.set_id="B", .expected_parts=4, .exact_parts=3}});
    ASSERT_EQ(selected.status, SelectionStatus::Selected);
    EXPECT_EQ(selected.set_id, "A");
    EXPECT_TRUE(may_auto_recover(
        {.explicit_auto_recover=true}, false, selected));
    EXPECT_FALSE(may_auto_recover(
        {.explicit_auto_recover=true}, true, selected));
}

TEST(InstantRecovery, StopsForMultipleConflictCorruptOrMissingSets) {
    using namespace instant_recovery;
    EXPECT_EQ(select_single_complete_set({
        {.set_id="A", .expected_parts=2, .exact_parts=2},
        {.set_id="B", .expected_parts=2, .exact_parts=2}}).status,
        SelectionStatus::MultipleRecoverable);
    EXPECT_EQ(select_single_complete_set({
        {.set_id="A", .expected_parts=2, .exact_parts=1},
        {.set_id="B", .expected_parts=2, .exact_parts=2, .conflict_count=1},
        {.set_id="C", .expected_parts=2, .exact_parts=2, .corrupt_count=1}}).status,
        SelectionStatus::NoneRecoverable);
}

TEST(InstantRecovery, JobStateIsAtomicAndRestartable) {
    using namespace instant_recovery;
    const auto root = std::filesystem::temp_directory_path() /
        ("vidstorex-job-test-" + make_job_id());
    initialize_job_directories(root);
    JobState state;
    state.job_id = make_job_id();
    state.playlist_url = "https://youtube.com/playlist?list=PL123&x=1";
    state.output_directory = "output";
    state.selected_set_id = "ABC";
    state.phase = Phase::Recovering;
    write_job_state_atomic(root / "job_state.json", state);
    const auto loaded = read_job_state(root / "job_state.json");
    EXPECT_EQ(loaded.job_id, state.job_id);
    EXPECT_EQ(loaded.playlist_url, state.playlist_url);
    EXPECT_EQ(loaded.phase, Phase::Recovering);
    EXPECT_TRUE(std::filesystem::is_directory(root / "returned"));
    EXPECT_TRUE(std::filesystem::is_directory(root / "logs"));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
