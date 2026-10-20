# Selecta — development status

Kept up to date by the head implementer during autonomous development. Newest first within each section. The
maintainer reviews the **Decisions taken autonomously** and **Waiting for the maintainer** sections.

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Definition and design | ✅ | PRD v0.4 (D1–D33), PLAN v0.4, research synthesis, ADR-001…013, mock |
| M1 Foundations | 🟡 almost | Build system, static deps, app shell, storage, governor, lint, sanitizers, debug tools, devenv ✅ · CI workflow ⏳ |
| M2 Protocol core and governor | ✅ | slsk S1–S7 merged (codec, server, peers, transfers, distributed, devnet, CLI) · core NetworkService, governor bookkeeping ✅ |
| M3 Search experience | 🟡 | SearchService + FileRanking ✅ · Search view (manual searches) ⏳ |
| M4 Library and Inbox | 🟡 most | FileOps, Inbox/History stores, naming + cleaner, Inbox view, History ✅ · match X1–X5 ✅ · collection scanner merged ✅ · core integration (index, Owned/In Inbox states, watcher) ⏳ |
| M5 Preview and waveform | 🟡 most | Player (decoder thread, lock-free ring, CoreAudio AUHAL), rekordbox-style waveform, transport, Inbox previews ✅ · Get tracks previews (peer file, yt-dlp, clips) ⏳ |
| M6 Converter and arrival pipeline | 🟡 most | Arrival pipeline: MusicBrainz label/genre, cover, MP3 320 (verified) or keep lossy, Inbox + history ✅ · media C1–C3, A3 merged ✅ · Converter view ⏳ |
| M7 Sharing and fairness | ⏳ | slsk answering hooks ready; share index, upload queue ⏳ |
| M8 Get tracks, wishlist, browse, messages, rooms | 🟡 | **Get tracks end to end ✅** (paste → release lookup → find → tick → Get → download with retries and resume → converted, tagged MP3 320 in the Inbox; verified against selecta-devnet) · wishlist, browse, messages, rooms ⏳ |
| M9 Live-network beta | ⛔ maintainer only | never automated (CLAUDE.md) |
| M10 v1.0 | ⏳ | |

## Library agents

All four library contracts are delivered and merged.

| Agent | Branch | Contract | State |
|---|---|---|---|
| slsk | `lib/slsk` | [contracts/slsk.md](contracts/slsk.md) | S1–S7 ✅ merged. Fixing one fixture race (stale upload token after a killed transfer over the indirect path) |
| meta | `lib/meta` | [contracts/meta.md](contracts/meta.md) | M1–M6 ✅ merged (Spotify, MusicBrainz, covers, Deezer, resolver, yt-dlp, clips, CLI; one manual smoke run) |
| media | `lib/media` | [contracts/media.md](contracts/media.md) | A1–A3, C1–C3 ✅ merged, plus `probeStreamInfo` (header-only probe) |
| match | `lib/match`, `lib/collection` | [contracts/match.md](contracts/match.md), [contracts/collection.md](contracts/collection.md) | X1–X5 ✅ and the collection scanner ✅ merged |

