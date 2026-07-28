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
- **169/169 automated tests passing**
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
counts, rates, expansion ratio, timings, and reliability settings, as JSON.

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
settings, progress reporting, and performance results.

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

The current Windows Release build passes **169/169 tests**:

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

## Known Limitations

- Video output can still be substantially larger than the source file.
- FFmpeg encoding and decoding are the main performance bottlenecks.
- Output size depends on the input content and FFV1 compressibility.
- The 5% Local profile does not guarantee recovery from severe video damage.
- Resilience on lossy platforms has not yet been benchmarked comprehensively.
- The project remains experimental.

## Roadmap

- Increase data density per frame
- Add a dedicated Fast Local Mode
- Develop balanced and platform-resistant encoding modes
- Estimate output size before encoding
- Validate available disk space
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
