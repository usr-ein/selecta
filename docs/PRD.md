# Selecta — Product Requirements Document (PRD)

| | |
|---|---|
| **Status** | Draft v0.3 — for review by the maintainer (question round 3 folded in) |
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

It is designed around one workflow, a DJ's digging session:

1. **Search** with strict quality filters ("MP3 = 320 kbps CBR", "lossless ≥ 16-bit/44.1 kHz").
2. **Audition** results instantly, with a Pioneer-style 3-band waveform (low = blue, mid = orange, high = white) and fast scrubbing.
3. **See at a glance** what you already own, what is waiting in your Inbox, and what you downloaded this session or before.
4. **Collect** downloads in an **Inbox**, then **file** them into the collection in a configured format (generally MP3 320) with complete metadata and artwork.
5. **Stay fair.** Searches are paced, downloads per peer are counted and capped, and your upload/download ratio is always visible.

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
- **Batch searching a tracklist** is manual, or done with command-line tools that can trip the server's search limits.

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
| G2 | Audition before downloading: progressive preview, 3-band waveform, keyboard-driven. |
| G3 | Never download what you already have by accident: collection, Inbox and history awareness; upgrade detection. |
| G4 | A clean pipeline: download → Inbox → (convert + tag) → file into the collection. |
| G5 | Batch search a tracklist with one filter set, safely paced, review-first. |
| G6 | Fair and safe by construction: rate limits, per-peer counters and caps, visible ratio, real sharing. |
| G7 | Feel native on macOS: system controls and font, light/dark appearance, accent colour, shortcuts, speed. |

### Non-goals (v1)

- Chat rooms (private messages are in scope).
- Reading rekordbox, Apple Music or other DJ-software libraries; writing to rekordbox. The collection = folders on disk.
- Windows/Linux packages (they must compile and pass unit tests in CI).
- Mobile apps, web UI, headless daemon mode.
- Fully automatic online tagging: metadata from online sources is applied only with confirmation (META-4).
- Downloading or ripping audio from streaming services (YouTube, YouTube Music, Spotify and the like). Selecta
  downloads only from Soulseek peers (D20).
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
| Inbox | Selecta-managed staging area where downloads land before being filed. Selecta may modify and trash Inbox files. |
| Filing | Moving (or converting) an Inbox item into the collection — the only way Selecta writes into the collection. |
| Collection | The user's music folders on disk. Read-only for Selecta, except for filing new files into it. |
| Single | A track downloaded on its own. Filed into the collection's Singles folder. |
| Album download | An explicit "download whole folder" of a remote release. Filed as one folder per album. |
| Supersede | An album download that contains a track the user already has as a single. |
| Preview cache | Where previews are downloaded; size-bounded; separate from the Inbox. |
| Search budget | The number of network searches the governor allows in the current time window. |

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
| SRCH-8 | P0 | **Filter presets:** named and saved (e.g. "DJ — MP3 320", "Lossless CD", "Hi-res"), one default. Search, Batch and Wishlist use the same presets. |
| SRCH-9 | P0 | **Filtered-out transparency:** "N of M results shown", with a breakdown of why results were hidden (format, bitrate, unknown quality, owned, peer limits…). |
| SRCH-10 | P0 | Two result views: grouped by remote folder (release) or a flat track list. Sort by quality, availability (free slot, queue, speed), size, duration, or fewest downloads from that peer. |
| SRCH-11 | P0 | **Ownership badges** on every result: *In collection* (exact match) · *Likely in collection* (fuzzy) · *Upgrade* (you own it in lower quality) · *In Inbox* · *Downloaded this session* · *Downloaded in a past session* · *Queued*. |
| SRCH-12 | P0 | **Search cache:** results are stored in SQLite with a TTL (default 24 h). Re-running a cached query shows the cached results instantly, labelled with their age; "Refresh" re-runs it on the network and costs one search from the budget. |
| SRCH-13 | P0 | Every network search goes through the governor's rate limiter. The UI always shows the remaining search budget and when it refills. |
| SRCH-14 | P1 | **Wishlist:** saved searches (each with a preset) re-run in the background at the server-provided interval; new matching results raise a notification. |
| SRCH-15 | P1 | Search history: recent queries with their presets. |
| SRCH-16 | P0 | Per-search caps (stop collecting after N results or T seconds) to bound memory and peer load. |
| SRCH-17 | P0 | **Release first.** Each search is resolved to release candidates (Spotify when signed in, otherwise Deezer or MusicBrainz), shown above the results with artwork, title, artist, album, year, length and label. When the query names a track, the best release is chosen automatically. Otherwise the user picks one, or "Any (name only)". The chosen release becomes the reference for ranking and filtering (D23). |
| SRCH-18 | P0 | **Length identifies the track.** Each file's length is compared with the release's: within ±3 s counts as the same recording; beyond that, the file is flagged as another version or track. "Only this release" (on by default) hides files whose length or name doesn't fit. "Best match" ranks by fit to the release (length first, then artist, title and version), then quality, availability and peer fairness. It never ranks by name alone. |

