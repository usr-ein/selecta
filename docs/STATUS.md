# Selecta — development status

Kept up to date by the head implementer (session `main-impl`). Newest first within each section. The maintainer reviews
the **Decisions taken autonomously** and **Waiting for the maintainer** sections. The progress report against every PRD
requirement (2026-10-20): https://claude.ai/code/artifact/e3077d17-237e-44dd-84ce-afc8aa483747

## Road to v1 (fixed 2026-10-20)

**End goal: v1.0 release candidate 1 (`v1.0.0-rc1`) in the maintainer's hands for the live-network test (M9).** That
is a release-preset build of `main`:
- every P0 requirement and the v1 P1s below done;
- CI green;
- a scripted end-to-end pass on the local test network;
- a pass against the real metadata services;
- an ad-hoc-signed app bundle that defaults to the live server (SAFE-3);
- a test script for the maintainer.

**v1 scope (frozen):**
- **In:** every P0, plus these P1s: NET-3 (D38), NET-7, NET-8, NET-10, SRCH-14, SRCH-15, BRWS-1, BRWS-2, BATCH-7,
  BATCH-8, PREV-9, PREV-11, PREV-12, DL-10, DL-12, LIB-6, CONV-8, META-5, META-7, FAIR-5, UI-6.
- **Out:** SHARE-4 buddy-only folders (D34); P2 (PREV-10, LIB-7, LIB-8, META-6, META-8, NFR-8); Linux and Windows
  packages (they compile and test in CI only).

Each step ends at its gate; the next starts only when the gate holds.

**Where it stands (2026-10-20, 18:45):** everything built so far is merged and pushed; every teammate is closed. CI's
fixes are in main. QA pass 1 on the local test network found 19 issues (`local/qa/findings.md`): 9 fixed, the rest
minor or notes. Now the maintainer tests a **beta build** on the live network with their normal setup
(`docs/BETA-TESTING.md`): it records an action trail, stalls and "Note a Problem…" notes, which main-impl gathers
with `tools/dev/collect-feedback.sh` to fix what comes up. Left of step 1: VoiceOver names for the Converter and
Settings (a partial patch is in `local/patches/`).

| Step | What | Who | Gate |
|---|---|---|---|
| 1. Feature-complete | CONV-1 (converter rate, depth, ALAC) and Spotify "Open Again" ✅ · PREV-9 ✅ · live-test fixes ✅ · UI-5 VoiceOver and keyboard ✅ (the Converter's and Settings' names to finish) · NET-3 port mapping and reachability ✅ · BATCH-7 + FAIR-5 ✅ · BATCH-4 ✅ · LIB-6 corrections ✅ | a11y (the last item) | All merged into main; full suite green; lint clean |
| 2. CI green | macOS build, tests, lint, ASan; Linux build and tests; TSan reporting (fully gating once Qt is TSan-built) | ci | A green run on main, linked here |
| 3. Freeze and QA | Feature freeze. A scripted end-to-end pass of every view on the local test network; the real-services pass again (Spotify sign-in, a playlist, YouTube previews, tags and cover); NFR-2 timings (filter + sort of 10 000 results < 50 ms, waveform < 300 ms); every bug found fixed | main-impl + teammates on bugs | QA checklist all pass; no open P0 bug |
| 4. Release candidate | `release` preset build, version 1.0.0-rc1, live server allowed (both SAFE-3 switches), ad-hoc signed bundle with the icon, user notes in the README, tag `v1.0.0-rc1` pushed | main-impl | The RC launches in a fresh user data folder and reaches the local test network; the tag is on origin |
| 5. Maintainer tests v1 (M9) | The live-network script: pre-flight (PLAN §5.5) with a dedicated account, then a playlist through Get tracks to tagged MP3 320s in the Inbox, previews, filing, sharing and the ratio, messages and rooms. Issues go to triage, then rc2 | maintainer | Two weeks of personal use without bans → M10 (Developer ID signing and notarisation by the maintainer, DMG, `v1.0.0`) |

## Milestones

