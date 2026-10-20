# Selecta — notes for coding agents

Selecta is a GPLv3 Soulseek client in C++/Qt 6, macOS-first. Start here:

- Product requirements: `docs/PRD.md`
- Implementation plan and milestones: `docs/PLAN.md`
- Research notes (protocol, etiquette, audio, conversion, Qt, sharing, library matching): `references/`
- UI mock: `mock/index.html` — once validated by the maintainer, it is the reference for layout and interactions.

## Network safety rules (non-negotiable)

- Never connect to the live Soulseek server (`server.slsknet.org`) from tests, scripts, CI, or debugging sessions.
  Development and integration testing use a local test server (see "Safe development" in `docs/PLAN.md`).
- Never weaken or bypass the network governor (search rate limits, per-peer caps, reconnect backoff).
  Its ceilings are compile-time constants, not settings.
- Never run automated searches or downloads against the live network. Live-network sessions are run
  manually by the maintainer only.

## Working conventions

- Do not run `git add -A` / `git add .` — several agents may work in this tree at once; stage explicit paths.
- Agents work as a crew (`docs/CREW.md`). The nightman coordinates pitches and never codes. Minions build a pitch
  each, in their own worktree and `crew/` branch, and open a PR labelled `review:needed`. A reviewer approves it or
  asks for changes. Only the nightman merges. Progress: `docs/STATUS.md`; the crew's board: `.crew/STATE.md`.
- Secrets (API keys, client secrets, cookies) are never committed, logged, or copied into docs or tests.
  Development secrets live in `local/secrets.env` (git-ignored); read them from there.
- Files Selecta keeps (Inbox, collection, shares) come only from Soulseek peers (PRD D20). Preview audio may come from
  the user's own installed yt-dlp, run as an external process with no cookies or accounts. That audio stays in the
  preview cache and is never filed, shared or tagged. Never bundle, download or reimplement yt-dlp (PRD D26, PREV-11).
  Tests use a fake yt-dlp script, never the real one.

## Build, test, lint, debug

```sh
export VCPKG_ROOT="$HOME/vcpkg"             # vcpkg clone; every dependency is a static library (ADR-004)
cmake --workflow --preset dev              # configure + build + test (build/dev)
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # likewise: tsan
tools/lint.sh [paths…]                     # clang-format check, clang-tidy, clazy, cppcheck; must be clean
tools/dev/run-headless.sh                  # start Selecta with a throwaway data folder and the debug socket
tools/dev/app.sh state | tree | screenshot path=/abs.png | navigate view=<id> | click name=<objectName> | log
```

- Media modules (`audio/`, `convert/`) are on by default (`SELECTA_WITH_MEDIA`); turn it off only for quick builds.
- Run the app with `--headless --debug-server` to inspect it without a window; never with a real library unless
  `--data-dir` points elsewhere.

## Code style and structure

- C++20, Qt 6 Widgets. `namespace selecta::<module>`. Qt naming: PascalCase types, camelCase functions, `m_` members,
  `k` constants. clang-format (`.clang-format`) decides layout. Braces around every conditional body.
- `QT_NO_CAST_FROM_ASCII` and `QT_NO_KEYWORDS` are on: write `u"…"_s` / `"…"_L1`, `Q_EMIT`, `Q_SIGNALS`, `Q_SLOTS`.
- Modules (`src/`): `base` (clock, logging, paths) · `governor` (limits, buckets, permits, server guard) · `storage`
  (SQLite) · `slsk` · `meta` · `match` · `audio` · `convert` · `library` · `core` (services, engine thread) · `app`
  (Widgets UI, devtools). Dependencies point downwards; nothing below `app` uses Qt Widgets; `slsk` has no policy.
- Every budgeted network operation takes a `governor::Permit`; only the Governor creates them (tests use
  `tests/support/PermitFactory.h`).

## Engineering standards (definition of done)

- Thread affinity, ownership and failure modes are documented on every public class; every failure path is handled and
  tested; edge cases live in tests. No blocking calls on the UI or engine threads.
- Input from the network or from files is bounded and treated as untrusted.
- No half-built features: a view or control that can't work yet isn't shown, or says plainly that it isn't built.
  No fake data in the app (the mock is the only place for that).
- Warning-free with `-Werror`; `tools/lint.sh` clean; tests pass under `dev`, `asan` and `tsan`.
- Commit early, in small steps, with explicit paths; grep for secrets before committing.