## Decisions taken autonomously (for review)

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | Downloads and conversions live in Application Support (`Downloads/Incoming`, `Downloads/Converting`), not the caches | macOS may empty caches under a partial download |
| 2026-10-20 | A Get tracks line whose peer refuses or fails for good moves on to the next peer's copy the search found; a file that arrived but couldn't be converted doesn't | The first failure is the peer's; the second isn't |
| 2026-10-20 | Transient download failures retry after 3, 10 and 30 minutes and resume the partial file; denials marked retryable by slsk (queue full) too | DL-2; the governor's back-off still applies on top |
| 2026-10-20 | The engine and the app need the media libraries (`SELECTA_WITH_MEDIA`); without them only the libraries build | Arrival converts and the player decodes: there is no useful app without them |
| 2026-10-20 | Worker threads hand results back through mutex-guarded mailboxes, not queued Qt events with payloads | The prebuilt static Qt isn't TSan-instrumented, so payloads inside its event queue show as races; standard primitives keep TSan meaningful |
| 2026-10-20 | Space plays and pauses (not a menu shortcut); ⌥←/⌥→ skip 5 s, ⇧⌥←/⇧⌥→ 30 s, ⌘. stops | A menu key equivalent would take Space from text fields; arrows alone belong to the lists |
| 2026-10-20 | Get tracks draws its own tick boxes | The macOS style draws item-view check boxes only for the current row in some window states |
| 2026-10-20 | Get tracks keeps only files that can make a real MP3 320: lossless sources or MP3s at 320 kbps (other presets: lossless only, any) | D29 converts everything to MP3 320; anything lower would be an upsample |
| 2026-10-20 | Tests and `--data-dir` runs point every metadata service at a closed loopback port (`--metadata-network` lifts it) | Tests and automated runs must never reach Spotify, Deezer or MusicBrainz |
| 2026-10-20 | Playback goes straight to CoreAudio (AUHAL) instead of `QAudioSink` first | Qt Multimedia is a heavy static dependency for one class; ADR-003 planned AUHAL before v1.0 anyway |
| 2026-10-20 | Sanitizer builds compile GoogleTest from source | An uninstrumented vcpkg GoogleTest caused false ASan container overflows |
| 2026-10-20 | TagLib 2.3.2 through a vcpkg overlay port (`cmake/vcpkg-ports/taglib`) | The baseline has 2.3.1; 2.3.2 hardens parsing of crafted files, and every download is untrusted |
| 2026-10-20 | Supersede circuit breaker: trips above 10 files, or at 4 or more files that are over 30 % of the Inbox singles | The literal 30 % rule would ask about every album that replaces one or two singles in a small Inbox; trashed files stay recoverable |
| 2026-10-20 | "Pause network activity" persists across launches | A paused network shouldn't wake up by itself after a restart |
| 2026-10-20 | Activity log kept 30 days, at most 100 000 lines | SAFE-4 needs a history; this bounds the database to about 10 MB |
| 2026-10-20 | A server protocol violation needs the user before reconnecting | Retrying can't fix it, and a codec bug must never hammer a server |
| 2026-10-20 | Image decodes capped at 256 MB process-wide in `main()`; artwork code also checks dimensions first | Cover art comes from untrusted files and services; the cap is global, so the app sets it, not a library |
| 2026-10-20 | Everything links statically from vcpkg (custom triplet `arm64-osx-selecta`, macOS 14.4 target); Homebrew only for tools | Maintainer: "full static. No dylib, or just the core macOS ones". Verified: the app loads only `/System/Library` frameworks and `/usr/lib` system libraries |
| 2026-10-20 | Qt **6.11.2** for now (vcpkg's current baseline), not 6.12 | 6.12 isn't in vcpkg yet; bump the baseline when it lands (ADR-004) |
| 2026-10-20 | Keychain access through the macOS Security framework directly, not QtKeychain | QtKeychain's vcpkg port pulls in all of qttools; Security.framework is a system framework (the static-linking rule) |
| 2026-10-20 | UI tests run on Qt's `minimal` platform | The static Qt build has no `offscreen` plugin; headless screenshots use the native `cocoa` platform with an off-screen window instead |
| 2026-10-20 | `governor::Permit`: budgeted sends need a token only the Governor can mint | Makes the CLAUDE.md network rule structural rather than a convention |
| 2026-10-20 | Debug builds ship a local debug socket (`--debug-server`) and `selecta-cli app …`; release builds don't | Maintainer asked for self-debugging capability; same-user socket only |
| 2026-10-20 | Settings theme is a popup button, not a segmented control | Qt has no native segmented control; a popup is the native equivalent |
| 2026-10-20 | Matching engine is a separate library (`src/match/`) built by its own agent | Pure logic with a large written spec: the clean boundary the maintainer asked for |

## Waiting for the maintainer (nothing blocks development)

0. **Try the main flow locally:** `build/dev/tools/devnet/selecta-devnet --generate ~/devnet-corpus` prints a
   server address; put it in Settings → Soulseek (any user name and password), paste lines from
   `~/devnet-corpus/tracklist.txt` into Get tracks, Find Them, then Get. The MP3s land in the Inbox.

1. Before the first push: rewrite the early commits' author to `usr-ein <usr-ein@pm.me>` (D16), create the GitHub
   repository, push. Not done autonomously: it's outward-facing.
2. Before M9: ask the Nicotine+ maintainers to reserve a login version number for Selecta.
3. Waveform calibration in rekordbox 7 and an import check of Selecta-tagged files (RESEARCH-SYNTHESIS §6). Two
   tag questions from the media agent (src/convert/DESIGN.md): which grouping frame rekordbox shows (GRP1 or TIT1),
   and whether it reads the M4A KEY atom.
4. Spotify: register a development app with redirect `http://127.0.0.1/callback`. The Client ID in
   `local/secrets.env` is used only by manual smoke tools.

## How to see it

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --workflow --preset dev                    # configure, build, test
open build/dev/src/app/Selecta.app               # run it
tools/dev/run-headless.sh && tools/dev/app.sh state   # or inspect it headless
```
