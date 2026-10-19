# Library matching and organisation

> Research note for Selecta (PRD LIB-1…8, SRCH-11, BATCH-3/5, DL-3…11; PLAN M4, ADR-005, ADR-008).
> Status: **draft in progress** — sections marked `TODO` are not yet verified against primary sources.
> Accessed/written: 2026-10-19.

## TL;DR

- A Soulseek search result gives us only a **remote path** (`@@share\Artist\Album\01 - Title.flac`, Windows-style
  backslashes), a **size**, and optional **attributes** (bitrate, duration in seconds, VBR flag, sample rate, bit
  depth). No tags. Every "do I already have this?" answer is therefore a *guess from a path*, and Selecta must say how
  confident it is (PRD LIB-3: exact / likely / none).
- Matching = **parse** the path (folder + filename) into artist / title / version / album / track number / year,
  **normalise** every string into comparison keys, **retrieve** candidates from an in-memory index of the collection,
  Inbox and history, then **score** with title/artist similarity, a version check and a duration check.
- Existing tools converge on the same recipe (TODO: verify constants): beets/Lidarr use a weighted *distance* over
  fields with a duration grace window; slsk-batchdl uses hard "path must contain title/artist" checks plus a length
  tolerance; Picard uses per-word edit distance. None of them parse Soulseek paths into fields as a first step for
  "already owned" detection — that part is Selecta's own design.
- Version (Original / Extended / Radio Edit / remix-by-X) is the main precision trap for a DJ library: two files with
  the same artist and title can be different tracks. Version mismatch must block "exact".
- Speed is not a problem at 50k–200k tracks if keys are precomputed: exact-key hash lookups are O(1) and fuzzy
  candidate retrieval through a token index stays well under 1 ms per result (TODO: benchmark numbers).
- Supersede (DL-7/DL-8) must be conservative: trash an Inbox single only on an *exact* match against a *verified,
  completed* album track of equal-or-better quality; everything else is a flag. Trash goes through the macOS Trash
  with Undo and an audit log; filed files are never touched.

## Key findings

TODO — filled in once the detailed sections are verified.

---

## 1. What we know about a remote file

Attribute codes verified in Nicotine+ `pynicotine/slskmessages.py` (`class FileAttribute`, commit f3b9d4d); full wire
details belong to `references/soulseek-protocol.md`.

| Field | Source | Notes for matching |
|---|---|---|
| Full virtual path | FileSearchResponse / share list / folder contents | Backslash-separated by convention whatever the peer's OS. The first component(s) are a share root chosen by the peer's client (an alias or a share name; some old clients leak real local prefixes). |
| Size (bytes) | always present | uint64. **Byte-identical copies propagate across the network**, so (size, basename) is a strong "same file" key for history matching (Nicotine+ uses exactly that to decide a file "is already downloaded", `pynicotine/downloads.py` `get_complete_download_file_path`). Useless against re-tagged or converted collection files. |
| Extension | separate protocol field | Often empty or inconsistent: re-derive from the filename, case-insensitively. |
| Bitrate (kbps) | attribute 0 `BITRATE` | Optional. Average or nominal for VBR depending on the client. |
| Duration (s) | attribute 1 `LENGTH` | Optional, integer seconds, sometimes 0 → treat 0 as missing. |
| VBR flag | attribute 2 `VBR` | Optional. (Attribute 3 `ENCODER` is obsolete.) |
| Sample rate (Hz) | attribute 4 `SAMPLE_RATE` | Optional, mostly lossless. |
| Bit depth | attribute 5 `BIT_DEPTH` | Optional, mostly lossless. |

Consequences:
- Every matcher input is `(path, size, duration?, quality?)`. There are no tags, no ISRC, no MusicBrainz IDs.
- The **parent folder** is a strong hint (artist, album, year, catalogue number, format), and **sibling files in the
  same response** help a lot: a shared numbering sequence, a constant artist across files, the track count.
