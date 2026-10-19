# Audio preview and waveform — decoding, progressive playback, 3-band waveform, quality analysis

> Status: **draft in progress** (2026-10-19). Items marked **TODO** / *unverified* are from memory and not yet checked
> against a primary source. Confidence levels: **high** = read in source code/spec; **medium** = documentation or
> consistent secondary sources; **low** = inference or hearsay.

## TL;DR

- **Pioneer 3Band, answered (§2):** the **overview** is a single-sided **cumulative stack** (blue low at the bottom,
  amber mid on top of it, a white high crest on top). The **zoomed detail** view is **mirrored** about a centre line,
  with **overlaid** envelopes: each band is measured from the centre, the low∩mid overlap is drawn **brown**, and
  white highs go **in front at ≈ 85 % opacity**. Measured rekordbox colours: blue `#0055E1`, amber `#FFA600`, brown
  `#B36607`, white. Crossovers and height scaling are not public; Selecta should calibrate them against rekordbox 7.
- Rest: TODO (filled in at the end).

## 1. Key findings

- Pioneer stores 8 bits per band, 150 detail columns per second and a fixed 1,200-column overview (§2.3). That is a good
  model for Selecta's analysis cache.
- Further findings: TODO (filled in as sections are verified).

## 2. Pioneer "3Band" waveform (rekordbox / CDJ-3000) — how it looks and is layered

**Answer (priority question).** Pioneer layers the three bands differently in its two views:

| View | Shape | Layering | Draw order (back → front) | Confidence |
|---|---|---|---|---|
| **Overview** (whole track, "preview"; 1,200 columns) | **Single-sided**: grows up from a baseline at the bottom | **Cumulative stack**: blue spans `0…L`, amber spans `L…L+M`, white spans `L+M…L+M+H` | white (total height) → amber (`L+M`) → blue (`L`) — or simply draw three adjacent segments | **High** |
| **Detail** (zoomed, scrolling; 150 columns/s) | **Mirrored** about a horizontal centre line | **Overlaid**: each band is its own envelope measured from the centre line (not added) | larger of low/mid in its own colour → the overlap `0…min(L,M)` in a **dark amber/brown** → high in **white, slightly translucent, on top** | **Medium–high** (layering); **low** (exact scaling) |

So the PRD's "stacked" is right for the overview only. The detail view is the mock's "Layered" mode, with two
refinements: the low∩mid overlap gets its own brown colour (it is not simply amber covering blue), and white is drawn
in front at ~85 % opacity.

### 2.1 Evidence

- **Deep Symmetry analysis docs** (crate-digger `doc/modules/ROOT/pages/anlz.adoc`, commit `7c9d535`, mirrored in
  dysentery `track_metadata.adoc`, commit `f62a24b`): `PWV6` (3-band preview, in `ANLZ0000.2EX`) has 1,200 entries of 3
  bytes: "one-byte height values representing the mid-range, high, and low frequencies, in that order … they seem to
  be drawn stacked on top of each other, with the lows in dark blue, the mid-range in amber, and the highs in white."
  `PWV7` (3-band detail) has the same entry layout, one entry per half-frame = **150 entries per second**, and "a
  different kind of scaling … it seems to be non-linear."
- **dysentery issue #33** (GitHub, 2021–2025): nudge (2021-02-15) decoded `PWV6`: byte 0 = mid, byte 1 = high,
  byte 2 = low; "in the preview format, the bands appear to be stacked on one another", with fixed height coefficients
  low × 0.49, mid × 0.32, high × 0.25 (gist `eca9f07f…`, which draws blue `0…L`, orange `L…L+M`, white
  `L+M…L+M+H`, bottom-anchored). James Elliott (brunchboy, 2025-03-31) on the detail view: instead of stacking opaque
  colours as in the preview, draw "the mid first, opaque, then the high over that translucently, so we get mixing as
  seen in rekordbox, and then finally the low", then "I ended up having to do my own version of blending the low and
  mid-range colors and drawing the overlap separately"; "there is non-linear scaling of some sort involved for the
  detail waveforms" (unsolved).
