# Conversion and metadata — research note

> Status: **DRAFT in progress** (2026-10-19). Items marked **TODO-verify** are from memory and not yet checked against
> a primary source. Confidence: **H** = verified in source code or spec; **M** = several consistent secondary sources;
> **L** = memory, single hearsay or inference.
> Source snapshots read for this note (shallow clones, read-only): TagLib `5fb1099` (2.3.2+, 2026-10-19), FFmpeg
> `f91b54f` (master, 2026-10-19), LAME svn-trunk mirror `lameproject/lame@1f5cc94` (3.100.1 "under construction"),
> beets `18967aa`, mediafile `faaf744`, Picard `84862c5`, picard-docs `2a6193e`, mutagen `ada28b2`.

## TL;DR

- **Pipeline:** decode with FFmpeg (libavformat/libavcodec) → sample-format conversion, resampling and dither
  (libswresample, or libsoxr directly) → encode (libmp3lame **called directly** for MP3; FFmpeg native encoders for
  FLAC/PCM/ALAC) → mux → **write every tag and the artwork with TagLib 2.x from Selecta's own normalised metadata
  model**. Never rely on FFmpeg's metadata copying: it silently turns BPM, key, ISRC, label and track totals into
  `TXXX` frames and ignores folder artwork (verified in `libavformat/id3v2enc.c`).
- **Default filing format (CONV-2):** MP3, LAME, CBR 320 kbps, joint stereo, 44.1 kHz (48 kHz sources stay 48 kHz),
  LAME "Info" tag with gapless data, **ID3v2.3 with UTF-16 text**, one APIC "front cover" baseline JPEG, no ID3v1, no
  APEv2.
- **TagLib writes ID3v2.3 correctly only if Selecta compensates:** `save(..., ID3v2::v3)` *discards* TMOO, TSST, TDRL,
  TDTG, TPRO (→ mood, disc subtitle, release date are lost) and writes multi-value frames null-separated (non-standard
  in v2.3). Selecta must remap those to `TXXX` and join multiple values before saving (H).
- **Homebrew's `ffmpeg` (9.0.2) is built `--enable-gpl --enable-version3` with `libmp3lame` but without `libsoxr`** →
  link libsoxr directly (LGPL-2.1+) for high-quality resampling instead of depending on FFmpeg's build flags (H).
- **libswresample does not dither by default** (`dither_method` default 0 = none); TPDF must be requested explicitly
  when reducing to 16-bit (H).
- **Never improve what can't be improved:** lossy sources are filed as-is by default; lossy → lossy only after an
  explicit confirmation; upsampling is never automatic (see the warning matrix, §6).

## Key findings

1. FFmpeg's MP3 muxer writes its own Xing/Info frame with a LAME-*style* extension whose encoder string is `Lavf`/
   `Lavf lame`, not `LAME3.100`; lowpass, VBR method and preset fields are zero. Encoder delay/padding *are* written.
   Calling libmp3lame directly with `lame_set_bWriteVbrTag(1)` + `lame_get_lametag_frame()` produces the same header as
   the LAME command line — the most widely tested MP3 flavour in DJ software (H for the code; L for DJ-app impact).
2. FFmpeg's `libmp3lame` wrapper sets no bitrate by default (`b=0`) → LAME's library default (128 kbps CBR) applies;
   it never calls `lame_set_quality` unless `compression_level` is set (LAME default `-q 3`); it disables LAME's
   internal resampler (`out_samplerate = in_samplerate`) (H).
3. LAME's CBR lowpass table: 320 kbps → 20.5 kHz, 256 → 19.7 kHz, 224 → 19.4 kHz, 192 → 18.6 kHz, 160 → 17.5 kHz,
   128 → 17 kHz (`libmp3lame/lame.c`, `optimum_bandwidth`) — useful for spectral-cutoff heuristics (H).
4. FFmpeg's WAV muxer always writes `WAVE_FORMAT_EXTENSIBLE` for > 16-bit PCM or > 48 kHz and has no switch to force
   plain `WAVEFORMATEX`; its AIFF muxer writes no ID3 chunk unless `write_id3v2=1` (H). TODO-verify whether
   extensible 24-bit WAVs cause problems on older CDJs.
5. TagLib 2.x has a format-independent artwork API: `complexProperties("PICTURE")` with `data`, `mimeType`,
   `description`, `pictureType` ("Front Cover") (H).