### 7.3 Browse user shares (BRWS)

| ID | Pri | Requirement |
|---|---|---|
| BRWS-1 | P1 | Browse a peer's shared folders as a tree, with the same filters and ownership badges; download files or whole folders from it. |
| BRWS-2 | P1 | Cache browsed share lists (TTL, default 24 h) and rate-limit browse requests through the governor. |

### 7.4 Batch search (BATCH)

| ID | Pri | Requirement |
|---|---|---|
| BATCH-1 | P0 | Paste a tracklist (one track per line, "Artist - Title" or free text) or open a `.txt` file. Lines are parsed, normalised and de-duplicated. |
| BATCH-2 | P0 | One filter preset applies to every line (e.g. "MP3 = 320 kbps CBR"). |
| BATCH-3 | P0 | Lines already in the collection or the Inbox are marked and skipped by default. |
| BATCH-4 | P0 | Searches queue through the governor and are paced. The UI shows progress, ETA and the current budget, with pause / resume / cancel. Cached results are reused without spending budget. |
| BATCH-5 | P0 | **Release first, then review.** Each line is first resolved to a release (title, artist, length, ISRC), shown before any network search. Files are then scored against that release: length first, then name and version, quality, availability and peer fairness. The line shows its best match and alternatives. The user auditions and approves; nothing downloads without approval. |
| BATCH-6 | P0 | "Approve all confident" approves every line above the confidence threshold in one click. It is still a user action. |
| BATCH-7 | P1 | Approved downloads are spread across peers, respecting per-peer caps. |
| BATCH-8 | P1 | Export unresolved lines (not found / ambiguous) as text. |

### 7.5 Preview and waveform (PREV)

| ID | Pri | Requirement |
|---|---|---|
| PREV-1 | P0 | Preview downloads the file into the preview cache (not the Inbox). Playback starts as soon as enough is buffered (progressive) for formats that allow it: MP3, FLAC, WAV, AIFF; M4A only when its index is at the start of the file. |
| PREV-2 | P0 | **3-band waveform:** low = blue, mid = orange, high = white, stacked Pioneer-style. It has an overview of the whole track and a zoomed, scrolling view around the playhead. The part not downloaded yet is visibly distinct. The final rendering style is settled on the mock. |
| PREV-3 | P0 | Transport: play/pause, click/drag to seek on either waveform, jump ±5 s / ±30 s, volume. Seeking beyond the downloaded range waits for data. |
| PREV-4 | P0 | **Keyboard audition:** ↑/↓ moves through results and (optionally) auto-previews the selection; Space plays/pauses; ←/→ seeks; Return downloads; K keeps the preview. |
| PREV-5 | P0 | **Keep** moves the cached file into the Inbox without re-downloading, and it counts as a download. **Discard** deletes it from the cache. |
| PREV-6 | P0 | Skipping a preview before its transfer completes cancels the transfer, saving the peer's bandwidth (D13). Settings can switch this to "let it finish". |
| PREV-7 | P0 | Previews are real transfers: they count in per-peer counters and the ratio, and respect per-peer caps. |
| PREV-8 | P0 | The preview cache is size-bounded (default 2 GB, LRU) and expires old files. Waveform analysis is cached separately (a few KB per track), so re-opening a preview is instant. |
| PREV-9 | P1 | **Spectrum check:** detect likely transcodes / fake lossless (spectral cut-off) and show a badge and a spectrogram. |
| PREV-10 | P2 | BPM and key detection. |

### 7.6 Downloads, Inbox and filing (DL)

