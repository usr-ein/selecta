# Selecta — Research synthesis

| | |
|---|---|
| **Status** | Draft v1 — 2026-10-19. The metadata note is still in progress, so §1.9 is provisional |
| **Inputs** | The nine notes in [../references/](../references/), [PRD.md](PRD.md) v0.4 (D1–D33), [PLAN.md](PLAN.md) v0.4 |
| **Purpose** | Turn the research into decisions: what it settles, where it disagrees with the PRD or with itself, one table of default numbers, and the ADRs to write |

The notes stay the reference for details and sources. This document records only the conclusions and the choices
made between conflicting recommendations. Section numbers in brackets point into the notes, e.g. *protocol §12.4*.

---

## 1. What the research settles

### 1.1 Protocol — write our own `slsk` library
- **No C++ library is usable** (*ecosystem §6*):
  - museek+ and newsoul are dead.
  - soulseekcpp is a one-commit port that carries Soulseek.NET's licence terms.
- **Specification:** Nicotine+'s `doc/SLSKPROTOCOL.md` plus aioslsk's `SOULSEEK.rst`. Behaviour is cross-checked
  against Nicotine+, aioslsk and soulseek-rs (*protocol §12*).
- **MVP subset:** about 30 server messages, 13 peer messages, 4 distributed messages and the file handshake
  (*protocol §12.1*). Minimal chat rooms are added for D32.
- **Identity:** log in as major **177** (the experimental-client number in the community registry) with a non-zero
  minor. Before the first live session (M9), the maintainer asks the Nicotine+ maintainers for a reserved number.
  Never reuse 157, 160, 169, 170, 175 or 176 (*protocol §12.2*).
- **Distributed-network membership is P0.** Without a parent, nobody's global searches reach us. Selecta must
  forward searches and accept children when eligible (*protocol key finding 2*).
- **Two download dialects:** Selecta sends `QueueUpload`. As an uploader, it answers the legacy downloader-sent
  `TransferRequest` with `Queued` (*protocol §6.4*).
- **Cancel = close the `F` socket**, then ignore the `UploadFailed` echo. Previews always stream from byte 0; no range
  requests (*protocol §7, §12.3.9*).
- **Bans are silent.** A banned login gets no reply, and a throttled IP gets an instant EOF. The 30-minute ban
  arrives as a PM from `server`. Never auto-reconnect after `Relogged` (*protocol §11; etiquette §4*).
- **Bounded parsing:** cap every frame and every inflate. `qUncompress` is not usable on Soulseek zlib streams
  (*protocol §2.5*).
- **Raw path bytes:** keep them and echo them back. UTF-8 with a Latin-1 fallback is for display only
  (*protocol §2.2*).

### 1.2 Licences — learn from everyone, port from few
- **Port only from** GPL-3.0-or-later or permissive sources: Nicotine+, aioslsk, soulseek-rs (MIT), seeleseek (MIT).
- **Never copy or translate** code from Soulseek.NET, slskd, Seeker, sockseek (AGPL since 2026-05) or soulseekcpp
  (*ecosystem §1.1, §10*). Since 2026, Soulseek.NET and slskd attach GPLv3 §7 "Additional Terms".
- **Ported fragments** go into `THIRD-PARTY-NOTICES.md` with their upstream path and commit.
- **Dependencies:**
  - Every planned dependency is GPLv3-compatible: FFmpeg (GPL build), LAME, libsoxr, TagLib, Qt modules
    (*qt §11*).
  - Never build FFmpeg with `--enable-nonfree`.
  - Never bundle SF Symbols images or SF fonts.
- **Upstream contributions** to Soulfind must be written by the maintainer themselves, because the project forbids
  AI-generated contributions. The same courtesy applies to the version-number request (*ecosystem §10.1.13*).

### 1.3 Network governor — token buckets, conservative numbers
- **Every outbound operation asks the governor first.** That includes searches from every source, peer lookups,
  connections, browses, user info, previews, downloads and reconnects.
- **Rates are token buckets or sliding windows, never fixed windows** (*etiquette §4.3*). sockseek's fixed window can
  burst twice its limit at the boundary.
