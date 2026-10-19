# Selecta — development status

Kept up to date by the head implementer during autonomous development. Newest first within each section. The
maintainer reviews the **Decisions taken autonomously** and **Waiting for the maintainer** sections.

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Definition and design | ✅ | PRD v0.4 (D1–D33), PLAN v0.4, research synthesis, ADR-001…010/012/013, mock |
| M1 Foundations | 🟡 almost | Build system, static deps, app shell, storage, governor basics, lint, sanitizers, debug tools ✅ · CI workflow, BUILDING.md, devenv (with the slsk agent) ⏳ |
| M2 Protocol core and governor | 🟡 started | `slsk/` agent running · Governor (head) ⏳ |
| M3 Search experience | ⏳ | needs M2 + `match/` + `meta/` |
| M4 Library and Inbox | ⏳ | head; uses `match/` |
| M5 Preview and waveform | 🟡 started | `audio/` (media agent) running · player and widget (head) ⏳ |
| M6 Converter and arrival pipeline | 🟡 started | `convert/` (media agent) running · pipeline (head) ⏳ |
| M7 Sharing and fairness | ⏳ | |
| M8 Get tracks, wishlist, browse, messages, rooms | ⏳ | |
| M9 Live-network beta | ⛔ maintainer only | never automated (CLAUDE.md) |
| M10 v1.0 | ⏳ | |

## Library agents

| Agent | Branch | Contract | State |
|---|---|---|---|
| slsk | `lib/slsk` | [contracts/slsk.md](contracts/slsk.md) | launched 2026-10-20 |
| meta | `lib/meta` | [contracts/meta.md](contracts/meta.md) | launched 2026-10-20 |
| media | `lib/media` | [contracts/media.md](contracts/media.md) | launched 2026-10-20 |
| match | `lib/match` | [contracts/match.md](contracts/match.md) | launched 2026-10-20 |

## Decisions taken autonomously (for review)

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | Everything links statically from vcpkg (custom triplet `arm64-osx-selecta`, macOS 14.4 target); Homebrew only for tools | Maintainer: "full static. No dylib, or just the core macOS ones". Verified: the app loads only `/System/Library` frameworks and `/usr/lib` system libraries |
| 2026-10-20 | Qt **6.11.2** for now (vcpkg's current baseline), not 6.12 | 6.12 isn't in vcpkg yet; bump the baseline when it lands (ADR-004) |
| 2026-10-20 | Keychain access through the macOS Security framework directly, not QtKeychain | QtKeychain's vcpkg port pulls in all of qttools; Security.framework is a system framework (the static-linking rule) |
| 2026-10-20 | UI tests run on Qt's `minimal` platform | The static Qt build has no `offscreen` plugin; headless screenshots use the native `cocoa` platform with an off-screen window instead |
| 2026-10-20 | `governor::Permit`: budgeted sends need a token only the Governor can mint | Makes the CLAUDE.md network rule structural rather than a convention |
| 2026-10-20 | Debug builds ship a local debug socket (`--debug-server`) and `selecta-cli app …`; release builds don't | Maintainer asked for self-debugging capability; same-user socket only |
| 2026-10-20 | Settings theme is a popup button, not a segmented control | Qt has no native segmented control; a popup is the native equivalent |
| 2026-10-20 | Matching engine is a separate library (`src/match/`) built by its own agent | Pure logic with a large written spec: the clean boundary the maintainer asked for |

## Waiting for the maintainer (nothing blocks development)

1. Before the first push: rewrite the early commits' author to `usr-ein <usr-ein@pm.me>` (D16), create the GitHub
   repository, push. Not done autonomously: it's outward-facing.
2. Before M9: ask the Nicotine+ maintainers to reserve a login version number for Selecta.
3. Waveform calibration in rekordbox 7 and an import check of Selecta-tagged files (RESEARCH-SYNTHESIS §6).
4. Spotify: register a development app with redirect `http://127.0.0.1/callback`. The Client ID in
   `local/secrets.env` is used only by manual smoke tools.

## How to see it

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --workflow --preset dev                    # configure, build, test
open build/dev/src/app/Selecta.app               # run it
tools/dev/run-headless.sh && tools/dev/app.sh state   # or inspect it headless
```