| ID | Pri | Requirement |
|---|---|---|
| DL-1 | P0 | Download single files or a whole remote folder ("album download"). The transfer list shows each state: queued at the peer (with place in queue), connecting, transferring (speed, ETA), completed, or failed (with reason). Group by folder or by peer. |
| DL-2 | P0 | Resume interrupted downloads, retry failures with backoff, and keep the queue across restarts. |
| DL-3 | P0 | **Downloads land in the Inbox:** singles in `Inbox/Singles/` (flat), album downloads in `Inbox/Albums/<Artist> - <Album> (<Year>)/` with all their files, including the cover image. |
| DL-4 | P0 | **Inbox view:** items with source peer, quality, tags, artwork and ownership status. Actions: play, edit basic tags, convert, **File**, Trash. |
| DL-5 | P0 | **Filing** moves or converts an Inbox item into the collection: singles → `<Collection>/<Singles folder>/`, albums → `<Collection>/<Albums folder>/<Artist> - <Album> (<Year>)/`. It applies the configured **filing format** (default MP3 320 CBR, ID3v2.3, artwork embedded — see CONV-2). Files already in the target format and quality are moved without re-encoding. Names come from templates; an existing file is never overwritten. When a lossless download is converted at filing, the original goes to the macOS Trash once the converted copy is verified (D11). |
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
| CONV-2 | P0 | **Filing format:** one configured preset, default "MP3 320 CBR · ID3v2.3 · artwork embedded", applied to everything filed (DL-5). Re-encoding is skipped when it cannot improve quality: an MP3 320 is moved as-is, and an MP3 192 is filed as-is with a "below target" badge rather than re-encoded. After a verified conversion of a lossless source, the original goes to the macOS Trash (D11). |
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
| SHARE-3 | P0 | Upload queue with slots, bandwidth limit, per-user queue limits and fair round-robin (defaults from research). |
| SHARE-4 | P1 | Buddy-only shared folders; ban / ignore users. |
| SHARE-5 | P0 | Upload statistics (files and bytes, per user) feed the ratio. |

### 7.10 Fairness and ratio (FAIR)

| ID | Pri | Requirement |
|---|---|---|
| FAIR-1 | P0 | **Per-peer counters:** files and bytes downloaded (this session, today, 7 days, all time), files currently queued, last download. Shown in the results (peer column), the inspector and the Peers view. |
| FAIR-2 | P0 | **Per-peer limits:** max concurrent transfers per peer, max queued files per peer, a daily soft cap (warning) and a hard cap (blocked unless overridden for that one item). Defaults come from the etiquette research. |
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
| META-1 | P1 | **Spotify access without a secret:** sign in once in the browser (Authorization Code with PKCE). Only a client ID is needed, and it isn't a secret, so it may ship with the app; the refresh token lives in the Keychain. Settings → Integrations has fillable Client ID and optional Client secret fields; a secret enables the client-credentials flow for development. Without a Spotify sign-in, metadata and previews come from keyless sources (Deezer, MusicBrainz). Credentials and tokens never appear in the repo, logs or settings files; development builds may read the git-ignored `local/secrets.env` (D21). |
| META-2 | P1 | **Match** a result, Inbox item or batch line to a Spotify track. Search by artist and title, plus ISRC when known, scored with the matcher of LIB-3 (duration tolerance, version awareness). The inspector shows the canonical metadata (title, artists, album, label, release date, ISRC, artwork) and the match confidence. |
| META-3 | P1 | **Playlist import:** a Spotify playlist or album link fills Batch search. Lines carry artist, title, duration and ISRC, which sharpens Soulseek matching. |
| META-4 | P1 | At filing and in the converter, offer canonical metadata and artwork from the matched source to fill missing tags. Show where each value came from; apply only with confirmation or a remembered choice. |
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
| UI-7 | P0 | **Essentials first.** Each screen opens with only what's needed to act. For search: the preset, the formats, the release, and results with status, title, quality, length, peer and availability. Extra filters, columns and inspector details open on demand (D24). |

### 8.2 Window and navigation

One main window:
- **Toolbar:** sidebar toggle, search field, search budget, ratio, connection status, "Pause network", inspector toggle.
- **Sidebar** (source list):
  - Search · Batch · Wishlist
  - **Searches:** open searches with their result counts
  - **Library:** Inbox · Collection · History
  - **Network:** Transfers · Peers · Messages · Shares
  - **Tools:** Converter
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
| Space | Preview the selection (the 30-s clip or the full file, per Settings), or play / pause |
| ⇧Space | The other kind of preview (full file from the peer, or the 30-s clip) |
| ← / → on a track, ⇧← / ⇧→ | Seek −5 s / +5 s, −30 s / +30 s |
| Return | Download selection (folder row: download the whole folder as an album) |
| K / ⌫ | Keep preview (to Inbox) / discard preview |
| ⌘I | Toggle inspector |
| ⌘1 … ⌘9 | Switch sidebar sections |
| ⌘, | Settings |