- **Search pace:** 2 per minute with bursts of 6 (D31). The often-quoted "34 searches per 220 s" is one developer's
  2023 measurement of the ban edge, and tools near it still get banned (*etiquette K6*).
- The full table is in §3. Ceilings are `constexpr`; SAFE-2 stays as written (settings only tighten limits) for v1.
  The etiquette note's E8 suggestion, letting per-peer limits loosen up to their ceiling, is deferred until M9 evidence.

### 1.4 UI toolkit — Qt Widgets plus a small Objective-C++ mac shell
- **Qt Widgets for the whole UI.** Qt Quick has no accessible table, and its macOS style lacks a header view, toolbar
  and tab bar (*qt key findings 7–8*).
- **The Objective-C++ shell** covers what Qt doesn't draw natively:
  - a native `NSToolbar` (decided by the M1 chrome spike);
  - native context menus via `QMenu::toNSMenu()`;
  - notifications, the Dock badge and a vsync ticker.
- **Qt 6.12.x** (LTS, released 2026-09-30). Open-source LTS patches stop around 2027-04, so plan a minor-version bump
  every ~6 months. Qt 6.12 needs macOS 14.4+.
- **Liquid Glass:** builds use the newest SDK, so Liquid Glass is on. Design for it; the opt-out is ignored on macOS 27.
- **Smooth waveform:** `QWidget::update()` is not vsync-aligned, so the waveform needs its own display-link tick
  (*qt §4; audio §8*).

### 1.5 Audio — FFmpeg in, CoreAudio out, Pioneer look
- **Decoding:** FFmpeg everywhere. For a growing file, use an `AVIOContext` that reports *non-seekable* until the file
  is complete, plus Selecta's own frame index for seeking. Otherwise the MP3, WAV/AIFF, FLAC and MP4 demuxers read
  the end of the file and stall playback (*audio §5.3*).
- **Format limit:** M4A with `moov` at the end can only play after the download completes.
- **Playback:** the `QAudioSink` callback API in M5, then CoreAudio AUHAL before v1.0, for latency-compensated
  position on Bluetooth (*audio §4, §11.3 D3*).
- **Analysis:**
  - Mono, LR4 crossovers at 200 Hz and 2.5 kHz.
  - Peak and RMS per band at 150 columns/s, √-companded `uint8`.
  - Chunked parallel decode meets the < 300 ms target: about 130 ms for a 6-min MP3 (*audio §7*).
- **Look:**
  - Overview: a single-sided cumulative stack (blue low, amber mid, white crest).
  - Zoomed view: mirrored and layered, with a brown low∩mid overlap and white highs in front at 85 %.
  - Colours: `#0055E1` / `#FFA600` / `#B36607` / white (*audio §2*).
  - This updates PREV-2's wording.
- **Rendering:** QPainter with cached tiles; `QRhiWidget` only if profiling demands it.
- **Transcode checks:**
  - Spectral cut-off cross-checked with the LAME tag's lowpass.
  - Exact fake-24-bit and upsampling checks.
  - Results are labelled "likely", never "fake" (*audio §9*).

### 1.6 Conversion and tags — LAME directly, TagLib for every tag
- **Pipeline:** FFmpeg decode → libsoxr (only when needed) → TPDF dither (only to 16-bit PCM) → **libmp3lame called
  directly**, with a genuine LAME Info tag.
  - FFmpeg's `Lavf` header shifts cues by 26 ms in some DJ apps (*conversion key findings 1–2*).
  - FLAC and ALAC use FFmpeg's encoders. AIFF uses FFmpeg's muxer with big-endian PCM. WAV uses Selecta's own plain
    PCM writer.
- **All tags and artwork are written by TagLib 2.x from Selecta's own metadata model.** Never let FFmpeg copy
  metadata.
- **ID3v2.3 needs help:**
  - TagLib drops TMOO, TSST, TDRL, TDTG and TPRO when saving v2.3, so remap them to `TXXX`.
  - Join multiple values before saving.
  - Text is UTF-16; APIC descriptions are Latin-1.
