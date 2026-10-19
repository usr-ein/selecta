# ADR-008: Matching: remote paths, typos, versions, supersede

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/library-matching-and-organization.md` §4–10, §15, §17; PRD D23, D28, LIB-3, SRCH-17…19, DL-7

## Decision

- **Pipeline:**
  1. `RemotePathParser`: folder and filename, validated against siblings.
  2. `Normalizer`: versioned keys.
  3. `VersionClassifier`: Original, Extended, Radio, remix-by-X.
  4. `MatchIndex`: an immutable snapshot of the collection, the Inbox and the history.
  5. `Matcher`: weights title 0.45, artist 0.30, duration 0.20, album 0.05; contradictions veto; tiers exact,
     likely and none.
  6. `AlbumMatcher`.
- **Length first (D23):**
  - Exact within ±3 s.
  - Likely within `max(6 s, 3 %)`.
  - Far within `max(15 s, 8 %)`.
  - Veto beyond `max(60 s, 25 %)`.
- **Typos (D28):**
  - Per-token OSA with budgets by length: ≤ 3 letters exact; 4–5 letters one edit, no substitution; 6–8 letters one
    edit; ≥ 9 letters two edits.
  - Same first letter up to 8 letters. A fuzzy token scores `1 − 0.15 × edits`.
  - Joined words are checked with `fuzz::ratio` on squashed strings.
  - Retrieval uses a symmetric-delete token index.
  - Scorers come from **rapidfuzz-cpp** (MIT).
- **Queries** are spelled from the matched release. "Search wider" is an explicit, budgeted click that drops words;
  never wildcards.
- **Supersede (DL-7)** trashes an Inbox single only when all of these hold:
  - the album is verified and complete;
  - the match is exact, the version is the same and the lengths agree within 2 s;
  - the album copy is at least as good.

  It goes through the Trash, with Undo and an audit log. A circuit breaker asks first beyond 10 files per event or
  30 % of the Inbox singles.

## Consequences

- A labelled corpus (about 500 pairs, from local sessions and the search cache) gates matcher changes in CI.
  Targets: exact precision ≥ 99.5 %, likely precision ≥ 90 %.
