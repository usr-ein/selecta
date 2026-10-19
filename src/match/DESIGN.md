# `match/` — name matching and ranking

Pure, thread-safe matching logic for Selecta: no I/O, no database, no network, no UI. Contract:
`docs/contracts/match.md`. Specification: `references/library-matching-and-organization.md` (§1–10, §14–15, §17, the
edge-case table) and ADR-008. Depends on Qt Core and rapidfuzz-cpp only.

## Layers

```
Types ── Normalizer ── VersionClassifier ── Lexicon (all word lists, one compiled-in data file)
            │
   Similarity (TokenRules, FieldMatcher) ── TokenIndex ── ArtistDictionary
            │
   RemotePathParser ──────────────┐
            │                     │
        MatchIndex ── Matcher ── AlbumMatcher
            │
   ReleaseScorer ── QueryPlanner     Tracklist · DurationConsensus · SupersedeDecider · Duration · Quality
```

| Header | Deliverable | What it does |
|---|---|---|
| `Types.h` | — | Source, Tier, Relation, VersionClass/VersionInfo/VersionCompatibility, Codec, AudioQuality, RemoteAttributes |
| `Normalizer.h` | X1 | `kNormVersion`; field keys (§4.2): NFKD, case fold, marks dropped, transliteration table, symbols, CamelCase split, mojibake repair, artist sets, title decomposition, `TrackKeys` for storage (§13) |
| `VersionClassifier.h` | X1 | version phrases → class, remixers, modifiers, remaster/mixed flags; title decomposition; compatibility (§5) |
| `RemotePathParser.h` | X2 | path + siblings → `ParsedRemote` with per-field confidence and rules (§4.1, §17.5 A), zones for §17.5 B |
| `Similarity.h` | X3 | per-token OSA budgets (§17.3), symmetric token alignment with joined pairs and the squashed/leet routes (§17.4) |
| `TokenIndex.h` | X3 | exact postings + symmetric-delete index, prefix 7 (§17.6) |
| `ArtistDictionary.h` | X2/X3 | known artists (collection, release candidates) with exact and fuzzy lookup |
| `MatchIndex.h` | X3 | immutable snapshot of collection, Inbox and history (§10) |
| `Matcher.h` | X3 | ownership: history, retrieval, §9 scoring + §17.5 B verification, tiers, relations, explanation |
| `AlbumMatcher.h` | X3 | remote folder ↔ local album: names, duration signatures, Hungarian mapping, coverage (§8) |
| `Duration.h`, `Quality.h` | X3 | §7 windows and s_dur; §9 quality ranks; SRCH-6/7 "established" |
| `ReleaseScorer.h` | X4 | fit of a remote file to a release (§17.5 B), "Only this release", identity order |
| `QueryPlanner.h` | X4 | query words from a release, excluded phrases, the "Search wider" ladder (§17.7) |
| `Tracklist.h`, `DurationConsensus.h` | X4 | Get tracks line parsing and duration clusters (§9.1, T50, T51) |
| `Supersede.h` | X5 | the §15 decision table, pure; core does the file operations |

## Threading, ownership, errors

- Every function is pure or a const member of an immutable object. No globals, no caches, no `static` regular
  expressions (hand-written scanners instead). Everything can be called from any thread concurrently.
- `MatchIndex::build()` is the only expensive call: run it on a worker thread and swap the
  `std::shared_ptr<const MatchIndex>` atomically. `Matcher`/`AlbumMatcher` hold the snapshot they were given.
- No exceptions are thrown on purpose and no function fails: untrusted input is bounded (path 4096 code units, fields
  512, 6 folders, 2000 siblings, 24 tokens per field, 200 candidates, 100 album tracks, 4 M records) and anything
  unparseable yields an explicit "unparseable"/None result.

## Decisions (within the contract; for review)

1. **Token alignment is the similarity.** `s_title`, `s_artist` and `s_album` come from the §17.4 alignment under the
   §17.3 budgets, not from raw `fuzz::ratio`/`token_sort_ratio` (§6): with ratio, `Lev Teh Wrld Behnd` (T80) would
   pass. rapidfuzz supplies the bit-parallel OSA pre-filter and `fuzz::ratio` for the squashed route.
2. **Symmetric by construction.** The alignment runs greedily in both directions and keeps the better one, so
   `compare(a, b) == compare(b, a)` (property-tested). Token weight = length in code points; stop words weigh 1.
3. **Squashed route = one token.** Equal squashed or leet keys give 1.0 (T53, T64, T65, T66, T78). Otherwise the two
   squashed keys are compared as one token under the field budget, digits must agree, and `fuzz::ratio ≥ 85` gates it,
   so `blink182`/`blink183` never pass.
