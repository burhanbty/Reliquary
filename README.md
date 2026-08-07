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
- **256 automated tests registered** (run the Release test command below
  to verify the current machine)
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
- Separate Resilient / Platform and Fast Local encoding modes

## User interface / Kullanıcı arayüzü

The desktop app opens on a card-based Video Set home screen with separate
Create, Recover, Recent, Advanced, and Settings paths. Resilient is always
the default; High Capacity remains an explicit opt-in. English and Turkish
can be switched at runtime from the header or Settings without clearing the
current workflow. The chosen language is saved for the next launch.

Masaüstü uygulaması; Oluştur, Kurtar, Son Kullanılanlar, Gelişmiş ve Ayarlar
yollarını ayıran kart tabanlı Video Set ana ekranıyla açılır. Dayanıklı mod
her zaman varsayılandır; Yüksek Kapasite açıkça seçilmesi gereken bir
seçenektir. İngilizce ve Türkçe, mevcut akış temizlenmeden üst çubuktan veya
Ayarlar'dan değiştirilebilir. Seçilen dil sonraki açılış için kaydedilir.

## Encoding Modes

VidStoreX has two explicitly different storage modes. Decode does not require
a mode selection: the first decoded frame is inspected for the versioned Fast
Local magic, and videos without it continue through the legacy resilient
decoder.

| Mode | Advantages | Trade-offs | Intended use |
|---|---|---|---|
| Resilient / Platform | Existing 8x8-block signal embedding, selectable FEC repair data, more tolerant of transcoding damage | Much larger output and more frames | Experiments involving transfers or platform re-encoding |
| Fast Local | Roughly one payload byte per lossless `GRAY8` pixel, far fewer frames, streaming encode/decode, SHA-256 verification | No FEC and no protection from lossy conversion | Lossless local storage in FFV1/Matroska |

Fast Local requires an `.mkv` output and uses FFV1 with `GRAY8` at the
project's 3840x2160/30 fps settings. It must not be renamed to `.mp4`.
Uploading a Fast Local video to YouTube or another service, transcoding it to
H.264/H.265, resizing it, or otherwise applying a lossy pixel conversion may
destroy the stored data.

### Fast Local format version 1

The first frame begins with an explicitly serialized 128-byte file header.
It contains the `VSXFAST1` magic, format/header versions, mode and encryption
flags, geometry, frame rate, original size, total frames, stored/plain frame
capacities, a 16-byte encryption salt/file ID, the original SHA-256 digest,
and a header checksum. Every frame has a 32-byte header containing its index,
total count, stored and plain lengths, flags, payload checksum, and header
checksum. Integer fields use a fixed little-endian representation; C/C++
structure memory and padding are never written directly.

At 3840x2160, a raw frame has 8,294,400 bytes. Reserving the fixed header
region leaves 8,294,240 stored bytes per frame. Encryption reuses the existing
XChaCha20-Poly1305/Argon2id implementation and its 20-byte per-record
overhead, leaving 8,294,220 original bytes per encrypted frame.

The encoder makes a streaming SHA-256 pass, then reads and encodes one frame
payload at a time. The decoder validates headers, ordering, CRCs, lengths,
decryption authentication, total size, and SHA-256 while writing to a unique
same-directory partial file. Only a completely verified output is atomically
committed.

## Improvements Introduced in VidStoreX

### YouTube Test Lab

The separate **YouTube Test Lab** measures how much Resilient-mode data can
be recovered after a lossy processing roundtrip. It does not alter the
VidStoreX packet format, and it does not support Fast Local: **Fast Local is
not designed for lossy YouTube processing.**

The GUI has a dedicated Test Lab tab. The CLI exposes the same workflow:

```powershell
media_storage testlab generate --preset quick --output C:\vsx-lab
media_storage testlab generate --preset full --output C:\vsx-lab
media_storage testlab simulate --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --profile yt-sim-1080p-medium
media_storage testlab analyze --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --case yt001 --video C:\Downloads\returned.webm --session-label "Initial upload"
media_storage testlab analyze-folder --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --folder C:\Downloads\youtube --dry-run
media_storage testlab analyze-folder --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --folder C:\Downloads\youtube --session-label "Initial upload"
media_storage testlab deduplicate --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --dry-run
media_storage testlab report --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --format markdown
```

`--estimate-only` performs generation preflight without creating a suite.
Custom generation can repeat `--repair-percent`, `--reliability-profile`,
`--input-size`, `--data-type`, and `--resolution`. A cancelled or interrupted
suite can be continued with:

```powershell
media_storage testlab resume --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json
```

The default Quick matrix has six cases: a requested 64 KiB deterministic
random payload,
5%/20%/50% repair, and 1080p/4K. The Full matrix has 36 unique cases:
5%/20%/50%, the current/1080p/1440p/4K resolutions (duplicates removed),
and these input variants:

