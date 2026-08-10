#include "youtube_api_client.h"
#include "youtube_auth.h"
#include "youtube_sync_controller.h"
#include "youtube_sync_state.h"
#include "youtube_upload_manager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(YouTubeAuth, PkceHasEntropyS256AndRandomState) {
    const auto first = youtube_sync::create_pkce_attempt();
    const auto second = youtube_sync::create_pkce_attempt();
    EXPECT_GE(first.verifier.size(), 43u);
    EXPECT_LE(first.verifier.size(), 128u);
    EXPECT_EQ(first.challenge, youtube_sync::s256_challenge(first.verifier));
    EXPECT_NE(first.verifier, second.verifier);
    EXPECT_NE(first.state, second.state);
}

TEST(YouTubeAuth, CallbackRequiresMatchingStateAndCode) {
    using youtube_sync::parse_oauth_callback;
    EXPECT_TRUE(parse_oauth_callback("/?code=abc&state=good", "good").accepted);
    EXPECT_FALSE(parse_oauth_callback("/?code=abc&state=bad", "good").accepted);
    EXPECT_FALSE(parse_oauth_callback("/?state=good", "good").accepted);
    EXPECT_EQ(parse_oauth_callback("/?error=access_denied&state=good", "good").error,
              "access_denied");
}

TEST(YouTubeAuth, AuthorizationUsesDesktopPkceAndFullYoutubeScope) {
    youtube_sync::OAuthClientConfig config{.client_id="desktop.apps.googleusercontent.com"};
    const auto attempt = youtube_sync::create_pkce_attempt();
    const auto url = youtube_sync::authorization_url(
        config, attempt, "http://127.0.0.1:40000/");
    EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos);
    EXPECT_NE(url.find("auth%2Fyoutube"), std::string::npos);
    EXPECT_NE(url.find("state="), std::string::npos);
}

TEST(YouTubeAuth, RejectsMalformedTokenAndRedactsSecrets) {
    EXPECT_THROW(youtube_sync::parse_token_record("{}"), std::runtime_error);
    const auto redacted = youtube_sync::redact_sensitive(
        "Authorization: Bearer secret-token\nrefresh_token=refresh-secret&x=1");
    EXPECT_EQ(redacted.find("secret-token"), std::string::npos);
    EXPECT_EQ(redacted.find("refresh-secret"), std::string::npos);
}

TEST(YouTubeAuth, ProjectStateNeverUsesQSettingsForTokensAndIgnoresCredentials) {
    const auto source = std::filesystem::path(VIDSTOREX_SOURCE_DIR);
    std::ifstream ui(source / "src" / "drive_manager_ui.cpp");
    const std::string uiText((std::istreambuf_iterator<char>(ui)), {});
    EXPECT_EQ(uiText.find("setValue(\"youtube/access_token"), std::string::npos);
    EXPECT_EQ(uiText.find("setValue(\"youtube/refresh_token"), std::string::npos);
    std::ifstream ignore(source / ".gitignore");
    const std::string ignoreText((std::istreambuf_iterator<char>(ignore)), {});
    EXPECT_NE(ignoreText.find("youtube_oauth_client.json"), std::string::npos);
    EXPECT_NE(ignoreText.find("*.dpapi"), std::string::npos);
}

