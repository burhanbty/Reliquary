# YouTube Sync setup

YouTube Sync is an optional beta feature. Instant Playlist Recovery and the
manual Video Set upload/download workflow do not require Google API
configuration and continue to work when Sync is unavailable.

## Configure a Desktop OAuth client

1. Create or select a project in Google Cloud Console.
2. Enable **YouTube Data API v3** for that project.
3. Configure the OAuth consent screen, its users, and the required publishing
   or verification state for your intended audience.
4. Create an OAuth client of type **Desktop app**.
5. Download the client JSON to a private local location. In VidStoreX Settings,
   choose that file under **YouTube Sync (Beta)**.
6. Never commit the downloaded file, its client values, tokens, DPAPI blobs, or
   generated `youtube_sync_state.json` files. The repository contains only
   `config/youtube_oauth_client.example.json`.

VidStoreX uses the installed-app Authorization Code flow, an ephemeral
`127.0.0.1` callback listener, PKCE S256, and a random CSRF `state`. It requests
`https://www.googleapis.com/auth/youtube` because automatic sync must upload
videos and also create/manage the playlist made for the set. The system browser
is used for consent. Credentials are protected for the current Windows user by
Windows DPAPI; access and refresh tokens are never stored in QSettings.

## Upload privacy and project audit

Google states that videos uploaded through `videos.insert` by unverified API
projects created after 28 July 2020 are restricted to **Private** viewing until
the project passes the required audit. VidStoreX records both requested and
actual privacy and will not call a Private upload “Unlisted.” It also does not
offer cookie, password, or browser-session workarounds. Complete the official
YouTube API audit/compliance process if the restriction must be lifted.

## Quota

The current default `videos.insert` bucket permits 100 upload calls per day.
Playlist creation and playlist item insertion currently cost 50 units each
from the general daily allocation, while `videos.list` costs 1 unit. VidStoreX
shows the 100-part threshold only as information: a project may have a different
quota. Quota errors preserve local sidecar state so Sync can be resumed later.

## Manual fallback

At any time you can use the existing workflow: open the generated videos,
upload them manually, wait for 1080p processing, create a playlist, paste its
URL into VidStoreX, download with yt-dlp, scan embedded metadata, and recover.
Cancelling Sync does not delete already uploaded videos or playlists.

Official references:

- https://developers.google.com/identity/protocols/oauth2/native-app
- https://developers.google.com/youtube/v3/guides/using_resumable_upload_protocol
- https://developers.google.com/youtube/v3/docs/videos/insert
- https://developers.google.com/youtube/v3/docs/playlists/insert
- https://developers.google.com/youtube/v3/docs/playlistItems/insert
- https://developers.google.com/youtube/v3/docs/videos/list
- https://developers.google.com/youtube/v3/determine_quota_cost
