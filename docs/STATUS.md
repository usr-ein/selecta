# Selecta — development status

Kept up to date by the head implementer during autonomous development. Newest first within each section. The
maintainer reviews the **Decisions taken autonomously** and **Waiting for the maintainer** sections.

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Definition and design | ✅ | PRD v0.4 (D1–D33), PLAN v0.4, research synthesis, ADR-001…013, mock |
| M1 Foundations | 🟡 almost | Build system, static deps, app shell, storage, governor, lint, sanitizers, debug tools, devenv ✅ · CI workflow ⏳ |
| M2 Protocol core and governor | ✅ | slsk S1–S7 merged (codec, server, peers, transfers, distributed, devnet, CLI) · core NetworkService, governor bookkeeping ✅ |
| M3 Search experience | 🟡 most | SearchService, FileRanking ✅ · **Search view** ✅ (release first, streamed results, filters and presets, why hidden, ownership badges, 24 h SQLite cache, preview and download from results) · search history (P1) ⏳ |
| M4 Library and Inbox | 🟡 most | FileOps, Inbox/History stores, naming + cleaner, Inbox view, History ✅ · match X1–X5 ✅ · collection scanned, watched and matched ✅ · Inbox inspector (tags, cover, provenance, edit, choose another release) ✅ · **album downloads** ✅ (Download Folder as Album, DL-12 offer, folder listed by the peer, one tagged MP3 320 album with cover.jpg in Inbox/Albums, resumes after a restart; verified against selecta-devnet) · **supersede** ✅ (Inbox singles to the Trash with Undo, the circuit breaker asks first, filed singles only flagged) · "also on album X" flags shown in the views ⏳ |
| M5 Preview and waveform | ✅ | Player, waveform, transport ✅ · Space previews from YouTube (the user's yt-dlp), else Deezer's 30-s clip, else the peer; ⇧Space the peer's file; K keeps it (a preview in flight becomes the download) ✅ · analysis cache, 2 GB preview cache, Preview as You Move ✅ · spectrum check (P1) ⏳ |
| M6 Converter and arrival pipeline | ✅ | Arrival pipeline: MusicBrainz label/genre, cover, MP3 320 (verified) or keep lossy, Inbox + history ✅ · media C1–C3, A3 merged ✅ · **Converter view** ✅ (drop or add files, or "Convert…" from the Inbox; MP3 CBR/VBR, AIFF, WAV, FLAC; ID3v2.3/2.4; tags filled from folder and file names, folder cover embedded; advisor warnings with "Convert anyway"; verified, placed as new files next to the originals or in a folder) |
| M7 Sharing and fairness | 🟡 most | share library merged ✅ · core ShareService: index from the collection, distributed network, search answers, browses, uploads in queue order, upload records ✅ · Shares view, toolbar ratio ✅ · **Activity → Transfers** (DL-1: by folder or peer, states with place in queue, speed and time left, cancel and retry) and **Activity → Peers** (FAIR-1: today, session, all time, last, queue, limit meter) ✅ · buddies/bans (P1) ⏳ |
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
| share | `lib/share` | [contracts/share.md](contracts/share.md) | ✅ merged: share index, safe opening for uploads, upload queue (135 tests) |

## Decisions taken autonomously (for review)

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | The Converter makes each file in Selecta's staging folder, verifies it, then places it as a new file (next to the original by default, or in a chosen folder); a taken name gets " (2)" | CONV-3: originals untouched, nothing overwritten; ADR-009 keeps writes outside the caches in FileOps |
| 2026-10-20 | Activity has three pages: Transfers (DL-1), Peers (FAIR-1) and the Log (SAFE-4) | The mock's Activity is the transfer list; the log still needs a home |
| 2026-10-20 | The transfer list shows this session's downloads and the unfinished ones; earlier finished ones are in History | A transfer list of every download ever would bury what's moving now |
| 2026-10-20 | A file of an album retried after the album was made arrives on its own, as a single | The album folder is already published; a late file shouldn't stay stuck |
| 2026-10-20 | An album download asks the peer for the folder's listing first (FolderContents, through the governor); after 30 s without an answer it takes the files the search found | The search only shows the files that matched the query; the album is the whole folder |
| 2026-10-20 | An album brings its audio files and images only (no .cue, .log, .nfo); a missing image doesn't make it incomplete; only a complete album sends singles to the Trash | Cue sheets and rip logs don't belong in a DJ library; supersede must never act on a partial album (T39) |
| 2026-10-20 | Album tags: the album's track list from Spotify when the release picked has an album id, else each file's own tags (or its name), with the album fields the same in every file; cover: fetched, else the folder's image, else the files' own | DJ software groups tracks by their album fields; D29/D30 for the sources |
| 2026-10-20 | Supersede and Inbox trash notifications offer Undo, which moves the item back from where the Trash put it | DL-7 asks for Undo; the mock does the same for items moved to the Trash by hand |
| 2026-10-20 | When the circuit breaker trips, the singles stay and the notification asks ("Move to Trash") | T42 says ask first; a notification is enough, nothing is lost if it's ignored |
| 2026-10-20 | `--data-dir` runs move "trashed" files to `<data>/Trash`, never the user's Trash | An isolated run must leave nothing outside its folder |
| 2026-10-20 | Selecta answers at most 10 searches a minute (burst 5), on top of the governor's connection budget | Each answer opens a connection: answering must never crowd out what the user asked for |
| 2026-10-20 | The ratio is "healthy" while under 1 GB was taken in 30 days or a quarter of it went back; "low" otherwise | FAIR-3 needs a status; generous on purpose until M9 shows real numbers |
| 2026-10-20 | Space previews from YouTube (the user's yt-dlp), else Deezer's clip, else the peer's file; Settings can pick one | D26 and PREV-12 as written; the fallback keeps Space useful without yt-dlp |
| 2026-10-20 | A Get (or K) while the same file previews makes the preview the download, without a second transfer | PREV-5: keep without re-downloading, also before the preview has finished |
| 2026-10-20 | A result that would be an "Upgrade" over a collection copy that is already MP3 320 (or lossless) shows "In collection" | Every download arrives as MP3 320 (D29): it would only make the same file again |
| 2026-10-20 | Search results only claim "Upgrade" or "In collection" for sure matches; an unclear version shows "Likely owned" | To a DJ another mix is another track |
| 2026-10-20 | Searches that found nothing aren't kept in the search cache | An empty answer is worth asking again later; the governor still keeps a repeat quiet for 10 minutes |
| 2026-10-20 | Free text that covers the release's artist and title words picks the release by itself, like "Artist - Title" does | SRCH-17: the query names the track either way |
| 2026-10-20 | Searching again keeps the results on screen until the new answers arrive | A refused or empty repeat (the governor's 10-minute quiet) shouldn't lose what was found |
| 2026-10-20 | Shares: audio, covers and text companions only; banned users get `Banned`; no buddy-only folders in v1; incoming searches match with diacritics folded | The sharing note's recommendations (D-S1, D-S2, D-S4, D-S7) |
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
5. Check that Undo after a move to the Trash works with the real macOS Trash (macOS may keep apps out of
   `~/.Trash`; if it does, Undo says the item is no longer in the Trash and nothing else happens).
6. Earlier development runs (before isolated runs got their own Trash) left a few synthetic FLAC files from the
   test network in your Trash (`01 - Saffron Tide.flac`, `02 - Glass Meridian.flac` and the like): safe to empty.

## How to see it

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --workflow --preset dev                    # configure, build, test
open build/dev/src/app/Selecta.app               # run it
tools/dev/run-headless.sh && tools/dev/app.sh state   # or inspect it headless
```