## 1. Libraries and licences

| Component | Role in Selecta | Licence (SPDX) | GPLv3-compatible | Notes |
|---|---|---|---|---|
| FFmpeg 9.x (libavformat, libavcodec, libavutil, libswresample) | Decode everything; encode FLAC/PCM/ALAC; mux; sample-format conversion | LGPL-2.1-or-later by default; GPL-2.0-or-later with `--enable-gpl`; `--enable-version3` turns these into LGPL-3.0/GPL-3.0-or-later. Homebrew's formula declares `GPL-3.0-or-later` | Yes — never `--enable-nonfree` (e.g. libfdk_aac) | Homebrew `ffmpeg` 9.0.2: `--enable-gpl --enable-version3 --enable-libmp3lame --enable-audiotoolbox`, **no `--enable-libsoxr`** (H, formula read 2026-10-19). Bundling obligations covered in `qt-app-architecture.md`. |
| LAME (libmp3lame) | MP3 encoding | LGPL-2.0-or-later (`COPYING` = LGPL v2 "Library GPL", headers say "or any later version") | Yes | Last release 3.100 (2017); svn trunk is "3.100.1 under construction". Homebrew `lame`. (H) |
| libsoxr | High-quality sample-rate conversion | LGPL-2.1-or-later | Yes | Use directly (simple C API) rather than through `swr` `resampler=soxr`, which Homebrew's FFmpeg lacks. TODO-verify SPDX from source. |
| libFLAC | Optional reference FLAC encoder | BSD-3-Clause (library); GPL-2.0+ for the command-line tools | Yes | Not needed: FFmpeg's native FLAC encoder writes STREAMINFO MD5 too (TODO-verify). |
| libsndfile | Alternative PCM I/O | LGPL-2.1-or-later | Yes | Not needed; writes RIFF INFO strings but no ID3 chunk (TODO-verify). |
| TagLib 2.3.x | All tag reading/writing | LGPL-2.1-only **or** MPL-1.1 (dual; `COPYING.LGPL` 2.1 + `COPYING.MPL` 1.1) | Yes (LGPL-2.1 is GPLv3-compatible) | 2.0 (Jan 2024) → 2.3.2 (Sep 2026). C++17. Writes ID3v2.3 via `ID3v2::v3`; unified `PICTURE` complex property; RIFF RF64/BW64 (2.3.2); BEXT/iXML (2.3). (H) |
| AudioToolbox (macOS) | Optional: Apple AAC/ALAC encoders, SRC | System framework | Yes (system-library exception) | Not needed for P0 formats; would only matter for AAC output (not in CONV-1). |

**Recommendation:** FFmpeg (decode, FLAC/PCM/ALAC encode, mux) + libmp3lame (direct) + libsoxr (direct) + TagLib. All
four are in Homebrew, vcpkg and Conan (TODO-verify vcpkg/Conan names for the qt note).

## 2. Encoding best practice

### 2.1 MP3 with LAME

| Setting | LAME CLI | libmp3lame API | Via FFmpeg CLI | Notes |
|---|---|---|---|---|
| CBR 320 | `-b 320` (CBR is the default mode) | `lame_set_brate(gfp, 320)`, `VBR = vbr_off` | `-c:a libmp3lame -b:a 320k` | Lowpass 20.5 kHz automatically (H). |
| CBR 256 / 192 | `-b 256` / `-b 192` | idem | `-b:a 256k` | Lowpass 19.7 / 18.6 kHz (H). |
| VBR V0 / V2 | `-V 0` / `-V 2` | `lame_set_VBR(gfp, vbr_default)` (= `vbr_mtrh`), `lame_set_VBR_quality(gfp, 0)` | `-q:a 0` / `-q:a 2` | V0 ≈ 245 kbps, V2 ≈ 190 kbps average (M, Hydrogenaudio; TODO-verify). |
| Algorithm quality | `-q 0…9`, default 3 | `lame_set_quality`; default `LAME_DEFAULT_QUALITY = 3` | `-compression_level` | For `vbr_mtrh`, any `-q` < 5 is mapped to 0 (`lame.c`) (H). Keep the default; `-q 0` costs time for negligible gain (TODO-verify HA recommendation). |
| Stereo mode | joint stereo (default) | `JOINT_STEREO` | `joint_stereo=1` (default) | Keep joint stereo. |
| Input samples | — | `lame_encode_buffer_ieee_float` / `_int` | wrapper accepts s32p, fltp, s16p | Feed float or 32-bit: never truncate a 24-bit source to 16 bit before LAME (H). |
| Sample rate | — | `in = out` rate; LAME *can* resample internally, but don't | wrapper forces `out = in` | MPEG-1 Layer III: 32 / 44.1 / 48 kHz only. Resample hi-res first (§2.2). |
| Info/LAME tag | written by default | `lame_set_bWriteVbrTag(1)`, then `lame_get_lametag_frame()` after `lame_encode_flush()` and overwrite the first frame | FFmpeg writes its *own* Xing/Info frame (`write_xing=1`) | See finding 1. |

