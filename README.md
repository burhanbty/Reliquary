# VidStoreX

VidStoreX is an experimental C++ application that encodes files into video
frames and reconstructs the original data without loss. It combines FFmpeg,
Qt 6, libsodium, CMake, and vcpkg to explore data-to-video storage and
recovery through both graphical and command-line interfaces.

VidStoreX is a research and engineering project. It is not a replacement for
an efficient, general-purpose archive or backup tool.

## Current Status

- Tested on Windows 10/11
- Release configuration builds successfully
- **214/214 automated tests passing**
- Successful encode/decode roundtrip with SHA-256 verification
- Both GUI and CLI applications are available

## Main Features

- Encode a file into FFV1 video frames
- Recover the original file from an encoded video
- Optional password-based encryption with libsodium
- FFmpeg-based video encoding and decoding
- Qt 6 desktop GUI
- Command-line interface
- Forward error correction (FEC) and repair packets
- SHA-256 integrity verification
- Stage-level performance profiler
- Machine-readable JSON benchmark output
- Configurable reliability profiles
- Encode preflight estimation and target-disk safety checks

## Improvements Introduced in VidStoreX

### Performance profiling

VidStoreX adds a shared performance-reporting system for GUI and CLI
operations:

- Monotonic measurements based on `std::chrono::steady_clock`
- Thread-safe atomic duration and invocation counters
- RAII-based `ScopedTimer` instrumentation
- Separate measurements for encode and decode stages
- A common human-readable report format for GUI and CLI workflows
- JSON export through `--benchmark-json`

The profiler separates work such as FEC generation, packet-to-frame
conversion, FFmpeg processing, muxing, demuxing, recovery, and disk I/O.
Timings from parallel stages represent accumulated work time, so their
percentages can overlap and may add up to more than 100%.

### Reliability calculation fix

The previous calculation passed `5.00` into an API that expected a ratio.
Instead of 5%, the value was therefore interpreted as `5x`, producing
approximately 500% repair packets.

VidStoreX centralizes percentage-to-ratio conversion at the CLI and GUI
boundaries. The default is now 5% (`0.05` internally), and validation rejects
negative values, NaN, infinity, percentages above 500%, and packet-count
overflow.

### Reliability profiles

VidStoreX provides three named profiles plus a custom range:

- **Local / Fast:** 5%
- **Balanced:** 20%
- **Durable:** 50%
- **Custom:** 0–500%

The CLI exposes these settings through `--reliability-profile` and
`--repair-percent`. An explicit repair percentage overrides the selected
profile.

### Encode preflight and disk safety

Before every normal file encode starts, a shared preflight service calculates chunk,
source/repair/total packet, frame, and video-duration counts with the same
helpers used by the real encoder. A bounded probe then encodes representative
input chunks through the production `VideoEncoder`, so codec, pixel format,
resolution, frame rate, muxer, and FFmpeg settings cannot drift from the real
encode.

The likely output size includes measured Matroska overhead. Its range is
derived from the observed compressed bytes-per-frame distribution with a 95%
Student-t interval; it is not a fixed percentage guess. For small inputs that
fit entirely inside the probe budget, the bounded encode is exact.

The estimate records the normalized input path, size, and last-write time.
Immediately before the encoder opens the input, these values are checked
again and target-disk capacity is re-queried. A changed input produces
`MS_ERR_PREFLIGHT_STALE`; the caller must run preflight again instead of
encoding with outdated packet or disk calculations.

Disk space is queried on the nearest existing ancestor of the output path.
Unknown disk values remain unknown and produce a warning, but do not block
encoding or require an override. When an output estimate is available, the
central policy is:

```text
safety_margin = max(1 GiB, ceil(estimated_output_max / 10))
required_space = estimated_output_max + safety_margin
```

The file encoder writes to a unique same-directory partial file and replaces
the requested target only after FFmpeg closes successfully. The available
space already excludes an existing target's occupied bytes, so existing
output size is not credited back: the old target and new partial may coexist
until commit. The partial is the future final output, not an additional copy,
so the temporary-space term remains zero. Failed or cancelled encodes remove
their partial and preserve the old target.

