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

Codec output (any FFmpeg sample format, planar or packed) → interleaved float → channel mapping (mono `(L+R)/2`,
stereo duplicating mono, surround downmixed with -3 dB centre and surrounds, normalised; or the source layout
untouched) → libsoxr when the output rate differs (HQ for playback, VHQ for transcoding; libsoxr's output is aligned
with its input, verified with an impulse test) → a FIFO serving `read()`. Gapless trimming and seek targets work on the
raw timeline before this stage; timeline gaps (frames a corrupt stream lost) are filled with silence up to 10 s so
positions stay true.

## Analysis (A2)

- `BandAnalyzer`: mono, LR4 crossovers at 200 Hz and 2.5 kHz (two cascaded RBJ Butterworth biquads each, eight
  biquads, transposed direct form II in double), peak and RMS per band per column, √-companded to `uint8`.
- Column c covers source frames `[floor(c·sr/150), floor((c+1)·sr/150))`: integer arithmetic, so chunk boundaries and
  progressive steps agree exactly.
- **Complete files:** `analyzeWaveform()` splits the stream into N chunks at column boundaries (N ≤ ideal thread count,
  ≥ 20 s each), clones the decoder per chunk (the frame index is shared), seeks 80 ms before the chunk for filter
  warm-up, and concatenates. The calling thread takes chunks too, so calling from a pool task can't deadlock; tasks
  that start late find no chunk left. The result is byte-identical to a sequential analysis (tested on WAV, FLAC and
  MP3: the MP3 preroll makes post-seek decoding bit-exact, and 80 ms of warm-up leaves the filters' memory far below
  8-bit resolution). The first failing chunk stops the others; a real error wins over the cancellations it caused.
- **Previews:** `ProgressiveAnalyzer::step()` analyses what has arrived (≤ 30 s of audio per step, so cancellation is
  quick). `WaveformJob` runs steps on the pool one at a time, re-armed by the source's notifications (coalesced: a
  notification during a step schedules one more step), and reports `columnsAdded` / `waitingForCompleteFile` /
  `finished` / `failed`. The final result equals `analyzeWaveform()` on the finished file (tested).
- **Overview:** 1 200 columns derived from the detail by max of peaks; `reduceColumns()` gives any other width (max of
  peaks, mean of RMS) for mip levels and the pixel-exact overview.
- **Job plumbing:** worker results reach the job through a mutex-protected mailbox and a data-free queued call
  (`drainMailbox`), never inside queued lambdas: Qt here isn't built with ThreadSanitizer, so data carried through its
  event queue would show up as races. Pool hand-offs go through `detail::startTask`, which tells ThreadSanitizer about
  the hand-over (`__tsan_release`/`__tsan_acquire`); every lock in the library is a `std::` one for the same reason.

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

`WaveformJob job(src); job.setCache(cache, key); job.start();` (`cache` is a `std::shared_ptr<AnalysisCache>`; work may
outlive a destroyed job) — `columnsAdded` while downloading, `waitingForCompleteFile` for M4A with `moov` at the end,
`finished` with the complete `Waveform`, `failed` otherwise. A cache hit (detail included) finishes without decoding.
For previews use a provisional `CacheKey::forName("peer|path|size")`; store the finished waveform under
`CacheKey::forContent(source)` as well when the file is kept. For the library: `cache.load(key)` gives the overview even
after the detail was evicted.

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
| TagLib ≥ 2.3.2, checked at build time | The head implementer added the 2.3.2 overlay port (crafted-input hardening). |
| `std::mutex`/`std::condition_variable`, mailbox delivery and annotated pool hand-offs | ThreadSanitizer can't see inside the static Qt (see Analysis); this keeps TSan runs meaningful for this library. |

## Benchmarks

`tests/audio/bench/AnalysisBenchmark.cpp` (Google Benchmark), Release build, Apple M2 Pro (8 P + 4 E cores),
2026-10-20, with other builds running (load average ≈ 10, so these are pessimistic). Six minutes of synthetic stereo
44.1 kHz audio (tones over white noise, 16-bit), decoded from memory:

| Benchmark | Time |
|---|---|
| Decode only, MP3 320 CBR (mono output) | 282 ms |
| Decode only, FLAC 16-bit (noise-heavy, so near-incompressible) | 392 ms |
| Decode only, WAV 16-bit | 37 ms |
| 3-band split only (8 biquads + peak/RMS, one thread) | 139 ms |
| **Analysis, MP3 320, parallel** | **53–84 ms** (target < 300 ms) |
| Analysis, MP3 320, one thread | 591 ms |
| Analysis, FLAC 16-bit, parallel | 99 ms |
| Analysis, FLAC 16-bit, one thread | 723 ms |
| Analysis, WAV 16-bit, parallel | 24 ms |

Reproduce: configure a Release tree with tests on, build `selecta_audio_bench`, run it with `--benchmark_min_time=1s`.
The CLI gives a quick check on any file: `selecta-cli media-analyze <file>`.

## Progress

- **Done:** A1 (decoder, merged into main), A2 (band analyzer, chunked parallel and progressive analysis,
  `WaveformJob`, two-tier cache, benchmark, `media-analyze`). 96 audio tests; dev, asan, tsan and lint clean.
- **Changed for C2:** `StreamInfo::bitsPerSample` for ALAC comes from the opened decoder (the depth is in the magic
  cookie, not in the container's sample entry); `DecoderStats::truncated` is also set when a complete FLAC holds fewer
  samples than STREAMINFO announced, or a LAME-tagged MP3 fewer frames than the tag counts (an interrupted download
  cut between frames).
- **Next:** C3 in `convert/`, then A3 (quality checks).
- **Open questions:** none.