- Duration is the only acoustic evidence before download, and it is integer-rounded and optional.
- After a *preview* download Selecta has the real audio and its tags, so a second, much stronger matching pass is
  possible on the cached file (tags + exact duration + optionally a fingerprint, §11).

## 2. Remote path conventions and pitfalls

Practitioner knowledge, cross-checked against the patterns that beets, Picard, Lidarr and Sockseek hard-code (§3).
Frequencies are not measured — **TODO: measure on a corpus of recorded search results from the local test network and,
later, from the maintainer's manual live sessions.**

### 2.1 Common layouts

| # | Example (virtual path) | What it tells us |
|---|---|---|
| L1 | `Music\Artist\Album (2019)\01 - Title.flac` | Artist from grand-parent, album/year from parent, title from file. Filename has no artist. |
| L2 | `share\Artist - Album (2019) [FLAC 16-44]\01. Artist - Title.flac` | Folder carries artist/album/year/quality; file repeats artist. |
| L3 | `DJ\Techno\2024\Artist - Title (Original Mix).mp3` | Beatport-style single: version always spelled out. Genre/year folders are noise. |
| L4 | `mp3\Artist-Album-WEB-2019-GRP\01-artist-title.mp3` | Scene naming: hyphens, lowercase, underscores, source tag (`WEB`, `CD`, `VINYL`), group suffix. |
| L5 | `Bandcamp\Artist - Album\Artist - Album - 01 Title.flac` | Bandcamp download naming: artist and album repeated in every file. |
| L6 | `iTunes Media\Music\Compilations\Album\1-01 Title.m4a` | iTunes/Music layout: `disc-track` prefix, VA under `Compilations`. |
| L7 | `Album\CD1\01 Title.flac`, `Album\Disc 2\...` | Disc sub-folders: the album is the grand-parent. |
| L8 | `Artist - Album [LP]\A1 Title.flac`, `B2. Title.flac` | Vinyl side/position instead of a number. |
| L9 | `VA - Compilation (2020)\03 - Artist - Title.mp3` | Various artists: artist varies per file. |
| L10 | `Mixes\Artist - Live @ Venue 2019-05-01.mp3` | Continuous mix / DJ set: one long file, not a track. |
| L11 | `Rips\Track 01.wav`, `AudioTrack 01.wav` | Untagged CD rip: no usable name at all. |
| L12 | `[LABEL042] Artist - Title.mp3`, `Artist - Title [LABEL042].mp3` | Catalogue numbers in brackets. |
| L13 | `Artist - Title (Official Video) [HQ].mp3`, `www.site.com - Artist - Title.mp3` | Web/YouTube junk to strip (Lidarr has website prefix/suffix regexes). |

### 2.2 Pitfalls that cause wrong matches