- 64 KiB compressible
- 64 KiB random
- 256 KiB random
- 1 MiB random

Every upload candidate is sized for at least **2.0 seconds and 60 real data
frames at 30 FPS**. If a small requested payload cannot produce that many
frames, Test Lab deterministically extends it to an effective payload using
the production packet/FEC/frame-capacity calculation. Cases in the same
resolution, data-type, and requested-size comparison group use the same
effective payload. The manifest keeps `requested_input_size` and
`effective_input_size` separately and records why extension occurred.
No blank, neutral, repeated, or other filler frames are added.

Payloads and their duration extensions are generated in bounded blocks from
recorded versioned seeds. Their SHA-256 hashes cover the complete effective
payload, so the same data can be regenerated without holding the whole file
in memory.

Each case first creates a Resilient FFV1/GRAY8/Matroska master using a
central `ResilientVideoConfig`. It then creates a progressive H.264,
YUV 4:2:0, MP4 upload candidate with FFmpeg. The candidate is immediately
reopened and checked for valid H.264/YUV420P metadata, resolution, 30 FPS,
at least 60 decoded frames, at least 1.95 seconds of reported duration,
monotonic PTS/DTS, a valid final timestamp, complete decoder flush, and a
written MP4 trailer. VidStoreX then decodes the embedded payload and compares
its SHA-256. A candidate is marked ready only if every container, timestamp,
decode, and exact-recovery check succeeds. The 2-second value is a tested
starting threshold, not a guarantee that YouTube will accept or process a
particular upload.

Local simulation profiles are:

- `yt-sim-1080p-light`
- `yt-sim-1080p-medium`
- `yt-sim-1080p-heavy`
- `yt-sim-720p-downscale`
- `yt-sim-4k-medium`

These profiles are controlled FFmpeg transcodes for quick feedback. **They
are not a guaranteed copy or predictor of YouTube's processing.** Reports
always distinguish `Source: Local simulation` from
`Source: Real YouTube roundtrip`.

The real YouTube workflow remains deliberately manual:

1. Generate a suite and use only candidates marked ready.
2. Upload the candidate to your own YouTube account as **Private** or
   **Unlisted**.
3. Wait for YouTube processing to finish.
4. Download your own processed video.
5. Import it in the Test Lab tab or pass it to `testlab analyze`.

VidStoreX performs no YouTube login, API upload, or automatic download.
Filename case-ID detection is attempted; `--case` or GUI selection handles
renamed downloads. Imported videos are inspected through the linked FFmpeg
libraries, with no required `ffprobe.exe` process.

Single-video and folder analysis share one central observation service.
Folder preview lists each supported video, detected case, resolution, codec,
size, mapping status, and duplicate status before any manifest change. Case
IDs such as `yt001` are matched case-insensitively inside original
`VSX_YT_..._yt001_...` names and downloader-added prefixes or suffixes.
Ambiguous names, missing IDs, and multiple files for one case remain
**Needs mapping**; use one or more `--map "filename.webm=yt001"` options or
the GUI mapping field rather than accepting a guess.

The real analyzer opens MP4/H.264, WebM/VP9, WebM/AV1, and other containers
and codecs that the linked FFmpeg build can safely decode. It analyzes the
download directly—there is no intermediate MP4 transcode. Each observation
records container, codec/profile/tag, pixel format, dimensions, display
aspect ratio, FPS, time base, duration, decoded frames, file size, SHA-256,
packet recovery, and stream/container bitrate. If reported bitrate is absent,
it uses `file_size_bytes * 8 / duration_seconds` when duration is valid and
marks the source as `calculated_from_size_duration`.

Duplicate prevention uses suite, case, source type, source-file SHA-256, and
the analysis fingerprint. An accidental second click returns the existing
observation ID, date, and result without changing the manifest or reports.
A different file hash is a new observation. To intentionally measure the
same bytes later, create a labeled session and opt in explicitly:

```powershell
media_storage testlab analyze --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --case yt001 --video C:\Downloads\returned.webm --session-label "24-hour retest" --record-new-observation
media_storage testlab analyze --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --case yt001 --video C:\Downloads\returned.webm --session-label "7-day retest" --record-new-observation
media_storage testlab analyze-folder --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --folder C:\Downloads\youtube-30-day --session-label "30-day retest" --record-new-observation
```

The v3 manifest distinguishes filesystem creation/modified timestamps,
VidStoreX's `imported_at_utc`, and `analyzed_at_utc`. Filesystem time is only
a file timestamp; VidStoreX does not claim it is the actual YouTube download
time. Sessions group initial, 24-hour, 7-day, and 30-day observations while
reports keep unique cases and unique observations as separate counts.

