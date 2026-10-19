# ADR-003: Audio stack: decode, playback, encode, tags

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/audio-preview-and-waveform.md` §3–5, §11; `references/conversion-and-metadata.md` §1–2, §9; RESEARCH-SYNTHESIS §1.5–1.6

## Decision

- **Decode:** FFmpeg (libavformat, libavcodec, libswresample).
  - A file still downloading is opened through an `AVIOContext` that reports *non-seekable* until it completes,
    plus Selecta's own frame/byte index for seeking.
  - M4A with `moov` at the end plays only after the download completes.
- **Playback** goes through an `AudioOutput` interface:
  - the `QAudioSink` callback API first (M5);
  - CoreAudio AUHAL on macOS before v1.0, with a latency-compensated playhead extrapolated per vsync.

  `QMediaPlayer` is not used for previews.
- **Encode:**
  - MP3: libmp3lame called directly, with a genuine LAME Info tag.
  - FLAC and ALAC: FFmpeg's encoders.
  - AIFF: FFmpeg's muxer with big-endian PCM.
  - WAV: Selecta's own `WAVE_FORMAT_PCM` writer.
  - Resampling: libsoxr, only when the target can't carry the source rate.
  - TPDF dither, only when reducing to 16-bit.
- **Tags:** TagLib ≥ 2.3.2 writes every tag and the artwork from Selecta's metadata model. The ID3v2.3 fix-ups
  remap TMOO, TSST, TDRL, TDTG and TPRO to `TXXX`, join multiple values, use UTF-16 text and a Latin-1 APIC
  description. FFmpeg never copies metadata.
- **Untrusted input:** every downloaded file is untrusted. Image allocation is capped. Probing and conversion should
  run in a helper process so a parser crash can't take the app down; this is decided during M6, once the cost is
  measured.

## Consequences

- MP3s get consistent cue positions across DJ apps: no `Lavf` header and no 26 ms shift.
- `convert/` and the analysis half of `audio/` are libraries with clean boundaries (PLAN §9).
