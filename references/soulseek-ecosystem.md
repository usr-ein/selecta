# Soulseek ecosystem: libraries, clients, servers and tools

> Research note for Selecta (GPLv3 C++/Qt 6 Soulseek client). Status: **DRAFT — in progress** (2026-10-19).
> Items marked **TODO(verify)** are from prior knowledge and not yet checked against a primary source.
> Repo facts (licence, last push, latest release, stars) come from the GitHub API on 2026-10-19; source citations are
> `owner/repo@<short-commit>:path:line` against shallow clones taken the same day.

## TL;DR

- **No maintained, licence-clean C++ Soulseek library exists.** museek+/newsoul (C++, GPL) are dead since 2014/2022;
  the only modern C++ code, *soulseekcpp* (C++20/asio, Sept 2026), is a one-commit, 0-star port of Soulseek.NET that
  carries Soulseek.NET's licence terms. Selecta should **write its own protocol layer in C++/Qt** (§6).
- **Port logic only from GPL-3.0-or-later or permissive sources** — Nicotine+ (GPL-3.0-or-later, the de-facto protocol
  reference and maintainer of the protocol doc), aioslsk (GPL-3.0-or-later), soulseek-rs-lib (MIT), seeleseek (MIT,
  Swift, macOS-native). **Do not copy or translate code from Soulseek.NET, slskd or Seeker**: since March–April 2026
  Soulseek.NET and slskd are GPL-3.0-only / AGPL-3.0-only **with Section 7 "Additional Terms"** that attach to any
  software incorporating any portion of them (mandatory notices, file-header format, rebranding rules). Read them for
  behaviour, implement from the protocol doc.
- **Soulfind** (GPL-3.0-or-later, D, maintained by the Nicotine+ maintainers) **is a proven local test server**: the
  Rust client soulseek-rs runs its whole e2e/CI suite against a freshly spawned `soulfind -p <port> -d <tmp.db>` per
  test. Selecta should copy that harness pattern. (Details and gaps in §5.)
- **slsk-batchdl (sldl) was renamed `fiso64/sockseek` and relicensed AGPL-3.0**; it is the closest prior art for batch
  search; its search limiter defaults are documented in §4.1. TODO(verify origin).
- **Direct prior art for Selecta's concept exists and is very recent**: *Seek* (IvaKap/seek, GPL-3.0, Mac "client for
  people who dig", on a Nicotine+ core: ownership awareness, spectrogram "proof", ratio, peer comparison) and
  *seeleseek* (bretth18, MIT, native SwiftUI macOS client). Study both for UX; neither is C++/Qt.

## Key findings

1. **Licences are the deciding factor for reuse, not code quality.** See §1.1 — Soulseek.NET/slskd Additional Terms
   make code-level reuse impractical for Selecta; Nicotine+/aioslsk/soulseek-rs/seeleseek are clean to learn from and
   port (keep their notices for any ported fragment).
2. **Soulseek.NET's Additional Terms §5 require every "Covered Software" that connects to the network to send a
   unique client version identifier** — matches Selecta's NET-9 (honest identification). Selecta should pick a unique
   login minor version regardless of reuse (coordinate with `soulseek-protocol.md`).
3. **Soulfind is the de-facto open test server** and is used for e2e testing by at least one active client
   (soulseek-rs). It does **not** send `EmbeddedMessage` (server code 93) — i.e. it does not emulate the real server's
   distributed-search injection — but it does hand out `PossibleParents` (102) so distributed-network adoption can be
   exercised. TODO: confirm search routing (§5.1).
4. **Seeker's `MockSoulseekClient`** (C#, in-process mock network driven by magic search keywords such as `n:N`,
   `speed:N`, `failat:N`, `stall`) is a good model for a Selecta UI-development fake that needs no server at all.

## 1. Comparison table

Activity = last push / latest release (2026-10-19). Coverage: **Full** = server + peer + file + distributed messages incl.
uploads; **Client** = searching/downloading/browsing but partial or no sharing/distributed participation.

| Project | Kind | Language | Licence (SPDX, verified) | Activity | Protocol coverage | Reuse for Selecta |
|---|---|---|---|---|---|---|
| **SoulseekQt** | Official client | C++/Qt (closed) | Proprietary freeware | TODO(verify) | Reference behaviour | UX baseline only |
| **Nicotine+** | Desktop client (GTK 3/4) | Python 3 | GPL-3.0-or-later (SPDX headers) | 2026-10-18 / 3.3.11 (2026-09-16), 3.4.0.dev2 on main; ~3.2k★ | Full; maintains the protocol doc | **Primary protocol reference; port logic** |
| **Soulseek.NET** | Library | C# (.NET) | GPL-3.0-only **+ §7 Additional Terms** | 2026-10-13 / 10.0.2 (2026-06-10) | Client side incl. uploads, distributed child/parent | Behavioural reference only |
| **slskd** | Daemon + web UI | C# + React | AGPL-3.0-only **+ §7 Additional Terms** | 2026-10-19 / 0.26.1 (2026-10-10); ~4.1k★ | via Soulseek.NET | Design ideas (limits, groups, API) |
| **aioslsk** | Library | Python asyncio | GPL-3.0-or-later (pyproject) | 2026-10-14 / v1.7.1 (2026-10-11) | Full (TODO details) | Port logic; good docs |
| **Seeker** | Android client | C# (.NET Android) | GPL-3.0-only (uses modified Soulseek.NET) | 2026-10-19 / v3.11.2 | via Soulseek.NET fork | UX ideas; mock-network idea |
| **museek+** (`eLvErDe/museek-plus`) | Daemon + Qt/GTK/curses clients | C++ (+Python) | GPL-2.0-or-later | 2022-03-08, README says "No longer maintained!" | Old subset | Historical only |
| **newsoul** | museek+ daemon fork | C++11 | GPL-3.0-or-later | 2014-09-13 | Old subset | Historical only |
| **soulseekcpp** | Library (port of Soulseek.NET) | C++20 + asio | GPL-3.0 + Soulseek.NET §7 Additional Terms | single commit 2026-09-17; 0★ | Claims full (untested by others) | Read only; do not depend on |
| **soulseek-rs** (`michel/soulseek-rs`) | Lib + TUI/CLI/daemon | Rust | MIT | 2026-10-14 / v19.1.0 | Claims full live message set; e2e vs Soulfind | **Port logic + test-harness pattern** |
| **seeleseek** | Native macOS client (SwiftUI) | Swift 6 | MIT | 2026-10-13 / v1.9.0; macOS 15+ | Client + sharing, distributed, obfuscation (heuristic) | macOS UX + Swift protocol code |
| **Seek** (`IvaKap/seek`) | Mac client on Nicotine+ core (Tauri + Python sidecar, TODO verify) | TypeScript + Python | GPL-3.0 | 2026-10-13 / v0.3.0 | via Nicotine+ | **Closest product prior art** |
| **slsk-batchdl / sldl** → `fiso64/sockseek` | Batch download CLI | C# | AGPL-3.0 (TODO: when relicensed) | 2026-10-15 / v3.0.6 | via Soulseek.NET | Batch/matching/rate-limit prior art |
| **soularr** | Lidarr ↔ slskd bridge | Python | GPL-3.0 | 2026-10-06 / v1.2.2 | via slskd API | Selection/matching ideas |
| **Soulfind** | Server | D | GPL-3.0-or-later | 2026-10-14 / rolling "latest" | Server side (subset, §5.1) | **Local test server** |
| nafsi / souls / soulsex | Servers | Rust / Rust / Elixir | GPL-3.0 / GPL-3.0 / BSD-3-Clause | 2026 (young, 0–4★) | Incomplete | Not needed |
| slsk-client | Library | TypeScript (Node) | MIT | 2026-10-05 / v2.0.2 | Client (search/download) | Low |
| bh90210/soul | Library | Go | Unlicense | 2025-04-20 / v1.1.0 | Client + some distributed | Low |
| others (crabseek MIT, slsk-rs GPL-3.0, slskr AGPL-3.0, zslsk GPL-3.0, goose/spotseek/vessel/soulseek-ts unlicensed, livelook AGPL-3.0, py-soulseek-lib GPL-3.0) | various | Rust/Zig/Go/TS/JS/Py | as listed | mixed | partial | None (§3.7) |
| Wireshark `packet-slsk.c` | Dissector | C | GPL-2.0-or-later TODO(verify) | TODO | old message set TODO | Debug local captures |

### 1.1 Licence consequences for a GPLv3 Selecta