Legacy duplicate cleanup is review-first and never deletes imported videos
or restored files:

```powershell
media_storage testlab deduplicate --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --dry-run
media_storage testlab deduplicate --suite C:\vsx-lab\youtube_test_lab\<suite>\manifest.json --apply
```

Apply creates timestamped manifest backups, keeps the oldest or most complete
canonical observation, writes the v3 manifest atomically, and regenerates
JSON/CSV/Markdown reports. Real YouTube outcomes remain observations, not
guarantees; YouTube may change processing, codec selection, or resolution.

Every suite is portable and uses this layout:

```text
youtube_test_lab/<suite-id>/
  manifest.json
  cases/
  generated/
  imported/
  restored/
  reports/
```

The versioned manifest stores relative paths, matrix inputs, seeds, source
and repair counts, frame/video configuration, master/candidate hashes,
processing state, technical video data, and results. Manifest and JSON/CSV/
Markdown report replacement is atomic. Video and payload writers use unique
partial files that are removed on failure. Generation saves state after each
case; resume skips completed cases.

Packet recovery telemetry includes frames read, frames containing a detected
pattern, extracted/valid/invalid/duplicate packets, source/repair packets,
recovered/missing chunks, the required packet threshold, decode failure
stage, elapsed time, and final SHA-256 status. These are observations, not
performance guarantees. Actual YouTube results can vary by account, region,
codec assignment, source resolution, and changes to YouTube processing.

Before generation, the Test Lab reports case count, frames, duration,
conservative master/candidate space, free disk space, and a safety margin.
Insufficient disk blocks generation unless the user explicitly supplies the
existing-style `--allow-low-disk` override. The Full matrix should be started
only after reviewing that estimate.

### YouTube Capacity Lab

The separate **YouTube Capacity Lab** searches for higher experimental data
density without changing Fast Local, the production Resilient defaults, the
packet/media format, or the public C API/ABI. Its settings exist only inside
the Capacity Lab GUI tab, `capacitylab` CLI commands, and schema-v4 Capacity
Lab manifests. An experimental result is never promoted to a production
profile automatically.

The production reference remains 8x8, 1 bit per block, a coefficient strength
of 500 (`1.00x`), a coefficient-sign decoder threshold at zero, and the
existing packet/FEC format. Capacity Lab can test:

- 8x8, 6x6, and 4x4 blocks
- 1 or 2 bits per block
- `0.75x`, `1.00x`, `1.25x`, and `1.50x` signal strength
- 0%, 1%, 2%, and 5% Wirehair repair
- 1920x1080 and 3840x2160 at 30 FPS

The experimental transform is a normalized, separable NxN DCT with one cached
cosine basis per supported size. The 8x8/1-bit/1.00x block generator retains
the production coefficient, truncation, and clamp behavior and is covered by
a byte-for-byte regression test. One-bit modulation uses the production
positive/negative AC(0,1) states. Two-bit modulation uses four ordered AC(0,1)
levels whose bit labels follow Gray order `00, 01, 11, 10`; decoder thresholds
are calibrated from the effective post-clamp levels. Per-block decisions
record the selected symbol, nearest-level distance, and confidence.

Geometry uses only complete blocks. Any right or bottom remainder stays at
neutral luma and is recorded as unused edge space. Production has no separate
frame-level sync reservation: packet `MAGIC_ID`, the v2 packet header, and CRC
provide synchronization and integrity inside the embedded byte stream.
Capacity reports therefore show zero reserved frame blocks and report packet
header/sync overhead separately. Checked arithmetic rejects invalid sizes,
zero packet capacity, and overflow.

Every comparison uses at least 60 real, distinct data frames at 30 FPS. The
payload generator streams deterministic high-entropy data and hashes the
complete payload. All repair levels in the same resolution/block/bits/signal
group use the same source payload, size, seed, and SHA-256; the 0% member sets
the group size. No blank filler frames, repeated frames, or duplicated packets
are introduced.

Capacity calculations include blocks, raw/useful bits, packets and source
payload per frame, repair overhead, expected frames and duration, useful
bytes/second, conservative preflight disk estimates, and comparison with the
8x8/1-bit/1.00x/5% baseline. Candidate size is replaced by the measured and
probed H.264 output size after a real local encode; it is not inferred from
theoretical capacity.

Search presets are deliberately bounded:

- **Smoke**: 12 1080p cases at 2% repair, covering all block/bit pairs at
  `1.00x` and `1.25x`.
- **Staged Sweep**: Stage 1 tests 24 geometry/modulation/signal cases at 2%
  repair; Stage 2 tests 0/1/2/5% repair for at most four passing Pareto
  candidates; Stage 3 tests light/medium/heavy resolution-preserving H.264 at
  1080p and 4K for at most three finalists.
