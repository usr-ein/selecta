# Contract: `match/`, name matching and ranking

| | |
|---|---|
| **Owner** | match library agent, branch `lib/match` |
| **Integrator** | head implementer (`library/` ownership badges, search ranking, Get tracks, supersede) and the `meta/` agent (`ReleaseResolver` uses the normaliser) |
| **Spec** | `references/library-matching-and-organization.md` §1–10, §14–15, §17, the edge-case table (≥ 25 cases) and "Implications for Selecta" |
| **Decisions** | ADR-008, PRD D23 (length first, ±3 s), D28 (typo tolerance), LIB-3, SRCH-11/17/18/19, BATCH-5, DL-7 |

## Purpose and boundary

Pure matching logic, with no I/O, no database and no UI:

- turn a Soulseek remote path, plus its sibling file names and attributes, into a parsed record;
- normalise names into comparison keys;
- score candidates against a collection snapshot or a release, typo-tolerantly and position-independently;
- plan network queries.

It depends on Qt Core (`QString` and Unicode normalisation) and rapidfuzz-cpp (MIT, from vcpkg).

## Threading

Everything is pure or immutable:

- A `MatchIndex` snapshot is built once and can then be shared read-only across threads as
  `std::shared_ptr<const MatchIndex>`.
- Building a snapshot can take a while; it happens on a worker thread and is swapped atomically by the caller.
- No globals and no hidden caches, unless they are thread-safe and documented.

## Components and deliverables

### X1. Normaliser and versions
- `Normalizer` producing keys as in §4.2:
  - NFKD, case folding, diacritic folding;
  - punctuation and separator rules;
  - "feat." handling; `&` → `and`;
  - number and roman-numeral rules where the note says so;
  - a `kNormVersion` constant.
- `VersionClassifier` (§5): Original, Extended, Radio Edit, remix-by-X, VIP, Dub, Instrumental and so on; remixer
  extraction; compatibility (`same`, `other version`, `unsure`). Word lists live in one data file compiled in.

### X2. Remote path parser
`RemotePathParser` (§4.1):
- input: a path (backslash-separated, raw display string), sibling names and attributes;
- output: `ParsedRemote` with artist, title, version, album, track number, year and disc, each with a confidence and
  the rule that fired;
- validates one filename pattern across siblings;
- takes artist and album from parent folders;
- merges `CD1`/`Disc 2` folders;
- recognises reversed "Title - Artist" names when the evidence says so (§17).

### X3. Similarity, index and matcher
- Per-token OSA with length budgets (§17.3):
  - ≤ 3 characters and numbers: exact;
  - 4–5: one edit, no substitution;
  - 6–8: one edit;
  - ≥ 9: two edits;
  - same first letter up to 8 characters;
  - fuzzy token score `1 − 0.15 × edits`;
  - joined-word check with `fuzz::ratio` on squashed strings.
- `TokenIndex`: exact postings plus a symmetric-delete fuzzy index (§17.6).
- `MatchIndex`: an immutable snapshot of library records (collection, Inbox, history) with exact-key maps.
- `Matcher` (§9):
  - weights title 0.45, artist 0.30, duration 0.20, album 0.05;
  - multiplicative vetoes for version conflict, artist contradiction and duration;
  - duration windows: exact ±3 s; likely `max(6 s, 3 %)`; far `max(15 s, 8 %)`; veto beyond `max(60 s, 25 %)`;
  - tiers `exact` / `likely` / `none` and relations `same`, `otherVersion`, `upgrade`;
  - an **explanation** of every decision, for the inspector;
  - past downloads match exactly on (size, basename).
- `AlbumMatcher` (§8): folder ↔ album by track count and sorted durations, with the coverage rule.

### X4. Release scoring and query planning
- `ReleaseScorer` (§17.5): fit of a network file to a release, `0.35·s_dur + 0.40·cov_T + 0.25·cov_A`. "Only this
  release" keeps fit ≥ 0.8 with `s_dur` = 1.
- `QueryPlanner` (§17.7):
  - network query words from a release: ≥ 3 characters, no stop or version words, ≤ 4–5 words, spelled as the release;
  - no wildcards, ever;
  - excluded server phrases removed or reported;
  - the "Search wider" ladder: each step is one explicit, budgeted user action.

### X5. Supersede decision (pure)
`SupersedeDecider` (§15): given an Inbox single and a verified album, decide trash, flag or nothing, applying every
guard:
- exact tier, same version, lengths within 2 s;
- album quality ≥ single quality;
- the circuit-breaker inputs.

File operations stay with `core/`.

## Tests and acceptance

- The whole edge-case table from the note, as parameterised tests, plus §17's tests (T53–T87 and the rest).
- Property tests:
  - normalising twice changes nothing;
  - scores are symmetric where they should be;
  - no typo budget makes 3-letter words fuzzy.
- Google Benchmark on a synthetic 200 k-track library: build time, memory, µs per result, p95 < 100 µs. Record the
  numbers in `DESIGN.md`.
- dev and asan are clean; lint is clean.
- `DESIGN.md`: API overview, thresholds, the integration points for `library/` and `meta/`.