**Pitfalls when using FFmpeg for MP3 (H, `libavcodec/libmp3lame.c`, `libavformat/mp3enc.c`):**
- No `-b:a`/`-q:a` → LAME default 128 kbps CBR.
- The muxer writes ID3v2.**4** unless `-id3v2_version 3`; ID3v1 off by default (`write_id3v1=0`).
- The Xing/Info frame's encoder field is `Lavf`/`Lavf lame`; FFmpeg's own demuxer accepts `LAME`, `Lavf` and `Lavc`
  for gapless trimming, other decoders may only trust `LAME` (L).

**Decision for Selecta:** encode MP3 by calling libmp3lame directly (encoder thread: decoded float frames → LAME →
file), write the LAME tag via `lame_get_lametag_frame()`, then add ID3v2.3 with TagLib. It is ~200 lines more than
going through libavcodec, removes the dependency on how FFmpeg was built, and yields byte-for-byte the same stream
layout as `lame -b 320` (the reference everyone tests against).

### 2.2 Resampling and dither

- **When to resample:** only when the target format cannot carry the source rate: MP3 (max 48 kHz), or a user-chosen
  "hardware compatibility" profile (§4.3). Never upsample automatically.
- **Target rate:** 88.2 / 176.4 kHz → 44.1 kHz (integer ratio). 96 / 192 kHz → 48 kHz keeps an integer ratio, but
  44.1 kHz is the CD/DJ norm; proposed default **44.1 kHz for MP3 output**, 48 kHz sources kept at 48 kHz (TODO decide
  with the maintainer).
- **Engine:** libsoxr, quality `SOXR_VHQ` (28-bit precision) or `SOXR_HQ` (20-bit); both far beyond what MP3 needs.
  FFmpeg's `swr` with `resampler=soxr` uses `precision=20` by default (H) but needs an FFmpeg built with libsoxr.
  FFmpeg's native swr resampler (`filter_size=32` default) is acceptable as a fallback (TODO-verify measured quality).
- **Dither:** only when reducing word length to integer PCM (24 → 16-bit WAV/AIFF/FLAC). Use TPDF
  (`dither_method=triangular` in swr, or Selecta's own ±1 LSB TPDF after soxr). Never dither before MP3 (LAME takes
  float). libswresample default is *no dither* (`dither_method` default 0; H). Noise-shaped variants (`shibata`,
  `f_weighted`…) exist in swr but plain TPDF is the safe default for material that may be processed again.
- **Order:** decode → (float) → resample → (gain = 0) → dither + quantise (PCM targets only) → encode.

### 2.3 WAV vs AIFF for DJs

- TODO: what rekordbox / CDJ read in WAV (RIFF INFO vs ID3 chunk) and AIFF (`ID3 ` chunk); artwork; AIFF-C/`sowt`.
- FFmpeg's AIFF muxer writes `FORM/AIFF` for big-endian PCM (`pcm_s16be`, `pcm_s24be`) and `FORM/AIFC` for anything
  else (e.g. `pcm_s16le` → `sowt`). Use big-endian PCM for plain AIFF (H, `libavformat/aiffenc.c`).
- FFmpeg's WAV muxer chooses `WAVE_FORMAT_EXTENSIBLE` for > 16-bit PCM, > 48 kHz or non-mono/stereo layouts (H,
  `libavformat/riffenc.c`); there is no option to force `WAVEFORMATEX`.