A known insufficient result blocks encode by default. `--allow-low-disk`
overrides only this known disk blocker and prints the available, required,
missing, estimated-maximum, and safety-margin values. It cannot override
invalid input/output paths, stale metadata, invalid reliability values, or
arithmetic overflow.

If the probe fails, or `--no-probe` is used, deterministic packet/frame
estimates remain available while output size and required space are explicitly
unavailable (`null` in JSON). Encoding may continue with a warning because
disk sufficiency cannot be fully verified. Technical probe errors remain in
diagnostic output, and probe temporary files are removed automatically.

`--estimate-only` prints the complete preflight report without creating,
removing, or replacing the requested output. `--estimate-json <path>` writes
the machine-readable form. In estimate-only mode,
`--benchmark-json <path>` is an alias for the same estimate schema; during a
real encode it retains its performance-report meaning. After a successful
encode, text and benchmark JSON reports compare estimated and actual bytes,
including absolute/relative error, range membership, preflight duration, and
actual encode duration.

Output estimates are measurements, not allocation guarantees. FFV1
compressibility depends on the embedded data, and a bounded statistical
interval can occasionally miss the final byte count. The safety margin is
therefore deliberately much larger than the output-size confidence interval.

## Benchmark Results

The following results are from a real **68,185,385-byte** input-file test.

| Metric | Before repair fix | After repair fix |
|---|---:|---:|
| Repair percentage / effective behavior | 500% | 5% |
| Source packets | 266,350 | 266,350 |
| Repair packets | 1,331,750 | 13,331 |
| Total packets | 1,598,100 | 279,681 |
| Frames | 30,733 | 5,379 |
| Encode time | 486.7 s | 85.4 s |
| Output size | 57,614,108,874 bytes | 10,076,530,323 bytes |
| Expansion ratio | 844.963x | 147.781x |
| Decode time | — | 75.6 s |
| SHA-256 match | — | `true` |

The corrected 5% configuration delivered:

- Approximately **5.7x faster encoding**
- Approximately **82.5% fewer frames**
- Approximately **82.5% fewer total packets**
- A complete lossless roundtrip, confirmed by SHA-256

Output size varies with the input data and how effectively FFV1 can compress
the generated frames. These results describe this test case and should not be
treated as universal performance guarantees.

## Performance Report Example

Every successful encode or decode prints a stage-level report. The following
abridged example shows the report shape while using the headline values from
the corrected benchmark:

```text
=== Performance report (encode) ===
Input / output: 68185385 B -> 10076530323 B  (ratio 147.781x)
Rates: 62.986 frames/s, 0.762 MiB/s
Stage timings:
  FEC / repair packet generation    ...
  Packets to frames                 ...
  FFmpeg video encoding             ...
  Mux and disk write                ...
  Total wall time             85.400000 s    100.00%
```

Use `--benchmark-json <path>` to write the complete report, including packet
counts, rates, expansion ratio, timings, reliability settings, and estimate
validation, as JSON.

## Reliability Profiles

| Profile | Repair | Intended use | Trade-off |
|---|---:|---|---|
| Local / Fast | 5% | Clean local storage and controlled transfers | Fastest processing and smallest output, but less recovery capacity for damaged or lossy video |
| Balanced | 20% | General-purpose experiments | More packets, larger output, and longer processing in exchange for higher packet-loss tolerance |
| Durable | 50% | Higher-risk storage or transfer scenarios | Highest time and size cost among the presets, with the most repair data |
| Custom | 0–500% | Controlled testing and workload-specific tuning | The operator is responsible for balancing overhead and recovery capacity |

These profiles have not yet been comprehensively validated on YouTube or
other lossy/recompressing platforms. They describe repair-packet overhead,
not a guarantee that a video will survive any particular platform or level of
damage.

## Build Requirements

The verified Windows build uses:

