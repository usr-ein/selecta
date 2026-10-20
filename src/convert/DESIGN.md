# `convert/` — design

Turns a source file plus a metadata model into a verified output file. Contract: `docs/contracts/media.md` (C1–C3).
Spec: `references/conversion-and-metadata.md` (§2 encoding, §3–4 tags, §5 artwork, §6 warning matrix, §9 module
design), ADR-003, ADR-009, PRD D29/D30, CONV-1…8.

## Classes

| Header | Class / function | Role |
|---|---|---|
| `MetadataModel.h` | `MetadataModel`, `Picture`, `Provenance`, `keys::` | Canonical fields (TagLib property keys), multiple values, pictures, per-field provenance. |
| `TagReader.h` | `readTags()`, `canonicalTagKey()` | TagLib properties + PICTURE complex property, read-only stream, alias table, bounds. |
| `TagPolicy.h` | `applyTagPolicy()`, `isDeniedTagKey()` | Deny-list (D-7, D-11), TENC/TSSE set by Selecta. |
| `TagWriter.h` | `writeTags()`, `id3v23View()`, `tagsAsWritten()` | Per-format writing with the ID3v2.3 fix-ups; the exact read-back, predicted in memory. |
| `EncodeSettings.h` | `EncodeSettings`, `resolveOutput()`, `checkSettings()` | Output format and the rules for rate, word length and dither (shared by transcoder and advisor). |
| `Transcoder.h` | `transcode()`, `TranscodeJob` | Decode → soxr → TPDF → encode → tag → verify → rename. Cancellable, with progress. |
| `OutputVerifier.h` | `verifyOutput()`, `pcmDigest()` | Full decode, length, PCM digest, MP3 frame bitrates and LAME tag, tags and artwork. |
| `QualityAdvisor.h` | `advise()`, `makeSourceQuality()`, `probeSourceQuality()`, `describeNotice()`, `judgeQuality()`, `describeSource()` | The warning matrix of §6.2 and its UI copy; the per-file verdict for the inspector. |
| `ArtworkProcessor.h` | `processArtwork()`, `inspectImage()` | Baseline sRGB JPEG q90 ≤ 800 px, near-square crop, never upscaled, bounded decoding. |
| `ArtworkFinder.h` | `chooseFolderCover()` | Folder cover ranking (§5.1), pure. |

Internal (`detail/`):

| File | Role |
|---|---|
| `Encoder.h` | The `Encoder` interface (float in, `write()`/`finish()`) and its factories. |
| `LameEncoder.cpp` | libmp3lame driven directly; the LAME tag frame written over LAME's placeholder by `finish()`. |
| `FfmpegEncoder.cpp` | FLAC, ALAC (`ipod` muxer) and AIFF (`pcm_s16be`/`pcm_s24be`) through a write `AVIOContext` over the sink. |
| `WavEncoder.cpp` | The `WAVE_FORMAT_PCM` writer: 44-byte header, sizes patched in `finish()`. |
| `Quantizer.h` | Float → 16/24-bit integers, TPDF ±1 LSB or plain rounding, deterministic noise. |
| `OutputFile.h` | `TempOutput` (the `.selecta-part` file), `Sink` (writes with DiskFull/WriteFailed classification), `renameExclusive()`. |
| `PcmHash.h` | The byte stream `pcmDigest()` hashes (float32 little-endian). |
| `TranscodeSeam.h` | `transcode()` with a device wrapper: the tests' full-disk simulation. |
| `TagLibUtil.h` | Qt ↔ TagLib conversions, container detection, readTags()'s property-map conversion. |

## Pipeline