- **Custom**: uses only the explicitly selected matrix and enforces case and
  disk limits. The full 192-case matrix requires an explicit 192-case limit.

Mandatory gates require exact master SHA-256, exact upload-candidate SHA-256,
valid resolution-preserving media metadata, required local simulation passes,
packet recovery at or above the configured gate, and bounded BER/SER. The
720p downscale profile remains a separate robustness observation called
`Resolution-change unsupported`; failure there does not reject a normal
capacity candidate. VP9/AV1 requests are optional capabilities and are
recorded as `Unavailable` without failing the experiment because this
Capacity Lab build does not enable those simulation paths. No external
`ffmpeg.exe` or `ffprobe.exe` dependency is introduced.

After gates, Capacity Lab exposes a Pareto frontier instead of hiding tradeoffs
in one opaque score. It compares useful payload rate, recovery margin, BER/SER,
candidate bytes per payload byte, and encode/transcode/decode time. Reports
mark dominated candidates and label readable representatives such as Most
robust, Best balanced, Highest capacity, Smallest upload, and
Experimental/risky.

Recovery telemetry includes source/repair packets, valid unique packets,
required threshold, packets above threshold, recovery margin in packets and
percent, duplicate/CRC-invalid/missing packets, packet recovery, raw BER/SER,
and average/minimum confidence. A 0%-repair exact pass with almost no recovery
margin is still marked risky.

CLI examples:

```powershell
media_storage capacitylab estimate --preset smoke --output C:\vsx-capacity
media_storage capacitylab run --preset smoke --output C:\vsx-capacity
media_storage capacitylab run --preset staged --output C:\vsx-capacity --max-cases 64 --max-disk-gib 20
media_storage capacitylab estimate --preset boundary-1080p --output C:\vsx-capacity
media_storage capacitylab run --preset boundary-1080p --output C:\vsx-capacity --include-simulation-failures
media_storage capacitylab resume --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json
media_storage capacitylab shortlist --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json --max-videos 8
media_storage capacitylab validate --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json
media_storage capacitylab validate --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json --repair
media_storage capacitylab analyze-folder --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json --folder C:\Downloads\youtube --session-label "Initial YouTube test"
media_storage capacitylab report --manifest C:\vsx-capacity\youtube_capacity_lab\<experiment>\manifest.json --format markdown
media_storage capacitylab analyze-folder --manifest C:\vsx-capacity\youtube_boundary_lab\<experiment>\manifest.json --folder C:\Downloads\youtube --session-label "Boundary initial YouTube test"
media_storage capacitylab boundary-report --manifest C:\vsx-capacity\youtube_boundary_lab\<experiment>\manifest.json --format markdown
media_storage capacitylab boundary-status --manifest C:\vsx-capacity\youtube_boundary_lab\<experiment>\manifest.json

media_storage capacitylab run --preset custom --output C:\vsx-capacity `
  --block-size 8,6,4 --bits-per-block 1,2 `
  --signal 0.75,1.0,1.25,1.5 --repair-percent 0,1,2,5 `
  --resolution 1080p,2160p --simulation h264-medium `
  --max-cases 192 --max-disk-gib 40
```

Each experiment is resumable and uses atomically updated state:

```text
youtube_capacity_lab/<experiment-id>/
  manifest.json
  payloads/
  masters/
  simulations/
  youtube_shortlist/
  imported/
  restored/
  reports/
```

Only selected local-gate passes are copied to `youtube_shortlist`, with a
sidecar containing the canonical config ID, capacity metrics, local result,
and selection reason. Upload/download remains manual. Returned MP4/WebM files
are mapped by config ID in the filename, decoded with the manifest-provided
experimental configuration, deduplicated by file SHA-256, and recorded as a
real observation. A wrong config must fail packet extraction or exact SHA; it
cannot silently produce a valid result.

Shortlist eligibility is evaluated once at config-ID level. For Stage 3,
light, medium, and heavy resolution-preserving H.264 results are mandatory;
one missing, incomplete, metadata-invalid, below-threshold, or SHA-mismatched
result makes the whole config ineligible. The 720p downscale observation is
explicitly non-gating. Pareto and category selection run only after this
filter.

`capacitylab validate` is read-only unless `--repair` is supplied. It detects
rejected-plus-shortlisted conflicts, missing mandatory profiles, ineligible
Pareto entries, and manifest/folder shortlist mismatches. Shortlist
regeneration backs up the manifest, builds the replacement in a temporary
sibling directory, archives the previous shortlist, and swaps the new
directory into place only after every selected artifact and sidecar is ready.
Markdown, JSON, CSV, GUI, and CLI consume the same derived eligibility fields.

