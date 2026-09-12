# Reliquary 1.5.0 CI audit

Baseline: `45d08d14b4dbb886a4d861eeef72f42472704b39`.
Run: https://github.com/burhanbty/Reliquary/actions/runs/34345653381

## Evidence and scope

The public Actions API confirms seven failed jobs and a skipped release job.
GitHub CLI is unavailable in this environment. Job-log download returns HTTP 403;
annotations expose exit code 1, not compiler diagnostics. Therefore remote compiler
root causes cannot be claimed as individually verified.

A fresh local Windows x64 Release build with `BUILD_TESTS=OFF` reproduces C3861 at
four calls to `exerciseResultCardPreview` in `src/main_gui.cpp`. Its definition is
inside `VIDSTOREX_ENABLE_TEST_HOOKS`, while those calls were outside the guard.
The calls now use the same guard. Test-enabled behavior is unchanged. This source
configuration defect affects every GUI build without test hooks, independent of
OS; it is not proof that no additional platform-specific failures exist.

| Platform | Old observed failure | Root cause evidence / change | Support decision |
|---|---|---|---|
| linux-x64 | Build, exit 1; Configure succeeded | Exact remote diagnostic unavailable. Shared test-hook defect corrected; vcpkg Linguist executables checked before configure. Not the ARM64 configure failure. | C: unverified CI target, retained |
| linux-arm64 | Configure, exit 1 | User-supplied log identifies missing `Qt6::lconvert`. Add `qt6-l10n-tools`, check package availability and both executables. Shared source fix also applies to the subsequent build. | C: unverified CI target, retained |
| windows-x64 | Build, exit 1; Configure succeeded | Test-disabled C3861 reproduced locally and corrected. Align with documented `x64-windows`, dynamic CRT/dependencies and static-binaries OFF. | A: planned release target; local build validated, remote gate pending |
| windows-x86 | Build, exit 1 | Exact remote diagnostic unavailable. Shared source fix applies; no architecture-specific fix claimed. | C: unverified CI target, retained |
| windows-arm64 | Build, exit 1 | Exact remote diagnostic unavailable. Shared source fix applies. VS2026 migration annotation is a notice, not failure. | C: unverified CI target, retained |
| macos-x64 | Build, exit 1 | Exact remote diagnostic unavailable. Shared source fix applies; preserve native runner and Homebrew OpenMP setup. | C: unverified CI target, retained |
| macos-arm64 | Build, exit 1 | Exact remote diagnostic unavailable. Shared source fix applies; preserve native runner and Homebrew OpenMP setup. | C: unverified CI target, retained |

All retained; all **not remotely re-run yet**. The matrix is a build-validation
matrix, not a promise of seven supported release platforms. README documents a
verified 64-bit Windows build and describes the project as Windows-focused.
CMake has non-MSVC/Apple/architecture branches and the existing matrix expresses
build intent, so absence of successful logs is insufficient evidence to declare
those platforms unsupported and remove them. No other platform qualifies as
confirmed B (build-supported) yet; none is proven D (unsupported/historical).
No new experimental workflow or failure-swallowing mechanism was added.

## Dependencies and actions

- Ubuntu Jammy ARM64 package file list confirms `qt6-l10n-tools` owns
  `/usr/lib/qt6/bin/lconvert` and `/usr/lib/qt6/bin/lrelease`:
  https://packages.ubuntu.com/jammy/arm64/qt6-l10n-tools/filelist
  The runner executes `apt-cache show` before installation and `test -x` afterward.
  Actual runner configure success remains pending. Linux x64 still uses manifest Qt.
- All vcpkg Qt jobs install manifest dependencies before CMake configure and check
  `tools/Qt6/bin/lrelease` and `lconvert` (with `.exe` on Windows).
  LinguistTools remains REQUIRED; TR/EN resources are unchanged.
- Remove `clear;x-gha,readwrite` and cache-token export. Use supported `files`
  binary archives in `.cache/vcpkg`, persisted by `actions/cache@v6`; keys include
  runner, triplet, manifest and workflow hashes. This caches archives, not build trees.
  Reference: https://learn.microsoft.com/en-us/vcpkg/reference/binarycaching