```
source ─▶ audio::Decoder (float, source layout; soxr VHQ only when the rate changes; > 2 channels downmixed)
       ─▶ quantise: TPDF dither to 16-bit when the audio isn't on the 16-bit grid, plain rounding otherwise
       ─▶ encoder: LAME (MP3) | FFmpeg flac | FFmpeg alac in ipod | FFmpeg aiff (pcm_s16be/s24be) | own WAV writer
       ─▶ <output>.<random>.selecta-part (same folder, created exclusively, unbuffered)
       ─▶ TagWriter (TagLib, from the policy-applied model)
       ─▶ OutputVerifier (full decode; bit-exact digest for lossless at the source's rate, depth and channels)
       ─▶ fsync ─▶ rename to <output> without replacing (renamex_np RENAME_EXCL, renameat2 RENAME_NOREPLACE,
          link + unlink, MoveFileEx)
```

KeepAudio (the audio-preserving path, §9.4): copy the source bytes into the temporary file, rewrite its tags, verify
that the decoded PCM digest and length equal the source's, rename. Any failure or cancellation deletes the temporary
file; the source is only ever opened read-only.

## Rules carried from the research

- MP3: libmp3lame directly, CBR 320 (default), joint stereo, `-q 3`, float input, `lame_set_bWriteVbrTag(1)`, and
  `lame_get_lametag_frame()` written over the first frame after `lame_encode_flush()`: a genuine LAME Info tag with a
  valid CRC (no `Lavf` header, no 26 ms shift). `lame_init()`/`lame_init_params()` serialised behind a mutex.
- Rates: MP3 keeps 32/44.1/48 kHz, anything else becomes 44.1 kHz (D-4). Lossless keeps the source rate up to
  `maxSampleRate` (96 kHz, D-3). Never upsampled automatically.
- Word length: lossless keeps the source depth (16 or 24; 32-bit/float → 24, ≤ 8-bit/lossy → 16). TPDF (±1 LSB)
  only when reducing to 16-bit; never before MP3. FFmpeg's FLAC encoder gets S16 for 16-bit and S32 with
  `bits_per_raw_sample = 24` for 24-bit, so 16-bit never becomes padded 24-bit.
- Tags: written fresh from the model (strip, then write). ID3v2.3, UTF-16, one APIC front cover with empty Latin-1
  description, no ID3v1/APE; TMOO/TSST/TDRL/TDTG/TPRO → TXXX; multi-values joined with ", " (artist fields) or "; ".
  Label → TPUB, ISRC → TSRC, source link → WOAS/WXXX (D30). WAV gets RIFF INFO + an ID3 chunk; FLAC Vorbis comments +
  a PICTURE block.
- Deny-list (TagPolicy), when transcoding: ReplayGain/MP3Gain/R128 (D-7), encoder fields, LENGTH and iTunes' gapless
  and normalisation data, all of which describe the source's encoding; Serato/Traktor fields (D-11) when transcoding
  or with `stripDjAppData`. A file kept as it is keeps its encoder fields, length and gain: they still describe it.
  Binary frames (GEOB, PRIV, RVAD…) never enter the model.

## Tags in practice (C1)

TagLib 2.3.2 (overlay port, `static_assert`ed in `detail/TagLibUtil.h`) does the reading and writing; these are the
behaviours Selecta relies on or compensates for, each covered by `tests/convert/`.

- **Reading.** An explicit read-only `TagLib::FileStream` (ADR-009), audio properties off. TagLib's property map gives
  the canonical keys; a second pass applies the alias table only to fields still missing, so a file with both `DATE`
  and `YEAR` keeps `DATE`. `TRACKTOTAL`/`TOTALTRACKS` and `DISCTOTAL`/`TOTALDISCS` fold into `n/N`. Bounds for a
  stranger's file: 512 fields, 64 Ki characters per value (256 Ki for lyrics), 8 pictures of at most 20 MiB each,
  picture descriptions of 1 Ki characters; everything beyond is dropped and logged at debug level.
- **Container from the content.** `writeTags` asks Selecta's own probe what the file is (a mislabelled `.mp3` that is
  really a FLAC gets Vorbis comments), and refuses Ogg, Opus and anything else with `Unsupported`.
- **Fresh tags.** Every writer starts from nothing: MP3 strips ID3v1, APE and ID3v2 and saves ID3v2 only with
  `StripOthers`; AIFF and WAV clear the ID3 chunk's frames (WAV also its INFO list); FLAC drops prepended ID3 tags,
  every Vorbis field and every picture; M4A removes every item. GEOB/PRIV/RVA2 and other binary frames never survive.