JSON, CSV, and Markdown reports distinguish `Local-only candidate`,
`Ready for real YouTube test`, `Real YouTube exact pass`,
`Real YouTube failed`, `Insufficient observations`, `Dominated`, and
`Rejected`. Local success is never called "YouTube proven." YouTube processing
can change over time, and 720p downscale remains an unsupported
resolution-change case, so real initial/24-hour/7-day/30-day observations are
still required before any production-profile decision.

### YouTube Boundary 1080p

`boundary-1080p` is a separate, fixed seven-video experiment that reuses the
Capacity Lab config, packet, streaming-payload, observation, duplicate
prevention, and returned-folder analysis machinery. Every run creates a new
`youtube_boundary_lab/<experiment-id>` folder and never changes an existing
Capacity Lab experiment.

| Case | Geometry | Bits/block | Signal | Repair | Approx. gain |
|---|---|---:|---:|---:|---:|
| B00 | 8x8 | 1 | 1.00x | 5% | 1.00x |
| B01 | 6x6 | 1 | 1.00x | 2% | 1.77x |
| B02 | 6x6 | 1 | 1.00x | 5% | 1.77x |
| B03 | 8x8 | 2 | 1.00x | 2% | 2.00x |
| B04 | 8x8 | 2 | 1.00x | 5% | 2.00x |
| B05 | 6x6 | 2 | 1.00x | 5% | 3.62x |
| B06 | 4x4 | 1 | 1.00x | 5% | 4.00x |

All cases are 1920x1080 at 30 FPS with at least 60 real data frames. Repair
pairs share the same deterministic source payload and SHA-256. B00 is asserted
against the production 8x8/1-bit/1.00x/5% configuration before encoding.
Master and upload-candidate exactness plus media/timestamp validity are hard
local gates. Light, medium, and heavy H.264 simulations are retained as
separate diagnostic evidence. `--include-simulation-failures` may export an
otherwise locally exact candidate with a visible simulation warning; it never
bypasses the hard local gates.

The generated `youtube_upload` directory contains the upload checklist,
locally valid MP4 files, and JSON sidecars. Upload and download remain manual.
Real YouTube evidence stays separate from local evidence. The central
inference refuses to report a boundary when B00 fails, observations are
missing, or results are non-monotonic. A safe candidate additionally requires
a 5% repair profile, positive (preferably at least 1%) recovery margin, and
exact passes in at least two differently labelled sessions.

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

### Resilient 5% versus Fast Local

These measurements were produced by the Windows Release build on the same
machine on 2026-07-28. Each row includes a complete encode/decode and a
matching SHA-256. Highly compressible zero-filled inputs can produce video
smaller than the source; random data is the more representative worst case
for local archival density.

| Input | Mode | Frames | Output bytes | Expansion | Encode | Decode |
|---|---|---:|---:|---:|---:|---:|
| 1 MiB compressible | Resilient 5% | 83 | 96,648,600 | 92.171x | 0.992 s | 0.777 s |
| 1 MiB compressible | Fast Local | 1 | 5,271 | 0.005x | 0.081 s | 0.044 s |
| 1 MiB random | Resilient 5% | 83 | 154,975,108 | 147.796x | 1.071 s | 0.836 s |
| 1 MiB random | Fast Local | 1 | 1,130,985 | 1.079x | 0.084 s | 0.051 s |
| 10 MiB compressible | Resilient 5% | 828 | 967,243,052 | 92.243x | 9.429 s | 8.146 s |
| 10 MiB compressible | Fast Local | 2 | 9,820 | 0.001x | 0.313 s | 0.262 s |
| 10 MiB random | Resilient 5% | 828 | 1,549,991,631 | 147.819x | 10.588 s | 9.017 s |
| 10 MiB random | Fast Local | 2 | 11,210,756 | 1.069x | 0.347 s | 0.305 s |
| Office Tool Plus.rar (68,185,385 B) | Resilient 5% | 5,379 | 10,076,545,011 | 147.782x | 69.926 s | 62.608 s |
| Office Tool Plus.rar (68,185,385 B) | Fast Local | 9 | 72,528,938 | 1.064x | 1.877 s | 1.756 s |

For the random 10 MiB case, Fast Local used 99.76% fewer frames, reduced the
output by 99.28%, encoded about 30.5x faster, and decoded about 29.5x faster.
For the Office archive it used 99.83% fewer frames, reduced output by 99.28%,
encoded about 37.3x faster, and decoded about 35.7x faster. Encryption creates
high-entropy payloads, so encrypted output should be expected to behave more
like random data than the compressible rows.

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

