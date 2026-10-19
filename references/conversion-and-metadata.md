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
   plain `WAVEFORMATEX` (H); the DJ community traces "CDJ refuses a supported 24-bit WAV" to exactly that header (M/L).
   rekordbox reads only RIFF INFO from WAV (no artwork, label or key) (H manual / M disassembly) → **AIFF, not WAV,
   for uncompressed filing**; Selecta writes its own plain-PCM WAV header when WAV is requested.
6. The "26 ms problem": Traktor treats FFmpeg's `Lavf`/`Lavc` Xing frame (and LAME tags with a zero CRC) as audio
   while rekordbox skips it, shifting cues by one MP3 frame between apps; genuine LAME tags with valid CRC are read
   consistently (M, dj-data-converter). → write MP3 with libmp3lame's own LAME tag.
7. rekordbox 7 reads tags once at import, reads `TKEY` verbatim and `TPUB` as label, and never reads BPM from tags
   (M, third-party disassembly of 7.2.19) → complete, correct tags at filing time matter more than anything later.
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

**Verdict: AIFF is the uncompressed format for a DJ library; WAV is a fallback for tag-less use.** Both carry the same
PCM, but only AIFF carries usable tags and artwork in rekordbox.

| Aspect | WAV (RIFF) | AIFF |
|---|---|---|
| Tags read by rekordbox | **RIFF `LIST/INFO` only** (manual: "When a WAVE file is added to [Collection], its tag information may not be displayed"; third-party disassembly of rekordbox 7.2.19: the `id3 ` chunk is ignored, no label, no key) (M) | ID3v2 (v2.2/2.3/2.4) in the `ID3 ` chunk, same fields as MP3 incl. `TPUB` label and `TKEY` key (H manual; M field detail) |
| Artwork in rekordbox | None (INFO has no picture; the ID3-chunk picture is ignored) (M) | Yes (APIC in the ID3 chunk) |
| INFO fields available | title `INAM`, artist `IART`, album `IPRD`, comment `ICMT`, genre `IGNR`, date `ICRD`, track `IPRT`/`ITRK` (no album artist, label, key, BPM) | — |
| CDJ playback caveat | "All players up to and including CDJ-3000 may have issues playing some WAV files" — community-traced to the `WAVE_FORMAT_EXTENSIBLE` header (M/L: GitHub `joeselway/Pioneer-DJ-File-Formats`, Reddit r/DJs) | Plain AIFF (not AIFF-C) is required; compressed AIFF-C is not playable (M) |
| Byte order | little-endian | big-endian (`FORM/AIFF`); little-endian `sowt` = AIFF-C (avoid) |
| Max size | 4 GiB (RF64/BW64 beyond; TagLib 2.3.2 reads them) | 4 GiB |

**Writer implications (H for the code facts):**
- FFmpeg's AIFF muxer writes `FORM/AIFF` for big-endian PCM (`pcm_s16be`, `pcm_s24be`) and `FORM/AIFC` for anything
  else (e.g. `pcm_s16le` → `sowt`) — always use big-endian codecs (`libavformat/aiffenc.c`).
- FFmpeg's WAV muxer always writes `WAVE_FORMAT_EXTENSIBLE` for > 16-bit PCM, > 48 kHz or non-mono/stereo layouts
  (`libavformat/riffenc.c`) and has no option to force plain `WAVEFORMATEX`. → **Selecta writes WAV headers itself**
  (≈ 100 lines: `RIFF`/`WAVE`/`fmt ` with `WAVE_FORMAT_PCM` = 1 for 16/24-bit mono/stereo, `data`), then TagLib adds
  `LIST/INFO` (and optionally an `ID3 ` chunk for other apps).
- TagLib writes the WAV ID3 chunk as `ID3 ` (upper case), reads both `ID3 ` and `id3 `, and writes `LIST/INFO`; AIFF
  gets an `ID3 ` chunk (`riff/wav/wavfile.cpp`, `riff/aiff/aifffile.cpp`). `RIFF::WAV::File::save(tags, strip,
  ID3v2::v3)` and `RIFF::AIFF::File::save(ID3v2::v3)` take the ID3 version.
