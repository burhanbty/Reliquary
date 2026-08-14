# Next steps

1. Upload the videos in batches as Unlisted.
2. Wait until every video has finished 1080p processing.
3. Add the videos to a playlist.
4. Run `tools/download_returned_playlist.ps1` with the playlist URL.
5. Keep downloads in `returned/`.
6. Run `media_storage set-status` or `set-recover`.
7. Re-download only missing or corrupt parts.
8. Trust recovery only after **Recovered exact** full-file SHA-256 validation.

Filenames and playlist order are never used as part identity.