#ifdef _WIN32
TEST(YouTubeAuth, DpapiCredentialStoreRoundTripsWithoutPlaintext) {
    const auto root = std::filesystem::temp_directory_path() /
        "vidstorex-dpapi-test";
    std::filesystem::create_directories(root);
    auto store = youtube_sync::make_platform_credential_store(root);
    const std::string secret =
        "{\"access_token\":\"access-secret\",\"refresh_token\":\"refresh-secret\"}";
    store->save("test-account", secret);
    ASSERT_EQ(store->load("test-account"), secret);
    std::ifstream in(root / "test-account.dpapi", std::ios::binary);
    const std::string protectedBytes((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(protectedBytes.find("access-secret"), std::string::npos);
    EXPECT_EQ(protectedBytes.find("refresh-secret"), std::string::npos);
    store->remove("test-account");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

TEST(YouTubeMetadata, PrivacyModeNeverSendsSourcePathOrFilename) {
    youtube_sync::VideoMetadata metadata{
        .set_id="29FD5937ABC", .part_index=0, .part_count=4,
        .source_basename="C:\\private\\taxes.zip",
        .privacy=youtube_sync::Privacy::Unlisted,
        .privacy_friendly_titles=true};
    const auto json = youtube_sync::video_metadata_json(metadata);
    EXPECT_EQ(json.find("taxes.zip"), std::string::npos);
    EXPECT_EQ(json.find("private"), std::string::npos);
    EXPECT_NE(json.find("29FD5937"), std::string::npos);
}

TEST(YouTubeUpload, ChunkRange308StatusAndRetryAreSafe) {
    using namespace youtube_sync;
    EXPECT_TRUE(valid_chunk_size(kDefaultUploadChunkBytes));
    EXPECT_FALSE(valid_chunk_size(1000000));
    const auto chunk = next_upload_chunk(0, 20000000);
    EXPECT_EQ(chunk.size(), kDefaultUploadChunkBytes);
    EXPECT_EQ(committed_bytes_from_range("bytes=0-999999"), 1000000u);
    const auto resume = decide_upload_response(308, "bytes=0-999999", "");
    EXPECT_EQ(resume.action, UploadResponseDecision::Action::NextChunk);
    EXPECT_EQ(resume.next_offset, 1000000u);
    EXPECT_EQ(decide_upload_response(503, "", "").action,
              UploadResponseDecision::Action::QueryStatus);
    EXPECT_EQ(decide_upload_response(404, "", "").action,
              UploadResponseDecision::Action::SessionExpired);
    EXPECT_EQ(decide_upload_response(201, "", "{\"id\":\"video1\"}").video_id,
              "video1");
    EXPECT_EQ(exponential_backoff_seconds(20), 64u);
}

TEST(YouTubeApi, BuildsPlaylistAndProcessingRequestsAndClassifiesErrors) {
    using namespace youtube_sync;
    const EndpointSet fake{"http://127.0.0.1:1/youtube/v3",
                           "http://127.0.0.1:1/upload/youtube/v3"};
    EXPECT_NE(create_playlist_request(fake, "token", "Set", Privacy::Unlisted)
                  .url.find("127.0.0.1"), std::string::npos);
    EXPECT_NE(add_playlist_item_request(fake, "token", "p", "v", 2)
                  .body.find("\"position\":2"), std::string::npos);
    EXPECT_NE(processing_status_request(fake, "token", {"v1", "v2"})
                  .url.find("processingDetails"), std::string::npos);
    EXPECT_EQ(classify_api_error(403, "quotaExceeded", "upload").category,
              "UploadQuotaReached");
    EXPECT_TRUE(classify_api_error(503, "backend", "upload").retryable);
    EXPECT_TRUE(upload_quota_warning(101));
    EXPECT_FALSE(upload_quota_warning(100));
}

TEST(YouTubeSync, SidecarIsVersionedAtomicAndRejectsAnotherSet) {
    using namespace youtube_sync;
    const auto root = std::filesystem::temp_directory_path() /
        "vidstorex-youtube-sync-state-test";
    std::filesystem::create_directories(root);
    SyncState state;
    state.set_id = "SET-A";
    state.requested_privacy = Privacy::Unlisted;
    state.actual_privacy = Privacy::Private;
    state.parts.push_back({.part_index=0, .part_id="P0",
        .upload_state=UploadState::Uploaded, .youtube_video_id="video0"});
    write_sync_state_atomic(root / "youtube_sync_state.json", state);
    const auto loaded = read_sync_state(root / "youtube_sync_state.json", "SET-A");
    EXPECT_EQ(loaded.parts.size(), 1u);
    EXPECT_EQ(loaded.actual_privacy, Privacy::Private);
    EXPECT_THROW(read_sync_state(root / "youtube_sync_state.json", "SET-B"),
                 std::invalid_argument);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(YouTubeSync, PrivateActualPrivacyBlocksAutomaticDownload) {
    youtube_sync::SyncState state;
    state.set_id = "SET";
    state.actual_privacy = youtube_sync::Privacy::Private;
    state.parts.push_back({.upload_state=youtube_sync::UploadState::Uploaded,
                           .youtube_video_id="v"});
    youtube_sync::YouTubeSyncController controller;
    controller.load(state);
    EXPECT_EQ(controller.view().phase,
              youtube_sync::SyncPhase::PrivateRestriction);
    EXPECT_TRUE(controller.view().can_use_manual_fallback);
}

TEST(YouTubeSync, ProcessingPollUsesControlledIntervalsAndTimeout) {
    EXPECT_EQ(youtube_sync::processing_poll_delay_seconds(0), 15u);
    EXPECT_EQ(youtube_sync::processing_poll_delay_seconds(2), 15u);
    EXPECT_EQ(youtube_sync::processing_poll_delay_seconds(3), 30u);
    EXPECT_FALSE(youtube_sync::processing_poll_timed_out(1000, 3599999));
    EXPECT_TRUE(youtube_sync::processing_poll_timed_out(1000, 3601000));
}
