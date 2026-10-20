# `audio/` — design

Decoding (including files still downloading), 3-band waveform analysis and its cache, quality checks. Contract:
`docs/contracts/media.md` (A1–A3). Spec: `references/audio-preview-and-waveform.md` (§3–5, §7, §9, §11), ADR-003,
ADR-007. Playback output and the waveform widget belong to `app/`; this library provides PCM and data, no UI.

## Classes

| Header | Class / function | Role |
|---|---|---|
| `Error.h` | `ErrorCode`, `Error`, `Result<T>`, `CancellationFlag` | Error model shared with `convert/`. Nothing throws. |
| `ByteSource.h` | `ByteSource`, `FileSource`, `GrowingFile`, `MemorySource`, `Subscription` | Thread-safe, non-blocking byte access; `GrowingFile` is the preview being downloaded. |
| `StreamInfo.h` | `StreamInfo`, `LameTag`, `MpegAudioStats` | What the decoder found: format, codec, rate, depth, exact or estimated length, LAME tag. |
| `Decoder.h` | `Decoder` | Pull decoder to interleaved float at a requested rate and layout; exact seeking; growing files. |
| `Waveform.h` | `Waveform`, `WaveformColumn`, `OverviewColumn`, constants | The analysis result: 150 columns/s detail + 1 200-column overview. |
| `BandAnalyzer.h` | `BandAnalyzer` | Streaming LR4 3-band split + peak/RMS per column. Pure DSP. |
| `WaveformAnalyzer.h` | `analyzeWaveform()`, `ProgressiveAnalyzer`, `WaveformJob` | Chunked parallel analysis of complete files; progressive analysis of previews; async Qt job. |
| `AnalysisCache.h` | `AnalysisCache`, `CacheKey` | Two-tier on-disk cache (overview long-term, detail LRU), versioned, checksummed blobs. |
| `QualityCheck.h` | `analyzeQuality()`, `QualityReport` | Spectral cut-off with LAME lowpass cross-check; exact fake-24-bit and upsampling checks (A3). |

Internal code lives in `detail/` (`namespace selecta::audio::detail`): the MPEG, FLAC, WAV/AIFF and MP4 parsers and
frame indexes, the demuxers, the FFmpeg glue (custom `AVIOContext`, RAII wrappers, log routing), the output stage
(channel mapping and libsoxr resampling).

## Decoding design (A1)

### Byte sources

- `ByteSource::read()` never blocks and never returns bytes at or beyond `available()`. A decoder that needs more stops
  with `NeedMoreData`; nothing waits on a condition variable (contract: no blocking calls on the owner thread).
- `snapshot()` reads the state *before* the available count (acquire/release), so `Complete` always comes with the
  final count.
- `GrowingFile` holds one read-only handle; reads use `pread` (thread-safe, no shared file offset). The writer (core's
  transfer) writes the cache file itself, then calls `setAvailable(n)` after `write()` returned, then `setComplete()`
  or `setFailed()`.
- `subscribe()` registers a listener called after each change, on the writer's thread, outside any lock. The RAII
  `Subscription` waits for an in-flight call on reset, so a listener's captures can be destroyed right after.

### Demuxers: Selecta's own for MP3, FLAC, WAV, AIFF; FFmpeg's for the rest

FFmpeg's demuxers make tail reads on a seekable input (audio §5.3), and its parser-based demuxers (MP3, FLAC) read
ahead in raw 1 KB chunks with up to ten frames of look-ahead, report approximate packet positions, and turn a premature
EOF into a flushed, truncated frame. Gating them on a growing file needs per-format margins and a recovery path, and
their timestamps become unreliable after a manual byte seek. So:

- **MPEG audio (MP3/MP2/MP1), native FLAC, WAV and AIFF/AIFF-C** are parsed by Selecta's own demuxers. They build a
  frame index as bytes arrive (MPEG: header chain validated by the next header; FLAC: sync code + header CRC-8 + the
  expected sample number; PCM: arithmetic) and hand FFmpeg's *codecs* exactly one complete frame per packet (PCM is
  converted directly). Consequences: reads never pass `available()` by construction, seeking is exact (index + MP3
  preroll of 9 frames, FLAC/PCM frames are independent), positions are exact (sample counts from the index), and
  chunked parallel decoding lines up exactly.