- **ID3v2.3.** On a v2.3 save TagLib drops TDRL, TDTG, TMOO, TPRO and TSST (`downgradeFrames`), so `RELEASEDATE`,
  `TAGGINGDATE`, `MOOD`, `PRODUCEDNOTICE` and `DISCSUBTITLE` are written as `TXXX` frames under those names (TagLib
  reads them back under the same keys). TagLib itself turns TDRC into TYER + TDAT and TIPL/TMCL into IPLS; `TORY` gets
  the year only. TSOA/TSOT/TSOP survive because TagLib's iTunes workarounds are on (`NO_ITUNES_HACKS` off, the
  default; the tests would catch a build that changes it). Frames created from properties hold UTF-8 until render,
  so every text, comment, lyrics and WXXX frame gets its encoding set explicitly: UTF-16 for v2.3, UTF-8 for v2.4.
- **WAV.** The ID3 chunk carries the full tag; RIFF INFO, the only thing rekordbox reads from WAV, gets the joined
  view (one value per field).
- **FLAC.** Vorbis comments keep multiple values as repeated fields. The PICTURE block's width and height come from
  the image header (`QImageReader::size()`, no decoding).
- **M4A.** `INITIALKEY` goes to `----:com.apple.iTunes:KEY` (rekordbox ignores TagLib's lower-case `initialkey`
  atom, conversion §3 and §4.2); everything else through TagLib's MP4 mapping; one `covr` (JPEG or PNG).
- **What core passes.** `writeTags` writes exactly the model it gets: core fills a `MetadataModel` from its
  `meta::Release` (no `convert` → `meta` dependency) and `applyTagPolicy` removes what must not be copied.

## Conversion in practice (C2)

- **Rates and word lengths** (`resolveOutput`, shared with the advisor so advice describes what happens). MP3: 32,
  44.1 and 48 kHz kept, everything else to 44.1 kHz (D-4); sources below 32 kHz are upsampled and flagged. Lossless:
  the source rate, or above `maxSampleRate` the highest rate of the source's family (44.1 or 48 kHz × 2ⁿ) that fits.
  Depth: ≤ 16-bit → 16, deeper or float → 24, lossy → 16. Dither only for 16-bit results whose samples aren't already
  on the 16-bit grid (deeper, float or lossy sources, resampled or downmixed audio); 24-bit results are rounded.
- **MP3.** CBR (128…320) or VBR (V0…V9), `-q 3`, joint stereo (mono stays mono), LAME's own resampler never used,
  `lame_set_write_id3tag_automatic(0)` so the stream starts with LAME's placeholder frame, which `finish()` replaces
  with `lame_get_lametag_frame()`. The verifier then demands a LAME encoder string and a valid tag CRC, every frame at
  the CBR bitrate and, thanks to the tag's delay and padding, exactly the source's length (± one frame accepted).
- **FFmpeg encoders.** One code path for FLAC (`flac`), ALAC (`ipod`) and AIFF (`aiff`): a write `AVIOContext` with
  read-free seeking over the sink (muxers patch sizes and STREAMINFO at the end), 16-bit as S16/S16P, 24-bit as
  S32/S32P left-justified with `bits_per_raw_sample = 24`. FFmpeg's own metadata (vendor comments, `©too`) is wiped
  when TagLib writes the tags. AIFF and WAV stop with TooLarge before their 32-bit sizes would wrap.
- **WAV.** `RIFF`/`WAVE`/`fmt `(16 bytes, format 1)/`data`, mono or stereo, 16- or 24-bit, odd data padded; TagLib
  appends LIST/INFO and `ID3 ` and fixes the RIFF size.
- **Damaged sources.** A source with packets that don't decode, or that ends before its header says (FLAC
  STREAMINFO, the LAME frame count, chunk sizes), is refused with Corrupt rather than converted with holes; it can
  still be filed as it is, where the copy's identical damage is proven by the digest.