- Official release API and action metadata verified checkout v7, cache v6,
  upload-artifact v7, download-artifact v8 and action-gh-release v3 (Node24).
  Checkout updated from v6 to v7; upload/download/release majors retained.
- Real Node20 annotations name `ilammy/msvc-dev-cmd@v1` and
  `seanmiddleditch/gha-setup-ninja@v6` on all three Windows jobs.
  Ninja action removed in favor of the runner's Ninja with a mandatory version check.
  Windows 2022 image inventory lists Ninja:
  https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md
  MSVC action retained: it successfully reached configure in the old jobs;
  replacing working architecture setup solely to silence a warning is unnecessary.
  Its Node20 warning is expected to remain. No unverified action major/fork is used.

## Windows x64 gate

CI: `windows-2022`, native MSVC x64, Ninja, `x64-windows`, Release,
`BUILD_GUI=ON`, `BUILD_TESTS=OFF`, `MEDIA_STORAGE_STATIC_BINARIES=OFF`.
The vcpkg baseline and dependency versions remain pinned by the manifest.
FFmpeg, libsodium, Wirehair and Qt application behavior are unchanged.
The smoke uses `QT_QPA_PLATFORM=offscreen`, explicit vcpkg plugin discovery,
a 60-second deadline and nonzero-exit propagation. It does not require an
interactive desktop or deploy plugins. Artifact upload is mandatory, includes
raw executables/DLLs and is not an installer or portable release package.

## Release safety

Guard unchanged: `github.event_name == 'push' && startsWith(github.ref, 'refs/tags/v')`.

| Event/ref | Guard |
|---|---|
| workflow_dispatch / master | false |
| push / master | false |
| pull_request | false |
| push / refs/tags/v1.5.0 | true |

A tag only makes the job eligible; successful build dependencies are also required.
No packaging, staging with windeployqt, application rename, tag, release or push
is part of this change. Version remains 1.5.0. User-owned stress records untouched.

## Next remote run

After reviewing and pushing the local commit, run Build on master. Verify all
retained jobs through configure/build/artifact, especially Windows x64 smoke;
verify Linux ARM64 package/tool checks, absence of x-gha warnings, and skipped
Publish Release. Other platforms remain unverified until that run succeeds.

## Local validation

- Fresh `build-ci-release` directory: MSVC 19.44 (VS2022 Build Tools), AMD64,
  `x64-windows`, Qt 6.10.3, Release, tests OFF, static binaries OFF.
  Configure and full application build passed after the source guard correction.
  Existing manifest-managed dependency installation was reused; dependencies were
  not rebuilt from scratch. Local generator is NMake; CI uses Ninja.
- Both CLI and GUI PE headers independently identify AMD64 (`0x8664`).
- New production GUI `--smoke-test` with the offscreen plugin: exit 0.
- `lrelease`: 423 finished translations, 0 unfinished.
- Baseline test suite: 502/502, no retry (139.82 seconds).
- actionlint 1.7.12: valid YAML and GitHub workflow/matrix/expression structure;
  no findings. External shellcheck/pyflakes were not installed or run.
- Four release-guard cases and unchanged 1.5.0 version checked locally.
- MSVC setup latest official release remains v1.13.0; Ninja setup latest is v6.
  Both Windows 2022 and Windows 11 ARM64 inventories list Ninja 1.13.2.

Local logs (ignored build outputs): `build-ci-validation.log`,
`build-ci-tests.log`, `build-ci-regression.log`. These are local evidence, not
remote GitHub Actions success. Artifact upload and remote cache behavior await
user-triggered validation after push.

Final post-change regression: existing Release test build reconfigured and rebuilt,
then **502/502 passed in 137.90 seconds**, with no retries or observed flaky failures.
Production GUI smoke also passed with the native qwindows plugin (exit 0), in
addition to offscreen. Final `git diff --check` and actionlint passed.