| Source licence | Can Selecta port/translate code? | Conditions |
|---|---|---|
| GPL-3.0-or-later (Nicotine+, aioslsk, Soulfind, newsoul) | Yes | Keep copyright notices on ported fragments; Selecta stays GPL-3.0(-or-later). |
| GPL-2.0-or-later (museek+) | Yes | Same; combined work is GPL-3.0. |
| MIT / Unlicense (soulseek-rs, seeleseek, slsk-client, crabseek, soul) | Yes | Keep the MIT notice for ported fragments (e.g. a `THIRD-PARTY-NOTICES` file). |
| GPL-3.0-only + §7 Additional Terms (Soulseek.NET, Seeker) | Technically yes, **but not recommended** | Selecta would become "Covered Software": would have to be GPL-3.0-**only** (conflicts with the PRD default OQ-7 "or-later"), ship the Additional Terms in LICENSE, add the "modified version of Soulseek.NET" README notice, follow its file-header format for every derived file, and use a unique client version id. |
| AGPL-3.0-only + §7 (slskd, sockseek) | **No** for a desktop app | AGPL network clause + Additional Terms; GPLv3 code may be combined with AGPLv3 (GPLv3 §13) but the AGPL parts keep their terms. Not worth it. |
| No licence (soulseek-ts, goose, vessel, spotseek) | **No** | All rights reserved. |