- **Verification.** A complete decode of the temporary file at its own rate and layout. Problems are collected, not
  stopped at: container, rate, channels, depth, clean decode, length, digest, MP3 bitrates and LAME tag, tags exactly
  as `tagsAsWritten()` predicts (no field missing, none extra), one decodable front cover at the intended size, and
  for WAV a RIFF INFO title and artist. `tagsAsWritten()` runs the same TagLib frames, comments or MP4 items the
  writer builds through TagLib's property map in memory, so format conversions (ID3 numeric genres, MP4 integer
  atoms, keys MP4 can't name) are predicted rather than reported as failures, while anything lost on the way to disk
  is caught.
- **Full disk.** Every write goes through `Sink`, which classifies a failed write as DiskFull (Qt's ENOSPC/EDQUOT
  message, or under 1 MiB free on the volume) or WriteFailed. Encoders surface the sink's error even when FFmpeg
  reports its own code. The tests wrap the temporary file in a device that fails like a full disk, for every
  output format and for KeepAudio.
- **Decoder fixes made for C2** (`audio/`): ALAC's word length now comes from the opened decoder (it lives in the
  magic cookie), and `DecoderStats::truncated` is set when a complete FLAC or LAME-tagged MP3 holds less than its
  header announced, not only when it ends inside a frame.

### Conversion speed

Release build, Apple M2 Pro, other builds running (load average 7). Each figure is one whole `transcode()`: decode,
encode, tags, full verification decode, fsync and rename (`selecta_convert_bench`).

| Case | Time |
|---|---|
| 6 min FLAC 16/44.1 → MP3 320 CBR | 4.6 s (about 80× real time; LAME-bound, one core) |
| 6 min FLAC → AIFF 16 | 0.94 s |
| 6 min FLAC → FLAC 16 | 1.7 s |
| 6 min FLAC → WAV 16 | 0.89 s |
| 6 min FLAC → ALAC 16 | 2.9 s |
| 6 min FLAC kept as it is (copy, retag, two decodes) | 1.4 s |
| 1 min FLAC 24/96 → MP3 320 (soxr VHQ to 44.1 kHz) | 1.1 s |
| `probeSourceQuality(…, runChecks)`: 6 min MP3 320 / FLAC 16 | 0.31 s / 0.39 s (decode-bound) |

A conversion keeps one core busy, so the worker pool (CONV-8) can run about one job per performance core.

## Artwork and advice in practice (C3)

- **Artwork.** `processArtwork()` refuses an image over 20 MB, then reads the header only (`QImageReader::size()`)
  and refuses more than 10 000 px a side or 50 megapixels, before any pixel is decoded; it never touches
  QImageReader's process-wide allocation limit. Selecta's own JPEG marker walk reports the frame type (baseline or
  not), the component count, an ICC profile, and a scan that never reaches its end-of-image marker (a cut download,
  which libjpeg would decode with grey filling in: refused as Corrupt). A baseline 3-component JPEG with no profile,
  no EXIF rotation, within 800 px and 500 KB and not near-square-but-not-square is returned byte for byte, after a
  decode check; anything else is decoded (EXIF orientation applied), converted to sRGB (CMYK and grey with
  profiles through `convertedToColorSpace`), flattened over white, centre-cropped when its ratio is within 0.9–1.1,
  scaled down to 800 px (never up) and written as a baseline JPEG at quality 90 with no ICC or EXIF. The result
  carries its MIME type and size for core.
- **Folder covers.** `chooseFolderCover()` looks only at the album folder and its `Scans/`, `Artwork/` or `Covers/`
  subfolder, never above (paths with `..` or a leading `/` are ignored). Exact names (cover, folder, front, in that
  order) beat whole-word matches (front, cover, folder, albumart) without an excluded word (back, cd, disc, inlay,
  inside, tray, booklet, label); otherwise the only plausible image or the largest roughly square one is offered as
  "guessed".