| Profile | Geometry | Bits | Repair | Purpose | Validation |
|---|---:|---:|---:|---|---|
| High Capacity | 4x4 | 1 | 5% | Higher useful capacity / shorter videos at the same resolution | Real YouTube roundtrip, 6/6 exact |
| Balanced | 8x8 | 1 | 20% | General-purpose experiments | Repair-overhead preset |
| Resilient | 8x8 | 1 | 5% | Safest default | Existing production baseline |
| Durable | 8x8 | 1 | 50% | Higher-risk storage or transfer scenarios | Repair-overhead preset |
| Custom | 8x8 | 1 | 0–500% | Controlled testing and workload-specific tuning | Operator-selected repair overhead |

**Resilient remains the default and most conservative production profile.**
High Capacity uses 1920x1080, 4x4 one-bit geometry, signal strength 1.0 and
5% repair (config ID `538F2B009FAB`). It passed six exact real YouTube
roundtrips across small, medium and large payloads in two separate upload
sessions: 6 passes and 0 failures. This demonstrates the tested workflow; it
is not an absolute data-survival guarantee. For important files, always
verify the recovered SHA-256 and decode result. Actual encoded file size
depends on content and codec behavior and is not guaranteed to be one quarter
of a Resilient file.

Reliability profiles themselves do not split inputs. The opt-in Video Set
layer described below provides multi-video splitting without changing these
profile definitions or their single-video validation evidence.

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

The encode section also exposes **Resilient / Platform** and **Fast Local**.
Selecting Fast Local disables reliability and repair controls, shows the
lossy-re-encoding warning, includes the mode in the asynchronous estimate
fingerprint, and reports header/frame-capacity fields in preflight. Returning
to Resilient re-enables the High Capacity/Balanced/Resilient/Durable/Custom
controls. Resilient is selected by default. High Capacity shows its 4x4,
one-bit, signal 1.0, 5% repair parameters and the six-case real YouTube
validation note; changing repair manually continues to require Custom.

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
build\Release\media_storage.exe encode input.rar output.mkv --mode fast-local
build\Release\media_storage.exe encode input.rar output.mkv --mode fast-local --benchmark-json fast-report.json
build\Release\media_storage.exe encode input.rar output.mkv --mode resilient --repair-percent 5
build\Release\media_storage.exe encode input.rar output.mkv --reliability-profile high-capacity
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

The default for a zero-initialized API option or omitted CLI `--mode` is
`resilient`; High Capacity is used only when explicitly selected. Fast Local
rejects non-`.mkv` outputs. Supplying
`--repair-percent` or `--reliability-profile` together with
`--mode fast-local` is an error rather than a silently ignored option.
Decode detects the format automatically and rejects `--mode`.

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
Zero-initialized options retain Resilient mode and the default 5% Local
reliability behavior.

The API reports version `1.4.0`. `ms_encoding_mode_t` and Fast Local layout
fields were appended to `ms_encode_options_t`, `ms_encoding_estimate_t`, and
`ms_result_t`; `MS_ENCODING_ESTIMATE_VERSION` is now 2. These changes alter
binary layouts, so applications built against an older header must be
recompiled. The legacy packet format is unchanged, existing videos remain
decodable, and zero initialization selects the old encode path. The
shared-library `SOVERSION` is therefore `3`.

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

The current Windows Release build passes **229/229 tests**:

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
- Fast Local endian-safe header serialization, checksums, boundary frame
  counts, padding, real FFV1 `GRAY8` roundtrips for empty/1-byte/text,
  compressible/random 1 MiB and 10 MiB inputs, encryption, wrong-password
  safety, corruption/truncation, automatic detection, preflight, JSON, CLI,
  and mode-fingerprint behavior

## Known Limitations

- Fast Local is not resilient to lossy transcoding, resizing, chroma/pixel
  conversion, or social-platform processing.
- Fast Local currently requires FFV1, `GRAY8`, Matroska `.mkv`, 3840x2160,
  and 30 fps.
- Fast Local makes two sequential input passes (one for SHA-256 and one for
  encoding) while keeping only frame-sized buffers in memory.
- Random or encrypted Fast Local output is typically around 1.06-1.08x in
  the measured cases; tiny files still pay container/frame overhead.
- Video output in Resilient mode can still be substantially larger than the source file.
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
### YouTube 1-bit Verification

The mixed Boundary result is non-monotonic because it combines two modulation
families: the 4x4/1-bit profile produced exact evidence while lower
density 2-bit profiles failed after real YouTube encoding. The 1-bit symbol
mapping and the 2-bit four-level mapping are not interchangeable points on a
single geometry axis. Subsequent six-case real YouTube stress validation
passed 6/6 across payload size and upload session, so the exact 4x4/1-bit/
signal-1.0/repair-5% configuration is now available as the opt-in High
Capacity profile. Resilient remains the safer default, and neither profile is
an absolute guarantee.