Evidence: `jpdillingham/Soulseek.NET@c07c187:LICENSE` lines 679–end ("ADDITIONAL TERMS", §1–6, incl. §3 "Modified Version
Identification", §4 "Rebranding", §5 "Client Version Identifier"); `NOTICE`; file headers e.g. `src/SoulseekClient.cs:1-22`
("version 3", `SPDX-License-Identifier: GPL-3.0-only`). `slskd/slskd@6a450e4:LICENSE` has the same terms for AGPLv3.
Terms added in commits between 2026-03-21 and 2026-04-03 (GitHub API, `commits?path=LICENSE`). Older Soulseek.NET
releases were plain GPL-3.0 (TODO(verify) exact first version with the terms) — irrelevant if Selecta ports nothing.

Recommendation: treat Soulseek.NET/slskd/Seeker/sockseek as **read-for-behaviour** sources (what to do), and Nicotine+'s
protocol doc + Nicotine+/aioslsk/soulseek-rs code as **port-from** sources (how). This keeps OQ-7 (GPL-3.0-or-later)
open.

## 2. Official client: SoulseekQt

| Aspect | Facts (confidence) |
|---|---|
| Who / licence | Written by Nir Arbel, the Soulseek network's operator; closed-source freeware; Qt; Windows, macOS, Linux (AppImage). (high) |
| Activity | Builds 2024-2-01 and **2026-4-30**, both login minor version **101** under major **157** (`nicotine-plus@f3b9d4d:doc/SLSKPROTOCOL.md:290-308`; FileHorse lists 2026.4.30 with "distributed network bug" fixes). macOS builds lag: last macOS-specific news post is the 2021-2-6 build ("newer Qt build with a redesigned interface"; 2020 fix for invisible text in Catalina dark mode). (medium) |
| Server address | **Not configurable** (Soulfind #50: a user patched the binary to replace `server.slsknet.org`; aioslsk suggests a hosts-file redirect). → not usable in Selecta's automated test network. (medium) |
| Search filter language | Typed into a box under every result/browse window (slsknet.org "On-the-fly filtering", node/103), then "Fun with Filters" (2011-11-09, node/267): words to require, `-word` to exclude, `isvbr`, `iscbr`, `minbitrate:N` (`mbr`), `minfilesize:MB` (`mfs`), `minfilesinfolder:N` (`mfif`, folder-level), `.ext` (per Nir's comment). Saved filters + "search correlations" + separate wishlist tabs added 2011-12-14 (node/303). The audio filters relied on attributes the official clients then extracted **only from MP3s**. (high for 2011 behaviour; current behaviour unverified) |
| Attributes it sends today | Contested: sldl's author says it "does not broadcast the bitrate"; slskd PR #402 discussion says it no longer sends the VBR flag; aioslsk's investigation lists bitrate/VBR for lossy and sample rate/bit depth for lossless. → measure on fixtures captured from real traffic by the maintainer (protocol note). (low) |
| Other features | Search tabs, wishlist (server-paced), "Download containing folder", browse shares, user info, PMs, chat rooms, interests/recommendations, upload/download panels, privileges. No preview, no collection awareness, no conversion. (high) |
| Role for Selecta | UX baseline that users know (the filter syntax is the lingua franca: slskd copies it); behavioural reference for what the server expects of a "full feature set" client. |

Recommendation: accept a **SoulseekQt/slskd-compatible text filter syntax** in Selecta's search field as a power-user
shortcut that maps onto the structured filters of SRCH-2…5 (slskd's parser and tests are a ready-made spec:
`slskd/slskd@6a450e4:src/web/src/lib/searches.js` and `searches.test.js`; slskd adds `islossless`, `islossy`,
`minbitdepth:N`, `minlength:N`, and `a|b` alternatives).

## 3. Open-source clients and libraries

### 3.1 Nicotine+

`nicotine-plus/nicotine-plus@f3b9d4d` (main = 3.4.0.dev2; release 3.3.11). Python 3 + GTK 3/4, GPL-3.0-or-later,
~22.5k lines in `pynicotine/*.py` without the GUI. The maintainers also maintain the **protocol documentation**
(`doc/SLSKPROTOCOL.md`, published at nicotine-plus.org) and Soulfind — this is the centre of gravity of open Soulseek
knowledge.

- **Architecture:** one networking thread (`slskproto.py`, ~3k lines: selectors-based event loop, all server/peer/
  distributed/file connections, indirect connection handling, timeouts) exchanging message objects (`slskmessages.py`,
  ~4.5k lines, every message with `make_network_message`/`parse_network_message` and docstrings) with a core event bus
  (`events.py`); feature components per file: `search.py`, `downloads.py`, `uploads.py`, `transfers.py` (shared base),
  `shares.py` (scanner + word index for answering searches), `userbrowse.py`, `privatechat.py`, `networkfilter.py`
  (ban/ignore by user and IP), `portmapper.py` (UPnP + NAT-PMP in pure Python), `pluginsystem.py`. GUI in `gtkgui/`,
  `headless/` for no-GUI runs (`--headless`).
- **Identification:** sends major **160**, minor **3** (3.4.0+) — `slskproto.py:1333-1347`. The protocol doc keeps the
  **registry of major versions** (`doc/SLSKPROTOCOL.md:225-283`): 157 NS/SoulseekQt, 158/159 reserved for SoulseekQt
  development, 160 Nicotine+, 169 seeleseek, 170 Soulseek.NET, 175 aioslsk, 176 soulseek-rs, **177 "experimental
  development and testing"**; obsolete 178/182 Museek+, 180 PySoulSeek, 181/198/200 old Nicotine. The `Login` docstring
  (`slskmessages.py:643-659`): "Numbers are chosen that avoid impersonating clients with reserved major versions …
  Experimental clients may use major version 177" and "It is unacceptable to use randomly generated usernames, as such
  automated scripting is disallowed by the official server rules".
- **Design ideas worth copying:** wishlist sends **at most one item per server-provided interval**
  (`search.py:542-557`); answering others' searches is capped (`maxresults` 300, `config.py:311`); results display cap
  2500 (`config.py:325`); search history capped at 200; the whole client is driven by message objects + events, which
  makes record/replay of protocol fixtures straightforward.
- **Tests:** unit tests for message round-trips and the network thread with mocked sockets
  (`pynicotine/tests/unit/protocol/test_slskmessages.py`, `test_slskproto.py`), search/wishlist, shares, transfer queues
  (`tests/unit/transfers/test_uploads.py`, `test_get_upload_candidate.py`). No server-based e2e.
- **Reuse:** port logic and message definitions freely (GPL-3.0-or-later), keeping notices on ported fragments.

### 3.2 Soulseek.NET

`jpdillingham/Soulseek.NET@c07c187` (10.0.2), C#, ~111k lines incl. tests; **GPL-3.0-only + §7 Additional Terms**
(§1.1). Powers slskd, sockseek, Seeker (fork), many *arr tools.

- **Architecture:** async/await throughout; `SoulseekClient.cs` (~5.3k lines) as the façade; `Network/` with
  `PeerConnectionManager` (message connections, transfer connections; **races direct and indirect connections in
  parallel and cancels the loser**, lines ~200-250), `DistributedConnectionManager` (parent/children), `MessageConnection`,
  TCP adapters behind interfaces (testable); `Messaging/` with `MessageBuilder`/`MessageReader`, per-family handlers
  (`ServerMessageHandler`, `PeerMessageHandler`, `DistributedMessageHandler`) and `Compression/`; `Common/Waiter.cs`
  — **"await-able server messages"**: callers wait on a `WaitKey` (message code + username/token…) with a timeout; the
  handler completes the oldest matching wait. This pattern maps directly onto a C++/Qt "pending replies" registry.
  `Common/TokenBucket.cs` for speed limits.
- **Search defaults** (`src/Options/SearchOptions.cs`): `SearchTimeout` 15 000 ms **measured from the last response**,
  `ResponseLimit` 250 responses, `FileLimit` 25 000 files, `FilterResponses` true, `MinimumResponseFileCount` 1,
  `RemoveSingleCharacterSearchTerms` true; client option `MaximumConcurrentSearches` **2** (`SoulseekClientOptions.cs:120`;
  lowered from 10 in PR #883, 2025-10-15). No time-window search rate limit.
- **Tests:** large unit suite with mocks behind every network interface (`tests/Soulseek.Tests.Unit`), plus
  `Soulseek.Tests.Integration`. The author tests against Soulfind (§5.1).
- **Reuse:** behaviour reference only (licence).

### 3.3 slskd

`slskd/slskd@6a450e4` (0.26.1), C# ASP.NET Core daemon + React web UI, ~4.1k★, **AGPL-3.0-only + §7 Additional Terms**.
A headless "client-server" Soulseek app (Docker-first) with REST API, Swagger, webhooks/scripts on events, Prometheus
metrics, a "relay" mode (one instance controls shares of others) and SQLite for transfers/searches.

Design ideas worth taking (not code) — all documented in `docs/config.md`:

| Idea | slskd detail | Selecta use |
|---|---|---|
| Upload governance by **groups** | Built-in `privileged` (priority 0), `default`, `leechers` (share count < threshold), `blacklisted`, plus user-defined groups; each with priority, `roundrobin`/`firstinfirstout` strategy, slots, speed limit, and **limits** on queued/daily/weekly files, megabytes and failures (`config.md:325-605`). Rejections like "Too many files this week". Example values: default group queued 150 files/1500 MB, weekly 1500 files; leechers 1 slot, 15 queued files. | SHARE-3 / SHARE-4 upload manager design; also tells Selecta what *other* sharers enforce → keep per-peer download caps well under these (FAIR-2). |
| Incoming-search **circuit breaker** | `throttling.search.incoming`: concurrency 10, max 500 pending requests (excess discarded), ≤ 500 files per response (`config.md:1053-1080`). Regex filters on incoming queries (e.g. drop queries < 3 chars). | SHARE-2 search answering under load. |
| **Retention** | Searches deleted after 1 day by default; transfers by state; files by age (`config.md:1018-1050`). | CACHE-1 TTLs. |
| Download retry | resume partial downloads by default; example config: 3 attempts, 5 s initial delay, 60 s max delay (`config.md:350-372`). | DL-2 defaults. |
| One search at a time via API | `SearchesController.cs:67-96` returns HTTP 429 "Only one concurrent operation is permitted". | Governor at the service boundary, not only at the socket. |
| Text search filters (web UI, client-side) | SoulseekQt syntax + `islossless`/`islossy`/`minbitdepth`/`minlength`, `a|b` (`src/web/src/lib/searches.js`, tests alongside). | Power-user filter syntax (§2). |

slskd does **not** rate-limit outgoing searches over time (only the API concurrency guard), and it is a daemon + web UI,
which the PRD rules out for v1. Running it as Selecta's backend was rejected (§6).

### 3.4 aioslsk

`JurgenR/aioslsk@a90ad82` (v1.7.1, 2026-10-11), Python ≥ 3.11 asyncio library, **GPL-3.0-or-later**, ~15k lines,
reserved major version **175**. The best-*documented* implementation:

- `docs/source/SOULSEEK.rst` (~2.6k lines) — protocol **flows**: direct vs indirect peer connections, obfuscation,
  delivering search results, distributed network (obtaining a parent, children, max children, how searches travel),
  transfer flows (incl. deprecated upload flow, "upload not allowed"), logon, and a server-side behaviour model per
  message ("Server Flows"), plus **search query rules** (exclusions, prefix-only wildcards `*oney`, case-insensitive,
  order-independent, path-separator matching only on directories, term-boundary matching algorithm) and the
  **attribute table** (0 bitrate, 1 length, 2 VBR, 4 sample rate, 5 bit depth; lossless vs compressed categories) —
  lines 2525-2595. `DESIGN.rst` (client init, distributed network roles), `MESSAGES.rst`, `SETTINGS.rst`.
- Code: manager per subsystem (`network/network.py` connection establishment, `distributed.py`, `search/manager.py`,
  `shares/manager.py` (index + query matching), `transfer/manager.py` + `transfer/state.py` (**explicit transfer state
  machine**), `network/rate_limiter.py` (token-bucket transfer speed), `network/upnp.py`), typed `events.py`,
  `protocol/messages.py` + `primitives.py` (declarative message fields).
- `tests/e2e/mock/server.py` — the mock server with a distributed network (§5.2).
- **Reuse:** port logic freely (keep notices); excellent cross-check for message layouts.

### 3.5 museek+ / museekd / newsoul
museek+ README: "No longer maintained!" and points users to slskd, Nicotine+ and Seeker. Architecture: `museekd` daemon
+ `museeq` (Qt GUI) / `mucous` (curses) / `murmur` (GTK) talking to the daemon over an "interface" protocol; `muscan`
share scanner. newsoul (2013–2014) was a C++11 resurrection of the daemon only (libevent, json-c, taglib, sqlite3).
Both predate obfuscation, modern login versions and current server behaviour. Value for Selecta: historical, plus the
daemon/UI split idea. TODO: any still-useful pieces (share DB format?).

### 3.6 Seeker

`jackBonadies/SeekerAndroid@ff1dcc6` (v3.11.2, 2026-10), C# .NET for Android, GPL-3.0-only, ~700★. Full-featured
mobile client (search with wishlist and filters, sharing, messages, chat rooms, port forwarding, user info, privileges)
built on a vendored, **modified Soulseek.NET** (`Seeker/Soulseek.NET-master/`). Ideas: compact mobile search/transfer
UX; the "Debug Mock" in-process fake network (§5.2). Reuse: none (licence lineage).

### 3.7 C++, Rust, Go, Swift, JavaScript and other implementations

**C++ — the only C++ code is either dead or licence-encumbered:**

| Project | Facts | Verdict |
|---|---|---|
| **soulseekcpp** (`comedyjohn/soulseekcpp@bade51b`) | C++20, standalone asio, ~16.5k lines; README: "a port of Soulseek.NET (upstream commit `76fddb6`, September 2026)" covering server/peer/distributed connections, search, browse, downloads/uploads, rooms, PMs. **One commit (2026-09-17), 0★, 790 lines of tests.** LICENSE = GPLv3 **with Soulseek.NET's Additional Terms** (identical text); README lacks the "modified version of Soulseek.NET" notice the terms require; example passes 170 = Soulseek.NET's major version. | **Do not build on it.** Using it makes Selecta "Covered Software" (GPL-3.0-only + terms) and it has no track record. Reading it is fine (shows how Soulseek.NET's design maps to C++20/asio). |
| museek+ / newsoul | §3.5 — unmaintained since 2014/2022, GPL-2.0+/GPL-3.0+. | Historical. |
| soulseek-basecamp (`xAlisher`) | C++/Qt plugin that talks to an **slskd REST API** (`src/core/SlskdClient.cpp`), not the protocol. | Not a protocol implementation. |
| slskPlusPlus, Bishop | 5-file stub (no licence); Bishop = music server using slskd's API (GPL-3.0). | No. |

**Rust:**
- **soulseek-rs** (`michel/soulseek-rs@20bd676`, MIT, created 2025-01, ~560 commits by one author, release v19.1.0):
  library `soulseek-rs-lib` (~23k lines, **zero dependencies** — own zlib inflate and MD5) + TUI/CLI/daemon. Actor
  model (`actor/server_actor.rs`, `actor/peer_actor.rs`, `peer_registry.rs`), `client/upload_queue.rs`,
  `client/distributed.rs`, `client/children.rs`, `shares/`, `download_store.rs` (resumable `.part` files). Claims the full
  live message set with a per-message coverage table proven e2e against Soulfind (`docs/protocol-coverage.md`); known
  gaps: locked (buddy-only) results ignored, obfuscated connections not dialled, no firewalled children. Reserved major
  version 176. **Best permissive reference and the model for Selecta's test harness.** Caveat: one maintainer, fast
  version churn.
- Others: crabseek (MIT, TUI, 2026-09), slsk-rs (GPL-3.0, TUI, WIP, last push 2026-02), slskr (AGPL-3.0, Rust web
  client from the "slskdN" slskd-fork authors), libsoulseek (GPL-3.0, created 2026-10-18), vessel (no licence, 2022),
  RustSoSeek (Apache-2.0), several unlicensed hobby clients. None mature enough to depend on.

**Swift:** **seeleseek** (`bretth18/seeleseek@195eddc`, MIT, v1.9.0, 2026-10; macOS 15+, Swift 6, SwiftUI, GRDB;
reserved major version 169). Core in a local Swift package `Packages/SeeleseekCore` (protocol, server/peer connections,
`NetworkClient+Distributed.swift`, UPnP `NATService`, obfuscated listener port, uploads, leech detection). UX: sidebar
sections (Search, Wishlists ⌘2, Transfers, Chat, Browse, Friends, Statistics, Settings) and **quick filter presets
"MP3 320", "FLAC", "Lossless", "Hi-Res"** plus an advanced panel (format, bitrate ≥, sample rate ≥, bit depth ≥, free
slots) — `site/src/content/docs/guide/searching.md`. Closest *native macOS* prior art; MIT, so fragments may be ported
with notice. Also: Soulseek-Arpeggio (SwiftUI, MIT, 2026-10), several iOS clients.

**Go:** bh90210/soul (Unlicense, client library incl. some distributed handling, last push 2025-04), goose (no licence,
2021 stub), spotseek (no licence), oto (AGPL-3.0 TUI), waves (GPL-3.0 terminal player with Soulseek integration).
**JavaScript/TypeScript:** slsk-client (MIT, Node, search/download, v2.0.2 2026-09; used by soulseek-cli), soulseek-ts
(no licence, 2024), livelook (AGPL-3.0, 2019), nslsk (2015). **Python:** py-soulseek-lib (GPL-3.0, 2022). **Zig:** zslsk
(GPL-3.0, incomplete), libsslsk (CC0, WIP). **Other mac clients:** iSoul (Obj-C, 2012, dead), Seek (§8).

None of these changes the conclusion: there is no maintained, licence-clean C++ library to adopt.

## 4. Automation tools

### 4.1 slsk-batchdl / sldl → sockseek

Evidence: `fiso64/sockseek@ff9a3ac` (HEAD tagged v3.0.7, 2026-10-15) and its GitHub history/issues.

**Identity and licence (high confidence).** `fiso64/slsk-batchdl` redirects to `fiso64/sockseek` (same repo, renamed;
README: "formerly named `sldl` (and `slsk-batchdl` before that)"). Code renamed 2026-05-23 (`b30135b2`); LICENSE changed
GPL-3.0 → **AGPL-3.0** on 2026-05-11 (`7d521792`). Last GPL-3.0 release: **v2.6.0 (2025-11-29)**. The 3.x line aims to
become "a fully-featured persistent Soulseek client". Built on Soulseek.NET (minor-version range 800850000–800859999).
Selecta should **reimplement ideas only**.

**Search rate limiter (high confidence on code; low confidence on the numbers' validity).**

| Item | Value / behaviour | Evidence |
|---|---|---|
| `--searches-per-time` (`--spt`) | **34** | `Sockseek.Core/Settings/EngineSettings.cs:48` |
| `--searches-renew-time` (`--srt`) | **220 s** (≈ 9.3 searches/min average) | `EngineSettings.cs:50` |
| Help text | "Higher values may cause 30-minute bans" | `Sockseek.Cli/Help.Content.cs:125-128` |
| Notes text | "The server bans users for 30 minutes if too many searches are performed within a short timespan … allow up to 34 searches every 220 seconds" | `Help.Content.cs:694-699`, `README.md:709` |
| Algorithm | **Fixed window**, not a token bucket: all 34 permits refill at once when the window lapses (lazy reset) → up to **68 searches back-to-back** across a window boundary | `Sockseek.Core/Search/RateLimitedSemaphore.cs:1-92` |
| When exhausted | Jobs wait (status `SearchRateLimited`/"Waiting"); nothing dropped | `Sockseek.Core/Search/Searcher.cs:639-659` |
| What counts | Every network search: main query, an extra no-diacritics variant, "artist maybe wrong" fallback, up to 5 `--desperate` fallbacks | `Searcher.cs:554-637` |
| Concurrency | `--concurrent-searches` 2, `--concurrent-jobs` 20, `--search-timeout` 5000 ms, `--max-stale-time` 30 000 ms | `EngineSettings.cs:42-44`, `Help.Content.cs:123-124` |
| Reconnect | 1 s doubling, capped at 8 s — not ban-aware | `Sockseek.Core/Soulseek/SoulseekClientManager.cs:236-280` |

**Origin of 34/220 (high confidence): empirical, single developer, Nov 2023.** Introduced in commit `20d8310d`
(2023-11-28, message "stuff") in response to issue #12 (a user disconnected "within 2–3 minutes" at ~4 concurrent
searches). The author wrote in that commit's README: "These values were determined through experimentation as
unfortunately I couldn't find any information regarding soulseek's rate limits, so they may be incorrect" (disclaimer
since dropped), and on #12: "From my own testing it seems to be around 34 searches every 220 seconds". The same commit
added `--random-login` (random credentials to dodge bans; now hidden, "for testing only"). **Not** derived from the
server operator, Soulseek.NET, slskd or Nicotine+ — none of which publish or implement a search *rate* limit (Soulseek.NET
only caps *concurrent* searches, default 2 since PR #883, 2025-10-15: "10 concurrent searches will put a strain on any
host").

Real-world reports (user statements = hearsay):

| Report | What happened |
|---|---|
| sockseek #12 (2023-11) | Disconnected within 2–3 min at ~4 concurrent searches, 6 s timeout (≈ up to 40 searches/min). |
| sockseek #95 (2025-04) | "Not found" after 10–20 items **even at default 34/220**; user says the IP got banned; workaround "one search per minute and a different IP". |
| soularr #124 (2025-05, fixed 2026-04) | "30 min ban" when searches return few/no results and run back-to-back (~1 search / 5–6 s sustained); another user: "30 mins ban every 11 hours". |
| sockseek #128 (2025-08) | Not a ban: the **server silently drops searches containing excluded substrings** (e.g. "clipse" inside "Eclipse"); "The list of banned terms appears to be growing rapidly." Zero results are therefore ambiguous. |
| slskd #1866 (2026-10) | slskd author (hearsay): "you might have been temporarily banned for too many login attempts". |

Searches of slskd, Nicotine+ and Soulseek.NET issue trackers found no search-rate ban reports. Exact limits stay unknown;
the etiquette note owns the final numbers (ADR-006).

**Filtering (high confidence).** Two condition sets (`Sockseek.Core/Matching/Conditions/FileConditions.cs:7-23`,
defaults `Sockseek.Core/Settings/SearchSettings.cs:14-29`):

| Set | Purpose | Defaults |
|---|---|---|
| Necessary (`--format`, `--length-tol`, `--min/max-bitrate`, `--min/max-samplerate`, `--min/max-bitdepth`, `--strict-title/artist/album`, `--banned-users`, `--allowed-users`, `--cond "br>=320; format=mp3"`) | Hard filter | formats mp3,flac,ogg,m4a,opus,wav,aac,alac; length tolerance 3 s |
| Preferred (`--pref-*`, `--pref "<conds>"`) | Ranking only ("never filter anything out") | format mp3; length tol 3 s; bitrate 200–2500; max sample rate 48 kHz; strict title + strict album |
| `--strict-conditions` | Reject files with **missing** attributes | off — by default a file with **no bitrate attribute passes `--min-bitrate`** (`AcceptMissingProps = true`) |

The author notes (`Help.Content.cs:466-475`) that "the standard Soulseek client does not broadcast the bitrate", so strict
bitrate filtering excludes SoulseekQt sharers. (Cross-check: aioslsk docs say the official client sends bitrate/VBR for
"compressed" formats and sample rate/bit depth for lossless — `JurgenR/aioslsk@a90ad82:docs/source/SOULSEEK.rst:2559-2595`.
The protocol note should settle which attributes appear per format.) Selecta's strict mode (SRCH-6) must make this
explicit — it is exactly the "unknown quality" policy.

**Ranking (high confidence)** — lexicographic key, `Sockseek.Core/Search/SearchProjection/ResultSorter.cs:213-542`:
1. user has not failed this run (downrank after 1 failure, ignore after 2);
2. necessary conditions met; 3. user allow/ban preferences; 4. has a valid length;
5. **bracket check**: file name has no `(...)`/`[...]` unless the query has brackets — penalises "(Extended Mix)";
6–11. strict then fuzzy match of title, album, artist ("Identity beats quality preferences");
12–17. preferred length, format, bitrate, sample rate, bit depth, all-preferred;
18. free upload slot; then upload speed in 650 KiB/s buckets; strict title/artist in full path; **consensus** (how many
results infer the same artist/title); speed in 350 KiB/s buckets; bitrate in 80 kbps buckets; stable hash tie-break.
Queue length and file size are never used.

**Matching a tracklist line (high confidence).** Inputs: CSV (column aliases), YouTube, Spotify, Bandcamp, MusicBrainz,
`slsk://` links, strings, list files with per-line conditions (`README.md:157-260`). Query = "artist title", lowercased,
punctuation → spaces (`Searcher.cs:661-696`, special set in `Utils.cs:439-449`), plus a hard-coded **banned-term list
dodged with a `*` wildcard** (`Searcher.cs:699-738`) — deliberate evasion of the server's filter; Selecta must not copy
this (use the server's ExcludedSearchPhrases list to explain zero results instead). Strict string match: replace `_ : | ? > < * "`
with spaces, strip diacritics, case-insensitive word-boundary containment; title vs file name, artist vs full path, album
vs parent folder (`FileConditions.cs:296-443`). Optional `--remove-ft` (`Utils.cs:502-535`), `--remove-brackets`,
`--regex`, `--parse-title`, `--extract-artist`. Filename inference for consensus counting: `Searcher.cs:356-470`.
`--fast-search` starts a provisional download from the first good result (free slot, ≥ 1 MB/s, preferred conditions)
after 300 ms.

**"Already have it" (high confidence)** — `Sockseek.Core/Files/TrackSkipper.cs`: default *Index* mode (its own record of
previous runs, an m3u-based index); optional *Name* mode over a music directory (file name contains title AND relative
path contains artist, word boundaries — false-positive prone, e.g. "Intro"); *Tag* mode (TagLib: title equal after
removing all spaces, artist contained). No version/remix awareness beyond `--remove-ft`, no duration check unless
`--skip-check-cond`.

**Etiquette episode (2026-03, sockseek #163/#165):** sldl reported fake share counts (10 files / 50 folders) to avoid
peers' "no shares" auto-bans; the Nicotine+ and Soulseek.NET maintainers objected ("done in bad faith") and quoted the
rule that third-party clients are tolerated only if they implement the full feature set including uploads; now defaults
to 0. Selecta must report true counts (it shares by design).

### 4.2 soularr and other *arr bridges

**soularr** (`mrusse/soularr@2037399`, GPL-3.0, Python, ~1k★) bridges Lidarr's wanted albums to slskd's HTTP API. One
search per album (album title, optional artist), `search_timeout` 5000 ms, `maximum_peer_queue` 50,
`minimum_search_interval` **5 s between search starts** (added 2026-04-25 for issue #124: "The search timeout only
applied to the actual length of a specific search not the time between searches … little to no results … can lead to
rapid searches"). Quality spec `allowed_filetypes = "flac 24/192,flac 16/44.1,flac,mp3 320,mp3"` — **exact** equality on
bitrate or bit depth/sample rate, list order = priority, files lacking the attribute are rejected when a spec has one
(`soularr.py:265-310, 710-752`). Album match: difflib ratio of "title.ext" vs file name (0.8 sample config, 0.5 code
fallback), every track must match, folder must have exactly the expected number of files of one extension; **no duration
check** (`soularr.py:91-166, 348-392`). Politeness: caches folder browses per user ("prevents us from hammering the
users"), remembers broken users.

**Lesson for Selecta's batch (BATCH-4):** pace on search *starts*, not on completion — a zero-result search completes
instantly, so "timeout" is not pacing.

Others (medium confidence, skimmed): **Tubifarry** (MIT, Lidarr plugin with slskd indexer) has per-user grab limits per
hour/day/week and max queued per user — but `SlskdIndexer.cs:28 RateLimit => new(3)` is 3 *ticks*, i.e. effectively no
indexer rate limit (likely bug). **setseeker** (no licence) — DJ set → Shazam IDs → slskd; refuses to download unless
the backend is actually sharing (a "reciprocity audit") — closest automation to Selecta's DJ use case. **decksmith**
(MIT) — turns a Soulseek/Nicotine+ download pile into a rekordbox-ready collection.

## 5. Servers, emulators and test harnesses

### 5.1 Soulfind — yes, it is the local test server

Evidence: `soulfind-dev/soulfind@5a256f5` (2026-10-15) + GitHub API/issues. Confidence high unless marked.

| Question | Answer |
|---|---|
| Purpose | README: "Soulfind exists for local testing, and should not be used in production." Nicotine+'s protocol doc: "It handles the protocol well enough for development and testing of client implementations" (`nicotine-plus@f3b9d4d:doc/SLSKPROTOCOL.md:315-317`). |
| Licence / people | GPL-3.0-or-later (dub.json, SPDX headers, REUSE). Created by seeschloss (2005); main developer **mathiascode (also the Nicotine+ maintainer; ~600 commits)**, plus slook. Last human commit 2026-10-10. |
| **AI policy** | `CONTRIBUTING.md:13-24` **forbids contributions generated or co-authored by LLMs/AI tools.** Selecta's agents must never open issues/PRs upstream; the maintainer may, in his own words. |
| Toolchain | D (ldc/dmd/gdc) + dub; system SQLite. Two binaries: `soulfind` (server), `soulsetup` (interactive admin TUI). |
| Binaries | Rolling release tag `latest` (2026-10-14) with `soulfind-macos-arm64.zip`, macos-x86_64, linux-{x86_64,arm64}, windows, BSDs. Record a checksum; there are no versioned tags. |
| Container | Multi-arch `ghcr.io/soulfind-dev/soulfind:latest` (`:latest-amd64`, `:latest-arm64`), static musl build `FROM scratch`, `WORKDIR /data` → DB `/data/soulfind.db`. Only `latest` exists → **pin by digest**. `docker run -d -v soulfind-data:/data -p 127.0.0.1:2242:2242 ghcr.io/soulfind-dev/soulfind`. |
| CLI | `soulfind -p/--port <port> -d/--database <file> [--debug]`; `soulsetup -d <file> [-b <backup>]` (`src/server/package.d:24-47`). |
| Defaults (`src/defines.d:14-41`) | port 2242; max users 100 000; login timeout 1 min; kick = 2 min ban; **search fan-out tick 650 ms**; wishlist interval **12 min (2 min privileged)**; max incoming server message 8192 bytes (larger → disconnect); max query length 256; usernames ≤ 30 printable-ASCII chars. Listens on 0.0.0.0, **IPv4 only**, no bind-address option. |
| Accounts | **Unknown users are auto-registered on first login** (PBKDF2-hashed), unless private mode (`src/server/user.d:141-180`). Wrong password → INVALIDPASS. Admin/privileges/bans via `soulsetup` (interactive only) or by PMing the user `server` as an admin (`ban`, `kick`, `announcement`, …). |
| Login checks | **Rejects major or minor version 0** (INVALIDVERSION, `user.d:106-109`); the MD5 hash field is parsed but **not verified**; success reply carries MOTD, client IP, MD5 of the password, supporter flag; then RoomList, WishlistInterval, PrivilegedUsers, **ExcludedSearchPhrases (160)**, queued PMs. Duplicate login → old session gets **Relogged (41)** and is dropped. |
| Search routing | **No distributed network.** FileSearch/WishlistSearch are queued and every 650 ms re-sent as server code 26 `FileSearch(user, token, query)` **directly to every connected user — including the searcher** (`src/server/server.d:75-145, 847-856`); RoomSearch → room members; UserSearch → one user. EmbeddedMessage (93), PossibleParents (102), ParentMinSpeed/Ratio (83/84), ResetDistributed (130) are defined but never sent; client distributed messages (71, 73, 100, 126, 127, 129) are ignored. Open issue #6 (reopened 2026-08 because projects use Soulfind in CI) — maintainer "can't promise soon". |
| Rate limits | **None** (open issue #62). It cannot validate Selecta's governor against server behaviour. |
| Other coverage | GetPeerAddress, ConnectToPeer + CantConnectToPeer relay (incl. obfuscation type/port), watch/status/stats, PMs + acks + MessageUsers, rooms (public/private/operators/tickers), interests/recommendations, privileges, SharedFoldersFiles, SendUploadSpeed, ChangePassword. ServerPing gets no reply (as the official server). |
| Known users | slskd/Soulseek.NET's author load-tested slskd with "as many as 2500 search requests concurrently … pointed at an instance of a soulfind server" (slskd #1306) and proposed a byte-logging flag (Soulfind PR #81 → #84 open); **soulseek-rs runs its whole e2e and stress CI against it** (below). |
| macOS caveat (medium) | Native binary reportedly dies at start-up with macOS's system SQLite ("SQLite error 7"); workaround `DYLD_LIBRARY_PATH=/opt/homebrew/opt/sqlite/lib` (`michel/soulseek-rs@20bd676:README.md:757-761`). The container is unaffected. |
| Loopback quirk (medium) | A client connecting from 127.0.0.1 is advertised under the host's primary LAN IP (found via a UDP `connect()` to 10.255.255.255, `src/server/conns.d:294-313`) → test clients must listen on 0.0.0.0; behaviour with no network interface up is unverified. |

**Precedent to copy — soulseek-rs CI** (`michel/soulseek-rs@20bd676`, MIT): job "End-to-end (soulfind)" builds Soulfind
from source (`dub build :server`, LDC) and runs four e2e suites with `SOULSEEK_E2E_REQUIRED=1`
(`.github/workflows/ci.yml:74-140`); a "Stress (soulfind)" job runs 128 mock peers, 32 searches and 128 downloads around
one real client. The harness (`soulseek-rs-lib/tests/e2e.rs:1-16, 76-125`) uses `SOULSEEK_TEST_SERVER=host:port` if
set, else spawns `soulfind -p <free port> -d <tmp>/soulfind-e2e-<port>.db`, polls TCP connect for ≤ 10 s, retries on a
port race, and **skips** when no binary is found (fails instead when `..._REQUIRED=1`). Its `docs/protocol-coverage.md`
lists which messages are proven e2e against Soulfind vs unit-only (e.g. 93 EmbeddedMessage: "soulfind never sends one").

### 5.2 Other emulators and harnesses

| Harness | Language / licence | What it gives Selecta | Verdict |
|---|---|---|---|
| **aioslsk mock server** `tests/e2e/mock/server.py` (+ `distributed.py`, `model.py`) | Python ≥ 3.11, GPL-3.0-or-later | Standalone CLI (`python -m tests.e2e.mock.server --port 2416 2242`), in-memory users (auto-register, seeded `admin0`/`priv0`), sends ParentMinSpeed/Ratio, **routes searches through the distributed network** (EmbeddedMessage 93 to roots chosen by a pluggable `--distributed-strategy`: `everyone_root`, `realistic_parents`, `chain_parents`, …), PossibleParents at an interval, tracks BranchLevel/Root/ChildDepth; `--mock-search-interval` injects dummy searches; `--wishlist-interval` configurable. | **Second harness, only for distributed-network tests** (what Soulfind cannot do). Fidelity = the aioslsk author's model of the server. Its README suggests a hosts-file entry `127.0.0.1 server.slsknet.org` for clients without a server setting — Selecta has a setting and must not rely on that trick. |
| **Seeker `MockSoulseekClient`** (`Common/MockSoulseekClient.cs`, 3.1k lines) | C#, GPL-3.0-only + Soulseek.NET lineage | In-process fake of the whole client API for a "Debug Mock" build; query terms `n:N`, `t:N` set result count/timing; file-name tokens `speed:N`, `failat:N`, `stall`, `slots:N` script transfers (`DEVELOPMENT.md:28-30`, `MockSoulseekClient.cs:1091-1093, 2163-2172`). | **Idea to copy (not code):** a `FakeNetwork` behind Selecta's core service interfaces for UI work and deterministic UI tests, with the same magic-token approach. |
| nafsi (`siohaza/nafsi`) | Rust, GPL-3.0 | One-commit (2026-03-11) port of Soulfind; SQLite/Postgres; per-connection token-bucket limiter that silently drops excess messages; sends PossibleParents but still broadcasts searches directly. | Optional cross-check only. |
| souls (`fstxz/souls`) | Rust, GPL-3.0 | Self-described "doesn't do anything useful" (login, ping). | No. |
| soulsex (`catuhana/soulsex`) | Elixir, BSD-3-Clause | Login + a few handlers; no search or peer relaying. | No. |
| python-SunSeek (`v72nico`) | Python, GPL-3.0 | Abandoned 2021; predates current login/obfuscation behaviour. | No. |
| Nicotine+ tests | Python | Unit tests with mocked sockets (`pynicotine/tests/unit/protocol/test_slskproto.py`); no server harness. | Message fixtures only. |

No official test server exists (nothing found on slsknet.org or in any client's docs); every project uses Soulfind or
its own mock.

### 5.3 Wireshark dissectors
TODO

## 6. Write our own protocol layer vs reuse/port

| Option | How | For | Against | Verdict |
|---|---|---|---|---|
| **A. Own C++/Qt `slsk` library, logic ported from GPL-3.0+/MIT sources** | QTcpSocket/QTcpServer on one network thread; codec + server/peer/distributed sessions + file streams; governor in-process | Full control of pacing (SAFE-1/2) at the socket boundary; one event loop; fuzzable codec (NFR-4); only zlib as dependency; no runtime to ship; keeps GPL-3.0-or-later open (OQ-7); matches PLAN M2 | Effort. Size guide: Nicotine+'s network thread + message catalogue ≈ 7.4k Python lines, aioslsk ≈ 15k, soulseek-rs-lib ≈ 23k Rust lines with tests — expect roughly 10–20k C++ lines for the full library with tests (low confidence) | **Recommended (ADR-002).** |
| B. Embed Nicotine+ core (Python) | Frozen Python sidecar + IPC, as *Seek* does (Tauri + `sidecar/` running Nicotine+ headless, pinned to a 3.4.0.dev commit — `IvaKap/seek@1317c84:RECON.md`) | Battle-tested behaviour, GPL-compatible, fastest to "works" | Ships a Python runtime; two processes and IPC on the hot path (search results, progress); governor split across languages; tracks a moving upstream; harder to sign/notarise and fuzz | Fallback only if M2 slips badly. |
| C. slskd as backend daemon | Selecta = GUI over slskd's REST API | Mature uploads/limits | AGPL-3.0 + Additional Terms; .NET runtime; daemon mode is a v1 non-goal; Selecta would log in **as slskd** (minor 760) — conflicts with NET-9; pacing not under Selecta's control; duplicated state | Rejected. |
| D. Link soulseek-rs-lib via FFI | Rust staticlib behind a C ABI | MIT, zero deps, e2e-tested on Soulfind | Rust toolchain on 3 CI OSes; bridging its actor threads to Qt; one maintainer, fast version churn; opaque to the governor; identity 176 belongs to soulseek-rs | Not now; keep as behavioural cross-check. |
| E. soulseekcpp | Vendor the C++20/asio port | Already C++ | Soulseek.NET licence terms; one commit, 790 lines of tests; second event loop (asio) | Rejected. |
| F. Port seeleseek's Swift core | Swift/C++ interop | macOS-native, MIT | Swift on Linux/Windows CI; poor fit with Qt | Rejected (read for macOS ideas). |

Rules for option A: implement from `doc/SLSKPROTOCOL.md` + aioslsk docs; consult Nicotine+/aioslsk/soulseek-rs code for
behaviour; record any ported fragment (file + upstream path + commit + licence) in a `THIRD-PARTY-NOTICES.md`; read
Soulseek.NET/slskd/sockseek/soulseekcpp **for behaviour only, never copy or translate**; prove interop with Soulfind +
Nicotine+ + slskd peers (§9) before any live use.

## 7. Per-subsystem reading list

Commits: Nicotine+ `f3b9d4d`, aioslsk `a90ad82`, soulseek-rs `20bd676`, Soulseek.NET `c07c187`, slskd `6a450e4`, sockseek
`ff9a3ac`, soularr `2037399`, Soulfind `5a256f5`, seeleseek `195eddc`, Seeker `ff1dcc6`. ✱ = behaviour-only source
(licence) — read, don't copy.

| Subsystem (PLAN module) | Read first | Then |
|---|---|---|
| Wire format, message catalogue (`slsk` codec) | Nicotine+ `doc/SLSKPROTOCOL.md` (packing, constants, every message); `pynicotine/slskmessages.py` (each message's pack/parse + docstrings; rejection strings `:105-117`, attribute codes `:120-126`) | aioslsk `protocol/messages.py`, `protocol/primitives.py` (declarative fields); soulseek-rs `soulseek-rs-lib/src/message/` (`server/message_factory.rs`). zlib: use the real zlib (soulseek-rs hand-rolls inflate in `utils/zlib.rs` to stay dependency-free; Qt's `qUncompress` expects a 4-byte length prefix — verify before using it). |
| Server session: login, version, keep-alive, reconnect, Relogged | Nicotine+ `slskproto.py:1255-1347` (`_server_connect`, Login 160/3), `:1591` (`_server_disconnect`); Soulfind `src/server/user.d:104-207, 337-385` (what a server checks and sends after login) | aioslsk `network/network.py:382-500` (server connection watchdog); ✱ sockseek `SoulseekClientManager.cs:236-280` (reconnect loop — too aggressive, counter-example). |
| Peer connections: direct, indirect (ConnectToPeer/PierceFirewall), timeouts, obfuscation | aioslsk `docs/source/SOULSEEK.rst:47-260` (direct, indirect, obfuscation with example); Nicotine+ `slskproto.py:593-635` (indirect timeouts), `:838-1000` (`_initiate_connection_to_peer`, `_connect_to_peer_indirect`), `:1850` (`_init_peer_connection`) | aioslsk `network/network.py:530+` (`create_peer_connection`); ✱ Soulseek.NET `Network/PeerConnectionManager.cs` (~200-250: **races direct vs indirect, first wins**). |
| Distributed network (to receive searches as a sharer) | aioslsk `DESIGN.rst` "Distributed Network" + `SOULSEEK.rst:284-416` + `distributed.py` | Nicotine+ `slskproto.py:2236-2290` (children); soulseek-rs `client/distributed.rs`, `client/children.rs`; ✱ Soulseek.NET `Network/DistributedConnectionManager.cs`. Test with the aioslsk mock server (§5.2). |
| Outgoing search + result intake (SearchService) | ✱ Soulseek.NET `Options/SearchOptions.cs` (completion = 15 s after *last* response; 250 responses / 25 000 files caps; 2 concurrent); Nicotine+ `search.py` (tokens, wishlist one-per-interval `:529-557`, excluded phrases) | aioslsk `search/manager.py`; ✱ sockseek `Search/Searcher.cs` (query cleaning `:661-696`, fallbacks `:554-637`). |
| Governor / pacing | Selecta's own design (etiquette note); ✱ sockseek `Search/RateLimitedSemaphore.cs` (**anti-pattern**: fixed window bursts 2×); soularr `soularr.py:754-773` (min interval between search starts) | ✱ Soulseek.NET `Common/TokenBucket.cs`, aioslsk `network/rate_limiter.py` (speed buckets). |
| Request/response correlation | ✱ Soulseek.NET `Common/Waiter.cs`, `WaitKey.cs` ("await-able server messages": wait on (code, user, token) with timeout; oldest wait completes first) | Nicotine+ token maps in `search.py`/`transfers.py`. |
| Answering searches, share index (ShareService) | aioslsk `SOULSEEK.rst:2525-2595` (**query rules**: exclusions, prefix-only wildcard, boundary matching, directory-only path terms; attribute table); Nicotine+ `shares.py` (`Scanner` `:241`, `Database` `:101`) + `search.py:757-911` (word index lookup, `maxresults` 300) | aioslsk `shares/manager.py`; slskd `docs/config.md:999-1080` (incoming filters, circuit breaker); soulseek-rs `shares/mod.rs`, `shares/audio.rs` (cheap attribute extraction). |
| Browse / folder contents | Nicotine+ `userbrowse.py` | soulseek-rs coverage table (codes 4/5, 36/37). |
| Downloads: queue → TransferRequest/Response → F connection → FileOffset → resume; place in queue; UploadFailed/Denied | aioslsk `SOULSEEK.rst:417-517` + `transfer/state.py` (**explicit state machine**) + `transfer/manager.py` | Nicotine+ `downloads.py`, `transfers.py`; soulseek-rs `peer/download_peer.rs`, `download_store.rs` (`.part` resume). |
| Uploads: slots, queues, fairness, limits | slskd `docs/config.md:325-605` (groups, priorities, round-robin, queued/daily/weekly limits) | Nicotine+ `uploads.py` + `tests/unit/transfers/test_get_upload_candidate.py`; soulseek-rs `client/upload_queue.rs`. |
| Port mapping (NET-3) | Nicotine+ `portmapper.py` (UPnP + NAT-PMP, pure Python) | aioslsk `network/upnp.py`; seeleseek `NATService`. |
| Test harness | soulseek-rs `soulseek-rs-lib/tests/e2e.rs:1-125` + `.github/workflows/ci.yml:74-181` | aioslsk `tests/e2e/mock/server.py`, `tests/e2e/fixtures.py`; Nicotine+ `tests/unit/protocol/`; ✱ Seeker `Common/MockSoulseekClient.cs` (fake network idea). |
| Search filter syntax (power users) | SoulseekQt "Fun with Filters" (slsknet.org node/267) | slskd `src/web/src/lib/searches.js` + `searches.test.js`; seeleseek `site/src/content/docs/guide/searching.md` (presets). |
| Batch matching and ranking | ✱ sockseek `Search/SearchProjection/ResultSorter.cs:213-542`, `Matching/Conditions/FileConditions.cs`, `Files/TrackSkipper.cs`, `Searcher.cs:356-470` (filename inference) | soularr `soularr.py:91-166, 265-310` (album match, exact quality specs). |

## 8. GUI client UX comparison for a DJ workflow

✓ = yes, ~ = partial, ✗ = no, ? = not verified. Sources: §2–§4, project docs/READMEs at the cited commits.

| Need (PRD) | SoulseekQt | Nicotine+ | slskd (web) | seeleseek | Seek | sockseek (CLI) | Selecta target |
|---|---|---|---|---|---|---|---|
| Native macOS look (UI-1) | ~ (Qt, own look) | ✗ (GTK) | ✗ (browser) | ✓ (SwiftUI) | ~ (Tauri web view) | n/a | ✓ (Qt Widgets, native style) |
| Hard format/quality filters (SRCH-2…4) | ~ (text: `iscbr`, `minbitrate`; no lossless rules) | ~ (filter bar: in/out words, type, size, bitrate, length, country) | ~ (text: + `islossless`, `minbitdepth`, `minlength`) | ✓ (format, bitrate ≥, sample rate ≥, bit depth ≥, free slot) | ? | ✓ (necessary vs preferred conditions) | ✓ incl. `=`/≤ operators, CBR/VBR classes |
| Named presets (SRCH-8) | ✓ saved filters | ~ (filter history) | ✗ | ✓ quick presets (MP3 320, FLAC, Lossless, Hi-Res) | ? | ✓ (config profiles, `--profile`) | ✓ |
| Unknown-quality policy (SRCH-6) | ✗ (MP3-only attrs hide others) | ✗ | ~ (`islossless` drops files without SR/BD) | ? | ? | ✓ `--strict-conditions` | ✓ strict mode + "unknown" marker |
| Plausibility / fake detection (SRCH-7, PREV-9) | ✗ | ✗ | ✗ | ✗ | ✓ spectrogram + confidence | ✗ | ✓ |
| Folder (release) grouping, whole-folder download | ✓ "Download containing folder" | ✓ | ✓ (by user) | ✓ | ✓ (record-centric) | ✓ album mode | ✓ |
| Audition before download (PREV-1…4) | ✗ | ✗ | ✗ | ~ (plays *downloaded* files, `RowAudioPreview.swift`) | ~ (20 s excerpt of an already-downloaded file, `sidecar/seek_sidecar/core_host.py:1026-1053`) | ✗ | ✓ progressive, keyboard-driven |
| Waveform | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ 3-band |
| Owned / Inbox / history badges (SRCH-11) | ✗ | ✗ | ✗ | ? | ✓ ("not in my library") | ~ (skip-existing index) | ✓ |
| Batch from tracklist (BATCH) | ✗ | ✗ (plugins) | ✗ (API tools) | ✗ | ~ (links/catalogues) | ✓ (core feature) | ✓ review-first |
| Visible search budget (SRCH-13) | ✗ | ✗ | ✗ | ✗ | ? | ~ ("Waiting" status) | ✓ |
| Per-peer counters/caps (FAIR-1/2) | ✗ | ✗ | ~ (upload-side limits only) | ? | ✓ ("your own history with them") | ~ (per-run failure downrank) | ✓ |
| Ratio (FAIR-3) | ✗ | ~ (transfer stats) | ✗ | ~ (Statistics view) | ✓ | ✗ | ✓ |
| Convert + tag on filing (CONV) | ✗ | ✗ | ✗ | ✗ | ~ ("Fix tags", "Organise") | ✗ | ✓ |
| Wishlist / browse / PMs | ✓ / ✓ / ✓ | ✓ / ✓ / ✓ | ? / ✓ / ✓ | ✓ / ✓ / ✓ | ? / ? / ? | ✓ (wishlist mode) / ✗ / ✗ | ✓ / ✓ / ✓ |

Takeaways: (1) nobody combines **audition + strict quality + ownership + pacing**; Selecta's niche is real.
(2) **Seek** is the closest concept (diggers, ownership, spectrogram proof, ratio honesty, per-peer history) — its
README is worth a careful read for copy and interaction ideas; it is Tauri + Python, not native. (3) **seeleseek**'s
quick presets ("MP3 320 / FLAC / Lossless / Hi-Res") validate Selecta's preset approach. (4) Every GUI client hides why
results disappear; Selecta's "N of M shown" breakdown (SRCH-9) is a differentiator. (5) Seek's README claims
"Soulseek allows one search at a time" — not a protocol rule (Nicotine+ runs several; Soulseek.NET allows 2
concurrently by default); treat as that app's own policy.

## 9. Standing up a local offline Soulseek test network

Nothing below has been run yet (research only); treat commands as a starting point for `devenv/` in M1 and verify each
step offline. Reference peers' config keys were read from source (`nicotine-plus@f3b9d4d:pynicotine/config.py:159-183`,
`pynicotine/__init__.py:42-84`; `slskd@6a450e4:docs/config.md:146-1441`, `config/slskd.example.yml:210-222`).

**Key networking fact.** Soulfind tells each client the source IP it sees for every other client. So every participant
must be reachable at the address the server sees. Two topologies work; a hybrid (Selecta native on macOS, peers inside
Docker Desktop's VM) most likely does **not** (peers would get unroutable 172.x addresses or the Docker gateway) —
medium-high confidence, reasoned from `soulfind/src/server/conns.d` and Docker Desktop networking, not tested.

### 9.1 Topology A — everything native on the Mac (daily dev loop, Selecta under the debugger)

1. **Server.** Download `soulfind-macos-arm64.zip` from the Soulfind `latest` release; record its SHA-256 in `devenv/`.
   `brew install sqlite`, then
   `DYLD_LIBRARY_PATH=/opt/homebrew/opt/sqlite/lib ./soulfind -p 2242 -d devenv/data/soulfind.db --debug`.
   It binds 0.0.0.0 (LAN-visible, no rate limits): keep the macOS firewall on, or accept LAN exposure on a trusted network.
2. **Accounts.** None to create — users auto-register on first login. Optional: `soulsetup -d devenv/data/soulfind.db`
   to make an admin (after its first login), grant privileges (privileged-queue tests), and add a *client phrase*
   (sent as ExcludedSearchPhrases) and a *server phrase* (query silently dropped) to test the "why zero results?" UI.
3. **Peer 1 — Nicotine+ headless** (`pip install nicotine-plus` in a venv). **Write its config before the first launch**
   (default server is the live one!):
   ```ini
   [server]
   server = ('127.0.0.1', 2242)
   login = 'np-peer1'
   passw = 'test'
   portrange = (2240, 2240)
   upnp = False
   auto_connect_startup = True
   [transfers]
   shared = [('Synthetic', '/abs/path/devenv/data/shares/peer1')]
   ```
   Run `nicotine --headless --config devenv/data/np1/config --user-data devenv/data/np1 --rescan`. (Value syntax =
   Python literals; confirm by letting Nicotine+ write a default config with networking off.)
4. **Peer 2 — slskd** (native osx-arm64 release):
   `SLSKD_SLSK_ADDRESS=127.0.0.1 SLSKD_SLSK_PORT=2242 SLSKD_SLSK_USERNAME=slskd-peer2 SLSKD_SLSK_PASSWORD=test
   SLSKD_SLSK_LISTEN_PORT=50300 SLSKD_SHARED_DIR=/abs/path/devenv/data/shares/peer2 SLSKD_NO_AUTH=true
   SLSKD_NO_VERSION_CHECK=true SLSKD_HTTP_PORT=5030 ./slskd`. Its web UI/REST API on 127.0.0.1:5030 drives peer-side
   searches, downloads from Selecta's shares, and upload-queue scenarios. (Default would be `vps.slsknet.org:2271`.)
5. **Selecta** (debug build): server 127.0.0.1:2242 (the SAFE-3 allow-list accepts loopback/private only), its own test
   username, listen port e.g. 2250 on 0.0.0.0, login version = Selecta's dev identifier (major 177 = "experimental" in
   the Nicotine+ registry, non-zero minor — Soulfind rejects 0).
6. **Synthetic shares**: the ffmpeg-generated corpus from PLAN §5.3 (tones/noise/chirps in every format and bitrate,
   tagged, with folder `cover.jpg`), split differently per peer to create overlaps, singles vs albums and "upgrade"
   cases. Never real music.
7. **Belt and braces** (optional, automated runs only): a temporary `/etc/hosts` sinkhole
   `127.0.0.1 server.slsknet.org vps.slsknet.org` so a misconfigured reference peer cannot reach the live network;
   remove it before the maintainer's manual live sessions.

### 9.2 Topology B — everything in Docker (CI and fully isolated runs)

```yaml
# devenv/compose.yml — sketch, unverified
networks:
  slsk: { internal: true }        # no internet route, no published ports
services:
  soulfind:
    image: ghcr.io/soulfind-dev/soulfind@sha256:<pin>    # multi-arch; only tag is :latest
    command: ["soulfind", "--debug"]                     # :2242, DB /data/soulfind.db
    volumes: ["soulfind-data:/data"]
    networks: [slsk]
  peer-slskd:
    image: slskd/slskd:<pinned version>
    environment:
      SLSKD_SLSK_ADDRESS: soulfind
      SLSKD_SLSK_PORT: "2242"
      SLSKD_SLSK_USERNAME: slskd-peer
      SLSKD_SLSK_PASSWORD: test
      SLSKD_SLSK_LISTEN_PORT: "50300"
      SLSKD_SHARED_DIR: /shares
      SLSKD_NO_AUTH: "true"
      SLSKD_NO_VERSION_CHECK: "true"
    volumes: ["./data/shares/slskd:/shares:ro"]
    networks: [slsk]
  peer-nicotine:
    build: ./nicotine          # python:3.12-slim + pinned `pip install nicotine-plus==<ver>` at build time
    command: ["nicotine", "--headless", "--config", "/cfg/config", "--user-data", "/data"]
    volumes: ["./nicotine/config:/cfg/config:ro", "./data/shares/np:/shares:ro"]
    networks: [slsk]
  selecta-cli:
    build: { context: .., dockerfile: devenv/Dockerfile.selecta-linux }   # headless selecta-cli + tests
    depends_on: [soulfind, peer-slskd, peer-nicotine]
    networks: [slsk]
volumes: { soulfind-data: {} }
```

- Server address = container DNS name (`soulfind`); containers get distinct IPs, so direct *and* indirect
  (ConnectToPeer/PierceFirewall) connections can both be exercised (block a peer's inbound port with a second network or
  `iptables` in the container to force the indirect path).
- Pull/build all images **before** starting (no registry access from an internal network); pin by digest/version.
- To watch slskd's UI from the Mac, attach only `peer-slskd` to a second, non-internal network and publish
  `127.0.0.1:5030` (that container then has an internet route — acceptable only because its server is pinned).

### 9.3 Per-test isolation (CTest / Qt Test)

Copy soulseek-rs's harness: an `E2E` CTest label whose fixture spawns `$SELECTA_SOULFIND_BIN -p <free port> -d <tmp>.db`
per test (fresh DB = fresh users), waits for TCP accept (≤ 10 s), retries on a port race, kills the process at teardown;
**skips** when the binary is absent locally, **fails** in CI (`SELECTA_E2E_REQUIRED=1`). Clients inside the test are
in-process `slsk::Client` instances (Selecta's own library) plus, for interop tests, a reference peer started from
Topology B images. Inject the governor's fake clock so pacing tests never sleep.

### 9.4 What the local network cannot test (needs other means)

| Gap | Mitigation |
|---|---|
| Distributed network (parents/children, EmbeddedMessage 93, BranchLevel/Root) | aioslsk mock server with `--distributed-strategy realistic_parents`; unit tests from Nicotine+/aioslsk message fixtures. Run slskd peers with `SLSKD_SLSK_NO_DNET=true` to reduce noise. |
| Server-side rate limits and bans | None in Soulfind (#62). Governor proven by unit/property tests with a fake clock; live behaviour only in the maintainer's M9 sessions. |
| Login MD5 validation | Not checked by Soulfind; unit-test it (the official server reportedly excludes users with a bad hash from the distributed network — Soulfind #6, hearsay). |
| Unicode usernames, IPv6 | Soulfind rejects non-ASCII usernames and is IPv4-only; behaviour of the official server may differ. |
| SoulseekQt interop | SoulseekQt has no server setting (Soulfind #50 reporter patched the binary; aioslsk suggests a hosts-file redirect). Optional manual check on a separate machine/VM with a hosts redirect — never automated. |
| Obfuscated connections | Soulfind relays obfuscation fields; whether reference peers use them is unverified — test Selecta's codec with fixtures. |
| Self-echo | Soulfind sends your own searches back to you → Selecta must ignore FileSearch requests from its own username (free robustness test). |

## 10. Implications for Selecta

TODO

## 11. Open questions and things to verify on a local test server

TODO

## 12. Sources

TODO (all accessed 2026-10-19).