| Pitfall | Example | Mitigation |
|---|---|---|
| Hyphen is both separator and part of names | `Jay-Z`, `A-ha`, `Ne-Yo`, `Run-DMC`; `Artist-Title` in scene names | Split on spaced dashes (` - `, ` – `, ` — `) first; only fall back to bare `-` when the folder looks scene-style (no spaces, group suffix). |
| Artist/title order reversed | `Title - Artist.mp3` | Folder consistency (constant part across siblings = artist, the beets `fromfilename` trick) and a dictionary of known artists from the collection. |
| Numbers that are not track numbers | `1999`, `7 Seconds`, `808 State`, `2 Unlimited`, `50 Cent`, `10cc` | Strip a leading number only when followed by a separator and plausible as a track number (1–3 digits, optional `disc-`); confirm with siblings forming a sequence. Sockseek's `TrackNumStartRegex` requires 2–3 digits or a `N. ` / `N - ` form. |
| Featured artists | `ft.`, `feat.`, `featuring`, `(feat. X)`, `with`, `vs.`, ` x `, `&` | Extract into a secondary-artist set; never part of the primary key. `&`, `and`, `x`, `vs` are ambiguous in artist fields (`Simon & Garfunkel`, `Above & Beyond`): treat as splitters only for *matching sets*, never for display. |
| Remixer inside the title | `Title (Bicep Remix)` | The remixer is an artist name; it must go to the version field, not match the artist index. |
| Version spelling | `(Extended Mix)`, `[Extended Mix]`, `- Extended Mix`, `Extended Version`, `(Extended)` | Version extractor over `()`, `[]` and a trailing ` - ` segment; canonical classes (§5). |
| Characters illegal on Windows replaced | `AC_DC`, `AC-DC`, `ACDC` for `AC/DC`; `Title - Subtitle` for `Title: Subtitle`; `Whats` for `What's` | Normalise punctuation to spaces and compare on alphanumeric tokens; also compare a "squashed" key with all separators removed. |
| Unicode | `Röyksopp`/`Royksopp`, `Sigur Rós`, NFD vs NFC (macOS peers), full-width forms, curly quotes | NFKC + case-fold + diacritic folding + a small transliteration table for letters that do not decompose (`ø`, `æ`, `ß`, `ł`, `đ`, `þ`). |
| Mojibake | `RÃ¶yksopp` | Detect common UTF-8-read-as-Latin-1 sequences and repair before normalising (P1; low priority). |
| Generic folders | `Music`, `MP3`, `Shared`, `Downloads`, `complete`, `Singles`, `Albums`, `A`, `#`, `0-9`, years, genres | Stop-list of generic folder names; never infer artist/album from them. |
| Truncated names | Windows MAX_PATH truncation, `...` | Prefix-tolerant comparison (token containment) and lower confidence. |
| Quality tags in names | `[FLAC 24-96]`, `(320)`, `[V0]`, `WEB`, `Vinyl` | Strip into a `quality_hint` field; useful to cross-check attributes. |
| Mixed compilations | `(Mixed)`, segued tracks | Durations differ from the original; down-weight duration, mark "mixed". |
| Non-audio files | `cover.jpg`, `.nfo`, `.m3u`, `.cue`, `.log`, `.url`, `.sfv` | Not tracks; they are companion files (§14). |

## 3. Existing parsers and matchers

None of the tools below parses Soulseek paths into fields for "do I already own this?" across a whole collection;
Selecta has to combine their ideas. Code read at the commits listed in Sources.

| Tool (licence) | Purpose | Technique worth borrowing |
|---|---|---|
| **beets** (MIT) | Tag files against MusicBrainz | Weighted *distance* over fields, Levenshtein string distance with "intuitive" tweaks, duration grace window, Hungarian track assignment, filename patterns validated across a whole folder. |
| **MusicBrainz Picard** (GPL-2.0-or-later) | Tag files against MusicBrainz | Per-word similarity (`similarity2`), weighted linear combination, **multiplicative discriminators** for contradicting evidence (artist mismatch, gross length mismatch), path-based album/artist inference. |
| **Sockseek** (formerly `sldl` / `slsk-batchdl`; **AGPL-3.0**) | Batch Soulseek downloader | Boolean "strict" checks (title in filename, artist in path, album in folder) with boundary-aware containment, 3 s length tolerance, bracket check against unwanted versions, lexicographic ranking, query-guided filename parsing, "skip if already in music dir". |
| **soularr** (GPL-3.0) | Bridge Lidarr wanted list → slskd | `difflib` ratio between expected title and remote filename with "strip leading junk" retries; album accepted only if every track matches ≥ 0.8. |
| **Lidarr** (GPL-3.0) | Album manager / importer | Release-name parser (folder regexes incl. scene names), a C# port of beets' distance with its own thresholds, folder-based release grouping, multi-disc folder detection, AcoustID fallback. |
| **Nicotine+** (GPL-3.0-or-later) | Soulseek client | "Already downloaded" = same basename **and** same byte size in the download folder. |
| **rapidfuzz-cpp** (MIT, v3.3.4) | String similarity library | Header-only C++17 implementations of Levenshtein/Indel/Jaro-Winkler and `fuzz::ratio/partial_ratio/token_sort_ratio/token_set_ratio/WRatio`, with cached one-to-many scorers. TODO: confirm API names in §6. |