- **beat-link** (`src/main/java/org/deepsymmetry/beatlink/data/`, commit `855d0bd`, 2026-10-07) — the most complete
  open implementation, rendered against CDJ-3000s:
  - `WaveformFinder.ThreeBandLayer`: LOW `rgb(32,83,217)` "dark blue"; LOW_AND_MIO (sic, = low+mid overlap)
    `rgb(169,107,39)` "brown"; MID `rgb(242,170,60)` "amber"; HIGH `rgb(255,255,255)`.
  - `WaveformPreview.segmentHeight()`: `low = b[2]·0.49`, `mid = low + b[0]·0.32`, `high = mid + b[1]·0.25`
    (cumulative). `WaveformPreviewComponent.updateWaveform()` draws each column bottom-up: HIGH to the total height, then
    MID, then LOW over it.
  - `WaveformDetail.segmentHeight()`: per band, not cumulative: `low = b[2]·0.4`, `mid = b[0]·0.3`, `high = b[1]·0.06`,
    averaged over `scale` entries per pixel column when zoomed out. `WaveformDetailComponent` (around line 1024) draws
    each column symmetric about `axis = height/2`: if `low > mid`, blue `±low` then brown `±mid`; else amber `±mid`
    (if different) then brown `±low`; finally white `±high`.
- **A real rekordbox 6 screenshot in 3Band mode** (attached to dysentery #33 by nudge, 2021). Sampling its pixels
  (screenshot colour space, so ±a few units):

  | Element | Measured colour |
  |---|---|
  | Overview low (blue) | `#0055E1` |
  | Overview mid (amber) | `#FFA600` |
  | Overview high | `#FFFFFF` |
  | Detail low (blue) | `#0053E1` |
  | Detail low∩mid overlap (brown) | `#B36607` |
  | Detail high | `#FFFFFF` where opaque; `#F5EBD7` over brown and `#FFF0D7` over amber, i.e. white at **≈ 85 % alpha** |

  In the detail view, kicks show as brown cores with blue flanks (low > mid); off-beat hats/claps as large white
  shapes; the bands are mirrored about the centre line. In the overview, blue sits at the bottom, a thinner amber band
  above it and a thin white crest on top.
- **Vendor material**: Pioneer's CDJ-3000 page says "3Band Waveform" shows the volume of each frequency band in
  different colours. AlphaTheta's FAQ lists which hardware can show BLUE / RGB / 3Band (CDJ-3000, OPUS-QUAD, XDJ-RX3,
  OMNIS-DUO, DDJ-FLX10, …). rekordbox sets it in Preferences › View › Waveform › Color. Tracks analysed before
  rekordbox 6 lack 3Band data until "Add New Analysis Data". The colour names (blue/amber/white) are community
  descriptions; no official colour values were found.

### 2.2 What is not known

- **Crossover frequencies and band filters.** Not documented anywhere. nudge's sine-chirp test (dysentery #33) shows
  broad, overlapping band responses: low falls off early, mid is wide, high rises slowly. The chirp's parameters are not
  stated, so no numbers can be derived. Selecta can measure them (see §12).
- **Height scaling.** Preview coefficients (0.49 / 0.32 / 0.25) are visual fits. Detail scaling is non-linear and
  unsolved. In the rekordbox screenshot white shapes are as tall as kicks, whereas beat-link's `high × 0.06` makes white
  a thin core. Treat beat-link's detail scaling as wrong for highs.
- **Value semantics.** Whether bytes are peak, RMS or loudness-weighted is unknown. rekordcrate (`src/anlz.rs`,
  `14d54ed`) names the `PWV6`/`PWV7` bytes bottom/mid/top third in file order, which contradicts the mid/high/low order
  above. Deep Symmetry's order is the one verified visually; rekordcrate's naming is probably wrong (medium confidence).
- **`PWVC`** (6 bytes, `.2EX`): rekordcrate calls it "per-band gain calibration" (`low_gain`, `mid_gain`,
  `high_gain`); dysentery calls it a vocal-detection configuration. Conflict unresolved. If it is a gain, rekordbox
  normalises band heights per track.

### 2.3 Reference: rekordbox ANLZ waveform tags

| Tag | File | Style | Entries | Entry format |
|---|---|---|---|---|
| `PWAV` | `.DAT` | Blue preview | 400 | 1 byte: height (5 bits) + whiteness (3 bits) |
| `PWV2` | `.DAT` | Tiny blue preview (CDJ-900) | 100 | 1 byte |
| `PWV3` | `.EXT` | Blue detail | 150/s | 1 byte: height (5 bits) + whiteness (3 bits) |
| `PWV4` | `.EXT` | RGB preview | 1,200 | 6 bytes |
| `PWV5` | `.EXT` | RGB detail | 150/s | 2 bytes: R, G, B (3 bits each) + height (5 bits) |
| `PWV6` | `.2EX` | 3-band preview | 1,200 | 3 bytes: mid, high, low |
| `PWV7` | `.2EX` | 3-band detail | 150/s | 3 bytes: mid, high, low |
| `PWVC` | `.2EX` | ? (per-band gain or vocal config) | — | 3 × u16 |