- **MP4/M4A** goes through libavformat with a custom `AVIOContext` that is *non-seekable while the source grows*
  (switched to seekable when it completes) and `AVSEEK_SIZE` = the announced size. A file whose `moov` comes after
  `mdat` returns `NeedsCompleteFile` until complete. With `moov` first, every read is gated exactly: the next sample's
  byte range comes from FFmpeg's own sample index. A read that would still reach `available()` returns a non-sticky
  EOF and flags starvation; the decoder drops that packet and re-seeks to its exact position once data arrives.
- **Everything else** (Ogg, Opus, WMA, APE, WavPack, DSF, …) needs the complete file and uses libavformat with a
  seekable `AVIOContext`. Own-demuxer formats also fall back to it, for complete files only, when Selecta's parser
  can't index the file (e.g. free-format MP3).
- Probing: the container is detected from the first bytes by Selecta (so FFmpeg is told the format and doesn't probe);
  for unknown formats FFmpeg probes with `probesize` 1 MB and `analyzeduration` 1 s. Large ID3v2 tags and FLAC
  PICTURE blocks are skipped by size, never read; `DecoderOptions::maxHeaderBytes` bounds tags and `moov` (TooLarge).

### Gapless MP3

The MPEG decoders run with `AV_CODEC_FLAG2_SKIP_MANUAL`, so FFmpeg never trims; the decoder removes LAME's encoder
delay + 529 decoder samples at the start and the padding at the end itself, from its own parse of the LAME tag, using
FFmpeg's exact rule (`mp3dec.c`: tags from LAME, Lavf or Lavc; end trim only when the Xing frame count matches the
index). A LAME-encoded file therefore decodes to exactly its source length, sequentially or after any seek.

### Output stage

Codec output (any FFmpeg sample format, planar or packed) → float planar → channel mapping (mono `(L+R)/2`, stereo
duplicating mono, multichannel downmix with standard coefficients, or the source layout untouched) → libsoxr when the
output rate differs (HQ for playback, VHQ for transcoding), with its delay compensated so frame N of the output is frame
N × ratio of the source → interleaved float.

## Analysis (A2)

- `BandAnalyzer`: mono, LR4 crossovers at 200 Hz and 2.5 kHz (two cascaded RBJ Butterworth biquads each, eight
  biquads, transposed direct form II in double), peak and RMS per band per column, √-companded to `uint8`.
- Column c covers source frames `[floor(c·sr/150), floor((c+1)·sr/150))`: integer arithmetic, so chunk boundaries and
  progressive steps agree exactly.
- **Complete files:** `analyzeWaveform()` splits the stream into N chunks at column boundaries (N ≤ ideal thread count,
  ≥ 20 s each), clones the decoder per chunk (the frame index is shared), seeks 80 ms before the chunk for filter
  warm-up, and concatenates. The calling thread takes part in the work, so calling from a pool task can't deadlock.
- **Previews:** `ProgressiveAnalyzer` steps through what has arrived; `WaveformJob` re-arms on the source's
  notifications and reports new columns through queued signals.
- **Overview:** 1 200 columns derived from the detail by max of peaks.

## Cache format (A2)

Files under the directory the caller gives: `overview/<key>.swo` (3.6 KB payload) and `detail/<key>.swd`.
All integers little-endian:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `SLWO` (overview) or `SLWD` (detail) |
| 4 | 2 | blob format version (1) |
| 6 | 2 | `kAnalysisVersion` |
| 8 | 4 | sample rate |
| 12 | 8 | frame count |
| 20 | 2 | columns per second (150) |
| 22 | 2 | low/mid crossover, Hz |
| 24 | 2 | mid/high crossover, Hz |
| 26 | 2 | flags (bit 0: payload zlib-compressed) |
| 28 | 4 | column count |
| 32 | 4 | payload size in bytes |
| 36 | n | payload: overview = 1 200 × {peakL, peakM, peakH}; detail = columns × {peakL, peakM, peakH, rmsL, rmsM, rmsH}, zlib |
| 36+n | 4 | CRC-32 of bytes 0…35+n |