### 3.1 beets (`beets/autotag/distance.py`, `match.py`, `config_default.yaml`, `beetsplug/fromfilename.py`)

- **String distance** (`string_dist`): lower-case; move trailing `, the` / `, a` / `, an` to the front; `&` → `and`;
  base distance = Levenshtein on `unidecode`-transliterated, lower-cased, alphanumeric-only strings, divided by the
  longer length. Then, for each "reduced weight" pattern, the part is deleted from both strings and only a fraction of
  the distance it caused is kept as a penalty: `^the ` 0.1, `(ep|single)` 0.0, `feat/ft/featuring …` 0.1,
  `(…)` 0.3, `[…]` 0.3, `, pt./part …` 0.2. Effect: a difference inside parentheses costs 30 % of what it would in the
  main title. (Selecta must *not* copy this blindly: for DJs a parenthesised version is identity, see §5.)
- **Distance object**: each penalty in [0, 1] is multiplied by a weight; total = Σ(penalty·weight) / Σ(weight of
  applied penalties). Default weights: `artist 3.0`, `album 3.0`, `track_title 3.0`, `track_artist 2.0`,
  `track_length 2.0`, `track_index 1.0`, `tracks 2.0`, `missing_tracks 0.9`, `unmatched_tracks 0.6`, `year 1.0`,
  `media 1.0`, `mediums 1.0`, `label/catalognum/country/albumdisambig 0.5`, `album_id/track_id 5.0`,
  `data_source 2.0`, `medium 1.0`.
- **Duration**: `penalty = clamp(|Δ| − track_length_grace, 0, track_length_max) / track_length_max` with grace = 10 s and
  max = 30 s; i.e. free up to 10 s, linear to full penalty at 40 s.
- **Recommendation**: strong if best distance < 0.04, medium if ≤ 0.25, else low only if there is a single candidate or
  the gap to the second candidate ≥ 0.25, otherwise none. Penalties like `missing_tracks` cap the recommendation at
  "medium".
- **Track assignment** inside an album: Hungarian algorithm (`lap.lapjv`) over a track-distance cost matrix.
- **Filename patterns** (`fromfilename`): tried from most to least specific, and a pattern is accepted only if **all
  files of the folder** match it; if `artist` is constant across files it is the artist, if instead `title` is constant
  the two are swapped. Patterns: `^(?P<track>\d+)\.?\s*-\s*(?P<artist>.+?)\s*-\s*(?P<title>.+?)(\s*-\s*(?P<tag>.*))?$`,
  `^(?P<artist>.+?)\s*-\s*(?P<title>.+?)(\s*-\s*(?P<tag>.*))?$`, `^(?P<track>\d+)\.?[\s_-]+(?P<title>.+)$`,
  `^(?P<title>.+) by (?P<artist>.+)$`, `^(track ?)?(?P<track>\d+).*$`, `^(?P<title>.+)$`.
- **Featured-artist tokens** (`beets/plugins.py` `feat_tokens`): `ft`, `featuring`, `feat`, `feat.`, `ft.`, plus for
  artist fields `with`, `vs`, `and`, `con`, `&`, matched only at word boundaries or after `(`/`[`.
- Plugins: `duplicates` (group by configurable key fields), `fuzzy` (SequenceMatcher, threshold 0.7), `chroma`
  (AcoustID).

### 3.2 MusicBrainz Picard (`picard/similarity.py`, `picard/metadata.py`, `picard/matching.py`, `picard/util/__init__.py`)

- `similarity2(a, b)`: split both strings into words (`\W+`), for each word of the shorter list take the best
  `astrcmp` (1 − Levenshtein/max length) against the other list; a word with score > 0.6 is consumed; result =
  Σ best / (words in shorter + 0.4 × unconsumed words in longer). Word-order-insensitive and tolerant to typos.
