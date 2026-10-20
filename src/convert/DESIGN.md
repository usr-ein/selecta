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
| `TagWriter.h` | `writeTags()`, `id3v23View()` | Per-format writing with the ID3v2.3 fix-ups. |
| `EncodeSettings.h` | `EncodeSettings`, `resolveOutput()` | Output format and the rules for rate and word length (shared by transcoder and advisor). |
| `Transcoder.h` | `transcode()`, `TranscodeJob` | Decode → soxr → TPDF → encode → tag → verify → rename. Cancellable, with progress. |
| `OutputVerifier.h` | `verifyOutput()`, `pcmDigest()` | Full decode, length, PCM digest, MP3 frame bitrates and LAME tag, tags and artwork. |
| `QualityAdvisor.h` | `advise()`, `makeSourceQuality()`, `probeSourceQuality()`, `describeNotice()` | The warning matrix of §6.2 and its UI copy. |
| `ArtworkProcessor.h` | `processArtwork()`, `inspectImage()` | Baseline sRGB JPEG q90 ≤ 800 px, near-square crop, never upscaled, bounded decoding. |
| `ArtworkFinder.h` | `chooseFolderCover()` | Folder cover ranking (§5.1), pure. |

Internal (`detail/`): the libmp3lame encoder with its LAME tag, the FFmpeg FLAC and AIFF encoders behind a write
`AVIOContext`, the plain PCM WAV writer, the TPDF quantiser, the temporary-file and no-overwrite rename helpers.

## Pipeline

```
source ─▶ audio::Decoder (float, source layout; soxr VHQ only when the rate changes)
       ─▶ TPDF dither + quantise (16-bit PCM/FLAC targets from deeper sources only)
       ─▶ encoder: LAME (MP3) | FFmpeg flac | FFmpeg aiff (pcm_s16be/s24be) | own WAV writer
       ─▶ <output>.<random>.selecta-part (same directory)
       ─▶ TagWriter (TagLib, from the policy-applied model)
       ─▶ OutputVerifier
       ─▶ rename to <output> without overwriting (renamex_np RENAME_EXCL on macOS, link + unlink elsewhere)
```

KeepAudio (the audio-preserving path, §9.4): copy the source bytes into the temporary file, rewrite its tags, verify
that the decoded PCM digest equals the source's, rename.

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
- Deny-list (TagPolicy): ReplayGain/MP3Gain when transcoding (D-7); Serato/Traktor fields (D-11); encoder fields,
  stale LENGTH, iTunes internal comments always. Binary frames (GEOB, PRIV, RVAD…) never enter the model.

## Threading

| API | Thread safety |
|---|---|
| `MetadataModel`, `EncodeSettings`, `Advice`… | Plain values |
| `readTags`, `writeTags`, `transcode`, `verifyOutput`, `processArtwork`, `probeSourceQuality` | Reentrant, blocking: workers only |
| `advise`, `applyTagPolicy`, `chooseFolderCover`, `id3v23View` | Pure |
| `TranscodeJob` | Lives in its creating thread; work on `QThreadPool::globalInstance()` |

## Integration: the arrival pipeline (core, D29)

1. `probeSourceQuality(path, /*runChecks=*/true)` → `advise(quality, inboxSettings, AdviceContext::Filing)`.
2. `Convert` → `TranscodeJob({source, stagingOutput, settings, releaseTags})`; `KeepAsIs` → the same job with
   `OutputFormat::KeepAudio` (tags rewritten on a copy, audio verified identical) plus the badge; `Ask` → the user.
3. On `finished`, FileOps publishes `stagingOutput` into the Inbox and trashes a lossless original (D11).
4. The cover: `processArtwork(bytes)` → `tags.setFrontCover(result.toPicture(Provenance::Release))`.

## Decisions (autonomous, for review)

| Decision | Why |
|---|---|
| One error type (`audio::Error`) for both media libraries | One model for the head implementer to handle. |
| Output written as `<name>.<random>.selecta-part` next to the output, renamed only after verification | "Through a temporary file" (contract); same volume so the rename is atomic; never overwrites. |
| A compliant JPEG cover is kept byte for byte | No generation loss on artwork that already meets §5.3. |
| `chooseFolderCover` added (pure ranking, no disk access) | CONV-5 needs it and it belongs with artwork; the caller lists files. |

## Progress

- **Done:** public headers (C1–C3), this document.
- **Next:** after A1 and A2: C1 (model, reader, policy, writer), C2, C3.
- **Open questions:** none yet.