- **Advice.** `advise()` follows the matrix of conversion §6.2 with `resolveOutput()` deciding the columns, so the
  advice describes exactly what `transcode()` will do. In the filing context, lossy audio is never re-encoded: MP3s
  at the target are kept, others kept with "below target" (judged on the bitrate the checks say a suspect MP3 really
  delivers) and AAC with "format differs"; Ogg/Opus/WMA are asked about (D-5). Lossy to a higher rate or depth is
  refused. Warnings are listed before information. `describeNotice()` fills §6.3's texts with the real numbers.
- **Padded 24-bit.** The quantiser leaves samples already on the target grid alone even with dither on, so reducing
  a padded 24-bit file to 16 bits is lossless (and digital silence stays silent); the advisor says so
  (LosslessReduction) instead of announcing dither.
- **Verdict.** `judgeQuality()` turns a `SourceQuality` with checks into Genuine / Suspect / Unchecked, the claimed
  quality ("MP3 320 kbps CBR", "FLAC 24-bit / 96 kHz") and a reason for the inspector, worded "likely" (A3's
  findings, plus the MP3 cut-off rule applied to AAC, Vorbis and Opus files claiming 256 kbps or more).

## Threading

| API | Thread safety |
|---|---|
| `MetadataModel`, `EncodeSettings`, `Advice`… | Plain values |
| `readTags`, `writeTags`, `transcode`, `verifyOutput`, `processArtwork`, `probeSourceQuality` | Reentrant, blocking: workers only |
| `advise`, `applyTagPolicy`, `chooseFolderCover`, `id3v23View`, `judgeQuality`, `describeNotice` | Pure |
| `TranscodeJob` | Lives in its creating thread; work on `QThreadPool::globalInstance()`. Results reach it through a mutex-guarded mailbox and a data-free queued call (the WaveformJob pattern, ThreadSanitizer-visible); progress is coalesced to 10 updates a second; nothing is emitted after `cancel()` or destruction. |
| Parallel conversions | Independent: LAME's `lame_init()`/`lame_init_params()`/`lame_close()` are serialised process-wide, every other state belongs to one job. Tested byte for byte against the same conversions run alone, under TSan. |

## Integration: the arrival pipeline (core, D29)

1. `probeSourceQuality(path, /*runChecks=*/true)` → `advise(quality, inboxSettings, AdviceContext::Filing)`, and
   `judgeQuality(quality)` for the inspector (fake-320 detection; 0.3–0.4 s for a 6-minute file: one decode).
2. `Convert` → `TranscodeJob({source, stagingOutput, settings, releaseTags})`; `KeepAsIs` → the same job with
   `OutputFormat::KeepAudio` (tags rewritten on a copy, audio verified identical) plus the badge; `Ask` → the user.
3. On `finished`, FileOps publishes `stagingOutput` into the Inbox and trashes a lossless original (D11).
4. The cover: `processArtwork(bytes)` → `tags.setFrontCover(result.toPicture(Provenance::Release))`.
5. `transcode()` applies the tag policy itself: `policy.transcoding` follows the format (false for KeepAudio), and an
   MP3's ENCODING is LAME's settings string unless core sets one; core sets `policy.encodedBy` ("Selecta x.y", D-10).
6. A crash mid-conversion can leave `<name>.<random>.selecta-part` next to an output; nothing else ever has that
   suffix, so core may delete such files in its staging folder at start-up.

## Decisions (autonomous, for review)