- TagLib writes the WAV ID3 chunk as `ID3 ` (upper case), reads `ID3 ` and `id3 `, and writes `LIST/INFO`; AIFF gets an
  `ID3 ` chunk (H, `riff/wav/wavfile.cpp`, `riff/aiff/aifffile.cpp`).

### 2.4 Verification

- TODO.

## 3. Metadata mapping

Selecta's internal model uses **TagLib property keys** (upper case) as canonical names; TagLib translates them to each
format. Rows marked ⚠ need Selecta-side handling. Sources: TagLib `mpeg/id3v2/id3v2frame.cpp` (`frameTranslation`),
`frames/textidentificationframe.cpp` (`txxxFrameTranslation`), `mp4/mp4itemfactory.cpp`, `riff/wav/infotag.cpp`;
Picard tag-mapping appendix (`picard-docs/appendices/tag_mapping.rst`); beets `mediafile/__init__.py`.

| Field | TagLib key | Vorbis comment (FLAC, Ogg, Opus) | ID3v2.3 | ID3v2.4 | MP4 / M4A | RIFF INFO (WAV) | Notes |
|---|---|---|---|---|---|---|---|
| Title | `TITLE` | `TITLE` | `TIT2` | `TIT2` | `©nam` | `INAM` | |
| Artist | `ARTIST` | `ARTIST` (may repeat) | `TPE1` | `TPE1` (null-separated multi) | `©ART` | `IART` | ⚠ v2.3: join multiple values into one string. |
| Album | `ALBUM` | `ALBUM` | `TALB` | `TALB` | `©alb` | `IPRD` | |
| Album artist | `ALBUMARTIST` | `ALBUMARTIST`; read also `ALBUM ARTIST`, `ALBUM_ARTIST` (mediafile) | `TPE2` | `TPE2` | `aART` | — | TPE2 is "band/orchestra" in the spec; every player uses it as album artist. |
| Track no. / total | `TRACKNUMBER` (`n/N`) | `TRACKNUMBER` + `TRACKTOTAL`; read also `TOTALTRACKS` | `TRCK` `n/N` | `TRCK` | `trkn` (n, N) | TagLib `IPRT`; Picard `ITRK` | ⚠ Combine total into `n/N`; ⚠ FFmpeg would write `TXXX:TRACKTOTAL`. |
| Disc no. / total | `DISCNUMBER` | `DISCNUMBER` + `DISCTOTAL`; read also `TOTALDISCS` | `TPOS` `n/N` | `TPOS` | `disk` | — | |
| Date | `DATE` | `DATE` (`YYYY` or `YYYY-MM-DD`) | `TYER` (`YYYY`) + `TDAT` (`DDMM`) [+ `TIME` `HHMM`] | `TDRC` | `©day` | `ICRD` | TagLib converts TDRC → TYER/TDAT/TIME when saving v2.3 (H). |
| Original date | `ORIGINALDATE` | `ORIGINALDATE`; read `ORIGINALYEAR` | `TORY` (year only) | `TDOR` | `----:com.apple.iTunes:ORIGINALDATE` | — | TagLib converts TDOR → TORY (H). |
| Release date | `RELEASEDATE` | `RELEASEDATE` | ⚠ `TXXX:RELEASEDATE` | `TDRL` | `----:com.apple.iTunes:RELEASEDATE` | — | TagLib *drops* TDRL on v2.3 save (H). |
| Genre | `GENRE` | `GENRE` | `TCON` (`(nn)Text` refs) | `TCON` (null-separated) | `©gen` (legacy `gnre`) | `IGNR` | TagLib builds v2.3 `(nn)` references itself (H). |
| Composer | `COMPOSER` | `COMPOSER` | `TCOM` | `TCOM` | `©wrt` | TagLib `IMUS` | |
| Lyricist | `LYRICIST` | `LYRICIST` | `TEXT` | `TEXT` | `----:com.apple.iTunes:LYRICIST` | TagLib `IWRI` | |
| Remixer | `REMIXER` | `REMIXER` | `TPE4` | `TPE4` | `----:com.apple.iTunes:REMIXER` | TagLib `IEDT` | DJ-relevant (remix credits). |
| Conductor | `CONDUCTOR` | `CONDUCTOR` | `TPE3` | `TPE3` | `----:com.apple.iTunes:CONDUCTOR` | — | ⚠ FFmpeg maps its `performer` key to TPE3. |
| BPM | `BPM` | `BPM` | `TBPM` | `TBPM` | `tmpo` (16-bit int) | TagLib `IBPM` (non-standard) | ID3 spec: integer string. ⚠ Write rounded integer to ID3/MP4; keep decimals only in Vorbis. TODO-verify what DJ apps write. |
| Musical key | `INITIALKEY` | read `INITIALKEY` (mediafile) and `KEY` (Picard); write `INITIALKEY` | `TKEY` | `TKEY` | `----:com.apple.iTunes:initialkey` | — | ID3 spec: ≤ 3 chars, e.g. `Dbm`, `o` = off key; DJ tools also write Camelot (`8A`) or Open Key (`1m`). Copy verbatim. |
| Label | `LABEL` | `LABEL`; read `ORGANIZATION` (Vorbis spec), `PUBLISHER` (Traktor, per mediafile) | `TPUB` | `TPUB` | `----:com.apple.iTunes:LABEL` | TagLib `IPUB` | |
| ISRC | `ISRC` | `ISRC` | `TSRC` | `TSRC` | `----:com.apple.iTunes:ISRC` | TagLib `ISRC` | |
| Catalogue no. | `CATALOGNUMBER` | `CATALOGNUMBER`; read `CATALOGID`, `DISCOGS_CATALOG` | `TXXX:CATALOGNUMBER` | `TXXX:CATALOGNUMBER` | `----:com.apple.iTunes:CATALOGNUMBER` | — | |
| Comment | `COMMENT` | `COMMENT`; read `DESCRIPTION` | `COMM` (lang `eng`, empty description) | `COMM` | `©cmt` | `ICMT` | ⚠ Skip iTunes-internal COMM frames (`iTunNORM`, `iTunSMPB`, `iTunPGAP`) when copying. |
| Lyrics | `LYRICS` | `LYRICS`; read `UNSYNCEDLYRICS` | `USLT` | `USLT` | `©lyr` | — | |
| Compilation | `COMPILATION` | `COMPILATION` = `1` | `TCMP` = `1` (iTunes, non-standard) | `TCMP` | `cpil` | — | |
| Grouping | `GROUPING` | `GROUPING` | `TIT1` (classic) / `GRP1` (iTunes ≥ 12.5) | idem | `©grp` | — | ⚠ TagLib maps TIT1 → `WORK` and GRP1 → `GROUPING`; decide which frame to write (TODO: what rekordbox shows). |
| Subtitle / mix | `SUBTITLE` | `SUBTITLE` | `TIT3` | `TIT3` | `----:com.apple.iTunes:SUBTITLE` | — | |
| Mood | `MOOD` | `MOOD` | ⚠ `TXXX:MOOD` | `TMOO` | `----:com.apple.iTunes:MOOD` | — | TagLib *drops* TMOO on v2.3 save (H). |
| Disc subtitle | `DISCSUBTITLE` | `DISCSUBTITLE` | ⚠ `TXXX:DISCSUBTITLE` | `TSST` | `----:com.apple.iTunes:DISCSUBTITLE` | TagLib `PRT1` | TagLib drops TSST on v2.3 save (H). |
| Encoded by | `ENCODEDBY` | `ENCODEDBY` | `TENC` | `TENC` | `©enc` | TagLib `ITCH` | |
| Encoder / settings | `ENCODING` | `ENCODER` / `ENCODERSETTINGS` | `TSSE` | `TSSE` | `©too` | `ISFT` | Selecta writes e.g. `LAME 3.100 -b 320` (TODO decide). |
| Copyright | `COPYRIGHT` | `COPYRIGHT` | `TCOP` | `TCOP` | `cprt` | `ICOP` | |
| Sort names | `ARTISTSORT`, `ALBUMSORT`, `TITLESORT`, `ALBUMARTISTSORT`, `COMPOSERSORT` | same keys | `TSOP`, `TSOA`, `TSOT` (iTunes extension in v2.3, kept by TagLib), `TSO2`, `TSOC` | `TSOP`, `TSOA`, `TSOT` (standard), `TSO2`, `TSOC` | `soar`, `soal`, `sonm`, `soaa`, `soco` | — | mutagen's `update_to_v23()` deletes TSOA/TSOP/TSOT; TagLib keeps them unless built with `NO_ITUNES_HACKS` (H). |
| ReplayGain | `REPLAYGAIN_TRACK_GAIN`, `…_PEAK`, `REPLAYGAIN_ALBUM_GAIN`, `…_PEAK` | same | `TXXX:REPLAYGAIN_TRACK_GAIN` … | idem | `----:com.apple.iTunes:REPLAYGAIN_TRACK_GAIN` … | — | ⚠ Values describe the *source* encoding; after lossy encoding the gain is still approximately right but the peak is not. Copy gain, drop peak (TODO decide). |
| MusicBrainz IDs | `MUSICBRAINZ_TRACKID`, `MUSICBRAINZ_ALBUMID`, … | same | `UFID:http://musicbrainz.org` + `TXXX:MusicBrainz Album Id` … | idem | `----:com.apple.iTunes:MusicBrainz Track Id` … | — | Copy verbatim (TagLib handles the TXXX names). |
| Artwork | `PICTURE` (complex property) | FLAC: native `PICTURE` metadata block; Ogg/Opus: `METADATA_BLOCK_PICTURE` (base64) | `APIC` (type 3 = front cover, MIME `image/jpeg`) | `APIC` | `covr` (data type 13 = JPEG, 14 = PNG; no picture type) | — (no artwork in INFO) | WAV/AIFF artwork only via the ID3 chunk. |
| DJ-app private data | — | Serato: base64 `SERATO_*` comments (TODO-verify) | `GEOB` "Serato Markers2", "Serato BeatGrid"…; `PRIV` "TRAKTOR4" (TODO-verify) | idem | Serato freeform atoms (TODO-verify) | — | ⚠ **Drop on conversion**: cue/beatgrid offsets are tied to the source encoding (encoder delay differs). Files moved as-is keep them untouched. |

