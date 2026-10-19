# Conversion and metadata — research note

> Status: **DRAFT in progress** (2026-10-19). Items marked **TODO-verify** are from memory and not yet checked against
> a primary source. Confidence levels: **H** (verified in source/spec), **M** (multiple secondary sources), **L** (memory
> or single hearsay).

## TL;DR

- **Pipeline:** decode with FFmpeg (libavformat/libavcodec) → convert sample format / resample / dither (libswresample,
  ideally with the libsoxr engine) → encode (LAME for MP3, FFmpeg native encoders for FLAC/PCM/ALAC) → mux → **write all
  tags and artwork with TagLib 2.x from Selecta's own normalised metadata model**. Never rely on FFmpeg's metadata
  copying (`-map_metadata`): it is lossy and format-dependent.
- **Default filing format (CONV-2):** MP3, LAME, CBR 320 kbps, 44.1 kHz (48 kHz kept if the source is 48 kHz — TODO
  decide), joint stereo, LAME/Info header written, **ID3v2.3 with UTF-16 text**, one APIC "front cover" JPEG, no ID3v1
  (TODO decide), no APEv2.
- **Tags:** ID3v2.3 is the safest common denominator for rekordbox 7, CDJs, Serato, Traktor and Engine DJ (TODO-verify
  per app). Writing v2.3 with TagLib requires `save(..., ID3v2::v3)`; TagLib then downgrades v2.4-only frames (some are
  dropped — mapping must compensate with TXXX frames, TODO-verify the exact list).
- **Never improve what can't be improved:** lossy sources are never re-encoded to a "higher" format by default; lossy →
  lossy only with explicit confirmation; upsampling is never automatic.
- **Licences:** FFmpeg (LGPL-2.1+ by default, GPL-2.0+ with `--enable-gpl`, never `--enable-nonfree`), LAME
  (LGPL-2.0+), libFLAC (BSD-3-Clause), libsoxr (LGPL-2.1+), libsndfile (LGPL-2.1+), TagLib (LGPL-2.1 or MPL-1.1) — all
  compatible with a GPLv3 application. (TODO-verify exact SPDX ids.)

## Key findings

1. TODO (filled in as sections are verified).

## 1. Libraries and licences

| Component | Role in Selecta | Licence (SPDX) | GPLv3-compatible | Notes |
|---|---|---|---|---|
| FFmpeg (libavformat, libavcodec, libavutil, libswresample) | Decode everything; encode FLAC/PCM/ALAC; mux; resample | LGPL-2.1-or-later (default build); GPL-2.0-or-later with `--enable-gpl`; `--enable-version3` → (L)GPL-3.0 | Yes (never `--enable-nonfree`) | TODO-verify Homebrew formula flags (is `libsoxr`, `lame` still enabled in `ffmpeg` vs new `ffmpeg-full`?) |
| LAME (libmp3lame) | MP3 encoding | LGPL-2.0-or-later (TODO-verify) | Yes | 3.100 is the last release (2017) — TODO-verify |
| libFLAC | Optional reference FLAC encoder | BSD-3-Clause (lib), GPL for tools | Yes | FFmpeg's native FLAC encoder may suffice |
| libsoxr | High-quality SRC | LGPL-2.1-or-later | Yes | Usable via `swr` option `resampler=soxr` |
| libsndfile | Alternative PCM I/O | LGPL-2.1-or-later | Yes | Not needed if FFmpeg is used |
| TagLib 2.x | All tag reading/writing | LGPL-2.1-only OR MPL-1.1 (TODO-verify) | Yes | C++17; `complexProperties("PICTURE")` unified artwork API (TODO-verify) |
| AudioToolbox (macOS) | Optional: Apple AAC/ALAC encoders, SRC | System framework | Yes (system library exception) | macOS-only; not needed for P0 formats |

## 2. Encoding best practice

### 2.1 MP3 (LAME)

- CBR 320: LAME `-b 320` (CBR is LAME's default mode). VBR V0: `-V 0` (≈ 245 kbps average), V2: `-V 2` (≈ 190 kbps).
  TODO-verify LAME's default `-q` (algorithm quality) and Hydrogenaudio's current recommendation.
- Via FFmpeg: `-c:a libmp3lame -b:a 320k` (CBR) / `-q:a 0` (V0). Pitfall: without `-b:a`/`-q:a`, FFmpeg's libmp3lame
  defaults to 128 kbps (TODO-verify).
- Feed LAME float or 32-bit samples (not 16-bit) so 24-bit sources are not truncated before encoding.
- MP3 (MPEG-1 Layer III) only supports 32 / 44.1 / 48 kHz → hi-res sources must be resampled (prefer 44.1 kHz for 88.2/
  176.4, TODO decide 48 vs 44.1 for 96/192).
- LAME/Xing/Info header: TODO — who writes it when encoding via FFmpeg (muxer `write_xing`), what fields, gapless
  delay/padding; relevance for DJ apps (MP3 offset issues between rekordbox and Serato/Traktor — TODO-verify).

### 2.2 Resampling and dither

- TODO: libswresample defaults (dither none?), `dither_method=triangular`, soxr precision; when to dither (only when
  reducing word length to 16-bit integer PCM; never for MP3 output).

### 2.3 WAV vs AIFF for DJs

- TODO: tag support in rekordbox/CDJ for WAV (RIFF INFO vs `id3 ` chunk) vs AIFF (`ID3 ` chunk); WAVE_FORMAT_EXTENSIBLE
  and older CDJ compatibility; AIFF-C/`sowt`.

### 2.4 Verification

- TODO.

## 3. Metadata mapping

TODO: full table Vorbis ↔ ID3v2.3 ↔ ID3v2.4 ↔ MP4 ↔ RIFF INFO / `id3 ` ↔ AIFF ID3, from TagLib's property-map tables
and Picard/mediafile mappings.

## 4. ID3v2.3 vs ID3v2.4, and what DJ software reads

TODO.

## 5. Cover art

TODO: discovery rules (filenames, ranking), embedding per format, DJ hardware size/format constraints, resizing.

## 6. Quality warning matrix and true-quality detection

TODO.

## 7. Naming templates and filename sanitisation

TODO: NFC/NFD, forbidden characters (macOS / Windows / FAT32 / exFAT), path and component length, collisions on
case-insensitive APFS.

## 8. Reference implementations

TODO: beets convert + mediafile, Picard, XLD, dBpoweramp, ffmpeg pitfalls.

## 9. Implications for Selecta

TODO.

## 10. Open questions and experiments

TODO.

## 11. Sources

TODO (accessed 2026-10-19).
