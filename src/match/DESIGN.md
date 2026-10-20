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
    `lengthUnknown` (SRCH-18 hides files whose length *doesn't fit*; unknown isn't a misfit). Confirmed by the head
    implementer (Q-M1, 2026-10-20).
12. **Excluded phrases block, never rewrite**: a query containing one is `Blocked` (T84; etiquette §7.3 forbids
    evasion). Words are never wildcarded.
13. **Supersede circuit breaker with a floor** — decided by the head implementer (Q-M2, 2026-10-20): it trips when
    more than 10 singles would go, or when at least 4 would go and that is more than 30 % of the Inbox singles.
    Supersedes of 1–3 files never trip it (the normal case; they only go to the Trash). Tested: 3 of 5 (no trip),
    4 of 10 (trips), 11 of 100 (trips).
14. **Out of scope here:** T52 (scanner read-only open: `library/`), the trash/Undo/notification side of T36–T42
    (core); the decision side of T36–T42 is tested. Aliases (LIB-6) are not built: the alias table belongs to storage
    and can be applied when building `MatchIndexInput`.
15. **Retrieval (§10).** One group per query word (its exact term, or up to 8 vocabulary terms within its typo budget)
    plus the title's squashed key. The two rarest *seed* groups (title and artist words, the title's squashed key) are
    united, and a third while there are fewer than 32 candidates: two, so one stray word can't keep the true record
    out. Every other group then narrows a list of more than 64 to its records (never to nothing) or boosts the ones it
    holds; body-only words and the artist's squashed key read as a title (reversed names) only boost. The 200
    heaviest records that aren't mixes are scored. Copy and layout recall on the benchmark: 100 %.
16. **Lossless pruning.** Every positive tier and the "other version" relation need a title of 0.85 or more. Before
    any alignment, the matcher asks whether the title similarity or the token-bag coverage could still reach 0.85,
    crediting each token with the best score it could get from lengths and first letters alone and stopping at the
    first token whose loss settles it; candidates that can't are dropped. Property-tested: "can't reach" is never
    wrong (20 000 random field pairs with typos, joins and leet forms), so results are the same as without pruning.
17. **Ties.** At equal tier and score the same version wins, so a named version beats an unversioned twin. The §9
    ambiguity cap (two fitting versions, the choice is a guess → likely) applies only when the best isn't the same
    version.
18. **Index layout for speed.** Term texts sit in one pool behind an open-addressing table (no allocation per lookup);
    the symmetric-delete and leet hashes are 32-bit, sorted, with a bucket directory; delete variants are hashed
    without building strings; per-term facts (length, light word, digits) are precomputed so scoring scans nothing.
    ASCII names skip NFKD/NFC in the normaliser (property-tested against the general path); the fuzzy lookup drops
    terms more than 2 units longer or shorter before verifying them (no rule matches beyond that); single-member
    artists are normalised once. The benchmark asks for a performance core (QoS user-interactive) so the numbers
    measure the code, not the scheduler.
19. **Album matching (§8).** The folder's album is the most common parsed album, folder artist (or the track artist
    at least half the files agree on) and year. Candidates: the albums holding the tracks owned, the album key and
    most album-title words, and the duration signature (same count ± 3 tracks, neighbouring 8 s bands, ≥ 5 lengths
    paired within ±3 s). A file fits a track by its §9 score, or by half of the length agreement when its name says
    nothing (unparseable, another script), whichever is higher; pairs with no evidence at all stay unmapped (missing
    and extra). A full signature (same count ≥ 5, every sorted length within ±3 s) makes the album likely in
    collection by itself (T48) unless artist and title both contradict it (similarity < 0.5); its pairs within ±3 s
    count as likely. "As singles" means owned outside the best album. The best album: in collection, tier, tracks
    vouched for, distance, collection before Inbox.
20. **Duration consensus (§9.1, T51).** Clusters join results at most 3 s apart and span at most 6 s, so a chain
    never merges two versions; peers count once per cluster. A cluster is named by the explicit version most of its
    labelled results carry, so one mislabelled file doesn't rename it. "Confident" (BATCH-6) needs ≥ 3 peers in the
    preferred cluster and no other cluster that could be the wanted version backed by two peers or more: a single
    peer's odd length (a truncated file) is noise, two unlabelled lengths for an unversioned line are a guess. With
    T51's data an unversioned line has no preferred cluster (both are named by non-original versions).
