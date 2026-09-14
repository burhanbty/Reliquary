# Windows x64 packaging

The local Reliquary 1.5.0 pipeline is `package.ps1`. It uses the approved `assets/windows/reliquary.ico`, canonical CMake version, matching installed vcpkg triplet, and Inno Setup 6. Nothing is downloaded or installed automatically. It never pushes, tags, commits, or publishes a release.

Run from the repository root in an x64 MSVC developer PowerShell. Supply `-TripletDir`, `-RedistDir` (the x64 directory containing Microsoft.VC143.CRT/OpenMP), `-YtDlp` (the reviewed 2026.07.04 executable), and `-Toolchain`. `-Iscc` and `-Python` can select existing tools. Use new `-BuildDir` and `-TestBuildDir` paths for clean builds; existing directories are rejected. Dependency installation is disabled.

The pipeline builds/tests with hooks enabled, then builds the public application with `BUILD_TESTS=OFF`. It requires 502/502 without retries. `-UseValidatedBuilds` is only for already completed clean builds: the source digest and test-log hash must match `release/validation/test-source-stamp.json`.

One tree, `release/staging/Reliquary-1.5.0-x64`, feeds both formats. The GUI becomes `Reliquary.exe`; `media_storage.exe` remains its internal helper. `windeployqt` and recursive import auditing supply release DLLs and qwindows. System ICU and DXC are not redistributed. Turkish QM is embedded and also copied under translations; English uses built-in source strings.

`audit-licenses.ps1` maps every bundled EXE/DLL, verifies reviewed package versions and the exact yt-dlp hash, and supplies original notices. Native FFmpeg uses shared libav libraries with x264/GPL enabled. Reliquary does not launch standalone ffmpeg/ffprobe; optional yt-dlp postprocessing requiring those tools is not bundled. This is not a live YouTube download validation.

Temporary candidates stay under `release/validation`. An extracted ZIP must pass isolated smoke and tree-hash checks. The actual installer is compiled and its approved icon payloads checked, installed into a unique validation directory, and checked for registration, Start Menu target, identical runtime files and isolated smoke. The test aborts if a Reliquary AppId already exists. It uninstalls and verifies preservation of a user-created sentinel. Installer tests need permission to write per-user Start Menu and HKCU entries.

Only after those checks are artifacts copied to `release/out`, with a rechecked SHA256SUMS and staging manifest. Existing named output files are never overwritten. Normal GUI observation uses `observe-launch.ps1` under an isolated Windows test account; screenshots require visual review. Local Defender scanning and source publication readiness are recorded in the validation report. No certificate is assumed: outputs are unsigned.

`preflight.ps1` checks input availability; it does not certify a release. `stage-runtime.ps1` alone creates an intermediate diagnostic tree; the license/tool audit and package validation must finish before distribution. Before publishing binaries, make the matching source revision and dependency sources/build patches available under their licenses. Packaging does not invent a release URL or source offer.
