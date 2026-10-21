# Contract: `audio/` and `convert/`, decoding, analysis and conversion

| | |
|---|---|
| **Owner** | media library agent, branch `lib/media` |
| **Integrator** | head implementer (the player and waveform widget in `app/`, the Inbox arrival pipeline in `core/`) |
| **Spec** | `references/audio-preview-and-waveform.md` (§2 look, §3–5 decoding and growing files, §7 analysis, §9 quality checks, §11 defaults); `references/conversion-and-metadata.md` (§2 encoding, §3–4 tags, §5 artwork, §6 warning matrix, §9 module design) |
| **Decisions** | ADR-003 (stack), ADR-007 (waveform), ADR-009 (file safety), PRD D29 (convert and tag on arrival), D30 (Spotify tags), CONV-1…8, PREV-1/2/8/9 |

Build with `-DSELECTA_WITH_MEDIA=ON`: FFmpeg (avcodec, avformat, swresample), libmp3lame, libsoxr and TagLib, all
static from vcpkg.

## Purpose and boundary

- **`audio/`** decodes audio, including files still being downloaded, analyses it into the 3-band waveform, and
  caches the analysis. Playback output and the waveform widget belong to the head implementer: you provide decoded
  PCM and analysis data, not UI.
- **`convert/`** turns a source file plus a metadata model into a verified output file:
  - MP3 320 CBR with a genuine LAME Info tag (default), AIFF, FLAC or WAV;
  - every tag written by TagLib with the ID3v2.3 fix-ups;
  - the cover processed to baseline JPEG ≤ 800 px;
  - the quality advisor deciding: convert, keep as-is, or ask.
- **No file-system policy:** you write only to the output path the caller gives, through a temporary file. Publishing
  (rename into the Inbox, trashing the original) belongs to `core/` FileOps (ADR-009).
- **Every downloaded file is untrusted input:**
  - cap image dimensions and bytes before decoding;
  - require TagLib ≥ 2.3.x;
  - handle truncated and corrupt files without crashing (fuzz the probing).

## Threading

Decoding, analysis and conversion are CPU work and run on worker threads (`QThreadPool` or `QtConcurrent`). Core
classes are thread-confined or pure; jobs report progress and results with queued signals and support cancellation.
A `Decoder` instance is used by one thread at a time. Document thread safety per class.

## Components and deliverables

### A1. Decoder
- FFmpeg via a custom `AVIOContext`.
- A complete file is seekable. For a **growing file** (a preview still downloading), report the input as
  **non-seekable** until it is complete, and keep Selecta's own frame and byte index for seeking (§5.3–5.4). Never read
  beyond the bytes available.
- Output float PCM at a requested rate; stereo for playback, mono for analysis.
- Small probe sizes (1 MB / 1 s), skipping large ID3v2 or PICTURE blocks.
- **Tests:**
  - a test `AVIOContext` that logs read offsets and proves no read goes past "available" while non-seekable;
  - synthetic files in each format (generate at test time with FFmpeg or LAME; no copyrighted audio);
  - truncation, garbage, a zero-length file, MP3 with large ID3, WAV with `LIST` before `data`, AIFF with `COMM`
    after `SSND`, M4A with `moov` at the end (must say "needs complete file").

### A2. Analysis and cache
- 3-band analysis:
  - LR4 crossovers at 200 Hz and 2.5 kHz (named constants), mono;
  - peak and RMS per band at 150 columns/s, √-companded `uint8`;
  - the overview derived from the detail.
- Chunked and parallel for complete files (target < 300 ms for a 6-minute MP3 on Apple Silicon; benchmark it with
  Google Benchmark); progressive while a preview downloads.
- Cache: a versioned binary blob keyed by content hash. The overview (~3.6 KB) is kept long-term; the detail lives in
  an LRU bounded by bytes. Validate on load: corrupt or old versions are recomputed.
- **Tests:** tones at 60 Hz, 1 kHz and 8 kHz land in the right band; silence; clipping; determinism; cache
  round-trip and corruption handling.

### A3. Quality checks (P1, after A1–A2)
- Spectral cut-off with a LAME-tag lowpass cross-check.
- Exact fake-24-bit and upsampling detection.
- Results labelled "likely …", never "fake" (§9).

### C1. Metadata model, tag reader and writer
- `MetadataModel`: canonical fields, multiple values, pictures, and per-field provenance.
- Reader: TagLib properties and complex properties, with the alias table of conversion §9.1.
- Writer: per format, as in conversion §9.1:
  - MP3 ID3v2.3 with UTF-16 text, Latin-1 APIC description, one front cover, no ID3v1 or APE;
  - remap TMOO, TSST, TDRL, TDTG and TPRO to `TXXX` for v2.3, and join multiple values with ", " for artists;
  - AIFF with an ID3 chunk;
  - FLAC with Vorbis comments and a PICTURE block;
  - WAV with RIFF INFO plus an ID3 chunk.
- A deny-list strips other DJ apps' private data on transcode (D-11).
- Label goes to `TPUB`, ISRC to `TSRC`, and the source link to `WOAS`/`WXXX` (D30).
- **Tests:** golden round-trips per format; reading back with an independent TagLib path; v2.3 compensation cases.

### C2. Transcoder and verifier
- Decode, then libsoxr resampling only when needed, TPDF dither only to 16-bit, then encode:
  - libmp3lame called **directly**: CBR 320, joint stereo, `-q 3`, with `lame_get_lametag_frame()` written into a
    genuine Info header;
  - FFmpeg FLAC and ALAC;
  - FFmpeg AIFF muxer with big-endian PCM;
  - Selecta's own plain PCM WAV writer.
- A cancellable job with progress, running in a worker.
- `OutputVerifier`: decodes fully, duration within tolerance, tags and artwork present. On failure the temporary file
  is deleted and an error returned.
- **Tests:** FLAC → MP3 320 (LAME tag present and parsed back), 24/96 → MP3 (44.1 kHz), → AIFF, cancellation
  half-way, a disk-full simulation (an output device that fails writes).

### C3. Quality advisor and artwork
- `QualityAdvisor`: the warning matrix of conversion §6.2. Never upsample automatically. Lossy → lossy or lossy →
  lossless only with explicit confirmation. A lossy source below the target is kept with a "below target" flag.
- `ArtworkProcessor`: `QImageReader` with allocation limits, then baseline sRGB JPEG at quality 90, ≤ 800 px, a square
  crop only for ratios within 0.9–1.1, and never upscaling.
- **Tests:** a decision table; CMYK, progressive, PNG and huge images.

## Acceptance

- Every component has tests for its failure paths; dev and asan are clean; lint is clean; the benchmarks are
  recorded in `DESIGN.md`.
- Demo commands in `tools/selecta-cli/commands/audio/` and `…/convert/`: `media-analyze <file>` prints the analysis summary and timing;
  `media-convert <in> <out.mp3> [--title … --artist … --cover img]` produces a verified, tagged MP3 that rekordbox can
  read. The maintainer checks rekordbox by hand later.
- `DESIGN.md`: classes, threading, cache format, and the integration points for the player (PCM pull API, seek, the
  growing-file "available bytes" updates) and for the arrival pipeline.