**Multi-value rule for ID3v2.3 (H):** TagLib renders a text frame with several values null-separated even when saving
v2.3 (`TextIdentificationFrame::renderFields`), which v2.3 readers show as only the first value. mutagen joins with
`/` (`v23_sep='/'`); Picard has a "join with" setting (default `/`). Selecta should join with `; ` for ARTIST-like
fields where `/` occurs in names (e.g. "AC/DC") — TODO decide; proposed `", "` for ARTIST, `"; "` elsewhere.

**Text encoding (H):** TagLib frames default to Latin-1 and are rendered as UTF-16 (v2.3) or UTF-8 (v2.4) only when a
value is not Latin-1-representable (`Frame::checkTextEncoding`). Setting
`ID3v2::FrameFactory::instance()->setDefaultTextEncoding(String::UTF16)` makes every v2.3 text frame UTF-16 with BOM,
which is what Picard does for v2.3 and avoids readers that decode "Latin-1" with a local code page (L). Recommended.

## 4. ID3v2.3 vs ID3v2.4, and what DJ software reads

### 4.1 Format differences (spec: id3.org v2.3.0 and v2.4.0, copies in TagLib `mpeg/id3v2/*.txt`)

| Aspect | ID3v2.3 (1999) | ID3v2.4 (2000) |
|---|---|---|
| Text encodings | ISO-8859-1, UCS-2/UTF-16 with BOM | + UTF-16BE without BOM, UTF-8 |
| Multiple values | Not supported (convention: `/` in TPE1/TCOM/TEXT/TOLY/TOPE) | Null-separated values in any text frame |
| Dates | `TYER`, `TDAT`, `TIME`, `TRDA`, `TORY` | `TDRC`, `TDOR`, `TDRL`, `TDEN`, `TDTG` (ISO 8601) |
| New in 2.4 | — | `TMOO`, `TPRO`, `TSST`, `TIPL`/`TMCL` (replace `IPLS`), `TSOA`/`TSOP`/`TSOT`, `SEEK`, `ASPI`, `SIGN`, `EQU2`, `RVA2` |
| Frame size field | plain 32-bit integer | sync-safe 28-bit integer (iTunes historically wrote non-sync-safe sizes in 2.4; TagLib tolerates) |
| Unsynchronisation | whole tag | per frame |
| Tag location | start of file | start, or end with footer |