- `Metadata.compare` weights: `title 22`, `album 12`, `artist 6`, `tracknumber 6`, `totaltracks 5`, `discnumber 5`,
  `totaldiscs 4`, plus `length 8` → weighted mean (`linear_combination_of_weights`).
- Length score: `1 − min(|Δ|, 30 s)/30 s` (linear, zero at 30 s). New in `matching.py`: a separate multiplicative
  **length discriminator** ramps from 1.0 at 60 s difference to 0.3 at 600 s, so a gross length mismatch can veto a
  partial title match.
- **Artist discriminator**: when the album title matches (≥ 0.8) but the artist clearly does not (< 0.4), the
  similarity is multiplied by 0.6 (0.5 if the track count also contradicts). Lesson: *contradicting evidence should
  multiply, not just average*.
- Thresholds: `track_matching_threshold` 0.4, `match_min_similarity` 0.25, `match_min_margin` 0.02.
- `album_artist_from_path`: drop a trailing `CD n` / `DVD n` / `Disc n` folder; album = last folder; if it contains
  ` - `, split once into artist and album; else artist = the folder above.

### 3.3 Sockseek / sldl / slsk-batchdl (`Sockseek.Core/Matching/Conditions/FileConditions.cs`, `Search/Searcher.cs`, `Search/SearchProjection/ResultSorter.cs`, `Files/TrackSkipper.cs`)

- **Strict checks** (`--strict-title`, `--strict-artist`, `--strict-album`): after preprocessing (`_` and
  Windows-illegal `: | ? > < * "` → space, diacritics removed for non-ASCII, whitespace collapsed, case-insensitive)
  the **filename without extension must contain the title**, the **full path must contain the artist**, the
  **folder path must contain the album** — as boundary-aware substrings (`ContainsWithBoundary`).
- **Length tolerance**: default **3 s** for both required and preferred conditions; a missing length is accepted
  (`AcceptNoLength = true`).
- **Defaults** (`Settings/SearchSettings.cs`): preferred = mp3, 200–2500 kbps, ≤ 48 kHz, strict title, strict album;
  required = any common audio format + 3 s tolerance.
- **Bracket check** (`CheapBracketCheck`): if the wanted title contains no `(…)`/`[…]`, prefer files whose name has
  none either (after removing `feat.` and a leading bracketed track number). A cheap guard against remixes/edits.
- **Ranking** is lexicographic over booleans, not a weighted score. Order: peer reliability, required conditions,
  preferred user conditions, has length, bracket check, strict title, fuzzy title, strict album, fuzzy album, strict
  artist, fuzzy artist, length within tolerance, format, bitrate, sample rate, bit depth, all preferred satisfied, free
  slot; then speed buckets, inferred track count, bitrate, and a deterministic hash tie-breaker. Comment in the code:
  "Identity beats quality preferences".
- **Query-guided parsing** (`InferSongQuery`): strips track numbers (`^(?:(?:[0-9][-\.])?\d{2,3}[. -]|\b\d\.\s|\b\d\s-\s)`,
  plus mid-string forms like ` - 03 `), splits on ` - `, and uses the *wanted* artist/title to decide which part is
  which (and flags `ArtistMaybeWrong`, e.g. when the artist only appears inside `(Artist Remix)`).
- **Skip existing** (`TrackSkipper`): *name* mode = local filename contains the title and the relative path contains
  the artist; *tag* mode = equal titles (lower-cased, spaces removed) and local artist contains the wanted artist.
  Implemented as a linear scan per lookup — fine for a CLI, too slow for per-result badges.
- Licence is AGPL-3.0: ideas only, no code copying into Selecta (GPLv3 §13 permits combination, but we have no reason to).

### 3.4 soularr (`soularr.py`)