Takeaways for Selecta's analysis format: Pioneer stores **8 bits per band**, **150 columns per second** for the
detail view (one column = 1/150 s ≈ 294 samples at 44.1 kHz) and a **fixed 1,200-column** overview, and computes the
pixel heights at draw time.

## 3. Decoding options

Requirements: MP3, FLAC, WAV, AIFF, M4A (AAC/ALAC), ideally Ogg/Opus; custom I/O for a file that is still growing;
sample-accurate seeking; licence compatible with GPLv3; works on macOS first and builds elsewhere.

| Option | Licence | Formats | Custom / growing I/O | Seeking | Verdict |
|---|---|---|---|---|---|
| **FFmpeg** libavformat + libavcodec + libswresample | LGPL-2.1+ by default; GPL-2.0+ with `--enable-gpl`; `--enable-version3` makes it (L)GPLv3. All are fine for a GPLv3 app. | Everything | `AVIOContext` with read/seek callbacks. Demuxers make tail reads unless told the stream is not seekable (§5.3). | Demuxer-dependent: MP3 by Xing TOC or estimate; FLAC by seektable or a binary search that reads the file end. | **Recommended core** (already ADR-003's choice). Use libavcodec decoders everywhere. Use libavformat for complete files, and for growing files only with the precautions in §5.3. |
| Qt Multimedia `QMediaPlayer` (FFmpeg backend) | LGPLv3 / GPL | What its bundled FFmpeg supports | `setSourceDevice(QIODevice*)`. A growing `QFile` hits EOF early unless wrapped. | Opaque | **Not for preview.** No PCM tap for analysis, no control over buffering or tail reads, and coarse position. Fine for UI sounds. |
| libsndfile | LGPL-2.1+ | WAV, AIFF, FLAC, Ogg/Vorbis/Opus; MP3 since 1.1 (via mpg123/LAME) | `SF_VIRTUAL_IO` callbacks | PCM exact; FLAC/MP3 via its codecs | Viable for PCM formats, but adds little over FFmpeg. No AAC/ALAC. |
| dr_libs (`dr_mp3`, `dr_flac`, `dr_wav`) / miniaudio | Public domain (Unlicense) or MIT-0 | MP3, FLAC, WAV (miniaudio adds device I/O) | Read/seek callbacks; single-header C | `dr_mp3`: precomputed seek points or decode from start; `dr_flac`: seektable or search | Good fallback or fuzz-friendly alternative for the three core formats. No AIFF (dr_wav reads AIFF since v0.13, *unverified*), no AAC. |
| libmpg123 | LGPL-2.1 | MP3 | Feeder API (`mpg123_open_feed`/`mpg123_feed`) and `mpg123_replace_reader_handle`. Its native mode is "push bytes as they arrive". | Xing TOC, frame index, `MPG123_FUZZY` without a full scan; gapless via LAME tag | Best-in-class MP3 decoder with NEON. Consider it only if FFmpeg's MP3 decoder proves too slow. |
| libFLAC | BSD-3-Clause (library) | FLAC, Ogg FLAC | Stream decoder with read/seek/tell/length/eof callbacks; a read callback may block | Seektable, else binary search needing the file length | Reference decoder; FFmpeg's FLAC decoder is equivalent for our needs. |
| AudioToolbox (`AudioFileStream`, `AudioConverter`, `ExtAudioFile`) | Apple system framework | MP3, AAC, ALAC, WAV, AIFF, CAF; FLAC on macOS 10.13+ (*unverified* for `AudioFileStream`) | `AudioFileStreamParseBytes` is *designed* for progressive network streams: push bytes, receive packets | `AudioFileStreamSeek` maps packet → byte offset (flagged as estimated for VBR without a packet table) | macOS-only. Useful as a reference design. Not worth a second decode stack. |
| Mixxx `SoundSource*` (FFmpeg, CoreAudio, FLAC, MP3/libmad, sndfile, Opus, …) | GPL-2.0-or-later (compatible with GPLv3) | Many | Files only (`SoundSourceFFmpeg` opens by path) | `SoundSourceFFmpeg` does sample-accurate seeking with per-codec preroll | **Reference for seek and preroll logic** (`src/sources/soundsourceffmpeg.cpp`). Not a drop-in because it opens by path. |

Notes:
- FFmpeg's MP3 decoder (`mp3float`) and FLAC decoder are mature and fast. The cost lies in the demuxer layer's
  assumptions about seekable files (§5.3), not in decoding.
- Encoder delay/padding (MP3 LAME tag, AAC priming of 2112 samples) matters for exact durations and gapless playback, not
  for preview. FFmpeg exposes it via `AVCodecParameters::initial_padding` / skip-samples side data.

## 4. Playback engine and accurate position reporting

Goal: the playhead on a 120 Hz scrolling waveform must match what the user hears within about one frame (~8 ms).
Scrubbing must feel instant (< 50 ms from key press to sound).

| Option | Position accuracy | Notes |
|---|---|---|
| `QAudioSink` (pull mode, our `QIODevice`) | You know how many bytes the sink pulled (`processedUSecs()`, `bytesFree()`). **Device latency is not exposed**, so Bluetooth outputs (AirPods: ~150–250 ms, hearsay) put the playhead visibly ahead of the sound. | Cross-platform. OK as the Linux/Windows fallback. |
| **CoreAudio AUHAL** (AudioUnit render callback) | Each callback has an `AudioTimeStamp` (`mHostTime`, `mSampleTime`). Device latency = `kAudioDevicePropertyLatency` + `kAudioDevicePropertySafetyOffset` + `kAudioStreamPropertyLatency` + buffer frame size. | Most precise. Real-time callback: lock-free ring buffer only, no allocation or locks. |
| `AVAudioEngine` + `AVAudioSourceNode` | `outputNode.presentationLatency` plus the render timestamp | Simpler Objective-C++ API over the same HAL. |
| miniaudio device | Callback frame counts. Latency only partly exposed (*unverified*). | Single-header and cross-platform; could replace `QAudioSink` everywhere. |

**Recommended design (high confidence: it is the standard approach and what Mixxx does).**
- An `AudioOutput` interface with a CoreAudio implementation on macOS and a `QAudioSink` implementation elsewhere.
- The render callback pulls float frames from a single-producer/single-consumer ring buffer of about 100–200 ms. A
  decoder thread fills it.
- On each callback the audio thread publishes `{hostTime, framePosOfFirstSample, outputLatency, rate}` to a lock-free
  seqlock. The UI computes `position = framePos + (now + timeToNextVsync − hostTime − outputLatency) × rate` at each
  vsync and clamps it monotonic.
- Mixxx does exactly this: `src/waveform/visualplayposition.{h,cpp}` records the callback-entry time, the callback-to-
  DAC delay and the play position, then extrapolates to the next vsync (`calcOffsetAtNextVSync`).
- Seek: bump a generation counter, flush the ring, let the decoder seek and refill, and apply a 5 ms fade-in to avoid
  clicks. Scrubbing while dragging can play short (~40 ms) grains at the drag position (CDJ-like "touch" audio, P2).
- Output format: decode to float32 at the file's rate and resample once (libswresample or libsoxr) to the device rate.
  The analysis path uses the decoded audio before resampling.

## 5. Progressive preview of growing files

### 5.1 Facts that shape the design

- Soulseek transfers are sequential from an offset (the downloader sends `FileOffset`; see
  `soulseek-protocol.md`). A preview file is therefore always a **prefix `[0, received)`** of a file of known total
  size (the size is in the search result). Nothing arrives out of order, and the end of the file arrives last.
- So any decoder step that needs the *end* of the file (ID3v1/APEv2 tags, MP4 `moov` at the end, FFmpeg's "find last
  timestamp", WAV/AIFF chunks after the audio) **stalls until the download completes**. Requesting the tail first
  would need a second transfer of the same file from the same peer. That is unusual peer behaviour; do not do it.
- Total duration is needed early (overview width, progress mapping). Sources in order of reliability: header (Xing
  frames / FLAC STREAMINFO total samples / WAV `data` size / AIFF `numSampleFrames`) → search-result duration attribute
  → size ÷ bitrate.

### 5.2 Per format

| Format | Can start after | Duration known from | Seeking inside the downloaded prefix | Pitfalls |
|---|---|---|---|---|
| **MP3** | ID3v2 tag (size in its 10-byte header; **embedded art can make it several MB**) + first frames | Xing/Info frame count (VBR and LAME CBR), VBRI, else size × 8 ÷ bitrate (CBR) | Exact: build a **frame index** (offset, samples) by parsing frame headers as bytes arrive. Xing TOC gives only 1 % granularity. After a seek, decode about 1–2 frames early for the bit reservoir and discard the output. | Free-format and corrupt frames; false syncs inside ID3 or art (validate 2–3 consecutive headers); ID3v1/APE at the end. |
| **FLAC** | `fLaC` + metadata blocks (**PICTURE blocks can be MBs**) | STREAMINFO `total_samples` (0 = unknown) | Exact: index frames by sync code `0xFFF8/9` + header CRC-8 as data arrives (FLAC frame headers carry the frame/sample number). The SEEKTABLE (encoder default ~10 s) helps but may point past the prefix. | FFmpeg's seek without a seektable bisects over the whole file and reads the end. |
| **WAV** | `fmt ` + start of `data` (normally < 100 KB; `LIST`/`bext`/`JUNK` may precede `data`) | `data` size ÷ block align (0 or 0xFFFFFFFF in streamed files: use the transfer size) | Trivial: `dataStart + frame × blockAlign` | `WAVE_FORMAT_EXTENSIBLE`; 24-bit packed; float; trailing `id3 `/`LIST` chunks. |
| **AIFF/AIFC** | `COMM` + `SSND` header (`offset`, `blockSize`) | `COMM.numSampleFrames` | Trivial (big-endian PCM; AIFC `sowt` is little-endian) | `COMM` may legally come *after* `SSND`: then wait for the full file. `ID3 ` chunk may be at either end. |
| **M4A/MP4** (AAC, ALAC) | Only if `moov` precedes `mdat` ("fast start") | `moov`/`mvhd` / `mdhd` | Via the `stbl` sample tables (exact) | `moov` at the end (ffmpeg default without `-movflags +faststart`; common for files from various tools; iTunes/Apple Music files usually have it at the front, *medium confidence*) → **no progressive preview**; show "preview after download". |
| Ogg Vorbis/Opus | First pages + headers | Last granule position (end of file) → estimate from size until then | Bisection on granule positions inside the prefix | Opus pre-skip; chained streams. |

### 5.3 FFmpeg on a growing file: verified pitfalls (FFmpeg `f91b54f`, 2026-10-19)

| Demuxer | Behaviour when `pb->seekable & AVIO_SEEKABLE_NORMAL` | Source |
|---|---|---|
| MP3 | If no ID3v2 metadata was found, reads the last 128 bytes for ID3v1 (`ff_id3v1_read`), i.e. seeks to `filesize − 128` | `libavformat/mp3dec.c` `mp3_read_header` (~L482); `libavformat/id3v1.c` `ff_id3v1_read` |
| WAV | After `data`, keeps parsing chunks *after* the audio ("footer metadata"), i.e. skips to `data_end`. If not seekable it stops at `data`. | `libavformat/wavdec.c` (~L462–489) |
| AIFF | On `SSND`, if seekable it `avio_skip`s the whole sound data to parse later chunks. If not seekable and `COMM` was seen, it starts at once. | `libavformat/aiffdec.c` (~L306–319) |
| FLAC | `flac_seek` uses the seektable only with `AVFMT_FLAG_FAST_SEEK`. Otherwise it falls back to `ff_seek_frame_binary` → `ff_find_last_ts`, which reads near `filesize − 1`, unless index entries bracket the target. | `libavformat/flacdec.c` `flac_seek`; `libavformat/seek.c` |
| MOV/MP4 | With `moov` after `mdat`, it skips `mdat` to find `moov` | `libavformat/mov.c` |

`avio_seek` with `seekable == 0` still works for our needs. Forward seeks read through the buffer, and backward seeks
call the seek callback when one is set (`libavformat/aviobuf.c` ~L278–300). A read error other than `AVERROR_EOF` is
**sticky** (`s->error`), whereas EOF is cleared by the next seek (`fill_buffer`, ~L551–558).

### 5.4 Safe growing-file reader (proposed)

- **`PreviewFile`** (shared by transfer and player): `totalSize` (from the search result), `available` (atomic u64,
  published *after* `write()` returns, release/acquire), `state` {downloading, complete, failed, cancelled}, and a
  condition variable signalled on each append or state change. The transfer writes into a pre-sized cache file. The
  reader opens its **own read-only fd** and uses `pread`. POSIX guarantees that a completed `write()` is visible to
  readers of the same file.
- **`read(pos, n)`**: if `pos + n ≤ available`, return at once. If complete, return a short read / EOF at `totalSize`.
  Otherwise **block** on the condition variable with a timeout and a cancel token. The UI shows "buffering" when the
  playback ring runs dry. Return `AVERROR_EXIT` on cancel, and **never** a premature EOF during playback.
- **FFmpeg adapter**: an `AVIOContext` with `seekable = 0` while downloading (switch to `AVIO_SEEKABLE_NORMAL` when
  complete). `AVSEEK_SIZE` → `totalSize`, so CBR duration and the MP3 Xing size check work. Read buffer 32–64 KB.
  `avformat_find_stream_info` is limited (`probesize` ≈ 1 MB, `analyzeduration` ≈ 1 s) and must skip past large
  ID3v2/PICTURE blocks.
- **Seeking** uses Selecta's **own index** built while bytes arrive (MP3 frame index, FLAC frame index, PCM arithmetic).
  - Targets inside the prefix: `avio_seek` to the indexed byte offset, `avcodec_flush_buffers`, then decode and discard up
    to the exact sample, with ~2 frames of MP3 preroll. For FLAC, `av_add_index_entry` from our index also prevents
    FFmpeg's tail-reading binary search.
  - Targets beyond the prefix: show a "waiting for data" state at the target and resume when `available` passes the
    estimated byte offset (PREV-3).
- **One demux/decode pass for analysis** runs ahead as data arrives and feeds the waveform analyser (§7) and the indexer.
  The **player has its own decoder instance**. Neither blocks the other.
- **Fallback (per PLAN risk table)**: in M5, ship "decode when complete" first and turn on progressive per format behind
  a flag: WAV/AIFF → MP3 → FLAC → M4A fast-start.
- **Robustness**: corrupt or hostile files come from strangers. Fuzz the index builders. Run decoding outside the UI
  thread with caps on allocation sizes (ID3 frame sizes, PICTURE sizes, FLAC block sizes). A decoder error ends the
  preview, never the app.

## 6. Mixxx waveform implementation (filtered / RGB / stacked)

Read at mixxx `44dc9ed` (2026-10-16, "2.7-alpha"), licence **GPL-2.0-or-later**. Code can be reused in a GPLv3 program
with copyright notices kept.

**Analysis** (`src/analyzer/analyzerwaveform.{h,cpp}`):
- **Filters:** `EngineFilterBessel4Low` at **600 Hz**, `EngineFilterBessel4Band` **600–4,000 Hz**,
  `EngineFilterBessel4High` at **4,000 Hz** (`kLowMidFreqHz`, `kMidHighFreqHz`). An 8th-order Butterworth variant is
  commented out. The filters are "settled" on silence to avoid a start-up ramp (`assumeSettled`, Mixxx issue #7776).
  Coefficients come from fidlib (`src/engine/filters/`).
- **Detail waveform:** a fixed **441 visual samples per second**, *per channel* (L and R kept separately), with
  `WaveformData = {low, mid, high, all}` as `uint8` plus up to 4 stem bytes. Each value is the **peak** (`max |x|`)
  over the stride (~100 samples at 44.1 kHz), stored linearly as `min(255, 255·peak + 0.5)`. There is no log or power
  scaling at analysis time. The code has a commented-out RMS alternative, and the protobuf labels the values "RMS"
  even though they are peaks.
- **Overview ("summary"):** **2 × 1,920 = 3,840** points per channel over the whole track ("two visual samples per
  pixel in full width overview in full HD"). Each point is the **average of the detail-stride peaks** it covers
  (`WaveformStride::averageStore`), so the overview is smoother than a max-reduction.
- **Progressive:** the analyser marks completion as it goes (`setCompletion`), and the renderers draw what is
  complete. The same pattern suits a growing download.
- **Storage** (`src/waveform/waveform.cpp`, `src/proto/waveform.proto`, `src/library/dao/analysisdao.cpp`): protobuf →
  `qCompress` (zlib, default level) → one file per analysis under the Mixxx analysis directory, indexed in SQLite
  (`track_analysis`), with a version string per analysis type (`WAVEFORM_CURRENT_VERSION`). A 2012 comment says a
  1.2 MB detail waveform compresses to ~600 KB.

**Rendering** (`src/waveform/renderers/allshader/`):
- **Filtered** (`waveformrendererfiltered.cpp`): for each device pixel, the max of each band over the visual samples
  under that pixel. Bands are multiplied by user "visual gain" and the deck EQ gains. Each band is drawn as a rectangle
  about the axis (top = left channel, bottom = right channel), **overlaid** in the order low, mid, high: overlaid
  layering like Pioneer's detail view, but without an overlap colour.
- **RGB** (`waveformrendererrgb.cpp`): colour = `low·lowColour + mid·midColour + high·highColour`, normalised by the
  largest component. The height follows the "all" signal scaled by the EQ gain.
- **"Stacked"**: in the allshader geometry path, `WaveformWidgetType::Stacked` is just the filtered renderer with RGB
  colours (overlaid). The textured path uses `res/shaders/stackedsignal.frag`, which is truly **cumulative** (low from
  0, mid above low, high above both) with unscaled "ghost" bands drawn at 60 % alpha. Mixxx's naming is
  inconsistent.
- The geometry is rebuilt every frame from the uint8 data. This is cheap on a GPU, and Mixxx renders at vsync through
  `VSyncThread` with `VisualPlayPosition` for latency-compensated positions (§4).

What to take from Mixxx: the peak-per-stride `uint8` model, the summary-by-averaging idea, the versioned zlib cache,
the progressive completion counter and `VisualPlayPosition`. What not to copy: 441 Hz × 2 channels is more than Selecta
needs (Pioneer uses 150 Hz mono), and fidlib's runtime filter design is unnecessary when fixed biquads will do.

## 7. Recommended analysis pipeline for Selecta

### 7.1 Signal path (per track; runs as bytes arrive)

1. Decode to float32 at the native rate. Mix to mono `(L+R)/2`. Pioneer's 3Band is mono, and the mirrored display does
   not need L/R.
2. Split into 3 bands with **Linkwitz–Riley 4th-order (LR4) crossovers**: each crossover is two cascaded 2nd-order
   Butterworth biquads (RBJ cookbook). LR4 bands sum to a flat magnitude, so "how much is in each band" stays honest,
   and the slopes (24 dB/oct) are steep enough that the colours do not bleed. Mixxx's 4th-order Bessel has gentler
   transitions, which mixes the bands more.
3. **Default crossovers: 200 Hz and 2.5 kHz** (low confidence; tune on the mock and against rekordbox). Context: Mixxx
   uses 600 Hz / 4 kHz. A community sine-sweep test of rekordbox (bbc/audiowaveform #210, 2025) estimated a blue low
   band rolling off around ~110 Hz, orange mids around 180–800 Hz and white highs from ~2.5 kHz, with odd gaps,
   i.e. not plain crossovers. A third-party "rekordbox-style" generator (mixgraph.io) uses 200 Hz / 2 kHz. Keep the
   crossovers as named constants and calibrate them (§12).
4. Per **column of 1/150 s** (294 samples at 44.1 kHz, 320 at 48 kHz: the Pioneer detail rate), compute per band:
   - **peak** = `max |x|` (transients: kicks and hats read as sharp shapes), and
   - **RMS** = `sqrt(mean x²)` (body/energy). Store both. Peak drives the shape. RMS can drive a slightly darker "inner
     body" like Pioneer's RGB/blue waveforms, and is better for perceptual tuning later.
5. Quantise each value to `uint8` with a **perceptual (compressive) curve** at *draw time*, not analysis time. Store
   linear amplitude ×255 with a √ curve (`q = round(255·sqrt(min(1, a)))`) so quiet passages keep resolution, and map
   to pixels with a configurable dB curve (default: −48 dB → 0, 0 dBFS → full height). Pioneer's detail scaling is
   "non-linear" (§2.2), and a linear peak scale makes quiet intros invisible.
6. A **per-track gain** for display: normalise so that the 99th percentile of the overall peak maps to ~90 % of the
   height (cf. the `PWVC` "per-band gain" hypothesis). This makes quiet and loud masters comparable and is applied at
   draw time.

### 7.2 Resolutions and overview

| Level | Columns | Use |
|---|---|---|
| Detail | 150 per second (54,000 for 6 min) | Zoomed view: 1 column ≈ 1 device pixel at the default zoom, which shows ~6–10 s across ~1,000 pt (2× DPR) |
| Mip levels | ÷2, ÷4, … (max for peak, mean for RMS) | Zooming out without aliasing (a GPU texture mip chain or a CPU pyramid) |
| Overview | Computed at draw time from the detail data for the actual pixel width (≈ 1,000–3,000 px) | Pioneer stores a fixed 1,200 columns. Deriving from detail data avoids a second analysis and fits any width. |

Overview reduction: **max of peaks per pixel** keeps transients visible. Mixxx averages, which looks smoother but hides
drops. Proposed: max of peaks for the shape, mean of RMS for the body.

### 7.3 Cache format (analysis cache, CACHE-1 / PREV-8)

- Key: **content hash** of the audio file. For a preview, before the file is complete: `(peer, remote path, size)` →
  rekeyed when complete. The hash should be a fast non-cryptographic one (e.g. XXH3-128) over the whole file, or
  head + tail + size for speed (decide in ADR-007).
- Value: a small header `{magic, analysisVersion, sampleRate, totalFrames, columnRate=150, crossovers, flags}` + a
  column array `[peakL, peakM, peakH, rmsL, rmsM, rmsH]` as `uint8` (6 bytes/column) → **~324 KB per 6-minute track
  raw**, ~100–200 KB with zstd/zlib. Optional spectrum summary (§9) adds a few KB.
- That is more than PREV-8's "a few KB per track". For "a few KB", store only an overview (e.g. 1,200 × 3 bytes ≈
  3.6 KB) for the library and keep full detail for previews and recent tracks under an LRU cap. Decide in ADR-007.
- Version the format. Bump `analysisVersion` when filters, crossovers or the column rate change, and re-analyse
  lazily.

### 7.4 Performance (measured on this machine: Apple M2 Pro, 8 P-cores; FFmpeg 9.0.2 CLI; synthetic 6-min stereo 44.1 kHz)

| Step (6-minute track) | Wall time | How measured |
|---|---|---|
| Decode MP3 320 CBR | ~330 ms (one stream) | `ffmpeg -benchmark -i six320.mp3 -f null -` |
| Decode MP3 V0 | ~285 ms | same |
| Decode FLAC 16/44.1 | ~180 ms | same |
| Decode AAC 256 (M4A) | ~245 ms | same |
| Read WAV | ~30 ms | same |
| Demux only (MP3, FLAC) | ~20–25 ms | `-c copy -f null -` |
| **8 parallel segments** (45 s each), MP3 320 | **~130 ms** | 8 `ffmpeg -ss … -t 45` processes, including process start-up |
| 8 parallel segments, FLAC | ~80 ms | same |
| 3-band LR4 split (8 biquads) + peak/RMS per 1/150 s, scalar C++ `-O2`, 1 thread | **~150 ms** | own micro-benchmark (`bandbench.cpp`, not in repo) |

The FFmpeg CLI figures include CLI overhead, so library decode is slightly faster.

Conclusions:
- A naive sequential decode + analyse of a 6-minute MP3 takes **~450 ms** and **misses the 300 ms target** (NFR-2).
- Meet it with **chunked parallelism**: split the file into N byte ranges at frame boundaries (MP3: from our frame
  index; FLAC: frame sync), decode each with ~2 frames of preroll (MP3 bit reservoir), and analyse each chunk with
  ~50 ms of filter warm-up overlap. Then concatenate the columns. Expect **~80–120 ms** on 6–8 cores.
- Further gains if needed: process the 4 biquad chains as one NEON `float32x4` lane set, or run the band analysis at
  half rate for the low band.
- For **previews** the budget hardly matters: analysis runs *during* the download, which is far slower than decoding,
  so when the last byte arrives only the last chunk remains (< 20 ms). The 300 ms target matters for files already on
  disk (Inbox, collection, re-opened previews with an evicted analysis cache).
- Run analysis on a worker pool at background QoS, but at **user-interactive priority for the track being
  previewed**.

## 8. Rendering in Qt (Widgets / Qt Quick / QRhi) at 60–120 fps

TODO.

## 9. Fake-lossless / transcode detection and spectrograms

TODO.

## 10. Optional BPM / key detection libraries and licences

TODO.

## 11. Implications for Selecta

TODO.

## 12. Open questions and things to verify experimentally (local test server)

TODO.

## 13. Sources

All accessed 2026-10-19. TODO.