- Never write 32-bit float WAV/AIFF for DJ use: rekordbox and CDJs accept only 16/24-bit integer PCM (H, rekordbox 7
  manual table; M for CDJs).

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
| BPM | `BPM` | `BPM` | `TBPM` | `TBPM` | `tmpo` (16-bit int) | TagLib `IBPM` (non-standard) | ID3 v2.3 spec: integer string. ⚠ Copy the source string unchanged (DJ tools write decimals such as `127.98`); MP4 `tmpo` rounds. rekordbox ignores it anyway (analysis). |
| Musical key | `INITIALKEY` | read `INITIALKEY` (mediafile, rekordbox) and `KEY` (Picard); **write `INITIALKEY`** | `TKEY` | `TKEY` | read `----:com.apple.iTunes:initialkey` (Picard/mediafile) and `…:KEY`; write `----:com.apple.iTunes:KEY` for rekordbox (M) | — | ID3 spec: ≤ 3 chars, e.g. `Dbm`, `o` = off key; DJ tools write Camelot (`8A`) or Open Key (`1m`). rekordbox stores the text verbatim → copy verbatim, never normalise. |
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

| Software | Reads | Writes | What matters for Selecta | Confidence |
|---|---|---|---|---|
| **rekordbox 7** | "ID3 tag (v1, v1.1, v2.2.0, v2.3.0, and v2.4.0) of MP3 and AIFF files, the meta tag of M4A files, RIFF INFO of WAVE files, and Vorbis Comment of FLAC files" (manual, identical wording since 5.x). Artwork images: JPEG or PNG. | Its own database (not the file) unless the user edits tags; tags are read **at import** — later changes need "Reload Tag". | Tags must be complete *before* filing (rekordbox imports once). Third-party disassembly of 7.2.19 (rbxport PR #231, Oct 2026): MP3/AIFF key from `TKEY` stored **verbatim**, label from `TPUB`; FLAC key from `INITIALKEY`, label from `LABEL`/`ORGANIZATION`; M4A key from `----:com.apple.iTunes:KEY` (lower-case `initialkey` ignored); **BPM is never read from tags** (analysis only); if an MP3 has ID3v2, ID3v1 is not used for title/artist. | H (manual) / M (PR) |
| **CDJ / XDJ (USB, rekordbox export)** | Metadata and artwork come from rekordbox's export database; exported thumbnails are 80×80 and 240×240 JPEG (`PIONEER/Artwork/…/a{id}.jpg`, `a{id}_m.jpg`). | — | Embedded tags matter only through rekordbox. | M |
| **CDJ (USB browsing without rekordbox data)** | ID3 v1, v1.1, v2.2, v2.3, v2.4; embedded **JPEG** artwork only; "files larger than 800 × 800 dots cannot be displayed" (CDJ-2000, CDJ-2000NXS, CDJ-900NXS manuals). | — | Embedded art: baseline JPEG ≤ 800×800. | H (old manuals), L for CDJ-3000 |
| **Serato DJ Pro** | ID3v2.2–2.4 (L). | GEOB frames (`Serato Markers2`, `Serato BeatGrid`, `Serato Overview`, …) in the existing ID3 tag; a reverse-engineering dump shows an ID3v2.3 tag (with RVAD, a v2.3-only frame) after Serato wrote to it → Serato appears to keep the file's version. FLAC: `SERATO_*` Vorbis comments; MP4: `----:com.serato.dj:*` atoms. | Copy nothing Serato-specific into converted files. | M (Holzhaus/serato-tags) |
| **Traktor Pro 3/4** | ID3v2.3/2.4; community reports say Traktor "prefers 2.4". | `PRIV` frame "TRAKTOR4" in ID3v2.3 or 2.4 MP3s; `NITR` chunk in WAV; older versions rewrote MP3 tags as v2.4. | Traktor misreads MP3s whose Xing/Info frame is FFmpeg's "Lavf/Lavc" reduced tag → 26 ms cue shift (§4.4). | L–M (forums) |
| **Engine DJ (Denon)** | ID3v2.3/2.4; converts embedded art to PNG blobs in its database. | — | Community fix for missing art: APIC with **ISO-8859-1** text encoding and a ~500–600 px baseline JPEG; re-import needed after tag changes. | L (forum) |
| **Windows Explorer / WMP (bystanders)** | ID3v2.3; v2.4 poorly supported | — | One more reason for v2.3. | M |

**Conclusion:** ID3v2.3 + UTF-16 text + Latin-1 APIC description + baseline JPEG ≤ 800 px is readable by every DJ
application and player above. ID3v2.4 is read by all of them too, but v2.3 is what Mp3tag/XLD/dBpoweramp users and the
DJ community standardise on, and what the maintainer asked for. Keep v2.4 as an option (CONV-4).

### 4.3 Hardware playback constraints (sample rate, bit depth, container)

From the rekordbox 7 manual (H) and Pioneer manuals compiled by `joeselway/Pioneer-DJ-File-Formats` (M):

| Target | MP3 | AAC (M4A) | WAV / AIFF | FLAC | ALAC |
|---|---|---|---|---|---|
| rekordbox 7 (software) | MPEG-1 L3 32/44.1/48 kHz, 32–320 kbps CBR/VBR; MPEG-2 L3 16–24 kHz | AAC-LC 16–48 kHz, 8–320 kbps | 16/24-bit, 44.1/48/88.2/96/192 kHz (no 176.4 listed, no 32-bit float) | same as WAV | same as WAV |
| CDJ-3000, CDJ-2000NXS2, CDJ-TOUR1, OPUS-QUAD | all | 44.1/48 kHz, 16-bit | ≤ 96 kHz / 24-bit | ≤ 96 kHz / 24-bit | ≤ 96 kHz / 24-bit |
| XDJ-1000MK2 | all | ≤ 48/16 | ≤ 48 kHz / 24-bit | ≤ 48/24 | ≤ 48/24 |
| XDJ-XZ, XDJ-RX3 | all | ≤ 48/16 | ≤ 48/24 | ≤ 48/24 | — |
| CDJ-2000NXS, CDJ-2000, CDJ-900(NXS), XDJ-1000, XDJ-RX/RX2/RR | all | ≤ 48/16 | ≤ 48/24 | — | — |

**Implication:** a "DJ-safe" profile for lossless filing is **16/24-bit integer, 44.1 or 48 kHz**, AIFF (or FLAC for
NXS2/3000-only setups). Hi-res (88.2/96 kHz) only plays on the newest players; 176.4/192 kHz plays nowhere on Pioneer
hardware. MP3 output is always ≤ 48 kHz anyway.

### 4.4 The "26 ms problem": which MP3 header to write

DJ-data converters (dj-data-converter issue #3, 2019; Lexicon, Rekordcloud) document that cue points on the same MP3
differ by one MPEG frame (1152 samples ≈ 26 ms at 44.1 kHz) between apps depending on the first frame:

| First frame of the MP3 | Traktor | rekordbox | Shift between them |
|---|---|---|---|
| No Xing/Info frame | — | — | none |
| Xing/Info frame, **FFmpeg "Lavf/Lavc" reduced tag** | decodes the frame as 26 ms of audio | skips it as a control frame | **26 ms** |
| LAME tag with invalid (zero) CRC | decodes as audio | skips | **26 ms** |
| Genuine LAME tag with valid CRC | skips | skips | none |

In that project's sample, 8 of 13 Lavf/Lavc-encoded files shifted vs 5 of 122 LAME-encoded files (M). **Selecta must
therefore write a genuine LAME tag (libmp3lame's own `lame_get_lametag_frame()`), not FFmpeg's muxer header** — this is
the decisive argument for driving libmp3lame directly (§2.1). Verify with a test: encode, then check that the first
frame is `Info`, the version string starts with `LAME`, and the tag CRC-16 is valid.

## 5. Cover art

### 5.1 Discovery (CONV-5) — where the folder image comes from

Reference rules (H, source read):
- **beets fetchart** (filesystem source): only the album directory itself (not subfolders); images sorted by the
  position of the first matching name in `cover_names = [cover, front, art, album, folder]`; a name matches as a whole
  word (`(\b|_)(name)(\b|_)`, case-insensitive) → "exact" candidate; otherwise the first remaining image is a fallback
  unless `cautious: yes`.
- **Picard** local files: regex `^(?:cover|folder|albumart)(.*)\.(?:jpe?g|png|gif|tiff?|webp)$` (case-insensitive);
  the suffix is split into words to detect cover types (`front`, `back`, `medium`, …); default type `front`.

**Proposed Selecta ranking** (album folder first, then a `Scans/`, `Artwork/`, `Covers/` subfolder one level down):
1. Exact basenames: `cover`, `folder`, `front` (+ `.jpg/.jpeg/.png/.webp`), case-insensitive.
2. Names containing a whole word `front`, `cover`, `folder`, `albumart` (e.g. `AlbumArt_{GUID}_Large.jpg`,
   `Artist - Album - Front.jpg`), excluding `back`, `cd`, `disc`, `inlay`, `inside`, `tray`, `booklet`, `label`.
3. Otherwise, if exactly one image is present, use it; if several, prefer the largest roughly square one (aspect within
   0.8–1.25) and mark the result "guessed" (shown for confirmation, per CONV-5).
4. Embedded art in the source file always wins over folder images; folder art is only *supplemented* when the source
   has none.

**Soulseek specifics:** album downloads must include the remote folder's images (DL-3). For singles, the remote
folder's `cover.jpg` is a separate file from the same peer — fetching it costs one more queued transfer, so it should be
**opt-in** ("also fetch folder artwork for singles") and counted by the governor like any file (cross-reference
`network-etiquette-and-safe-development.md`).

### 5.2 Embedding per target format

| Target | Container field | Selecta writes |
|---|---|---|
| MP3 / AIFF / WAV-ID3 | `APIC`, picture type 3 (front cover), MIME `image/jpeg`, empty description, description encoding Latin-1 | Exactly one APIC (front). |
| FLAC | native `PICTURE` metadata block, type 3, width/height/depth filled | One PICTURE block. |
| M4A (ALAC, P1) | `covr` atom, data type 13 (JPEG) | One image. |
| WAV for rekordbox | — (rekordbox ignores pictures in WAV) | Nothing; warn that art is lost (or suggest AIFF). |

TagLib: `file.setComplexProperties("PICTURE", {{{"data", bytes}, {"mimeType", "image/jpeg"}, {"description", ""},
{"pictureType", "Front Cover"}}})` works for every format; it replaces all existing pictures of that tag (H,
`id3v2tag.cpp`: `removeFrames("APIC")` then adds).

### 5.3 Size and format constraints → resize policy

| Constraint | Value | Source / confidence |
|---|---|---|
| CDJ embedded-art display (USB browsing) | JPEG only; > 800 × 800 not displayed | CDJ-2000/NXS/900NXS manuals (H for those models) |
| rekordbox library artwork | JPEG or PNG; makes 80×80 and 240×240 thumbnails on export | manual (H); export analysis (M) |
| Engine DJ | baseline JPEG ~500–600 px helps; Latin-1 APIC description | forum (L) |
| Progressive / CMYK JPEG | refused by some devices (shows nothing) | tagging guides (L) |
| Very large images | multi-MB tags slow imports; "above about 4 MB some hardware stalls loading the track" | Mixgraph (L) |

**Proposed default (decision for the maintainer):** when filing/converting, if the chosen art is not a baseline RGB
JPEG, or is larger than **800 px** on its longest side, or larger than 500 KB, re-encode it as baseline sRGB JPEG,
quality 90, max 800 × 800, preserving the aspect ratio (optionally centre-crop to square when the ratio is within
0.9–1.1, as XLD offers). Never upscale. The original image file stays in the album folder untouched. Setting: "Embedded
artwork max size: 600 / 800 / 1000 / 1400 px / original". Implement with `QImage` (JPEG via Qt's image plugins;
baseline is Qt's default; strip EXIF/ICC after converting to sRGB).

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
- **XLD** (macOS; official changelog): MP3 tags are ID3v2.3 with UCS-2 text (since 2006), date written as `TYER` +
  `TDAT` (2015), PCM outputs resample with libsoxr (2014), LAME 3.100 bundled (2018), embedded-art scaling and
  crop-to-square options. A good model for Selecta's defaults (M).
- **dBpoweramp** (Windows/macOS, closed): ID3v2.4 writing only since R14.3; community instructions configure
  "ID3v2 version 2.3, text encoding Unicode 16"; its author argued that v2.4 harms cross-compatibility. DSP chain
  recommended by the author: bit depth → float, resample, bit depth → 16 with **TPDF** dither; dither is "not needed"
  for MP3 output (M, forum).

## 9. Implications for Selecta

TODO (will consolidate): converter architecture, defaults table, decisions for the maintainer.

## 10. Open questions and experiments

TODO.

## 11. Sources

TODO (accessed 2026-10-19).