`onebit-verification-1080p` creates exactly six 1920x1080, 30 FPS, 1-bit,
1.00x-signal, 5%-repair videos in `R00, R01, G04, R02, R03, G05` order. It
requires `--source-manifest`: R00, R01, and R02 reuse and SHA-verify Boundary
B00, B02, and B06 payloads. R03 uses an independent deterministic payload;
the new 5x5 and 3x3 geometry points use prefixes of one deterministic random
stream family. Repair is fixed at 5% to isolate geometry.

Upload the six files together manually in YouTube Studio. The experiment
contains `upload_checklist.md`, titles, CSV metadata, sidecars, and
`tools/download_returned_playlist.ps1`; the latter downloads a playlist's
video-only 1920x1080 streams, preferring H.264 without merging or transcoding.
Historical observations remain read-only provenance. Repeated exact evidence
requires distinct sessions/uploads, at least one current observation, positive
recovery margin, and independent-payload evidence. Local light/medium/heavy
H.264 simulation is diagnostic only and never counts as real YouTube proof.

```powershell
media_storage capacitylab run --preset onebit-verification-1080p `
  --source-manifest C:\path\to\boundary\manifest.json `
  --output C:\path\to\onebit-staged
```

## Video Sets / Large Files

Video Set is the opt-in, file-only archive layer for sources that should be
carried by more than one video. It does not replace the single-video format.
The source is split into byte ranges; each logical part is an explicit
little-endian `VideoSetPartEnvelopeV1` followed by that range, and the result
is passed to the existing encoder unchanged. Consequently every video keeps
its own packetization, repair data, file ID, encryption nonces (when a
password is supplied), and local decode/SHA verification. There is no shared
cross-video parity in version 1.

The embedded `VSXSET01` envelope records the version and header length, flags,
random 128-bit set ID, deterministic part ID, index/count, source size and
full SHA-256, chunk offset/size/SHA-256, stable profile/config IDs, geometry,
signal, repair percentage, sanitized display filename, descriptor hash, and
header checksum. Recovery reads this metadata after the normal decoder, so
renamed videos, underscore/space changes, shuffled downloads, and playlist
order do not affect identity. A bounded parser rejects truncation, invalid
ranges, unsafe names, unsupported versions, and corrupt checksums.

`set_manifest.json` is an atomic, extensible convenience index containing the
same core identity plus the split policy, estimates, actual video metrics,
per-part verification/upload/recovery state, and aggregate state. It contains
no source absolute path, password, or key. The sidecar speeds up normal use,
but `set-inspect` and `set-recover` can reconstruct a set from embedded
envelopes alone. Missing, corrupt, or conflicting parts never produce a final
file. Identical exact duplicates are reported and one is selected safely.

Defaults are a 600-second target, a configurable 1500 MiB actual-video cap,
and 10% reserve. These are conservative VidStoreX project defaults, not
official YouTube limits or a delivery guarantee. Planning uses the production
packet/frame-capacity and repair calculations. The first full part measures
actual container size; if it exceeds the hard cap, all ranges, hashes, IDs,
and the descriptor are replanned with a smaller chunk (at most three retries).
Set publication is an atomic same-filesystem directory rename only after every
part locally roundtrips exactly.

### Real YouTube validation

VidStoreX Video Sets were validated through a real YouTube roundtrip using
the verified High Capacity profile.

- Source payload: 32 MiB (33,554,432 bytes)
- Parts: 4
- Profile: High Capacity
- Geometry: 4x4, one bit per symbol, signal strength 1.0
- Repair: 5%
- Config ID: `538F2B009FAB`
- Upload format: 1920x1080
- All four videos were uploaded to YouTube and downloaded again from
  YouTube's re-encoded 1080p streams.
- Recovery completed successfully from all four returned parts.
- Original and recovered SHA-256 values matched exactly:
  `C3EEFBBCB32EE6D0A93DCB13098385985783400C2B64C6E6923523A1FE1F8277`.

**Real YouTube: 4/4 parts, full-file SHA exact.** This validates the tested
four-part workflow under the observed YouTube encoding conditions. It is not
an absolute storage guarantee; successful recovery must always be confirmed
using the final full-file SHA-256 check.

### Guided workflow

The **Video Set Assistant** in the desktop GUI presents two simple starting
choices: **Create a Video Set** and **Recover a Video Set**. Create guides the
user through file selection, a simple reliability choice, automatic planning,
local video creation and verification, manual Unlisted YouTube upload,
playlist download, returned-part scanning, and exact recovery. Recover accepts
a set folder, `set_manifest.json`, returned-video folder, or individual video
and goes directly to the shared scan and recovery steps.

Resilient remains the preselected **Most Reliable** mode. High Capacity is the
explicit **Fewer and Shorter Videos** choice and shows its separate 6/6
single-video and 4/4 Video Set validation evidence. Duration, actual-size cap,
reserve, geometry, config ID, part ranges, and technical logs remain available
under **Advanced settings**, **Show part details**, and **Advanced / Classic
Video Set Tools**, but are hidden from the normal path.

