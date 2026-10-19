# Library contracts

Selecta is built by one head implementer (the main session) plus one agent per library with a clean boundary
(`docs/PLAN.md` §9). Each library has a contract here. The head implementer wrote it and integrates the result. The
library agent designs the details inside it and builds it.

| Library | Contract | Directories the agent owns | Branch |
|---|---|---|---|
| Soulseek protocol | [slsk.md](slsk.md) | `src/slsk/`, `tests/slsk/`, `tests/fuzz/slsk/`, `devenv/` | `lib/slsk` |
| Metadata and previews | [meta.md](meta.md) | `src/meta/`, `tests/meta/` | `lib/meta` |
| Audio analysis and conversion | [media.md](media.md) | `src/audio/`, `src/convert/`, `tests/audio/`, `tests/convert/` | `lib/media` |
| Name matching | [match.md](match.md) | `src/match/`, `tests/match/` | `lib/match` |

## Rules for every library agent

1. **Read first:**
   - `CLAUDE.md`, the non-negotiable network-safety rules;
   - your contract;
   - the research note it cites;
   - `docs/RESEARCH-SYNTHESIS.md` §3 for the numbers;
   - the relevant ADRs in `docs/adr/`.
2. **Stay in your directories.** Your library's `CMakeLists.txt` is picked up automatically: `src/CMakeLists.txt` adds
   `src/<lib>/`, and `tests/CMakeLists.txt` adds `tests/<lib>/`. Don't edit files outside your directories. If you need a
   change elsewhere (a vcpkg port, a governor operation, a shared helper), message the head implementer (`main`) and
   explain why.
3. **Work on your branch** (table above) in your own git worktree. Commit early and often, at least after every
   working step, so a crash loses little. Use explicit `git add <paths>`, never `git add -A`. Don't merge into `main`
   yourself: the head implementer reviews and merges.
4. **Public headers first.** Your first commit is the public API with documentation (threading, ownership, error
   model) and a short `DESIGN.md` in your source directory. Then send the head implementer a one-paragraph summary.
   Keep going while waiting; adjust if asked.
5. **Definition of done:**
   - Every public function documents its thread affinity, ownership and failure modes, and every failure path is
     handled and tested.
   - Edge cases are listed in the tests, not just in comments.
   - No blocking calls on the thread that owns your objects: no `waitFor*` and no sleeps, except in tests.
   - Memory is bounded for anything read from the network or from files.
   - No `TODO` in code paths that run. Something unsupported returns a clear error.
   - The build is warning-free (`-Werror`). `tools/lint.sh src/<lib> tests/<lib>` is clean: clang-format, clang-tidy,
     clazy, cppcheck. Tests pass under the `dev`, `asan` and `tsan` presets.
   - Tests never touch the real network. Allowed: loopback fakes, the local test server in `devenv/` (opt-in label),
     and recorded fixtures.
6. **Style:**
   - Qt naming (PascalCase types, camelCase functions, `m_` members, `k` constants), `namespace selecta::<lib>`.
   - `u"…"_s` strings: the build defines `QT_NO_CAST_FROM_ASCII`. `Q_EMIT`, `Q_SIGNALS` and `Q_SLOTS`: `QT_NO_KEYWORDS`.
   - Match the surrounding code's comment density: say why, not what.
7. **Budgeted operations take a `governor::Permit`** (`src/governor/Permit.h`) by value and call `consume()`. Tests
   get permits from `tests/support/PermitFactory.h`. Never create a back door around this.
8. **Report:** when a milestone in your contract is done, message `main` with what was built, the test count, and
   anything the head implementer must know to integrate it.

Build and test:

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
tools/lint.sh src/<lib> tests/<lib>
cmake --preset asan && cmake --build --preset asan && ctest --preset asan   # likewise tsan
```

Media libraries need `-DSELECTA_WITH_MEDIA=ON` (FFmpeg, LAME, soxr and TagLib from vcpkg; the binary cache makes it
fast after the first build).