- For each expected track (Lidarr title + extension) and each remote filename: `difflib.SequenceMatcher.ratio()`; if
  below the threshold, retry on the **last N words** of the remote name (N = words in the expected title, splitting on
  space or `_`) and with the album name prepended. Accept an album only when **every** expected track reaches
  `minimum_filename_match_ratio` (0.8 in the shipped `config.ini`), plus a track-count check. Simple, but shows that
  "strip leading junk, compare the tail" recovers many names.

### 3.5 Lidarr (`src/NzbDrone.Core/Parser/Parser.cs`, `MediaFiles/TrackImport/Identification/*`, `…/Specifications/*`)

- **Folder/release regexes** (`ReportAlbumTitleRegex`): `Artist - Album (Year)`, `Artist - Album - Year`,
  `Artist-Album-(Version)-WEB|CD-…-Year` (scene), `Artist - Year - Album`, discographies (`Artist - Discography
  (1990-2010)`), plus website prefix/suffix and release-group cleanup.
- **Normalisation**: `CleanArtistName` removes `a/an/the/and/or/of` (except at the start), all non-word characters,
  lower-cases and removes accents; `CleanTrackTitle` removes `feat./ft./produced by …` and bracketed
  `version|limited|deluxe|single|clean|album|special|bonus|promo|remaster(ed)` (again: right for album identity, wrong
  for DJ versions).
- **Distance**: a port of beets with weights `artist 3`, `album 3`, `track_title 3`, `track_artist 2`,
  `track_length 2`, `track_index 1`, `tracks 2`, `missing_tracks 0.6`, `unmatched_tracks 0.9`, `recording_id 10`
  (MBID or AcoustID), same 10 s grace / 30 s max for length.
- **Acceptance**: album distance ≤ 0.20 and every track ≤ 0.40 (`CloseAlbumMatchSpecification`,
  `CloseTrackMatchSpecification`); fingerprinting is triggered when album distance > 0.15, worst track > 0.40, or there
  are extra/missing tracks (`IdentificationService.ShouldFingerprint`).
- **Grouping files into releases** (`TrackGroupingService`): first by folder, accepted if `LooksLikeSingleRelease`
  (album tag: at most 25 % distinct values after merging values with Levenshtein coefficient > 0.9); fallback to
  album tag, then artist+album. VA when no artist covers ≥ 75 %. Multi-disc folders: sibling folders whose names match
  `^(?<root>.*(dis[ck]|cd)[\W_]*)\d` with the same root are merged into one release.

### 3.6 What none of them do (Selecta's own design work)

1. Blind parsing (no query) of arbitrary result paths into fields, with a confidence per field.
2. Per-result ownership lookup against 50k–200k tracks fast enough to badge results as they stream in.
3. DJ-grade version semantics (Original vs Extended vs Radio Edit vs remix-by-X are different tracks).
4. Matching against three sources with different evidence: collection (tags), Inbox (tags + origin), history
   (remote path + size of what was downloaded).

## 4. Normalisation pipeline

TODO.

## 5. Version handling

TODO.

## 6. Similarity measures

TODO.

## 7. Duration tolerance

TODO.

## 8. Album-level matching

TODO.

## 9. Confidence tiers and scoring formula

TODO.

## 10. Speed: indexing 50k–200k tracks

TODO: SQLite FTS5 (unicode61 / trigram tokenizer), Qt's bundled SQLite build flags, in-memory key maps.

## 11. Acoustic fingerprints (Chromaprint / AcoustID)

TODO.

## 12. Library scanning

TODO: TagLib 2.x, FSEvents (event IDs, renames, offline volumes), file identity, moves/renames.

## 13. SQLite schema draft

TODO.

## 14. Album detection for downloads, naming templates, companion files

TODO.

## 15. The Inbox supersede rule

TODO.

## 16. rekordbox (future option only)

Out of scope for v1 (PRD D1, LIB-8). TODO: one paragraph on rekordbox XML export vs `master.db`.

---

## Implications for Selecta

TODO.

## Edge-case test table (≥ 25)

TODO.

## Open questions and experiments

TODO.

## Sources

TODO (all accessed 2026-10-19).