| Decision | Why |
|---|---|
| One error type (`audio::Error`) for both media libraries | One model for the head implementer to handle. |
| Output written as `<name>.<random>.selecta-part` next to the output, renamed only after verification | "Through a temporary file" (contract); same volume so the rename is atomic; never overwrites. |
| A compliant JPEG cover is kept byte for byte | No generation loss on artwork that already meets §5.3. |
| `chooseFolderCover` added (pure ranking, no disk access) | CONV-5 needs it and it belongs with artwork; the caller lists files. |
| Container for tag writing detected from the content, not the extension | Downloaded names lie; the wrong TagLib class would corrupt or refuse the file. |
| v2.4-only frames written as `TXXX:<key>` in v2.3 | TagLib 2.3.2 drops them on a v2.3 save; TXXX keeps the value and reads back under the same key. |
| `GROUPING` follows TagLib's mapping (GRP1; `WORK` is TIT1) | Round-trips exactly and matches iTunes ≥ 12.5; whether rekordbox shows TIT1 or GRP1 is open (conversion §10, E-1). |
| Aliases fill only missing fields | A file's canonical field is more deliberate than a legacy spelling of it. |
| ALAC added as an output format (`OutputFormat::Alac`, `.m4a`) | The contract lists FFmpeg FLAC and ALAC; it shares the FLAC/AIFF code path. |
| Surround sources downmixed to stereo for every output | DJ players and rekordbox are stereo; WAVE_FORMAT_PCM is mono or stereo. |
| A damaged source is refused for conversion (Corrupt), not converted with silence in the holes | A conversion that "passes" with dropouts would be filed as good; keeping the file as it is stays possible. |
| Tag verification is exact, against `tagsAsWritten()` computed with TagLib in memory | Catches anything lost or stale on disk without failing on format conversions TagLib makes by design. |
| MP4: BPM rounded, non-numeric track/disc numbers left out | TagLib would truncate "127.98" to 127 and store "A1" as 0. |
| Encoder fields, length and iTunes data kept when the audio is kept | They describe the kept audio (iTunSMPB is AAC's gapless data); the reference's deny-list is "on transcode". |
| MD5 for the PCM digest | Not adversarial (both sides are computed by Selecta); fast enough next to LAME. |
| Dither skips samples already on the target grid | Padded 16-bit audio and digital silence stay exact; for genuine deeper audio the skipped samples are a random 1/256 with zero error, so the noise stays signal-independent. |
| A truncated JPEG cover is refused | libjpeg decodes it with grey filling the missing part; embedding that would be worse than no cover. |
| Filing never re-encodes lossy audio (except formats rekordbox can't play, after asking) | CONV-2: a re-encode can't improve it, only lose more. |
| The verdict extends the MP3 cut-off rule to AAC/Vorbis/Opus at ≥ 256 kbps | Core asked for fake-high-bitrate detection on every download; the rule is as safe for them as for MP3. |

## Progress

- **Done:** public headers (C1–C3); C1: `MetadataModel`, `readTags` (aliases, totals, bounds), `applyTagPolicy`,
  `writeTags` for MP3 (v2.3/v2.4), AIFF, WAV, FLAC and M4A, each checked by reading back through Selecta's reader and
  independently through TagLib's format classes. C2: `resolveOutput`/`checkSettings`, the LAME, FFmpeg (FLAC, ALAC,
  AIFF) and WAV encoders, `transcode()` with KeepAudio, `TranscodeJob`, `verifyOutput()`/`pcmDigest()`,
  `tagsAsWritten()`, the conversion benchmark. 53 tests; dev, ASan and TSan clean.
- **Done (C3):** `processArtwork()`/`inspectImage()`, `chooseFolderCover()`, `advise()`/`describeNotice()`,
  `makeSourceQuality()`/`probeSourceQuality()`, `judgeQuality()`/`describeSource()`. 76 convert tests.
- **Done (CLI):** `selecta-cli media-convert <in> <out.mp3|.aiff|.flac|.wav|.m4a> [--title … --artist … --cover img]`
  prints the source, the verdict (`--check`), the advice and its notices, then converts, tags and verifies (the
  output format follows the extension; `--keep-audio`, `--bitrate`, `--vbr`, `--bits`, `--rate`, `--id3v24`,
  `--tag KEY=VALUE`; an Ask needs `--yes`, a Refuse stops).
- **Next:** nothing planned in C1–C3; rekordbox checks by the maintainer (below).
- **Open questions:** which ID3 frame rekordbox shows as grouping (TIT1 or GRP1, conversion E-1); the M4A key atom
  (E-7). Both need the maintainer's rekordbox import.