## 9. Non-functional requirements (NFR)

| ID | Requirement |
|---|---|
| NFR-1 | **Platforms:** macOS on Apple Silicon is the release target; the minimum macOS version is whatever the chosen Qt release supports. Linux and Windows compile and pass unit tests in CI. |
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
| OQ-4 | Default per-peer caps and search pacing. | From `references/network-etiquette-and-safe-development.md` |
| OQ-6 | Waveform style: cumulative stack vs overlaid layers vs separate lanes. | Decide on the mock |
| OQ-8 | Is there an Apple Developer ID for signing and notarisation? | Unsigned / ad-hoc signed personal builds |
| OQ-12 | Should files from outside Soulseek (e.g. Bandcamp purchases) dropped into the Inbox use the same filing pipeline? | Yes (P1) |
| OQ-13 | Should Space play the 30-s clip or the full file from the peer? | The clip when one exists (it costs peers nothing) |
| OQ-14 | Which metadata source should rank first (Spotify, Deezer, MusicBrainz, Discogs)? | Spotify, pending ADR-011 and Spotify's developer terms |

Resolved: OQ-2 → D11, OQ-5 → D13, OQ-7 → D14, OQ-9 → D15, OQ-10 → D12, OQ-11 → D22, OQ-15 (Spotify credentials) → D21.

---

## Appendix A — Decisions log

Answers given by the maintainer on 2026-10-19.

| # | Question | Decision |
|---|---|---|
| D1 | What defines "what I already have"? | **Music folders on disk** only (no rekordbox / Apple Music reading in v1). |
| D2 | Where do downloads land? | **Inbox, then file** into the collection. |
| D3 | A downloaded album contains a single I already have. | **Flag only** for the collection — "Rekordbox already linked to them, so can't move them." **Trash** the single if it is still in the Inbox. |
| D4 | Classic Soulseek features in v1 | **Private messages, Wishlist, Browse a user's shares.** No chat rooms. |
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
| D16 | Git author identity | **usr-ein <usr-ein@pm.me>.** |
| D17 | First working slice after the mock | **Search + preview** (protocol on a local test server, strict filters, preview with waveform). |
| D18 | Mock format | A **local HTML** mock in `mock/`, not a hosted page; it exists to decide what goes where in the Qt UI, in macOS visual language. |
| D19 | Spotify integration | **Yes:** a matcher for quality metadata, playlist import, and quick previews from official 30-s clips. It needs no secret: PKCE sign-in with a client ID, or keyless sources (D21). Any credentials stay out of the repo and are fillable in Settings (META-1…8). |
| D20 | Downloading audio from YouTube Music or Spotify, spotDL-style, with a Premium cookie | **Not built** (architect's call, 2026-10-19). Stream ripping breaks those services' terms, means working around their stream protection, puts the maintainer's accounts at risk, and exposes the open-source project legally. Full-length listening goes through the services' own apps (META-6); purchases (Bandcamp, Beatport) can enter through the Inbox (OQ-12). |
| D21 | Spotify metadata "without the API secrets", like spotDL | spotDL ships its own client ID and secret inside its source code (`spotdl/utils/config.py`). Selecta won't publish a secret or reuse anyone else's. It gets the same convenience officially: **PKCE sign-in** (client ID only, one browser login) plus **keyless sources** (Deezer, MusicBrainz). It won't reverse-engineer Spotify's web-player tokens. The maintainer's working credentials (verified 2026-10-19) stay only in the git-ignored `local/secrets.env` for development. |
| D22 | File names | Template **`{artist} - {title}`** for singles, editable in Settings. A **name cleaner** keeps Unicode but removes or replaces spaces, quotes, `%`, `$` and other characters that are awkward in paths (DL-13). |
| D23 | How search and batch find the right file | **Release first, length as the key signal.** Show the matched release (Spotify or Deezer) first, then rank network files against it by length (±3 s), name and version, never by name alone (SRCH-17, SRCH-18, BATCH-5). |
| D24 | How much to show | **Start with the essentials** and reveal more on demand (UI-7). |
| D25 | Mock review | The design language and layout are approved ("spot on", "well native"). Building starts once the preliminary research is done. |

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