21. **Supersede (§15).** Local against local: a small MatchIndex of the album tracks (or of the copy already there)
    and `Matcher::matchLocal` give the §9 verdict with exact lengths; another version is reported (VersionDiffers,
    T40), likely pairs only flag. Exact then needs the strict rule on top: equal squashed title keys and bracket
    extras, equal artist sets, the same version, both lengths known and within 2 s. Guards for an exact pair, in
    order: changed → Recheck, busy → Defer, filed → Flag, arrived after the album → Flag, album not complete and
    verified → Flag, a quality rank unknown → Flag, the single better → Flag, else Trash. The circuit breaker counts
    the Trash pairs of the event (> 10, or ≥ 4 and > 30 % of the Inbox singles); they stay Trash and
    `needsConfirmation` asks core to confirm first. The arrival pipeline (`decideArrival`) uses the same matching
    and guards (without the album-only ones) and replaces only when the new copy ranks strictly higher; equal →
    SameQuality, worse → SingleBetterQuality, both flagged. Quality is ranked as given (the source quality from the
    history). Every decision has a one-line `summary` for the activity log and an explanation of each check.

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

Benchmark (`selecta_match_benchmark`, Release, Apple M-series, performance core; 5 runs, median; load average
below 3 from other agents' builds):

| Synthetic library | 200 000 tracks, 20 000 artists, 30 000-word Zipf vocabulary, versions, albums |
|---|---|
| `MatchIndex::build` | 1.1–1.3 s |
| Index memory | 91.6 MB (`memoryUsage()`, ≈ 0.46 KB per track); RSS growth of the first build ≈ 104 MB |
| Parse + match per result | p50 38 µs, **p95 78 µs**, p99 123 µs |
| Match only (`Matcher::match`) | p50 20 µs, **p95 58 µs** |
| Parse only (`RemotePathParser::parse` with the artist dictionary) | p50 18 µs, p95 29 µs |
| `ReleaseScorer::score` | 2.0 µs |
| Quality (10 000 results) | exact copies found 100 %, other layouts 100 %, a typo in both artist and title 83 %, not-owned names matched 0.1 % |

The contract's p95 < 100 µs holds for parse + match together. On a saturated machine (load average 8 on 12 cores)
the same run measures parse + match p95 at 115–140 µs, even on the CPU clock: shared caches and lower clocks, not the
code. The 17 % of double-typo results that aren't found are the §17.3 rules at work (a typo in a word of ≤ 3
letters, two fuzzy fields, an uncorroborated short-word edit), not retrieval: precision comes first.

## Progress

- Done: public headers and this design (approved); X1 normaliser, version classifier, lexicon (merged to main as
  8d21dcf); token rules, field alignment, token index, artist dictionary; X2 remote path parser; X4 TracklistParser,
  ReleaseScorer (rank(): identity, then quality, availability only as a tie-breaker) and QueryPlanner; X3 MatchIndex
  and Matcher (merged as cc3e064); the benchmark and the performance work of decisions 15–18 (match p95 1.5 ms →
  58 µs, parse + match p95 78 µs, index 104 → 92 MB); 533 tests.
- Parser notes: hand-written scanners in `detail/RemoteName` (cleaning, numbering) and `RemotePathParser.cpp`
  (folders, sibling facts, artist/title order as log-odds evidence: prior 1.4, sibling constant 3.0, folder artist
  2.0, known artist 2.5 + up to 1.0 by weight, member-only 1.5, fuzzy 1.2, release artist 2.5, title features 1.0;
  ambiguous below 1.0). A number prefix the rules would keep is stripped when the dictionary knows the name only
  without it ("101 Artist - Title"); "(Artist - Title) Remixer Remix" names are unwrapped. Folders literally named
  "Album" or "Albums" are generic containers.
- Done since: AlbumMatcher (X3, decision 19) with a Hungarian assignment tested against brute force, and a TSan test
  of one AlbumMatcher shared by many threads; Matcher and AlbumMatcher share `detail::bestOf`.
- Supersede API, revised with the head implementer (2026-10-20): `decideArrival` for core's arrival pipeline returns
  its own `SupersedeArrivalDecision`; `LocalTrackFacts::quality` is the best evidence of the real quality (the
  source quality from the history, since downloads become MP3 320); every decision carries a one-line `summary`.
- Done since: DurationConsensus (X4, decision 20), so X4 is complete.
- Done since: SupersedeDecider (X5, decision 21): T36–T42 on the decision side, the circuit breaker's 3/5, 4/10,
  4/20 and 11/100 cases, and the arrival pipeline; 578 tests. Every deliverable of the contract is in.
- Next (optional): a `selecta-cli` demo command; memory could drop further by interning artist sets and versions
  apart (records now intern one meta per artist set × version × extra text).
- Open questions: none (Q-M1 and Q-M2 answered, decisions 11 and 13).
- Working rules from the head implementer: `tools/lint.sh src/match tests/match` before each commit; a note to
  `main` at each milestone; `Matcher` and everything else safe to call concurrently on one shared index.