| Milestone | State | Notes |
|---|---|---|
| M0 Definition and design | ✅ | PRD v0.4 (D1–D33), PLAN v0.4, research synthesis, ADR-001…013, mock |
| M1 Foundations | 🟡 almost | Build system, static deps, app shell, storage, governor, lint, sanitizers, debug tools, devenv ✅ · CI on GitHub (macOS, lint, ASan/TSan, Linux; Windows experimental) 🔨 selecta-ci |
| M2 Protocol core and governor | ✅ | slsk S1–S7 merged (codec, server, peers, transfers, distributed, devnet, CLI) · core NetworkService, governor bookkeeping ✅ |
| M3 Search experience | ✅ | SearchService, FileRanking ✅ · **Search view** ✅ (release first, streamed results, filters, why hidden, ownership badges, 24 h SQLite cache, preview and download from results) · search history ✅ · **named filter presets** shared by Search, Get tracks and the Wishlist, min peer speed ✅ (SRCH-8, SRCH-5) |
| M4 Library and Inbox | 🟡 most | FileOps, Inbox/History stores, naming + cleaner, Inbox view, History ✅ · match X1–X5 ✅ · collection scanned, watched and matched ✅ · Inbox inspector (tags, cover, provenance, edit, choose another release) ✅ · **album downloads** ✅ (Download Folder as Album, DL-12 offer, folder listed by the peer, one tagged MP3 320 album with cover.jpg in Inbox/Albums, resumes after a restart; verified against selecta-devnet) · **supersede** ✅ (Inbox singles to the Trash with Undo, the circuit breaker asks first, filed singles only flagged) · "also on album X" flags in the Inbox and the Collection ✅ (DL-8) · Library numbers ✅ (LIB-5) · downloads carry the quarantine attribute ✅ (NFR-4) · arrival length check ✅ (DL-10) · user corrections (LIB-6) ✅ |
| M5 Preview and waveform | 🟡 most | Player, waveform, transport ✅ · Space previews from YouTube (the user's yt-dlp), else Deezer's 30-s clip, else the peer; ⇧Space the peer's file; K keeps it ✅ · analysis cache, Preview as You Move ✅ · previews that survive a restart + Discard ✅ · spectrum check: spectrogram, cached quality reports, badge and spectrogram view ✅ (PREV-9) |
| M6 Converter and arrival pipeline | ✅ | Arrival pipeline: MusicBrainz label/genre, cover, MP3 320 (verified) or keep lossy, Inbox + history ✅ · media C1–C3, A3 merged ✅ · **Converter view** ✅ (drop or add files, or "Convert…" from the Inbox; MP3 CBR/VBR, AIFF, WAV, FLAC; ID3v2.3/2.4; tags filled from folder and file names, folder cover embedded; advisor warnings with "Convert anyway"; verified, placed as new files next to the originals or in a folder) · sample rate, bit depth, ALAC; MP3 at 32/44.1/48 kHz ✅ (CONV-1) |
| M7 Sharing and fairness | 🟡 most | share library merged ✅ · core ShareService: index from the collection, distributed network, search answers, browses, uploads in queue order, upload records ✅ · Shares view, toolbar ratio ✅ · **Activity → Transfers** (DL-1: by folder or peer, states with place in queue, speed and time left, cancel and retry) and **Activity → Peers** (FAIR-1: today, session, all time, last, queue, limit meter) ✅ · buddies, ignore and ban ✅ (NET-8) · port mapping (UPnP/NAT-PMP) and the reachability count ✅ (NET-3) · buddy-only folders out of v1 (D34) |
| M8 Get tracks, wishlist, browse, messages, rooms | 🟡 most | **Get tracks end to end ✅**, also against real Spotify, yt-dlp, Deezer and MusicBrainz with the maintainer's playlist (D35; findings in task "live-test fixes") · Wishlist ✅ · Browse ✅ · Messages ✅ · Rooms ✅ · Export Not Found ✅ · native notifications and Dock badge ✅ · the whole list's time left (BATCH-4), spreading across peers (BATCH-7) ✅ |
| M9 Live-network beta | ⛔ maintainer only | never automated (CLAUDE.md) |
| M10 v1.0 | ⏳ | |

## Team (since 2026-10-20 midday)

Each teammate is its own Claude session (`claude agents` lists them; `claude attach selecta-<name>`), working in its
own worktree under `.claude/worktrees/`. They report to `main-impl`, which merges into `main` locally and pushes (D37).
Briefs: `local/briefs/` (git-ignored).

No teammate is running. Merged and closed today: selecta-previews (CACHE-1, PREV-5), selecta-notify (UI-6, NET-7), selecta-logo (D36),
selecta-fixes (live-test findings), selecta-media (PREV-9, LIB-6), selecta-upnp (NET-3), selecta-gaps (quarantine,
Spotify notice, CONV-1, Open Again), selecta-ci (CI green), selecta-a11y (UI-5 except the Converter's and Settings'
names).
The library agents of the first night (slsk, meta, match, collection, share) are merged; their worktrees are removed and
their branches are on origin.

## Decisions taken autonomously (for review)

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | CI: GCC leaves `-Wshadow` and `-Wmissing-field-initializers` to Clang; no C++20 module scanning; caches in `build/cache`; TSan reports without failing the run until Qt is TSan-built; a failing QTest check fails the GoogleTest test (`setThrowOnFail`), and QTRY waits are replaced by `ASSERT_TRUE(QTest::qWaitFor(…))`; `tools/lint.sh` analyses only what the build compiles | ~1 200 GCC hits on idioms Clang accepts; GCC scanned every TU and ccache refused to cache; uninstrumented Qt makes queued payloads look like races; a timed-out QTRY only returned, so the test passed (selecta-ci, selecta-a11y) |
| 2026-10-20 | QA pass 1: an MP3 320 or V0-class download is only tagged, with no warning (only files below the format, or in another one, get "Kept as …"); a line's title found only in the parent folder no longer fits when the file's own name says something else; the Get tracks counts and pace get their own line | CONV-2 and the mock; "DJ Orchard - Station Orchard" (an album) took that album's first track as Ready |
| 2026-10-20 | Corrections pair two track identities (artist, title, version and length within ±3 s), never file ids; supersede verdicts are kept apart from search and Get tracks verdicts; the newest verdict on a pair wins; "Same" never trashes anything | LIB-6: a correction must survive rescans, filing and a re-arrival; two different tracks can share a name and a length (selecta-media) |
| 2026-10-20 | Port mapping: NAT-PMP, then UPnP IGD, written in Selecta (no miniupnpc) with bounded reads (256 KiB HTTP, 2 KiB datagrams), every wait sliced to 100 ms, no name resolution, SSDP and control URLs only on the answering host; no PCP; renew at half the lease (≥ 1 min), permanent leases re-added every 30 min; three tries then quiet for the session; removed at quit (≤ 1.5 s) | NET-3, D38: miniupnpc reads whatever length a LAN device claims and can't be pointed at a test fake; libnatpmp is used only for the default gateway (selecta-upnp) |
| 2026-10-20 | Reachability counts the distinct peers that reached our port (PeerInit, or a PierceFireWall answering us); no outside check; isolated runs leave the router alone unless `--port-mapping` | NET-3: no third-party service; LAN peers count too, which only matters on the devnet (selecta-upnp) |
| 2026-10-20 | Converter: lossless rates up to 192 kHz (above 96 kHz it asks: players' limit), bit depth for lossless only, MP3 at 32, 44.1 or 48 kHz, each format family keeps its own choice; an MP3 at its target is copied; "Open Sign-In Page Again" reuses the pending sign-in (same PKCE state and listener) | CONV-1; a restarted sign-in would make a stale tab fail with a state mismatch (selecta-gaps) |
| 2026-10-20 | ⌃Return opens a table's context menu for its current row; pop-ups get a visible label as their buddy; the waveform tells VoiceOver about seeks, not every playback tick; the player transport and toolbar stay out of the Tab chain (every command has a menu and a shortcut) | UI-5: macOS has no other context-menu key outside VoiceOver; Qt reads a combo's name as its current item (selecta-a11y) |
| 2026-10-20 | Compilation copies give way to the artist's original release; one extra Spotify `isrc:` lookup only when a compilation copy leads; YouTube previews rank by whose upload it is (the artist's or their Topic channel, then YouTube Music's, then anybody's), with one Topic-aimed second search; genre ties broken by the recording's, then the artist's votes; a bracket group opening a title is title text | Findings of the live test (selecta-fixes); the Topic search wording is to be checked in the next live run |
| 2026-10-20 | The spectrum check informs and never changes what the Inbox gets: a likely-transcoded FLAC still becomes MP3 320; only peer previews are judged (YouTube and Deezer clips are lossy by design); spectrograms live in the analysis cache (evictable, the badge stays); inferno colour scale, fixed −120…0 dB | PREV-9; keeping suspect files as FLAC would be a product call (selecta-media) |
| 2026-10-20 | Get tracks moves a line to another peer's copy only when it's as good (fit within 0.05, a free slot if the best had one); the best stays first in line if that fails | BATCH-7; spreading must never cost quality |
| 2026-10-20 | Previews: the cache index lives in the cache folder (index.json); files expire 30 days after last use within the 2 GB LRU; Discard is D (⌫ removes rows); discarding a kept preview doesn't cancel its download | CACHE-1, PREV-5, PREV-8 (selecta-previews) |
| 2026-10-20 | Notifications only while the window isn't active (toasts always); messages and mentions coalesce per peer or room; permission asked at the first notification, never at launch; Dock badge = unread messages + arrivals while away | UI-6, NET-7 (selecta-notify) |
| 2026-10-20 | The app icon is an Icon Composer `.icon` (dark tile, the disc at 92 %) compiled by actool when Xcode 26+ is there, with the round `.icns` as the fallback; macOS 26+ never draws a truly round app icon | D36 (selecta-logo) |
| 2026-10-20 | The dev preset builds with line tables only (`-g1`) | Keeps file:line backtraces; build folders about half the size (the maintainer asked for about 10 GB for the whole repo, worktrees included) |
| 2026-10-20 | Presets: Selecta's four (MP3 320 or lossless, the default; lossless only; hi-res 24-bit; any quality) plus the user's, kept in settings.ini; each Wishlist search keeps a copy of its filter | A saved search shouldn't change under a later edit of a preset; SRCH-8 |
| 2026-10-20 | Changing Get tracks' quality ranks the lines already found again, from the search cache | Spends no search budget; a new network search only when the cache has nothing |
| 2026-10-20 | Quarantine: written directly as `0081;<time>;Selecta;`, never replacing an existing attribute; filing never adds one | LaunchServices' setter writes a row in the user's quarantine database for every file (tests too); NFR-4, ADR-009 (selecta-gaps) |
| 2026-10-20 | The Spotify terms notice shows before the first Save or Sign In; accepted once, kept in settings.ini; the engine's Spotify switch is off until then | META-4, D30 (selecta-gaps) |
| 2026-10-20 | Rooms: the room list at most once a minute, one join every 2 s, 10 rooms at most, rejoined after login; lines kept in memory (500 a room); private-message and room lines share one governor budget | references/soulseek-protocol.md §12.7; the shared budget makes the governor stricter |
| 2026-10-20 | Messages: kept in SQLite; ignored users' messages dropped unread; banned users get "Banned" and none of our shares; buddies watched at login (200 at most) | NET-7, NET-8 |
| 2026-10-20 | Live private messages take Selecta's clock; only messages re-delivered after being offline keep the server's time | A conversation stays in order whatever the server's clock says |
| 2026-10-20 | Browse lists are kept a day; Refresh asks again through the governor; a browsed folder downloaded as an album skips the folder-contents request | BRWS-2; the listing is already known |
| 2026-10-20 | The Wishlist uses the server's wishlist search, one at a time, at most every 2 minutes whatever the server says; Get tracks and the Wishlist leave a reserve of the search budget for searches typed by hand | NET-6, SRCH-14 |
| 2026-10-20 | Arrival flags a file more than ±5 s off the release (matching keeps ±3 s) | DL-10 |
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

0. **Review the decisions** above and in the progress report; answer with "keep N, change M".
1. Before M9: ask the Nicotine+ maintainers to reserve a login version number for Selecta.
2. Waveform calibration in rekordbox 7 and an import check of Selecta-tagged files (RESEARCH-SYNTHESIS §6). Two
   tag questions from the media agent (src/convert/DESIGN.md): which grouping frame rekordbox shows (GRP1 or TIT1),
   and whether it reads the M4A KEY atom.
3. Check that Undo after a move to the Trash works with the real macOS Trash (macOS may keep apps out of
   `~/.Trash`; if it does, Undo says the item is no longer in the Trash and nothing else happens).
4. Earlier development runs (before isolated runs got their own Trash) left a few synthetic FLAC files from the
   test network in your Trash (`01 - Saffron Tide.flac`, `02 - Glass Meridian.flac` and the like): safe to empty.
5. Native notifications need a real logged-in session to see the permission prompt and the banners; if macOS refuses
   the ad-hoc-signed dev bundle, `codesign --force --sign - build/dev/src/app/Selecta.app`.

Done today: the first push (main and the library branches are on origin); Spotify sign-in and a playlist import with
the maintainer's own playlist; the logo variant (B).

## How to see it

```sh
export VCPKG_ROOT="$HOME/vcpkg"
cmake --workflow --preset dev                    # configure, build, test
open build/dev/src/app/Selecta.app               # run it
tools/dev/run-headless.sh && tools/dev/app.sh state   # or inspect it headless
```