4. **Version modifiers.** Besides class and remixer, other class words are kept as modifiers: "Bicep Dub" ≠ "Bicep
   Remix", "Extended Vocal Mix" ≠ "Extended Mix", "X Edit" ≠ "X Remix". Conservative for exact precision (≥ 99.5 %).
   A live venue is the version's `by`. `VersionCompatibility` adds `ProbablySame` (Unspecified vs Original, §5 row 2)
   to the contract's same / unsure / other version.
5. **Retrieval keys without version.** Exact-key retrieval ignores the version so conflicts are still found and
   reported as "other version"; the version decides in scoring. Squashed artist and title keys are extra terms of the
   token index, so joined names (`okumadrift`) are retrieved too.
6. **Hybrid ownership (§17.5).** Each candidate is scored on the parsed fields and as a zone token bag against its own
   tags; the better title/artist evidence wins. The bag route gives exact only when the body is fully explained
   (u = 0) by exact tokens and the parse had a segment split.
7. **Unexplained share per segment.** u is measured within the dash-separated body segment(s) holding the matched
   title, so a long artist name can't dilute "Love Story" against "Love" (T75).
8. **Track-number prefixes** are stripped only with a leading zero, a punctuation separator, or a sibling sequence —
   `808 State`, `2 Unlimited`, `50 Cent` keep their numbers (T11); `01.flac` alone is a number, not a title (T48).
9. **Mojibake repair (P1) is implemented**: cheap, strict (valid UTF-8 required), idempotent; T47 is exact.
10. **History is exact only** ((size, basename), (peer, path)); no fuzzy history matches (T20/T21).
11. **"Only this release" with an unknown length** keeps a file when the name-only fit is ≥ 0.8 and marks it
    `lengthUnknown` (SRCH-18 hides files whose length *doesn't fit*; unknown isn't a misfit). Open question Q-M1.
12. **Excluded phrases block, never rewrite**: a query containing one is `Blocked` (T84; etiquette §7.3 forbids
    evasion). Words are never wildcarded.
13. **Supersede circuit breaker** is literal: > 10 trashes or > 30 % of the Inbox singles asks first, even for a small
    Inbox (1 of 2 singles). Open question Q-M2.
14. **Out of scope here:** T52 (scanner read-only open: `library/`), the trash/Undo/notification side of T36–T42
    (core); the decision side of T36–T42 is tested. Aliases (LIB-6) are not built: the alias table belongs to storage
    and can be applied when building `MatchIndexInput`.

## Integration points

- **`library/` (head):** build `MatchIndexInput` from SQLite on a worker thread (tags, durations, quality, album ids,
  history rows); `MatchIndex::build`; swap atomically; keep `generation` for lazy re-scoring. Store `TrackKeys`
  columns from `Normalizer::trackKeys` with `kNormVersion`; re-key when it changes.
- **Search (head):** per response folder, `RemotePathParser::parseFolder(folder, files, {&index->artists(),
  releaseArtists})`, then `Matcher::match` per file for badges (off the UI thread). With a chosen release:
  `ReleaseScorer(release).score(parsed, length)`, `fits` for "Only this release", `compareIdentity` for "Best match".
  Queries: `QueryPlanner(excludedPhrases).plan(release, step, typed)`; send `text` through the governor only when
  `isReady()`.
- **Album views / DL-12:** `AlbumMatcher::match(folderFiles)`.
- **Get tracks:** `TracklistParser::parse`, `ReleaseScorer` per line, `durationConsensus` for pre-ticking.
- **Supersede (core):** on `AlbumDownloadCompleted`, candidates via `Matcher::matchLocal` per album track, then
  `SupersedeDecider::decide(state, tracks, singles, inboxSingleCount)`; execute Trash only after the confirmation
  when `needsConfirmation`; re-stat right before (set `changedSinceIndexing`).
- **`meta/`:** `Normalizer::fieldKey/titleKeys/artistKeys` and `VersionClassifier` for `ReleaseResolver`;
  `FieldMatcher::compare` gives typo-tolerant per-field similarity with the same rules as the matcher.

## Thresholds

See the constants in `Duration.h`, `Matcher.h`, `AlbumMatcher.h`, `ReleaseScorer.h`, `QueryPlanner.h`,
`Similarity.h`, `Supersede.h`; they are the note's "Proposed defaults" and ADR-008, unchanged.

## Tests and benchmark

`tests/match/`: one parameterised suite per component; the note's edge-case table T01–T87 as named parameterised
cases (T52 excepted, see above); property tests (idempotent normalisation, symmetric field similarity, no fuzzy
3-letter tokens); concurrency test under TSan; Google Benchmark on a synthetic 200 k-track library.

Benchmark numbers: pending (X3).

## Progress

- Done: public headers and this design (first commit).
- Next: X1 — lexicon, Normalizer, VersionClassifier, their tests.
- Open questions: Q-M1 (unknown length and "Only this release"), Q-M2 (circuit breaker on a small Inbox).