- **rekordbox reads only RIFF INFO from WAV**, so AIFF is the uncompressed filing format.
- **Cover:** one front cover, baseline JPEG ≤ 800 px (the CDJ limit).
- **Security:** downloads are untrusted input to TagLib and FFmpeg. Require TagLib ≥ 2.3.2, cap image sizes, and
  consider parsing in a helper process (*conversion §9.4*).

### 1.7 Library matching — parse, normalise, index, score
- **Each result is parsed blind:** a remote path, validated against its siblings, gives artist, title, version, album,
  track and year. It is normalised into keys and looked up in an immutable in-memory `MatchIndex`.
- **Scoring:**
  - Weights: title 0.45, artist 0.30, duration 0.20, album 0.05.
  - Contradictions *veto* rather than average (*library §9*).
- **Versions are identity for DJs.** Original, Extended, Radio Edit and remix-by-X are never "exact" for each other.
- **Typos (D28, *library §17*):**
  - Per-token OSA with budgets by length: ≤ 3 letters exact; 4–5 letters one edit, no substitution; 6–8 letters one
    edit; ≥ 9 letters two edits.
  - Same first letter required up to 8 letters. A fuzzy token scores `1 − 0.15 × edits`.
  - A symmetric-delete token index handles retrieval.
  - **rapidfuzz-cpp** (MIT) provides the scorers.
  - The mock approximates these rules; the C++ follows §17.
- **Network queries** are spelled from the release. "Search wider" drops words and never uses wildcards
  (*library §17.7*).
- **Supersede safety comes from guards** (*library §15*):
  - only after a verified, complete album;
  - only on an exact match within 2 s;
  - only when the album copy is at least as good;
  - through the Trash, with Undo and an audit log;
  - a circuit breaker above 10 files.
- **The scanner opens TagLib read-only**, so Selecta never holds a write handle on a collection file
  (*library key finding 11*).

### 1.8 Sharing — one immutable index, Nicotine+-style uploads
- **One `ShareIndex` snapshot** serves browse (pre-compressed blobs), folder contents, search answers and upload
  lookups. It is rebuilt off-thread and swapped atomically.
- **The only path from a peer's string to a file is an exact index lookup,** then an `O_NOFOLLOW` open and a
  `(dev, ino)` check (*sharing §9.1*).
- **Uploads:**
  - 3 slots, one active upload per user (a protocol constraint, not only fairness).
  - Privileged users first, round-robin between users.
  - Per-user limits of 100 files / 10 000 MB, answered with the exact standard denial strings.
- **Honest reporting:** `SharedFoldersFiles` reports what is actually served, and `SendUploadSpeed` is sent after each
  upload.
- **macOS:** FSEvents with a persisted event ID; skip iCloud "dataless" files with materialisation off; NFC names on
  the wire, native bytes on disk.

### 1.9 Metadata and previews — provisional, the note is in progress
- **Spotify development mode (2026)** — the owner needs Premium:
  - 5 users per app; search returns at most 10 results;
  - no label and no popularity; ISRC was restored in March 2026;
  - playlists readable only if owned or collaborative;
  - refresh tokens expire after 6 months.
- **Consequence:** every user brings their own Client ID and signs in with PKCE. Selecta ships no Client ID
  (META-1, updated).
- **Terms:** Spotify's developer terms forbid storing Spotify metadata and artwork in files (*metadata §4*). The
  maintainer chose Spotify tags anyway, with the risk on their own developer app (D30). Label and genre come from
  MusicBrainz, by ISRC.
- **Taken from spotDL** (*metadata §7.5*):
  - ISRC-first lookups;
  - token-sort scoring with rapidfuzz;
  - an exponential duration score, applied only after the version matches;
  - junk-word penalties: "slowed", "nightcore", etc.

  Not taken: penalising "remix", "extended", "dub" or "edit", which are DJ versions.
- **Previews:** the full track via the user's installed yt-dlp (D26, PREV-11), with a 30-second clip as fallback.
  The yt-dlp section of the note is pending.

---

## 2. Conflicts and how they're resolved