Planning, encode, scan, and recovery reuse the existing CLI/backend in child
processes, so the GUI remains responsive and established resume/atomic-output
behavior is preserved. After upload, the Assistant can invoke a selected or
detected `yt-dlp` executable directly with a separate argument list; it does
not build a shell command and does not depend on PowerShell ExecutionPolicy.
It uses `bv*[height=1080]/bv*[height<=1080]`, downloads into the set's
`returned/` folder, and automatically scans successful downloads. The
generated PowerShell helper remains available for CLI/manual workflows.

Missing or corrupt parts are shown with re-download guidance, identical
duplicates are reported but remain usable, and conflicting duplicates block
recovery. A recovered file is presented as successful only when the backend
reports **Recovered exact** after the final full-file SHA-256 check. Up to five
recent manifest paths are remembered for convenience; their actual state is
always reloaded from the manifest and recovery files, not inferred from GUI
settings.

### Clear operation progress

The Assistant uses one persistent activity panel for planning, encoding,
playlist download, returned-video scanning, and recovery. It shows the current
phase, real item or byte counters when the backend knows a total, the current
filename, elapsed time, an estimate only when enough information exists, and a
safe Cancel action. Work with no trustworthy denominator stays visibly active
without inventing a percentage. After 30 seconds without a new progress event,
an informational “taking longer than usual” message appears; it does not stop,
retry, or mark the operation as failed.

Scanning and recovery are deliberately separate. A scan first discovers video
files, then checks their embedded Video Set information while reporting live
candidate, checked, verified, missing, corrupt, duplicate, and conflict counts.
It does not rebuild the source file and never displays a misleading `0/0`
result during discovery. When every required part is verified, the Assistant
enables **Recover Original File** and waits for that explicit action. Recovery
then reports decoding, part verification, exact byte writing, final full-file
SHA-256 checking, and atomic publication as distinct phases.

The GUI receives these updates through an optional operation-ID-tagged JSONL
channel from its child CLI process, so stale events from an earlier operation
are ignored. Normal CLI output and defaults are unchanged when that internal
progress option is absent. Technical output remains collapsed by default and
is bounded to the most recent 5,000 lines.

Encoding keeps one temporary logical payload at a time and hashes source
ranges with a bounded streaming buffer. `--resume` accepts a prior part only
when source/plan identity, recorded video size/SHA, and exact local
verification state still agree. Recovery writes a set-scoped `.vsx.partial`
file and `recovery_state.json`; resumed ranges are rehashed before being
skipped. The final name is sanitized and the partial file is renamed only
after its complete SHA-256 equals the embedded original SHA-256. Source and
video files are never moved, deleted, or automatically uploaded.

Typical CLI flow:

```powershell
build\Release\media_storage.exe set-plan "D:\archive\large-file.rar" "D:\VidStoreX Sets" --reliability-profile high-capacity --target-duration-seconds 600 --max-video-size-mib 1500
build\Release\media_storage.exe set-encode "D:\archive\large-file.rar" "D:\VidStoreX Sets" --reliability-profile high-capacity --target-duration-seconds 600 --max-video-size-mib 1500 --resume
build\Release\media_storage.exe set-status --manifest "D:\VidStoreX Sets\large-file_AB12CD34\set_manifest.json"
build\Release\media_storage.exe set-inspect "D:\VidStoreX Sets\large-file_AB12CD34\returned"
build\Release\media_storage.exe set-recover "D:\VidStoreX Sets\large-file_AB12CD34\returned" "D:\Recovered" --resume
```

The set directory includes `upload_checklist.md`, `upload_checklist.csv`,
`README_NEXT_STEPS.md`, reports in `reports/`, and
`tools/download_returned_playlist.ps1`. Upload videos manually as Unlisted,
wait for 1080p processing, then download YouTube's re-encoded video-only
streams. The helper uses `yt-dlp` without logging in or uploading; downloads
made another way work equally well.

The GUI exposes the same opt-in workflow in the **Video Sets** tab: source,
output root, Resilient/High Capacity selection, duration, size cap, reserve,
plan table, encode/resume/cancel, scan, and recovery controls. With “Split as
Video Set” off, the established GUI encode/decode path is unchanged.

Resilient remains the conservative default (8x8, one bit, signal 1.0, 5%
repair). High Capacity remains explicit opt-in (4x4, one bit, signal 1.0, 5%
repair, config `538F2B009FAB`) and was exact in 6/6 real YouTube stress cases,
but no profile is an absolute data guarantee. Version 1 intentionally omits
cross-video parity, multi-source/folder archives, automatic YouTube account
or upload integration, compression redesign, and multi-video stream encode.
