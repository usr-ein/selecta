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

TODO: confirm attribute codes and units against `references/soulseek-protocol.md` / Nicotine+ source.

| Field | Source | Notes |
|---|---|---|
| Full virtual path | FileSearchResponse / shared file list | Backslash separators by convention, regardless of the peer's OS. First component is a virtual share root chosen by the peer's client. |
| Size (bytes) | always present | uint64. |
| Extension | in the protocol message, separate from the name | Often empty or inconsistent; re-derive from the filename. |
| Bitrate (kbps) | attribute 0 | Optional. For VBR some clients report the average, some the nominal. |
| Duration (s) | attribute 1 | Optional, integer seconds. Sometimes 0. |
| VBR flag | attribute 2 | Optional. |
| Sample rate (Hz) | attribute 4 | Optional, mainly lossless. |
| Bit depth | attribute 5 | Optional, mainly lossless. |

Implication: every matcher input is (path, size, maybe duration, maybe quality). The folder the file sits in is a
strong hint (album, artist, year, format), and sibling files from the same response help (track count, numbering).

## 2. Remote path conventions and pitfalls

TODO: expand with real-world examples and frequencies.

## 3. Existing parsers and matchers

TODO: beets (autotag distance, fromfilename), Picard (astrcmp, similarity, clustering, compare weights),
slsk-batchdl (strict title/artist/album, length tolerance, ranking), soularr (filename match ratio), Lidarr (Parser.cs,
DistanceCalculator, track grouping), others (rapidfuzz-cpp, Nicotine+).

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