| # | Topic | Disagreement | Resolution |
|---|---|---|---|
| C1 | Search pace | PRD v0.3 and the mock used 34 per 220 s. *etiquette §9.1* proposes a burst of 6, then one every 30 s | **D31:** burst 6, one per 30 s. The mock is updated |
| C2 | Per-search collection cap | *Protocol §12.4*: 120 s / 15 s idle / 5 000 files / 500 peers. *Etiquette §9.1*: 15 s / 300 responses / 10 000 files. *Ecosystem*: Soulseek.NET's 15 s / 250 / 25 000 | Stop at the **first** of: 15 s without a new response, 120 s in total, 300 responding peers, 10 000 files. Late results are dropped by token (SRCH-16) |
| C3 | Reconnect policy | *Protocol*: 30 s base, ×2, cap 15 min. *Etiquette*: 10–30 s, ×2 ±25 %, cap 10 min, plus ban-specific waits | The **etiquette** policy, which is more specific (§3) |
| C4 | Download retries and queue polling | *Protocol*: retries at 3 → 15 → 60 min; place in queue every 5 min per file. *Etiquette*: retries at 3, 10, 30 min; head of queue only, every 10 min | The **etiquette** values: fewer server operations |
| C5 | Lossless originals | *Conversion §9.2* keeps them in `~/Music/Selecta/Originals/`. PRD D11 says Trash | **D11 stands** (the maintainer's answer). "Keep in Originals" stays a setting |
| C6 | Waveform style wording | PREV-2 says "stacked Pioneer-style". *Audio §2*: the overview is stacked, the zoomed view is layered | PREV-2 is updated to describe both views |
| C7 | Analysis cache size | PREV-8 says "a few KB per track". *Audio §11.1.6*: the detail is about 0.1–0.3 MB per track | Keep a 3.6 KB overview long-term and the full detail in an LRU (PREV-8 updated; ADR-007) |
| C8 | Persistence | PLAN: SQLite via Qt SQL. *Qt §6* and *library key finding 10*: Qt SQL's SQLite depends on the build (system vs bundled) | **Own pinned SQLite** (amalgamation) behind a thin wrapper, with WAL and FTS5. The matching hot path stays in memory (ADR-005) |
| C9 | Qt version | PLAN: "latest stable, track LTS". *Qt §1*: open-source LTS patches stop around 2027-04 | Pin **6.12.x**; bump minors about every 6 months |
| C10 | Playback API | PLAN: `QAudioSink` or CoreAudio | `QAudioSink` callback first (M5), then CoreAudio AUHAL before v1.0 (ADR-003) |
| C11 | MP3 encoding | PLAN: "LAME via FFmpeg or directly" | **Directly**, for the genuine LAME Info tag (ADR-003) |
| C12 | Spotify tags | *Metadata §4.2* recommends never writing Spotify data into files | **D30:** Spotify tags, chosen knowingly; one-time notice |
| C13 | Shipping a Client ID | META-1 v0.3 said one "may ship with the app". *Metadata §3.1*: dev mode caps an app at 5 users | **Bring your own** Client ID (META-1 updated) |
| C14 | Chat rooms | PRD D4 had no rooms. The official rules expect "the full range of features (including chat…)" | **D32:** minimal rooms (NET-10, P1) |
| C15 | Privileged users | Not in the PRD. The rules require "respect / recognition of privileges" | Privileged users go first in the upload queue (SHARE-3, P0) |
| C16 | Loosening limits | *Etiquette E8* would let per-peer limits loosen up to their ceiling. SAFE-2 says "only stricter" | SAFE-2 stays as written for v1 |
| C17 | Minimum macOS | *Qt*: 14.4 with official Qt and vcpkg dependencies; 27 if the bundle copies Homebrew dylibs | Personal builds (M1–M9) use Homebrew, so macOS 27 is the minimum and is declared honestly. Release builds (M10) use vcpkg, with macOS 14.4 as the target (ADR-004) |

---

## 3. Default numbers

These become named constants. Each one has a `constexpr` ceiling and a `static_assert(default <= ceiling)`. "Ceiling"
is the hard bound; settings can only make limits stricter (SAFE-2). Sources: *etiquette §9.1* unless noted.

| Area | Default | Ceiling or note |
|---|---|---|
| Network searches (every source) | Token bucket: burst 6, refill 1 per 30 s (D31) | Burst 8, refill ≥ 30 s; never user-loosened |
| Same query repeated | Not re-sent within 10 min; the cache answers. Refresh obeys this too | ≥ 5 min |
| Query hygiene | Drop 1-character tokens. Refuse queries under 3 or over 256 characters. A query with an excluded phrase is not sent; the user sees why | — |
| Get tracks pacing | One search per line at the refill rate, keeping 2 tokens for manual searches. Cached lines are free. No automatic fallback queries | Same bucket |
| Collecting results (SRCH-16) | Stop at 15 s without a new response, 120 s in total, 300 peers or 10 000 files (C2) | 60 s idle / 1 000 peers / 50 000 files |
| Wishlist | One term per server-sent interval (usually 12 min), round-robin | Never under 120 s |
| Reconnect (NET-4) | First retry 10–30 s, then ×2 ±25 %, cap 10 min. After the ban PM: wait 30 min + 1–5 min, pause the network, ask the user. After a login timeout: ≥ 5 min, ×2, cap 60 min. Never after `Relogged` or a rejection (except `SVRFULL`: ≥ 5 min) | ≤ 10 login attempts per hour |
| Login reply timeout | 30 s, then "possibly banned" | *protocol §12.4* |
| Keep-alive | TCP keepalive (60 s idle, 10 s interval, 6 probes) + `ServerPing` every 300 s | *protocol §12.4* |
| Sessions | One Selecta per account per machine (lock file) | 1 |
| Peer lookups / new connections | ≤ 10 in flight, ≤ 30 per minute | ≤ 60 per minute |
| Peer connect / indirect wait | 10 s / 20 s | *protocol §12.4* |
| Idle `P` / `D` connections | 60 s / 60 s; close `P` right after sending a search response | *protocol §12.4* |
| Sockets | ≤ 512 in total | — |
| Inbound frame caps | Server 16 MiB; peer-init 16 KiB; peer 1 MiB; search/folder 16 MiB (128 MiB inflated); share list 256 MiB (1 GiB inflated, only if requested); distributed 16 KiB | *protocol §12.4* |
| Downloads per peer (FAIR-2) | 1 active, 20 queued | 2 / 50 |
| Per-peer daily cap (FAIR-2) | Warning at 20 files or 1 GB; stop at 50 files or 3 GB; 7-day stop at 150 files | ≤ 100 files / 6 GB a day |
| Global downloads | 4 active + 1 preview; 200 queued | 10 / 1 000 |
| Place in queue | Head of our queue at each peer, every 10 min (C4) | ≥ 2 min per file |
| Download retries (DL-2) | Connection failures: 3 retries at 3, 10 and 30 min. `Too many files/megabytes`: re-queue only as earlier files finish. `Banned`, `File not shared.` or unknown text: never | ≤ 5 retries |
| Peer denial back-off | `Banned`: 30 days. Repeated `File not shared.`: 24 h. `Pending shutdown.`: ≥ 15 min | — |
| Previews (peer) | Auto-preview after the selection rests ≥ 600 ms, only for free-slot results. 1 active; none with a peer we're already downloading from. ≤ 30 per 10 min. Cancel on skip unless ≥ 85 % has arrived | ≤ 2 active; ≤ 60 per 10 min |
| Previews (yt-dlp) | One process at a time, a 60 s timeout, cached by release | PREV-11 |
| Browse | Burst 3, then 1 per 100 s. A user isn't browsed again within 24 h unless the user refreshes (≥ 10 min apart) | ≤ 30 per hour |
| User info / folder contents | ≤ 1 per user per 10 min, ≤ 30 per hour / burst 5, ≤ 20 per 10 min | 2× default |
| Private messages and room posts | User-typed only; ≤ 1 per 2 s; the same text to the same user or room not within 10 min | Same |
| Answering searches | Queries of ≥ 3 characters with at least one whole word; ≤ 300 files per response; never files matching an excluded phrase; queue of 200 pending, then drop | ≤ 500 files; *sharing §9.2* |
| Uploads (SHARE-3) | 3 slots; 1 active per user; privileged first; round-robin; per user 100 files / 10 000 MB; start timeout 45 s; idle 60 s | *sharing §9.2* |
| Listen port | 2234, IPv4, no obfuscation | *protocol §12.4* |
| Search cache (SRCH-12) | 24 h TTL | PRD |
| Matching durations | Exact ±3 s; likely `max(6 s, 3 %)`; far `max(15 s, 8 %)`; veto beyond `max(60 s, 25 %)` | *library* defaults |
| Waveform | 150 columns/s; LR4 at 200 Hz / 2.5 kHz; −48…0 dBFS display; white at 85 %; zoom ~8 s | *audio §11.2* |
| Inbox format | MP3 CBR 320, joint stereo, LAME `-q 3`, LAME Info tag, ID3v2.3 UTF-16, one front cover ≤ 800 px baseline JPEG, no ID3v1/APE | *conversion §9.2* |

---

## 4. ADRs to write

ADRs go in `docs/adr/` as short records: context, decision, consequences, sources. **Draft now** means the research
is enough. **After M1 spike** means the decision waits on an experiment.

| ADR | Decision to record | Main inputs | When |
|---|---|---|---|
| ADR-001 | Qt Widgets + Objective-C++ mac shell (`PlatformShell`); no Qt Quick in v1; the waveform is a QPainter tile widget with its own display-link tick | *qt §2–4* | Draft now. The `NSToolbar` question waits for the M1 chrome spike |
| ADR-002 | Own `slsk` library on Qt Core + Network + zlib; spec sources; identity 177; port policy; FakeServer/FakePeer for the distributed network | *protocol §12; ecosystem §6, §10* | Draft now |
| ADR-003 | Audio stack: FFmpeg decode with a non-seekable growing-file AVIO; `QAudioSink`, then CoreAudio; libmp3lame direct; libsoxr; TagLib ≥ 2.3.2; parser helper process (decide) | *audio §3–5, §11; conversion §1–2, §9.4* | Draft now |
| ADR-004 | Dependencies: Homebrew + `Brewfile` for development; aqtinstall Qt 6.12 in CI; vcpkg audio-only FFmpeg + LAME + soxr for release; FetchContent for small dependencies | *qt §6* | Draft now |
| ADR-005 | Persistence: own SQLite with a wrapper, WAL, migrations, FTS5 for UI search; in-memory snapshots on hot paths | *qt §6; library §10, §13* | Draft now |
| ADR-006 | Governor: token buckets, the §3 numbers, `constexpr` ceilings, one budget per operation class, activity log | *etiquette §9; protocol §12.4* | Draft now |
| ADR-007 | Waveform analysis, cache format and rendering; calibration against rekordbox | *audio §2, §7, §8* | Draft now; calibration later (maintainer) |
| ADR-008 | Matching: parser, normaliser, version classifier, `MatchIndex`, OSA budgets, symmetric-delete index, rapidfuzz-cpp; supersede guards | *library §4–10, §15, §17* | Draft now |
| ADR-009 | File safety: conversion and tagging on arrival (D29); create-only in the collection, trash-only in the Inbox; copy → verify → publish; read-only scans | PRD D2/D3/D11/D29; *conversion §9.4; library §12, §15* | Draft now |
| ADR-010 | Licence policy: GPL-3.0-or-later; allowed and forbidden sources; notices; no nonfree FFmpeg; no SF assets | *ecosystem §1.1; qt §11* | Draft now |
| ADR-011 | Metadata and previews: Spotify (bring your own ID, PKCE, D30 tags), MusicBrainz gap-filling, the yt-dlp runner (D26), cache TTLs, HTTP budgets | *metadata* (in progress) | After the note lands |
| ADR-012 | Sharing and uploads: `ShareIndex`, `UploadManager`, privileged first, limits, FSEvents | *sharing §8–9* | Draft now (new) |
| ADR-013 | Test network: Soulfind + Nicotine+ + slskd as reference peers; FakeServer/FakePeer; a counting proxy as referee; a Soulfind per test in CTest | *ecosystem §9; etiquette §8; protocol §12.5* | Draft now (new) |

---

## 5. Experiments, by milestone

Everything below runs locally. Nothing touches the live Soulseek network.

| Milestone | Experiment | From |
|---|---|---|
| M1 | Chrome spike: Qt unified toolbar vs native `NSToolbar` vs effect-view sidebar on macOS 27 (the maintainer picks) | *qt §2.4* |
| M1 | Signing identity vs Keychain, Local Network and firewall prompts across rebuilds (D33) | *qt open questions 7–8* |
| M1 | Bundle check: dylibs copied by `qt_generate_deploy_app_script`, size and `minos` | *qt open question 9* |
| M2 | Soulfind interop: login burst, loopback substitution, indirect path, attribute coverage per reference client | *protocol §13; ecosystem §11* |
| M3 | 10 000 streaming rows with a sort active: frame times; `QAbstractItemModelTester` clean | *qt open question 5* |
| M4 | Scan throughput (TagLib, worker pool); matcher at 200 k tracks (< 100 µs p95 per result); token-index benchmark | *library E1, E2, E7* |
| M5 | FFmpeg tail-read proof; growing-file playback per format on a throttled peer; ProMotion pacing; latency (speakers, USB, AirPods); analysis speed | *audio §12* |
| M6 | LAME vs FFmpeg header cue shift; encode speed; resampler fallback quality | *conversion E-2, E-4, E-5* |
| M9 | Passive only: distributed parameters, excluded phrases, M4A `moov` position, incoming search volume | *protocol §13; audio §12.7; sharing §10* |

## 6. Tasks only the maintainer can do

1. **Waveform calibration** (offline, about 15 min):
   1. Analyse the synthetic test file (tones, sweep, pink noise) in rekordbox 7 with 3Band on.
   2. Export to a USB folder.
   3. Hand over `ANLZ0000.2EX`.

   Selecta fits its crossovers and scaling to it (*audio §12.1*).
2. **rekordbox import check:** import a sample of Selecta-tagged MP3 and AIFF files and confirm every field shows,
   especially label, key and artwork (*conversion E-1*). If CDJs are at hand, also run the USB test (*E-3*).
3. **Version number:** before M9, ask the Nicotine+ maintainers to reserve a login major version for Selecta. The
   request should be written personally (*ecosystem §10.1.13*).
4. **Spotify app:** register a development-mode app with the loopback redirect `http://127.0.0.1/callback`, and keep
   its Client ID in `local/secrets.env` for development (D21, META-1).

## 7. Smaller open points (defaults apply until someone objects)

| Note | Point | Default taken |
|---|---|---|
| Conversion D-1…D-11 | ID3v2.3 UTF-16 only; 800 px art; lossless ≤ 96/24; hi-res → 44.1 kHz MP3; ask for Ogg/Opus; join artists with `", "`; drop ReplayGain on transcode; no folder art for singles; AAC/M4A kept as-is; `TENC = Selecta x.y`; strip other DJ apps' cue data | All as proposed in *conversion §9.3* |
| Sharing D-S1…D-S7 | Audio + covers + text companions; reply `Banned`; unlimited upload with a throttle during previews (P1); buddy folders hidden; "Pause network" stops new uploads and search answers but lets active uploads finish; no leecher rule; fold diacritics | All as proposed in *sharing §9.3* |
| Library 1–10 | "Other version" badge on; unspecified version counts as Original when lengths agree; in-memory engine; rapidfuzz-cpp; automatic supersede with guards; flatten disc folders as `d-tt`; Chromaprint P2; symmetric-delete index; Search wider as explicit clicks; a labelled corpus in CI | All as proposed in *library, "Decisions to make"* |
| Audio D1–D4 | Overview kept long-term, detail in an LRU; calibrate against rekordbox; `QAudioSink` first; not-yet-downloaded part shown hatched | As proposed in *audio §11.3* |
| Library Q1, Q2, Q5, Q6 | Singles naming today; disc sub-folders; filing 1–2-track folders as albums; importing into rekordbox straight from the Inbox | Ask in a later round. Q6 matters: trashing breaks such links |