Decoding checks magic, both versions, the crossovers and column rate against the current constants, the column count
against a bound (24 h of audio), the payload size against the column count, the inflated size exactly, and the CRC.
Any mismatch is a miss; the file is deleted and the waveform recomputed. Keys: BLAKE2b-256 of size + first/last
256 KiB + 32 spread 4 KiB blocks (`CacheKey::forContent`), or of a name for provisional preview keys.

## Threading summary

| Class | Thread safety |
|---|---|
| `ByteSource` and subclasses | Thread-safe |
| `Decoder`, `BandAnalyzer`, `ProgressiveAnalyzer` | One thread at a time |
| `analyzeWaveform()`, `analyzeQuality()` | Reentrant, blocking (workers only) |
| `WaveformJob` | Lives in its creating thread (event loop); work on a `QThreadPool` |
| `AnalysisCache` | Thread-safe |
| `Waveform` | Immutable, implicitly shared |

## Integration points

### Player (head implementer, `app/`)

- Open: `auto src = GrowingFile::open(path, announcedSize)` (or `FileSource::open(path)`), then
  `Decoder d; d.open(src, {.sampleRate = deviceRate, .channels = 2})`. `NeedMoreData` → retry on the next source
  notification; `NeedsCompleteFile` → show "preview after download" and retry once complete.
- Pull: on the decoder thread, `d.read(buffer, frames)` into the ring buffer. `NeedMoreData` → show buffering, wait for
  the source's `subscribe()` callback (it fires on the transfer thread: post to the decoder thread), call `read()`
  again. `EndOfStream` → drain. `Error` → `d.error()`.
- Seek: `d.seek(frame)` (output frames). `NeedMoreData` = the target hasn't downloaded yet: show "waiting for data" at
  `d.position()`; the seek completes by itself on a later `read()` once the bytes are there.
- Buffered range overlay: `d.decodableFrames()` against `d.lengthFrames()`.
- The player has its own Decoder; the analysis has another. Neither blocks the other.

### Growing-file updates (core's transfer)

`GrowingFile::setAvailable(bytesWritten)` after each write (cheap: an atomic store and the listener calls),
`setComplete()` at the end, `setFailed()` on failure or cancel. Readers never see bytes beyond the published count.

### Waveform (app, core)

`WaveformJob job(src); job.setCache(&cache, key); job.start();` — `columnsAdded` while downloading, `finished` with the
complete `Waveform`, `failed` otherwise. For the library: `cache.load(key)` gives the overview even after the detail
was evicted.

### Arrival pipeline and converter (`convert/`)

`convert/` opens its own `Decoder` with `channels = 0` (source layout), and `sampleRate` set only when it resamples,
with `ResamplerQuality::VeryHigh`.

## Decisions (autonomous, for review)

| Decision | Why |
|---|---|
| Own demuxers for MP3/FLAC/WAV/AIFF instead of gating FFmpeg's | Exact, structural "never read past available"; exact seeks and positions; see above. FFmpeg's codecs still decode everything. |
| No blocking reads: `NeedMoreData` instead of waiting in the AVIO callback (audio §5.4 proposed a blocking wait) | Contract: no blocking calls on the owner thread; a pool thread must never sit waiting for a slow peer. |
| libsoxr for the decoder's resampling too (not libswresample) | One resampler everywhere; VHQ for transcodes, HQ for playback. |
| Cache key = BLAKE2b-256 of size + head + tail + 32 spread blocks | Content-based, reads ≤ 640 KiB whatever the size (audio §7.3 allows head + tail + size). |
| Overview tier bounded too (64 MiB default) | "Kept long-term" but nothing on disk unbounded. |
| TagLib requirement checked as ≥ 2.3 at build time | vcpkg's baseline ships 2.3.1; ADR-003 says ≥ 2.3.2. The contract says ≥ 2.3.x. Flagged to the head implementer. |

## Benchmarks

To be recorded here (Google Benchmark, `tests/audio/bench`).

## Progress

- **Done:** public headers (A1–A3), this document.
- **Next:** A1 implementation: byte sources, container probe, MPEG/FLAC/PCM indexes and demuxers, FFmpeg glue,
  decoder, tests with synthetic files.
- **Open questions:** none yet.
