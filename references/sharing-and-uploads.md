# Sharing and uploads — share index, answering searches, upload queue

> Status: **DRAFT in progress** (skeleton committed early; items marked `TODO(verify)` are from memory and not yet
> checked against source). Accessed/researched 2026-10-19.

## 1. TL;DR

- Shares are exposed as **virtual paths** (`<virtual root>\sub\dir\file.ext`, backslash-separated). The local absolute
  path must never appear on the wire; incoming requests are resolved **only** by looking the virtual path up in the
  share index — never by building a filesystem path from peer input.
- Four request types hit a sharer: **browse** (peer code 4 → 5, zlib share list), **folder contents** (36 → 37),
  **search** (server code 26 FileSearch for user/room searches; distributed `DistribSearch` from the parent, or server
  code 93 embedded messages when we are a branch root) answered with **FileSearchResponse** (peer code 9), and
  **QueueUpload** (43) for downloads. Plus `UserInfoRequest` (15 → 16) and `PlaceInQueueRequest` (51 → 44).
- Search answering must be an in-memory **inverted index** over tokenised, case-folded virtual paths: AND of
  included words, minus excluded (`-word`) words, `*suffix` wildcard terms, plus the server's **excluded search
  phrases**. 100k files fits in a few MB of RAM and answers in well under 1 ms.
- Upload management = per-user FIFO queues served **round-robin** across users, privileged users first, a small number
  of **slots** and/or a bandwidth cap, per-user queue limits (files / MB) with standard denial strings ("Too many
  files", "Too many megabytes", "File not shared.", "Banned", …), resume from `FileOffset`.
- On macOS: scan with `fts`/`getattrlistbulk`-class APIs (or QDirIterator) once, then use **FSEvents** with persisted
  event IDs to catch up between runs; skip **iCloud dataless** files; exclude Selecta's Inbox/preview cache; expect
  **TCC** prompts for Documents/Desktop/Downloads/removable/network volumes; normalise names to **NFC** on the wire.

(TL;DR to be refined once the detailed sections are verified.)

## 2. Key findings

TODO — filled after verification against Nicotine+ / slskd / aioslsk / Soulseek.NET sources.

## 3. How shares appear on the protocol

### 3.1 Virtual roots and path format
TODO(verify) Nicotine+ virtual folder aliases; `virtual2real`; separators; NFC.

### 3.2 Share list (browse) — `SharedFileListRequest` (peer 4) / `SharedFileListResponse` (peer 5)
TODO(verify) zlib payload: dirs → files (code byte, name, uint64 size, extension, attributes), trailing private
(locked / buddy-only) directories section.

### 3.3 Folder contents — `FolderContentsRequest` (peer 36) / `FolderContentsResponse` (peer 37)
TODO(verify)

### 3.4 File attributes and cheap extraction
TODO(verify) codes 0 bitrate, 1 duration, 2 VBR, 3 encoder, 4 sample rate, 5 bit depth; per-format sets.

### 3.5 Buddy-only / locked shares
TODO(verify)

### 3.6 Server-side bookkeeping: `SharedFoldersFiles` (server 35), `SendUploadSpeed` (server 121), user stats
TODO(verify)

### 3.7 `UserInfoResponse` fields driven by the upload manager
TODO(verify)

## 4. Answering searches from a local index

### 4.1 Where search requests come from
TODO(verify)

### 4.2 Matching semantics (tokenisation, exclusions, wildcards, diacritics, minimum length)
TODO(verify)

### 4.3 Excluded search phrases (server 160)
TODO(verify)

### 4.4 Result limits and response fields (slot free, speed, queue length)
TODO(verify)

### 4.5 Index data structures and performance for 100k+ files
TODO

## 5. Upload management

### 5.1 Protocol flow (QueueUpload → TransferRequest → F connection → FileOffset)
TODO(verify)

### 5.2 Queues, slots, bandwidth, round-robin, privileged users
TODO(verify)

### 5.3 Per-user limits and denial reasons
TODO(verify)

### 5.4 Bans, ignores, buddies, auto-ban / leecher rules
TODO(verify)

### 5.5 Resume, stalls, timeouts, cancellation
TODO(verify)

### 5.6 Statistics for the ratio
TODO

## 6. macOS share indexing

### 6.1 Initial scan
TODO

### 6.2 Change tracking: FSEvents vs QFileSystemWatcher
TODO(verify) Qt's macOS watcher backend.

### 6.3 Offline volumes, iCloud dataless placeholders
TODO(verify)

### 6.4 Exclusions (hidden/system files, Selecta Inbox and preview cache)
TODO

### 6.5 TCC (privacy prompts)
TODO(verify)

### 6.6 Symlinks, aliases, hard links
TODO

### 6.7 Unicode normalisation (NFD vs NFC)
TODO(verify) Qt `QFile::decodeName` on Darwin.

## 7. Security

TODO path traversal, serving only indexed files, rate limiting, hostile peers.

## 8. Design sketch: ShareIndex + UploadManager

TODO

## 9. Implications for Selecta

TODO recommendations, proposed defaults table, decisions to make.

## 10. Open questions / verify on the local test server

TODO

## 11. Sources

Clones (shallow, read-only) under `local/research-clones/sharing/`, accessed 2026-10-19:

| Repo | Commit (date) | Use |
|---|---|---|
| nicotine-plus/nicotine-plus | `f3b9d4d` (2026-10-18) | shares, search answering, uploads, protocol doc |
| slskd/slskd | `6a450e4` (2026-10-18) | share scanning/SQLite, upload groups/limits |
| JurgenR/aioslsk | `a90ad82` (2026-10-11) | shares manager, query parsing, transfers |
| jpdillingham/Soulseek.NET | `c07c187` (2026-10-11) | message formats, upload/resume handling |
| qt/qtbase (sparse: `src/corelib/io`) | `dd7a64e` (2026-10-19) | QFileSystemWatcher FSEvents backend, `QFile::decodeName` |