### 4.2 DJ software support

TODO (web research in progress): rekordbox 7, CDJ-3000/NXS2 USB standalone, Serato DJ, Traktor Pro, Engine DJ.

### 4.3 Hardware playback constraints

TODO: sample-rate / bit-depth / format limits of CDJ-3000, CDJ-2000NXS2, XDJ, Denon; WAV extensible header issue.

## 5. Cover art

TODO: discovery rules (filenames, ranking), embedding per format, DJ hardware size/format constraints, resizing.

## 6. Quality warning matrix and true-quality detection

TODO.

## 7. Naming templates and filename sanitisation

TODO: NFC/NFD, forbidden characters (macOS / Windows / FAT32 / exFAT), path and component length, collisions on
case-insensitive APFS. beets reference rules recorded in §8.

## 8. Reference implementations

### 8.1 beets `convert` plugin (`beetsplug/convert.py`, `beets/config_default.yaml`) (H)

- Shells out to `ffmpeg` per format; MP3 default `ffmpeg -i $source -y -vn -aq 2 $dest` (= LAME V2), FLAC
  `-acodec flac`, ALAC `-acodec alac`, AAC `-acodec aac -aq 1`, Opus `-ab 96k`. `-vn` drops artwork in the transcode;
  tags and art are then re-written by beets itself (`embed: yes`, `album_art_maxwidth`), not by FFmpeg.
