# Selecta — Product Requirements Document (PRD)

| | |
|---|---|
| **Status** | Draft v0.4 — the Get tracks flow, yt-dlp previews, typo-tolerant matching and question round 4 folded in (D26–D33) |
| **Date** | 2026-10-19 |
| **Product** | Selecta — a native Soulseek client for DJs and collectors |
| **Stack** | C++ / Qt 6, macOS-first, cross-platform, GPLv3 |
| **Companion docs** | [PLAN.md](PLAN.md) (implementation plan) · [../references/](../references/) (research) · [../mock/index.html](../mock/index.html) (UI mock) |

Requirement IDs (`SRCH-3`, `DL-7`, …) are stable: the plan, the code and the tests refer to them.
Priorities: **P0** = first usable version (MVP), **P1** = v1.0, **P2** = later.
The raw brief is reproduced verbatim in [Appendix B](#appendix-b--raw-brief-verbatim); the answers to the first question rounds are in [Appendix A](#appendix-a--decisions-log).

---

## 1. Summary

Selecta is a free, open-source desktop client for the Soulseek peer-to-peer network, written in C++ with Qt 6 and
released under the GPLv3. It runs on macOS first and builds on Linux and Windows.

It is designed around one workflow, **Get tracks** (D27): a list of tracks goes in, and tagged MP3 320 files come out.

1. **List** the tracks you want: paste lines or a Spotify playlist link.
2. **Match** each line to its release on Spotify (title, artists, length, ISRC, cover) before any network search.
3. **Find** the best file on Soulseek for each line. Quality is strict, the length must be within ±3 s of the release,
   and names are matched with typo tolerance. Searches are paced.
4. **Preview** before committing, with a Pioneer-style 3-band waveform (low = blue, mid = orange, high = white). The
   full track comes through the user's own yt-dlp, or from the peer's file.
5. **Get** them. Each file lands in the **Inbox** as MP3 320 with Spotify tags in ID3v2.3 and the cover embedded, and
   is then **filed** into the collection.

Three things support that flow:

- **Digging:** search with strict filters ("MP3 = 320 kbps CBR", "lossless ≥ 16-bit/44.1 kHz").
- **Collection awareness:** what you already own, what is waiting in your Inbox, and what you downloaded before.
- **Fairness:** searches are paced, downloads per peer are counted and capped, and your upload/download ratio is
  always visible.

It looks and behaves like a native Mac app, in light and dark appearance.

## 2. Problem

Existing clients (SoulseekQt, Nicotine+, slskd) are general-purpose. For a DJ/collector:

- **Results are noisy.** You can't enforce "MP3 = 320 kbps CBR only" or "lossless ≥ 16/44.1" as hard filters, and
  mislabelled files (e.g. a "320" that is really 128) slip through.
- **You download blind.** There's no audition before committing to a download, and no waveform.
- **They don't know your collection.** You re-download what you already own and miss upgrade opportunities.
- **There's no post-download pipeline.** Turning a FLAC into a properly tagged MP3 320 with ID3v2.3 artwork takes
  separate tools (XLD, dBpoweramp, Mp3tag, ffmpeg).
- **There's no self-regulation.** How many files did I take from this peer today? Am I sharing back enough?
- **Getting a tracklist** is manual, or done with command-line tools that can trip the server's search limits.

## 3. Users

**Primary user (v1): the maintainer** — a DJ and collector on macOS (Apple Silicon) who uses rekordbox 7.
- The collection is a set of music folders on disk, imported into rekordbox, which **references the files by path**.
- The DJ library is generally MP3 320. Lossless is downloaded when it's the best available source.
- Music is organised as:
  - **Singles:** all downloaded single tracks in one flat folder.
  - **Albums:** one folder per album, with all its tracks.
  - A single later obtained as part of a full album is redundant.

**v1 is "me first":** optimise for this workflow and make things configurable only where it's cheap. Other users are
welcome (it's open source), but v1 spends no effort on onboarding polish or on Windows/Linux packaging.

## 4. Goals and non-goals

### Goals (v1)

| # | Goal |
|---|---|
| G1 | Find the right file in the right quality fast: strict filters, presets, sensible ranking. |
| G2 | Audition before downloading: the full track through the user's yt-dlp or the peer's file, a 3-band waveform, keyboard-driven. |
| G3 | Never download what you already have by accident: collection, Inbox and history awareness; upgrade detection. |
| G4 | A clean pipeline: download → convert and tag on arrival → Inbox → file into the collection (D29). |
| G5 | **Get tracks:** a list in, tagged MP3 320s in the Inbox out; safely paced, with a preview before committing (D27). |
| G6 | Fair and safe by construction: rate limits, per-peer counters and caps, visible ratio, real sharing. |
| G7 | Feel native on macOS: system controls and font, light/dark appearance, accent colour, shortcuts, speed. |

### Non-goals (v1)

- Full chat-room features (tickers, private rooms, room administration). A minimal rooms view is in scope (NET-10, D32).
- Reading rekordbox, Apple Music or other DJ-software libraries; writing to rekordbox. The collection = folders on disk.
- Windows/Linux packages (they must compile and pass unit tests in CI).
- Mobile apps, web UI, headless daemon mode.
- Tagging from a guess. Tags come from the release a file was matched to (D29). A file with no matched release keeps
  its own tags until the user picks a release (META-4).
- Getting files from streaming services. Every file in the Inbox and the collection comes from a Soulseek peer
  (D20). Preview audio from the user's own yt-dlp stays in the preview cache and is never filed or shared (D26).
- Anything that circumvents the server's or peers' rules: multiple accounts, ban evasion, impersonating the official
  client, queue jumping.
- Telemetry of any kind.

## 5. Product principles

1. **Good network citizen by construction.** Limits live in one core "network governor", not in UI code. Defaults are
   conservative. Hard ceilings are compile-time constants; settings can only make limits stricter.
2. **Never touch filed files.** Once a file is filed into the collection, Selecta never moves, renames, re-tags or deletes
   it (rekordbox links files by path). Selecta writes *new* files into the collection when filing, and only ever trashes
   files in its own Inbox and caches.
3. **Quality truth.** Show real quality; never silently upsample or transcode; warn when a conversion cannot improve
   quality; flag files whose size contradicts their declared quality.
4. **Keyboard-first audition.** ↑/↓ moves through results and the preview follows; Space plays; ←/→ scrubs.
5. **Native and quiet.** Follow the macOS Human Interface Guidelines, system appearance and accent colour. No custom skin.
6. **Local and private.** No telemetry. Credentials live in the Keychain. Local paths are never revealed to peers.

## 6. Glossary

| Term | Meaning |
|---|---|
| Peer | Another Soulseek user. |
| Share | The folders a user makes available to others. |
| Slot / queue | Uploaders serve a limited number of transfers at once (slots); others wait in their queue. |
| Session | One run of Selecta, from launch to quit. Sessions are numbered and dated. |
| Inbox | Selecta-managed staging area where downloads land, already converted and tagged, before being filed. Selecta may modify and trash Inbox files. |
| Filing | Moving an Inbox item into the collection — the only way Selecta writes into the collection. Conversion and tagging already happened on arrival (D29). |
| Collection | The user's music folders on disk. Read-only for Selecta, except for filing new files into it. |
| Single | A track downloaded on its own. Filed into the collection's Singles folder. |
| Album download | An explicit "download whole folder" of a remote release. Filed as one folder per album. |
| Supersede | An album download that contains a track the user already has as a single. |
| Preview cache | Where preview audio is stored (peer files and yt-dlp audio); size-bounded; separate from the Inbox; never shared. |
| Release | The canonical track or album a line or search is matched to (Spotify first, else Deezer or MusicBrainz): title, artists, album, length, ISRC, cover. |
| Search budget | The network searches the governor allows right now: a token bucket (by default up to 6 at once, then one more every 30 s). |

## 7. Functional requirements

### 7.1 Account and connection (NET)

| ID | Pri | Requirement |
|---|---|---|
| NET-1 | P0 | Log in with username and password; credentials stored in the macOS Keychain. Show the connection state (offline / connecting / online / away) and which server is in use. |
| NET-2 | P0 | Configurable listen port, reported to the server; indirect connections through the server when a direct connection fails. |
| NET-3 | P1 | Automatic port mapping (UPnP / NAT-PMP) and a reachability check. |
| NET-4 | P0 | Reconnect with exponential backoff and jitter. Never reconnect automatically after being disconnected because the account logged in elsewhere. |
| NET-5 | P0 | Server address is configurable. Debug builds default to the local test server and refuse the live server unless explicitly enabled (see SAFE-3). |
| NET-6 | P0 | Honour server-provided parameters, e.g. the wishlist interval and the excluded search phrases. |
| NET-7 | P1 | Private messages: receive, read and reply; unread badge; system notification. |
| NET-8 | P1 | Buddy list, ignore list and ban list (they affect uploads and private messages). |
| NET-9 | P0 | Identify honestly as a third-party client. The exact login version fields follow the protocol research. |
| NET-10 | P1 | **Minimal chat rooms** (D32): list rooms, join and leave, read and post, unread counts. One view under Network in the sidebar; no tickers, private rooms or room administration. Posts are user-typed only, under the same limits as private messages. |

### 7.2 Search and filters (SRCH)

| ID | Pri | Requirement |
|---|---|---|
| SRCH-1 | P0 | Free-text search using Soulseek query syntax (e.g. `-word` to exclude). Each search stays open in the sidebar with its result count; results stream in live. |
| SRCH-2 | P0 | **Strict format filter:** multi-select among FLAC, MP3, WAV, AIFF, ALAC/M4A, AAC, OGG, OPUS. |
| SRCH-3 | P0 | **Lossy quality rule:** bitrate operator (any / = / ≥ / ≤) and value (128 … 320 kbps); CBR only, or VBR allowed (V0/V2 classes). Example: "MP3 = 320 kbps CBR". |
| SRCH-4 | P0 | **Lossless quality rule:** minimum bit depth (16 / 24) and allowed sample rates (44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz). |
| SRCH-5 | P0 | Other filters: duration range, size range, free upload slot only, maximum peer queue length, minimum peer speed, excluded words, and owned / downloaded results shown, dimmed or hidden. |
| SRCH-6 | P0 | **Strict mode:** results whose quality can't be established (missing attributes) are hidden. With strict mode off they show an "unknown" marker. Bitrate estimated from size ÷ duration is always labelled "estimated". |
| SRCH-7 | P0 | **Plausibility check:** flag results whose size contradicts their declared format/bitrate/duration (e.g. declared 320 kbps but the size implies ~128). |
| SRCH-8 | P0 | **Filter presets:** named and saved (e.g. "DJ — MP3 320", "Lossless CD", "Hi-res"), one default. Search, Get tracks and the Wishlist use the same presets. |
| SRCH-9 | P0 | **Filtered-out transparency:** "N of M results shown", with a breakdown of why results were hidden (format, bitrate, unknown quality, owned, peer limits…). |
| SRCH-10 | P0 | Two result views: grouped by remote folder (release) or a flat track list. Sort by quality, availability (free slot, queue, speed), size, duration, or fewest downloads from that peer. |
| SRCH-11 | P0 | **Ownership badges** on every result: *In collection* (exact match) · *Likely in collection* (fuzzy) · *Upgrade* (you own it in lower quality) · *In Inbox* · *Downloaded this session* · *Downloaded in a past session* · *Queued*. |
| SRCH-12 | P0 | **Search cache:** results are stored in SQLite with a TTL (default 24 h). Re-running a cached query shows the cached results instantly, labelled with their age; "Refresh" re-runs it on the network and costs one search from the budget. |
| SRCH-13 | P0 | Every network search goes through the governor's rate limiter. The UI always shows the remaining search budget and when it refills. |
| SRCH-14 | P1 | **Wishlist:** saved searches (each with a preset) re-run in the background at the server-provided interval; new matching results raise a notification. |
| SRCH-15 | P1 | Search history: recent queries with their presets. |
| SRCH-16 | P0 | Per-search caps bound memory and peer load. Collecting stops at the first of: 15 s without a new response, 120 s in total, 300 responding peers or 10 000 files. Later results are dropped by token. |
| SRCH-17 | P0 | **Release first.** Each search is resolved to release candidates (Spotify when signed in, otherwise Deezer or MusicBrainz), shown above the results with artwork, title, artist, album, year, length and label. When the query names a track, the best release is chosen automatically. Otherwise the user picks one, or "Any (name only)". The chosen release becomes the reference for ranking and filtering (D23). |
| SRCH-18 | P0 | **Length identifies the track.** Each file's length is compared with the release's: within ±3 s counts as the same recording; beyond that, the file is flagged as another version or track. "Only this release" (on by default) hides files whose length or name doesn't fit. "Best match" ranks by fit to the release (length first, then artist, title and version), then quality, availability and peer fairness. It never ranks by name alone. |
| SRCH-19 | P0 | **Typo-tolerant matching** (D28): names match word by word with Damerau–Levenshtein (OSA) distance scaled to word length, wherever the words appear in the path (artist, title or album folder); joined words count ("okumadrift"). A misspelled query is sent to the network spelled as the matched release, and the summary says so. "Search wider" sends fewer words, to catch misspelled file names, for one search from the budget. |

### 7.3 Browse user shares (BRWS)

| ID | Pri | Requirement |
|---|---|---|
| BRWS-1 | P1 | Browse a peer's shared folders as a tree, with the same filters and ownership badges; download files or whole folders from it. |
| BRWS-2 | P1 | Cache browsed share lists (TTL, default 24 h) and rate-limit browse requests through the governor. |

### 7.4 Get tracks (BATCH)

| ID | Pri | Requirement |
|---|---|---|
| BATCH-1 | P0 | **Add tracks:** paste a tracklist (one track per line, "Artist - Title" or free text), open a `.txt` file, or paste a Spotify playlist or album link (META-3). Lines are parsed, normalised and de-duplicated; adding more keeps the lines already resolved. |
| BATCH-2 | P0 | One filter preset applies to every line (e.g. "MP3 = 320 kbps CBR"). |
| BATCH-3 | P0 | Lines already in the collection or the Inbox are marked and skipped by default. |
| BATCH-4 | P0 | Searches queue through the governor and are paced. The UI shows progress, ETA and the current budget, with pause / resume / cancel. Cached results are reused without spending budget. |
| BATCH-5 | P0 | **Release first, then review.** Each line is first resolved to a release (title, artist, length, ISRC), shown before any network search. Files are then scored against that release: length first, then name and version, quality, availability and peer fairness. The line shows its best match and alternatives. Confident matches (length within ±3 s, version agrees) are pre-ticked; the user previews, unticks what they don't want and presses Get. Nothing downloads before Get. |
| BATCH-6 | P0 | **Get N tracks** downloads every ticked line; each arrives in the Inbox converted and tagged (D29). Lines already owned, in the Inbox or downloading say so and are never ticked. |
| BATCH-7 | P1 | Approved downloads are spread across peers, respecting per-peer caps. |
| BATCH-8 | P1 | Export unresolved lines (not found / ambiguous) as text. |
| BATCH-9 | P0 | **Get tracks is the home screen** (D27): Add tracks, the quality wanted, Find them / Pause with progress and time left, then a table of Track (with its matched release and length), Status, Best file on Soulseek (quality, length check, peer, alternatives) and Preview. The inspector shows the tags the file will get and the other candidates. |

### 7.5 Preview and waveform (PREV)

| ID | Pri | Requirement |
|---|---|---|
| PREV-1 | P0 | Preview downloads the file into the preview cache (not the Inbox). Playback starts as soon as enough is buffered (progressive) for formats that allow it: MP3, FLAC, WAV, AIFF; M4A only when its index is at the start of the file. |
| PREV-2 | P0 | **3-band waveform**, Pioneer 3Band style: low = blue, mid = amber, high = white. The **overview** of the whole track is a single-sided cumulative stack. The **zoomed**, scrolling view around the playhead is mirrored and layered: a brown low∩mid overlap, with white highs in front at about 85 % opacity. The part not downloaded yet is visibly distinct. Other styles stay available in Settings. |
| PREV-3 | P0 | Transport: play/pause, click/drag to seek on either waveform, jump ±5 s / ±30 s, volume. Seeking beyond the downloaded range waits for data. |
| PREV-4 | P0 | **Keyboard audition:** ↑/↓ moves through results and (optionally) auto-previews the selection; Space previews from the configured source or plays/pauses; ⇧Space previews the peer's file; ←/→ seeks; Return downloads; K keeps the preview. |
| PREV-5 | P0 | **Keep** moves the cached file into the Inbox without re-downloading, and it counts as a download. **Discard** deletes it from the cache. |
| PREV-6 | P0 | Skipping a preview before its transfer completes cancels the transfer, saving the peer's bandwidth (D13). Settings can switch this to "let it finish". |
| PREV-7 | P0 | Previews are real transfers: they count in per-peer counters and the ratio, and respect per-peer caps. |
| PREV-8 | P0 | The preview cache is size-bounded (default 2 GB, LRU) and expires old files. Waveform analysis is cached separately: a ~3.6 KB overview kept long-term and the full detail (~0.1–0.3 MB per track) in an LRU, so re-opening a preview is instant. |
| PREV-9 | P1 | **Spectrum check:** detect likely transcodes / fake lossless (spectral cut-off) and show a badge and a spectrogram. |
| PREV-10 | P2 | BPM and key detection. |
| PREV-11 | P1 | **Preview through yt-dlp** (D26). When the user has yt-dlp installed (found on `PATH` or in Homebrew, or set in Settings), a preview can fetch the full track from YouTube Music for the matched release. The search uses artist and title, and the duration must be within ±3 s of the release. The waveform is the same as for peer previews. Selecta runs yt-dlp as an external process: an argument list with no shell, audio only, no cookies or account, writing into the preview cache, with a timeout and one process at a time. The audio is never filed, shared, converted or tagged, and it isn't a download. Selecta doesn't bundle, install, update or reimplement yt-dlp. |
| PREV-12 | P1 | **Preview source** setting: Automatic (yt-dlp when installed, else the 30-s clip of META-5), yt-dlp only, the clip only, or always the peer's file. Space uses it; ⇧Space always previews the peer's file. The player says where the audio comes from. |

### 7.6 Downloads, Inbox and filing (DL)

| ID | Pri | Requirement |
|---|---|---|
| DL-1 | P0 | Download single files or a whole remote folder ("album download"). The transfer list shows each state: queued at the peer (with place in queue), connecting, transferring (speed, ETA), completed, or failed (with reason). Group by folder or by peer. |
| DL-2 | P0 | Resume interrupted downloads, retry failures with backoff, and keep the queue across restarts. |
| DL-3 | P0 | **Downloads land in the Inbox, converted and tagged** (D29): singles in `Inbox/Singles/` (flat), album downloads in `Inbox/Albums/<Artist> - <Album> (<Year>)/` with all their files, including the cover image. On arrival each audio file is converted to the **Inbox format** (default MP3 320 CBR, ID3v2.3, artwork embedded — see CONV-2) and tagged from its matched release (META-4). Files already in that format and quality are kept without re-encoding. Lossy files below it are kept as they are, with a warning, because re-encoding can't add quality. A converted lossless original goes to the macOS Trash once the converted file is verified (D11). |
| DL-4 | P0 | **Inbox view:** items with source peer, quality, tags, artwork and ownership status. Actions: play, edit basic tags, pick another release, **File**, Trash. |
| DL-5 | P0 | **Filing** moves an Inbox item into the collection: singles → `<Collection>/<Singles folder>/`, albums → `<Collection>/<Albums folder>/<Artist> - <Album> (<Year>)/`. Conversion and tagging already happened on arrival (DL-3). Names come from templates; an existing file is never overwritten. |
| DL-6 | P0 | Files are filed by copy → verify → remove from Inbox, so a crash never loses a file. After filing, Selecta never moves, renames, re-tags or deletes them. |
| DL-7 | P0 | **Supersede in the Inbox:** when an album download completes and contains a track that matches a single still in the Inbox, that single goes to the macOS Trash, with a notification and Undo. Uncertain matches are flagged for review instead. |
| DL-8 | P0 | **Supersede in the collection — flag only:** when an album (in the Inbox or filed) contains a track matching a single already filed in the collection, the pair is flagged in the Library and Inbox views ("also on album X"). The filed single is never touched. |
| DL-9 | P0 | **History:** every completed download is recorded with its session, source peer, original remote path, quality, and where it is now (Inbox / filed path / trashed). |
| DL-10 | P1 | Post-download verification: the file decodes fully and its duration matches the search result; mismatches are flagged. |
| DL-11 | P0 | **Duplicate guard:** warn before downloading something already in the collection (same or better quality), in the Inbox, or queued. Downloading a single that is already on an album you own also warns. |
| DL-12 | P1 | When several tracks of one remote folder are selected for download, offer "Download the whole folder as an album". |
| DL-13 | P0 | **Naming templates and the name cleaner.** Settings → Filing has templates for single files (default `{artist} - {title}`), album folders (`{albumartist} - {album} ({year})`) and album track files (`{track} - {title}`), with a live preview. The cleaner makes every name path-safe while keeping Unicode letters: it normalises to NFC, drops quotes, turns `&` into `and`, and turns spaces, brackets and other symbols (`%`, `$`, `/`, `:`, `*`, `?`, `\|`, …) into the separator (default `_`). It then collapses repeats, trims separators and dots at both ends, and caps the length. Example: `Halden & Rue - Tidewater (Original Mix)` → `Halden_and_Rue_-_Tidewater_Original_Mix.mp3` (D22). |

### 7.7 Collection awareness (LIB)

| ID | Pri | Requirement |
|---|---|---|
| LIB-1 | P0 | Configure one or more collection folders, plus the names of the Singles folder and the Albums folder. Scan recursively and read tags (TagLib), audio properties and file identity. Scanning is read-only. |
| LIB-2 | P0 | Incremental updates via FSEvents. Tolerate offline volumes (external drives) without forgetting their contents. |
| LIB-3 | P0 | **Matching engine:** parse remote paths into artist / title / album / track number / version, then match them against the collection, the Inbox and the history. Confidence is exact, likely or none. Matching uses a duration tolerance and is version-aware (Original / Extended / Radio Edit / remixes). |
| LIB-4 | P0 | **Upgrade detection:** you own the track, but in lower quality than the result. |
| LIB-5 | P0 | **Library view:** collection stats (tracks, formats, size), recently filed items, sessions with their downloads, flagged single/album pairs, Inbox count. |
| LIB-6 | P1 | User corrections: mark a match as wrong or right; Selecta remembers the correction. |
| LIB-7 | P2 | Acoustic fingerprints (Chromaprint) for robust duplicate detection inside the collection. |
| LIB-8 | P2 | Optional extra source: rekordbox XML export (out of scope for v1, per the maintainer). |

### 7.8 Converter and metadata (CONV)

| ID | Pri | Requirement |
|---|---|---|
| CONV-1 | P0 | Convert between FLAC, MP3 (CBR 320 / 256 / 192, VBR V0 / V2), WAV and AIFF; ALAC is P1. Sample rate and bit depth can be changed, with high-quality resampling and TPDF dither when reducing bit depth. |
| CONV-2 | P0 | **Inbox format:** one configured preset, default "MP3 320 CBR · ID3v2.3 · artwork embedded", applied to every download as it arrives in the Inbox (DL-3, D29). Re-encoding is skipped when it cannot improve quality: an MP3 320 is only tagged, and an MP3 192 is kept as-is with a "below target" badge rather than re-encoded. After a verified conversion of a lossless source, the original goes to the macOS Trash (D11). |
| CONV-3 | P0 | **Manual converter:** any Inbox or collection file → a *new* file; the original is untouched. Output folder is configurable. |
| CONV-4 | P0 | **Metadata mapping** between Vorbis comments, ID3v2.3 (default for MP3/AIFF/WAV), ID3v2.4 (option), MP4 atoms and RIFF INFO. It covers title, artist, album, album artist, track and disc numbers with totals, date/year, genre, composer, BPM, key, label, ISRC, catalogue number, comment, lyrics, compilation and ReplayGain. ("IDFv3" in the brief is read as ID3v2.3.) |
| CONV-5 | P0 | **Supplement missing metadata:** when the source has no embedded art, embed the folder image (`cover.jpg`, `folder.jpg`, `front.jpg`, …) as the ID3 APIC "front cover". Fill missing album/artist/track fields from the folder and file names, shown for confirmation. Optionally resize artwork to a DJ-hardware-friendly size. |
| CONV-6 | P0 | **Quality warnings**, shown before converting: lossy → lossless ("no quality gain, ~N× larger"); lossy → lossy at a higher bitrate ("no quality gain"); lossy → lossy at any bitrate ("generation loss"); upsampling sample rate or bit depth ("no quality gain"); source flagged as a likely transcode. No-gain conversions need explicit confirmation. |
| CONV-7 | P0 | Verify each output (decodes fully, duration within tolerance, tags and artwork present) before reporting success. |
| CONV-8 | P1 | Batch queue using all cores, with progress and cancellation. |

### 7.9 Sharing and uploads (SHARE)

| ID | Pri | Requirement |
|---|---|---|
| SHARE-1 | P0 | Choose shared folders. The whole collection is shared by default (D12); the Inbox and the preview cache never are. Scan and index the shares, excluding hidden and system files. |
| SHARE-2 | P0 | Answer browse, folder-contents and search requests from the index. Honour the server's excluded search phrases. Never reveal full local paths. |
| SHARE-3 | P0 | Upload queue: 3 slots, one active upload per user, **privileged users first**, round-robin between users, per-user limits (100 files / 10 000 MB) answered with the standard denial strings, optional bandwidth limit. |
| SHARE-4 | P1 | Buddy-only shared folders; ban / ignore users. |
| SHARE-5 | P0 | Upload statistics (files and bytes, per user) feed the ratio. |

### 7.10 Fairness and ratio (FAIR)

| ID | Pri | Requirement |
|---|---|---|
| FAIR-1 | P0 | **Per-peer counters:** files and bytes downloaded (this session, today, 7 days, all time), files currently queued, last download. Shown in the results (peer column), the inspector and the Peers view. |
| FAIR-2 | P0 | **Per-peer limits:** max concurrent transfers per peer, max queued files per peer, a daily soft cap (warning) and a hard cap (blocked unless overridden for that one item). Defaults: 1 active and 20 queued files; a warning at 20 files or 1 GB a day; a stop at 50 files or 3 GB a day, or 150 files in 7 days (`docs/RESEARCH-SYNTHESIS.md` §3). |
| FAIR-3 | P0 | **Ratio:** uploaded vs downloaded (files and bytes; this session / 30 days / all time) next to the number of files shared. Always visible in the toolbar, with a status (healthy / low) and guidance. |
| FAIR-4 | P0 | **Respect peer signals:** after a denial ("Too many files", "Banned", queue full…), back off from that peer automatically and record why. |
| FAIR-5 | P1 | Ranking prefers peers you have downloaded less from, all else being equal. |

### 7.11 Network governor and safe development (SAFE)

| ID | Pri | Requirement |
|---|---|---|
| SAFE-1 | P0 | One **network governor** enforces every outbound budget: searches per window (with burst), browse and user-info requests, connection attempts, reconnects, and transfers per peer and overall. Search, Batch, Wishlist, Preview and Browse all go through it. |
| SAFE-2 | P0 | Hard ceilings are compile-time constants; settings can only make limits stricter. |
| SAFE-3 | P0 | Debug builds and tests can't reach the live server unless it is explicitly enabled at both build time and run time. CI only ever uses a local test server. |
| SAFE-4 | P0 | An in-app network activity log records searches, requests, denials and disconnect reasons. |
| SAFE-5 | P0 | A global "Pause network activity" switch stops new searches and new transfers. |

### 7.12 Caching (CACHE)

| ID | Pri | Requirement |
|---|---|---|
| CACHE-1 | P0 | Persistent caches: search results (TTL 24 h), browsed share lists (TTL 24 h), preview audio (2 GB LRU), waveform and spectrum analysis (keyed by content hash), peer statistics and the library index. Default values are configurable. |
| CACHE-2 | P0 | Each cache can be inspected (size) and cleared from Settings. |

### 7.13 Metadata services and quick previews (META)

Added on 2026-10-19 at the maintainer's request (D19). The source choices follow
`references/metadata-and-preview-services.md` and ADR-011.

| ID | Pri | Requirement |
|---|---|---|
| META-1 | P0 | **Spotify access without a secret:** sign in once in the browser (Authorization Code with PKCE, loopback redirect `http://127.0.0.1/callback`). Only a client ID is needed. Each user brings their own: Spotify's 2026 development mode caps an app at 5 users and needs the owner's Premium, so Selecta ships none. The refresh token lives in the Keychain; Spotify expires it after 6 months, and Selecta then asks the user to sign in again. Settings → Integrations has fillable Client ID and optional Client secret fields; a secret enables the client-credentials flow for development. Without a Spotify sign-in, metadata and previews come from keyless sources (Deezer, MusicBrainz). Credentials and tokens never appear in the repo, logs or settings files; development builds may read the git-ignored `local/secrets.env` (D21). |
| META-2 | P0 | **Match** a result, Inbox item or Get tracks line to a Spotify track. Search by artist and title, plus ISRC when known, scored with the matcher of LIB-3 (duration tolerance, version awareness). The inspector shows the canonical metadata (title, artists, album, label, release date, ISRC, artwork) and the match confidence. |
| META-3 | P0 | **Playlist import:** a Spotify playlist or album link adds its tracks to Get tracks. Lines carry artist, title, duration and ISRC, which sharpens Soulseek matching. Development mode reads only playlists the user owns or collaborates on, plus Liked Songs and albums. For anyone else's playlist, the user selects all songs in Spotify and copies them: the pasted track links are looked up one by one. |
| META-4 | P0 | **Tags from the release.** On arrival in the Inbox (DL-3), write the matched release's metadata and artwork: ID3v2.3, cover as JPEG ≤ 800×800 (D29). From Spotify (D30): title, artists, album, album artist, release date, track and disc numbers, ISRC and the cover; label and genre come from MusicBrainz, by ISRC. A one-time notice explains the Spotify terms risk before the first Spotify-tagged file. The inspector shows the tags a file will get before Get. A file with no matched release keeps its own tags, and the Inbox offers release candidates to pick from. The converter offers the same metadata, applied with confirmation. |
| META-5 | P1 | **Quick preview:** play an official 30-second clip (e.g. Deezer by ISRC, iTunes) with no Soulseek transfer, so the audition costs peers nothing. The player names the clip's source; the full preview from the peer stays one key away. |
| META-6 | P2 | **Full-length listening:** "Open in Spotify / YouTube Music" in their own apps. Optionally start playback in the user's Spotify app (Premium, with the user's authorisation). |
| META-7 | P1 | Every request to these services goes through the network governor (rate limits, cached results with TTLs). The inspector says what was sent where. |
| META-8 | P2 | Additional metadata sources (MusicBrainz, Discogs, Deezer), chosen by research. |

## 8. UX and UI requirements

### 8.1 Look and feel (UI)

| ID | Pri | Requirement |
|---|---|---|
| UI-1 | P0 | **Native macOS look:** system controls and font (SF), unified toolbar, source-list sidebar, standard sortable tables, context menus, the standard menu bar and shortcuts. Native, not flashy. |
| UI-2 | P0 | **Light and dark appearance**, following the system setting by default (with an in-app override), and using the system accent colour. |
| UI-3 | P0 | The waveform area keeps a dark background in both appearances, like a CDJ screen, so the white high band stays readable. |
| UI-4 | P0 | Colour is never the only carrier of meaning: badges pair a symbol with a label (colour-blind safety). |
| UI-5 | P0 | Full keyboard navigation; VoiceOver labels on controls, badges and the waveform (position/duration). |
| UI-6 | P1 | Dock badge and system notifications for unread messages and completed batches / albums. |
| UI-7 | P0 | **Essentials first.** Each screen opens with only what's needed to act. For search: the quality wanted, the release, and results with status, title, quality, length, peer and availability. Formats and the other filters sit behind a Filters button; extra columns and inspector details open on demand (D24). |
| UI-8 | P0 | **One main flow.** The app opens on Get tracks (D27). Search, the Inbox, Collection, History, Activity, Shares, Messages, Wishlist and Converter stay one click away in the sidebar, out of the way. |

### 8.2 Window and navigation

One main window:
- **Toolbar:** sidebar toggle, view title, search budget, ratio, "Pause network", appearance, inspector toggle.
- **Sidebar** (source list):
  - Search field
  - **Get tracks** · Inbox
  - **Digging:** open searches with their result counts · Wishlist
  - **Library:** Collection · History
  - **Network:** Activity (transfers, ratio, per-peer counts) · Shares · Messages
  - Converter · Settings
  - Footer: connection status
- **Content:** the selected view.
- **Inspector** (right, toggleable): details for the selected result or file: full remote path, quality and
  plausibility, ownership matches, peer statistics, actions.
- **Player** (bottom, persistent): track info, transport, Keep / Discard / Download, and the waveform screen
  (zoomed view above the overview).

The mock ([mock/index.html](../mock/index.html)) is the reference for layout and interactions once validated.

### 8.3 Keyboard shortcuts (initial set)

| Keys | Action |
|---|---|
| ⌘F / ⌘T | Focus search field / new search |
| ↑ / ↓ | Previous / next result (auto-preview when enabled) |
| → / ← on a folder row | Expand / collapse |
| Space | Preview the selection from the preview source in Settings (default: the full track via yt-dlp when installed, else the 30-s clip), or play / pause |
| ⇧Space | Preview the file from the Soulseek peer |
| ← / → on a track, ⇧← / ⇧→ | Seek −5 s / +5 s, −30 s / +30 s |
| Return | Download selection (folder row: download the whole folder as an album) |
| K / ⌫ | Keep / discard the preview. Keeping a peer preview moves it to the Inbox; keeping a yt-dlp or clip preview downloads the peer's file |
| ⌘I | Toggle inspector |
| ⌘1 … ⌘9 | Switch sidebar sections |
| ⌘, | Settings |

## 9. Non-functional requirements (NFR)

| ID | Requirement |
|---|---|
| NFR-1 | **Platforms:** macOS on Apple Silicon is the release target; the minimum macOS version is whatever the chosen Qt release supports. Linux and Windows compile and pass unit tests in CI. Release builds are signed with the maintainer's Developer ID and notarised; development builds use a stable local signing identity, never the Developer ID (D33). |
| NFR-2 | **Performance:** the UI stays smooth (60 fps, 120 on ProMotion) while results stream in. Filter + sort of 10 000 results < 50 ms. Waveform analysis of a 6-minute track < 300 ms once downloaded, and progressive while downloading. Cold start < 2 s. 100 000-track collection: initial scan < 5 min, memory < 500 MB. |
| NFR-3 | **Reliability:** crash-safe persistence (SQLite WAL); transfers resume after restart; filing is copy → verify → delete, so no data loss. |
| NFR-4 | **Security:** fuzzed protocol parsers; bounded allocations from network input; path-traversal protection on uploads; downloaded files are never executed and carry the macOS quarantine attribute. |
| NFR-5 | **Privacy:** no telemetry; credentials in the Keychain; logs stay local; local paths are never sent to peers. |
| NFR-6 | **Licensing:** GPL-3.0-or-later (D14); every dependency GPLv3-compatible; SPDX headers in source files. |
| NFR-7 | **Testability:** the protocol layer is testable without a network; integration tests run against a local test server; fixtures are deterministic, and the test audio corpus is synthetic (no copyrighted music in the repo). |
| NFR-8 | **i18n:** English UI; strings are translatable (Qt Linguist) — P2. |

## 10. Success criteria (v1)

- In strict mode, 100 % of displayed results satisfy the active filter (verified by tests).
- Query → audible preview in < 10 s median on the live network (< 1 s on the local test network).
- Zero server-side bans during development and personal use.
- Zero accidental duplicate downloads of exact-match tracks without an explicit override.
- Filed MP3s import into rekordbox 7 with title, artist, album and artwork displayed correctly (manual check on a sample set).
- No filed file is ever modified by Selecta (enforced by the file-operations API and its tests).

## 11. Open questions

| # | Question | Default until answered |
|---|---|---|
| OQ-1 | Path of the collection folder. The layout is settled: `Singles/` plus `Albums/<album>/` (D10). How are album folders named today? | `<Collection>/Albums/<Artist> - <Album> (<Year>)/` |
| OQ-3 | Ratio definition: uploads vs downloads (files and bytes), shown next to the number of files shared. | As stated |
| OQ-4 | Default per-peer caps. (Search pacing is settled: D31.) | From `references/network-etiquette-and-safe-development.md` §9.1: 1 active and 20 queued files per peer, warning at 20 files a day, stop at 50 |
| OQ-6 | Waveform style: cumulative stack vs overlaid layers vs separate lanes. | Decide on the mock |
| OQ-12 | Should files from outside Soulseek (e.g. Bandcamp purchases) dropped into the Inbox use the same filing pipeline? | Yes (P1) |

Resolved: OQ-2 → D11, OQ-5 → D13, OQ-7 → D14, OQ-9 → D15, OQ-10 → D12, OQ-11 → D22, OQ-13 (what Space plays) → D26, OQ-14 (metadata source) → D29 and D30, OQ-15 (Spotify credentials) → D21, OQ-8 (Developer ID) → D33.

---

## Appendix A — Decisions log

Answers given by the maintainer on 2026-10-19.

| # | Question | Decision |
|---|---|---|
| D1 | What defines "what I already have"? | **Music folders on disk** only (no rekordbox / Apple Music reading in v1). |
| D2 | Where do downloads land? | **Inbox, then file** into the collection. |
| D3 | A downloaded album contains a single I already have. | **Flag only** for the collection — "Rekordbox already linked to them, so can't move them." **Trash** the single if it is still in the Inbox. |
| D4 | Classic Soulseek features in v1 | **Private messages, Wishlist, Browse a user's shares.** No chat rooms (revised by D32: a minimal rooms view is in). |
| D5 | Audience | **Me first.** |
| D6 | Batch downloads | **Review first.** |
| D7 | Conversion at filing | "Everything to a format I configure. Generally **MP3 320**." |
| D8 | Session definition | **App launch → quit.** |
| D9 | Look and feel | **macOS-native** ("not too crazy, but native"), **with a dark theme**. |
| D10 | Collection layout on disk | **`Singles/` + `Albums/<album>/`** under one collection root. |
| D11 | Lossless original after filing as MP3 320 | **Move it to the Trash** (after the converted copy is verified). |
| D12 | What to share by default | **The whole collection** (read-only). |
| D13 | Preview skipped before its transfer finishes | **Cancel the transfer.** |
| D14 | Licence | **GPL-3.0-or-later.** |
| D15 | Hosting | **GitHub** (CI on GitHub Actions). |
| D16 | Git author identity | **usr-ein <usr_ein@pm.me>.** The first version had `usr-ein@`, a typo; on 2026-10-20 every commit was rewritten to this address. |
| D17 | First working slice after the mock | **Search + preview** (protocol on a local test server, strict filters, preview with waveform). |
| D18 | Mock format | A **local HTML** mock in `mock/`, not a hosted page; it exists to decide what goes where in the Qt UI, in macOS visual language. |
| D19 | Spotify integration | **Yes:** a matcher for quality metadata, playlist import, and quick previews from official 30-s clips. It needs no secret: PKCE sign-in with a client ID, or keyless sources (D21). Any credentials stay out of the repo and are fillable in Settings (META-1…8). |
| D20 | Downloading audio from YouTube Music or Spotify, spotDL-style, with a Premium cookie | **Not as a source of files.** Everything Selecta files or shares comes from Soulseek peers, and Selecta takes no cookie. Purchases (Bandcamp, Beatport) can enter through the Inbox (OQ-12). The first version of this decision also ruled out preview audio; D26 replaces that part. |
| D21 | Spotify metadata "without the API secrets", like spotDL | spotDL ships its own client ID and secret inside its source code (`spotdl/utils/config.py`). Selecta won't publish a secret or reuse anyone else's. It gets the same convenience officially: **PKCE sign-in** (client ID only, one browser login) plus **keyless sources** (Deezer, MusicBrainz). It won't reverse-engineer Spotify's web-player tokens. The maintainer's working credentials (verified 2026-10-19) stay only in the git-ignored `local/secrets.env` for development. |
| D22 | File names | Template **`{artist} - {title}`** for singles, editable in Settings. A **name cleaner** keeps Unicode but removes or replaces spaces, quotes, `%`, `$` and other characters that are awkward in paths (DL-13). |
| D23 | How search and batch find the right file | **Release first, length as the key signal.** Show the matched release (Spotify or Deezer) first, then rank network files against it by length (±3 s), name and version, never by name alone (SRCH-17, SRCH-18, BATCH-5). |
| D24 | How much to show | **Start with the essentials** and reveal more on demand (UI-7). |
| D25 | Mock review | The design language and layout are approved ("spot on", "well native"). Building starts once the preliminary research is done. |
| D26 | Preview audio for the waveform | **The full track through the maintainer's own yt-dlp:** "Selecta is not breaking any term, it simply calls yt-dlp that is already on my machine." Selecta runs the installed yt-dlp as an optional external tool. The audio stays in the preview cache: it is never filed, shared, converted or tagged. Selecta uses no cookie or account, and doesn't bundle or reimplement yt-dlp (PREV-11). Space plays this preview when yt-dlp is installed, else the 30-s clip; ⇧Space plays the peer's file (PREV-12). |
| D27 | The product's main flow | **Get tracks**, the home screen: "I have a list of tracks I'm interested in obtaining, and at the end I want them … In MP3 320kbps, with metadata from spotify in the mp3 id3v2, cover in there too, in my inbox. I want to preview them and their waveform before fully comitting." The other views stay one click away, and every screen starts with fewer knobs (UI-8, extends D24). |
| D28 | Name matching | **Typo-tolerant and position-independent:** "I want a levenstein distance kind of thing … robust to typos, missing letters etc." Word-level Damerau–Levenshtein (OSA) with length-scaled thresholds, words matched wherever they sit in the path, and joined words. Length (±3 s) stays the main signal (D23). Details: `references/library-matching-and-organization.md` §17 (SRCH-19). |
| D29 | When conversion and tagging happen | **On arrival in the Inbox**, not at filing (follows from D27). Downloads are converted to the Inbox format (MP3 320 CBR by default) and tagged from the matched release, with the cover embedded. Spotify ranks first ("very reliable in my usage of spotdl, well tagged"); Deezer and MusicBrainz cover what Spotify can't match. Filing only moves. Lossless originals go to the Trash once the converted file is verified (D11). |
| D30 | Tagging from Spotify despite its developer terms | **Spotify tags**, chosen by the maintainer after hearing that Spotify's developer terms forbid storing Spotify metadata and artwork in files (`references/metadata-and-preview-services.md` §4). Files get title, artists, album, album artist, date, track and disc numbers, ISRC and the cover from Spotify; label and genre, which dev mode no longer returns, come from MusicBrainz. The risk is that Spotify revokes the maintainer's own developer app (bring-your-own Client ID). Selecta shows a one-time notice explaining this before the first Spotify-tagged file. |
| D31 | Search pace (OQ-4, search part) | **2 searches per minute with bursts of 6** (token bucket: burst 6, one more every 30 s), one budget for every search source, no setting that loosens it, and the same query not repeated within 10 min. A 50-line Get tracks list takes about 25 minutes; cached lines are instant (`references/network-etiquette-and-safe-development.md` §9.1). |
| D32 | Chat rooms vs the official "full range of features" rule | **A minimal rooms view** (NET-10): list, join, read, post. It closes the gap in the rules at little cost; everything else about rooms stays out. |
| D33 | Apple Developer ID (OQ-8) | The maintainer has one: "Yes I do, but don't use it for development." Release builds (M10) are signed with it, use the hardened runtime and are notarised. Development builds use a stable local identity so Keychain and firewall approvals survive rebuilds. |

## Appendix B — Raw brief (verbatim)

Original request:

```text
You are the main architect of my new project: a Qt client for Soulseek protocol, in C++, crossplatform (mainly macOs).
I want you to draft a plan on the aspects to develop.
Here are the big lines of what I imagine you will need:
1. Document yourself extensively on the Soulseek protocol, and existing libraries for it in C++
2. The base layout of the Qt app repo etc (this is an open source project, GPLv3)
3. Ways to safely develop without spamming/getting banned of the network
4. Ways to share local files/folders
5. The part that I want to customise compared to existing offerings: the browsing UI, which I will explain more in details.

Browsing UI:
I want to be able to give strict filters of the files available (e.g. FLAC, MP3, WAV, AIFF, ...) and bitrate quality (wherever applicable to format), I want to be able to preview the track (download and play, browse through the track quickly, see the waveform of it in a tri-bad high/mid/low in white orange blue stacked, like pioneer).
I want to have a good glance of:
What do I already have (I point you to my existing collection)
What I have downloaded in the current/past sessions
I want you to feature a converter of format directly in the app (FLAC to MP3, keeping and supplementing any metadata needed, e.g. conver.jpg be put in the IDFv3 fields of the MP3, or MP3 to WAV etc, with warning when converting between bitrates that are not improving quality).
A count of tracks downloaded per peer, to not overburden any one peer. Count of tracks I have shared, and a ratio so I see if I'm downloading too much.
Learn about how not to get banned for excessive download on soulseek.
Cache results for example.
A way to launch multiple searches from a list of text names of tracks, all with the same filters (e.g. all MP3 ==320kbps for tracks in the list track A\ntrackB\n trackC etc).

Make a mock of this UI in HTML/JS/CSS so I can guide you on the design and UI interactions I want, then, when validated, we build that.

I like my music organised as such:

Single tracks downloaded go all in one folder
When I download a whole album, I keep a folder of just that album, with all tracks in it
If I download a single, but then the whole album containing that single, I delete the dangling single and keep only the full album.

Draft out the specs I layed out first, as well as this raw prompt. Call that the PRD Product Requirement Document. Then, make a plan to implement it. Start with the mock, and in the background, launch a research team which commits their finidings in references markdown files.

The project is called Selecta.
```

Follow-up messages:

```text
Also, include dark theme in the requirements. I want the UI to feel macOS native (not too crazy, but native)
```

```text
Get ready to ask me the largest questions unanswered you need to come up with a well defined plan. I will answer rounds of questions initially, then you work, then more questions as you go.
```

Later request (2026-10-19; credentials redacted):

```text
I want you to integrate a spotify downloader and matcher for getting quality metadata, and also for getting a quick preview without downloading the quality track from soulseek. Look into how spotdl does it. I want that but in C++. A library would be best. If not, build it too. I have a spotify API key if need be.

Keep these uncomitted, and as fillable fields in the settings:
client id <redacted>
client secret <redacted>

For actual download, I also have youtube music premium. I can give you my cookie.
```

Further messages the same day (credentials redacted):

```text
Also for the file names to save, offer a setting to choose how to write them. I like the format "{artist} - {title}". I want a track name "cleaner" to be implemented which turns spaces and quotes and weird chars to have in a path into path safe versions (unicode okay though, just not spaces, quotes, double quotes, percentages, dollars etc etc).
```

```text
secret is <redacted>
id is <redacted>
```

```text
But I want you to make the spotify metadata scraper without those api secrets actually, shouldnt be that hard
```

```text
look at spotdl approach
```

```text
For the download and search, I want the length of the track to be used as an indicator of quality, because sometime names are too generic, and that helps to differentiate the possible good candidates from the unrelated tracks. Also cross checking with the spotify metadata first help. When batch searching, or even just searching, it should show me the spotify metadata matched first, then based on that, suggest the right matching tracks on the network, and not just match by track name.
```

```text
The UI is pretty good. There is a lot of information, so perhaps only starting with the essential and adding is better than starting with a super full one like this.
```

```text
The mock design language is spot on. The layout is good and well native. You can start building it when the prelimenary research work is done.
```

```text
For name matching, I want a levenstein distance kind of thing as well. Also files can have artist / track / album names in the paths in different position. I want the search algo to be robust to typos, missing letters etc.
```

```text
The preview waveform/listening should come from either spotify or youtube music. The metadata from spotify (very reliable in my usage of spotdl, well tagged)
```

Answers to a question round:

```text
"Selecta can play full tracks through an official player (no download, no waveform). Which should it use?"="No, I want it to download from the source. Youtube music allows that, that's how spotdl works. Do it like that and display the waveform. The waveform is a very useful tool for the dj to preview the shape of the track.", "What should Space do on a result?"="no", "Do you have Spotify Premium?"="Yes"
```

```text
Remember my UI decision that the mock has too many knobs and buttons and things shown.
All I care about is this in my flow:
I have a list of tracks I'm interested in obtaining, and at the end I want them at the end:
In MP3 320kbps, with metadata from spotify in the mp3 id3v2, cover in there too, in my inbox.
I want to preview them and their waveform before fully comitting.

This tool is only for myself locally as a nice .app so I fear nothing from youtube terms of service. I already use spotdl every day, so it does not change anything whether you refuse this. This is purely for my convenience. I am also not saving those tracks from youtube/spotify, but merely using them to preview before downloading from a soulseek peer.
```

```text
Selecta is not breaking any term, it simply calls yt-dlp that is already on my machine.
```

```text
I think a single agent building from the plan makes more sense, as they have the whole context, compared to a swarm. The swarm can be used to build the distinct libraries that have clear boundaries, like metadata, soulseek etc etc. I want a head implementer, and for the very separate things distinct agents.
```
