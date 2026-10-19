# Audio preview and waveform — decoding, progressive playback, 3-band waveform, quality analysis

> Status: **draft in progress** (2026-10-19). Items marked **TODO** / *unverified* are from memory and not yet checked
> against a primary source. Confidence levels: **high** = read in source code/spec; **medium** = documentation or
> consistent secondary sources; **low** = inference or hearsay.

## TL;DR

- TODO (filled in at the end).

## 1. Key findings

- TODO (filled in as sections are verified).

## 2. Pioneer "3Band" waveform (rekordbox / CDJ-3000) — how it looks and is layered

TODO — priority question; being verified against Deep Symmetry (crate-digger, beat-link, djl-analysis docs),
pyrekordbox and rekordcrate.

What is known before verification (*unverified*):
- rekordbox offers three waveform colour modes: BLUE (monochrome), RGB, and 3Band (low = blue, mid = orange/amber,
  high = white). The CDJ-3000 has the same setting.
- rekordbox stores waveforms in ANLZ files: `PWAV`/`PWV2`/`PWV3` (blue), `PWV4`/`PWV5` (RGB), `PWV6`/`PWV7` (3-band,
  in the `.2EX` file). Detail waveforms are 150 entries per second.

## 3. Decoding options

TODO: FFmpeg (LGPL/GPL), Qt Multimedia FFmpeg backend, libsndfile, dr_libs/miniaudio, libmpg123, libFLAC,
AudioToolbox, Mixxx SoundSource.

## 4. Playback engine and accurate position reporting

TODO: QAudioSink vs CoreAudio (AUHAL / AVAudioEngine) vs miniaudio; latency compensation; playhead interpolation.

## 5. Progressive preview of growing files

TODO: per format (MP3, FLAC, WAV/AIFF, M4A moov position, Ogg/Opus); seeking in the downloaded range; VBR seeking;
safe growing-file reader design (blocking reads, cancellation, FFmpeg AVIOContext pitfalls such as tail reads for
ID3v1/APE/moov).

## 6. Mixxx waveform implementation (filtered / RGB / stacked)

TODO: files, crossovers, filters, summarisation, storage; GPLv2+ reuse.

## 7. Recommended analysis pipeline for Selecta

TODO: crossovers, filters, peak vs RMS, perceptual scaling, overview vs zoom resolution, cache format, < 300 ms for a
6-minute track on Apple Silicon, progressive analysis.

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