- Windows 10 or Windows 11
- Visual Studio Build Tools 2022
- MSVC v143 toolset
- Windows SDK
- CMake 3.22 or newer
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

The project requires a C++23 compiler. Its vcpkg manifest installs FFmpeg and
libsodium; the `gui` feature adds Qt 6.

## Build Instructions

Clone the repository and its submodules:

```powershell
git clone --recurse-submodules https://github.com/burhanbty/VidStoreX.git
cd VidStoreX
```

Configure a 64-bit Windows build. Replace `C:/vcpkg` if vcpkg is installed
elsewhere:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_FEATURES=gui -DBUILD_TESTS=ON
```

Build and run the test suite:

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Deploy the Qt runtime beside the GUI executable before distributing or
running it outside the development environment:

```powershell
& "C:\Users\<USERNAME>\VidStoreX\build\vcpkg_installed\x64-windows\tools\Qt6\bin\windeployqt.exe" --release "C:\Users\<USERNAME>\VidStoreX\build\Release\media_storage_gui.exe"
```

## Usage

### GUI

Launch the Release GUI from PowerShell:

```powershell
build\Release\media_storage_gui.exe
```

The GUI provides file selection, encode/decode controls, reliability
settings, progress reporting, performance results, and a compact
**Preflight Estimate** panel. Selecting a different input/output path or
changing encryption or reliability settings automatically schedules the
shared estimate service on a background Qt thread. Custom repair-percentage
editing is debounced, and request generations prevent an older probe result
from being applied to newer settings.

The panel shows input size, profile and repair percentage, deterministic
source/repair/total packet and frame counts, video duration, likely and
minimum/maximum output sizes, available and required disk space, safety
margin, probe frame count/duration, and estimation method. Details are
collapsible so the existing scrollable controls and decode workflow remain
easy to reach.

A known insufficient-disk result disables Encode by default and exposes the
explicit **Proceed despite insufficient disk space** option. The option
requires confirmation, is reset by relevant setting changes, and is passed
to the central API as a disk-only override. Unknown disk capacity and an
unavailable output-size probe produce separate warnings and require
confirmation, but do not automatically block an otherwise valid encode.

Before encode, the GUI checks that the accepted estimate still matches the
normalized paths, input size/last-write time, reliability, and encryption
state. Missing, stale, or older estimates are refreshed; `ms_encode` then
rechecks metadata and disk space immediately before opening the encoder.
Existing outputs require an explicit overwrite confirmation and retain the
central same-directory partial/atomic-replace safety path.

After encode, the GUI's performance log reports the central estimate
validation fields: likely/minimum/maximum estimates, actual bytes,
absolute/relative error, range membership, preflight duration, and actual
encode duration. These estimates are measurements rather than guarantees;
FFV1 size depends on input content and compressibility.

### CLI

The CLI accepts either positional input/output paths or explicit
`--input`/`--output` flags. The following positional examples are supported
by the current implementation:

```powershell
build\Release\media_storage.exe encode input.rar output.mkv
build\Release\media_storage.exe encode input.rar output.mkv --reliability-profile balanced
build\Release\media_storage.exe encode input.rar output.mkv --repair-percent 7.5
build\Release\media_storage.exe decode output.mkv restored.rar
build\Release\media_storage.exe encode input.rar output.mkv --benchmark-json benchmark.json
build\Release\media_storage.exe encode input.rar output.mkv --estimate-only
build\Release\media_storage.exe encode input.rar output.mkv --estimate-only --estimate-json estimate.json
build\Release\media_storage.exe encode input.rar output.mkv --estimate-only --benchmark-json estimate.json
build\Release\media_storage.exe encode input.rar output.mkv --allow-low-disk
build\Release\media_storage.exe encode input.rar output.mkv --repair-percent 5 --allow-low-disk
```

Use `--no-probe` when only deterministic packet/frame counts are wanted.
This intentionally leaves output-size and required-space values unavailable
and warns that disk safety could not be fully verified:

```powershell
build\Release\media_storage.exe encode input.rar output.mkv --estimate-only --no-probe
```

Equivalent explicit-path syntax is also available:

```powershell
build\Release\media_storage.exe encode --input input.rar --output output.mkv
build\Release\media_storage.exe decode --input output.mkv --output restored.rar
```

For password-based encryption:

```powershell
build\Release\media_storage.exe encode input.rar output.mkv --encrypt --password "your-password"
build\Release\media_storage.exe decode output.mkv restored.rar --password "your-password"
```

Avoid exposing sensitive passwords in shared terminal history or logs.

### Public C API preflight

`ms_estimate_encode` returns a versioned `ms_encoding_estimate_t` with
explicit availability flags for output estimates and disk values. A caller
can pass that estimate to `ms_encode`; the implementation rejects unsupported
structure versions, stale input metadata, and newly insufficient disk.
Zero-initialized options retain the default 5% Local reliability behavior.

The API reports version `1.2.0`. The additions to `ms_encode_options_t`,
`ms_encoding_estimate_t`, and `ms_result_t` change their binary layouts, so
applications built against an older header must be recompiled. The encoded
file and packet formats are unchanged, and existing videos remain decodable.
The shared-library `SOVERSION` is therefore `2`.

## Project Structure

```text
VidStoreX/
├── include/          Public C API
├── src/              Core codec, FFmpeg, profiler, CLI, and Qt GUI sources
├── tests/            GoogleTest unit, integration, and roundtrip tests
├── CMakeLists.txt    Root build and installation configuration
└── vcpkg.json        Dependency manifest and optional GUI feature
```

## Testing

The current Windows Release build passes **214/214 tests**:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Coverage includes:

- Unit tests for integrity, chunking, codec, encryption, streaming, and API behavior
- Repair-packet calculations
- Reliability-profile selection and overrides
- CLI repair-option validation
- Negative, NaN, infinity, upper-bound, and overflow error paths
- Encode/decode roundtrips
- SHA-256 reconstruction verification
- Human-readable and JSON performance reports
- Probe-based output estimates, JSON nullability, and disk-safety behavior
- Stale metadata, disk re-check, low-disk override, safe replacement, and
  probe/partial cleanup paths
- End-to-end CLI estimate-only, JSON alias, no-probe, option-order, and
  disk-known/unknown behavior
- Estimate-versus-actual validation, zero/unavailable arithmetic, and
  overflow protection
- GUI preflight generation acceptance, fingerprint staleness, state
  transitions, encode eligibility, low-disk override reset, warning states,
  and shutdown-result rejection

## Known Limitations

- Video output can still be substantially larger than the source file.
- FFmpeg encoding and decoding are the main performance bottlenecks.
- Output size depends on the input content and FFV1 compressibility.
- A bounded 95% Student-t interval is not a guarantee and can be narrower than
  small systematic container/position effects; rely on the disk safety margin,
  not the confidence interval alone.
- Disk availability can be unknown on unsupported or inaccessible
  filesystems; encoding then proceeds with an explicit warning.
- Missing output parent directories are reported as errors rather than
  automatically created.
- The 5% Local profile does not guarantee recovery from severe video damage.
- Resilience on lossy platforms has not yet been benchmarked comprehensively.
- The project remains experimental.

## Roadmap

- Increase data density per frame
- Add a dedicated Fast Local Mode
- Develop balanced and platform-resistant encoding modes
- Add pause and resume support
- Support streaming decode with lower memory requirements
- Evaluate more efficient FFmpeg settings
- Build a platform-recompression benchmark suite

## Upstream Project and Credits

VidStoreX is based on and forked from
[yt-media-storage](https://github.com/PulseBeat02/yt-media-storage) by
[Brandon Li (PulseBeat02)](https://brandonli.me/). The upstream authorship
and copyright notices remain in the source files.

VidStoreX is distributed under the **GNU General Public License, version 3 or
later (GPL-3.0-or-later)**. See [LICENSE.txt](LICENSE.txt) for the complete
license text. Existing copyright and third-party notices are retained in the
repository.
