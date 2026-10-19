# ADR-005: Persistence

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/qt-app-architecture.md` §6; `references/library-matching-and-organization.md` §10, §13; RESEARCH-SYNTHESIS C8

## Decision

- **Selecta links its own pinned SQLite** (the amalgamation, via FetchContent) behind a thin C++ wrapper, not Qt SQL:
  Qt SQL's SQLite differs between Homebrew and official Qt builds.
- One database file in `~/Library/Application Support/Selecta/`, in WAL mode, with versioned migrations and
  repositories in `storage/`.
- FTS5 is used for UI text search only. Match keys are computed in C++ and stored; no custom SQL functions or
  extensions.
- Hot paths (matching, share lookups) run on immutable in-memory snapshots, rebuilt off-thread and swapped atomically.
- Caches (previews, analysis, search results) have explicit TTLs and size bounds (PREV-8, SRCH-12).

## Consequences

- The same SQLite behaviour on every machine and in CI.
- The wrapper must stay small: statements, transactions, migrations.