- Transcode decision (`should_transcode`): skip when the item matches `no_convert`, or when
  `never_convert_lossy_files` is on and the source is lossy (`LOSSLESS_FORMATS = ape, flac, alac, wave, aiff`);
  transcode when bitrate ≥ `max_bitrate`, or when the format differs. Options to link/hardlink untranscoded files.
- Path legalisation defaults (`replace:`): `[<>:\?\*\|]` → `_`, `"` → `_`, `[\\/]` → `_`, leading `.` → `_`, trailing
  `.` → `_`, control chars → `_`, leading `-` → `_`, leading/trailing whitespace stripped. `max_filename_length: 0`
  (= query `statvfs`, capped at 200 bytes), truncation preserves the extension and never splits a UTF-8 sequence;
  sanitise → truncate → re-sanitise, at most three rounds. `asciify_paths: false` (when on, macOS paths are NFD).
  `id3v23: no` by default (mediafile then calls mutagen `update_to_v23()`).

### 8.2 FFmpeg as a one-shot converter — pitfalls (H unless noted)

1. Metadata keys: FFmpeg's generic keys map only a handful of ID3 frames (`ff_id3v2_34_metadata_conv`); FLAC's `BPM`,
   `INITIALKEY`, `ISRC`, `LABEL`, `TRACKTOTAL`, `DISCTOTAL` become `TXXX:<key>` frames (`id3v2enc.c` fallback).
2. v2.3 mode splits `date` into `TYER` + `TDAT`, but `compilation`, sort keys and `grouping` are only converted for v2.4.
3. Artwork from FLAC is an "attached picture" video stream; it must be mapped explicitly (`-map 0:v -c:v copy`) or it
   is dropped/re-encoded; folder images are ignored unless added as a second input.
4. MP3 defaults: ID3v2.4, no ID3v1, Xing/Info frame on, 128 kbps if no rate given.
5. AIFF: no ID3 chunk unless `-write_id3v2 1`; WAV: RIFF `LIST/INFO` only, no artwork.
6. Dither: none unless `-af aresample=...:dither_method=triangular` (or `-dither_method`).
7. FFmpeg tags its own name into `TSSE`/`encoder` unless `-fflags +bitexact` / `-metadata encoder=` (L).

### 8.3 Picard, mutagen, XLD, dBpoweramp

- Picard: canonical cross-format mapping (used in §3); v2.3 is a user option with a "join multiple values with"
  separator; TODO: confirm Picard's default ID3 version and encoding for v2.3.
- mutagen `update_to_v23()`: converts TDOR → TORY, TDRC → TYER/TDAT/TIME, TIPL/TMCL → IPLS, deletes v2.4-only frames
  including TSOA/TSOP/TSOT/TSST/TMOO; `save(v2_version=3, v23_sep='/')` joins multi-values with `/` (H).
- XLD / dBpoweramp: TODO.

## 9. Implications for Selecta

TODO (will consolidate): converter architecture, defaults table, decisions for the maintainer.

## 10. Open questions and experiments

TODO.

## 11. Sources

TODO (accessed 2026-10-19).
