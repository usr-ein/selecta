# Soulseek ecosystem: libraries, clients, servers and tools

> Research note for Selecta (GPLv3 C++/Qt 6 Soulseek client). Status: **DRAFT — in progress** (2026-10-19).
> Items marked **TODO(verify)** are from prior knowledge and not yet checked against a primary source.
> Repo facts (licence, last push, latest release, stars) come from the GitHub API on 2026-10-19; source citations are
> `owner/repo@<short-commit>:path:line` against shallow clones taken the same day.

## TL;DR

- **No reusable, maintained C++ Soulseek library exists.** The only substantial C++ code is museek+/newsoul
  (2000s–2014, unmaintained, protocol long out of date). Selecta should **write its own protocol layer in C++/Qt**.
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

TODO

## 3. Open-source clients and libraries

### 3.1 Nicotine+
TODO

### 3.2 Soulseek.NET
TODO

### 3.3 slskd
TODO

### 3.4 aioslsk
TODO

### 3.5 museek+ / museekd / newsoul
museek+ README: "No longer maintained!" and points users to slskd, Nicotine+ and Seeker. Architecture: `museekd` daemon
+ `museeq` (Qt GUI) / `mucous` (curses) / `murmur` (GTK) talking to the daemon over an "interface" protocol; `muscan`
share scanner. newsoul (2013–2014) was a C++11 resurrection of the daemon only (libevent, json-c, taglib, sqlite3).
Both predate obfuscation, modern login versions and current server behaviour. Value for Selecta: historical, plus the
daemon/UI split idea. TODO: any still-useful pieces (share DB format?).

### 3.6 Seeker
TODO

### 3.7 Rust, Go, JavaScript and other libraries
TODO

## 4. Automation tools

### 4.1 slsk-batchdl (sldl)
TODO: search rate-limiter defaults and their origin; filtering, ranking and matching options.

### 4.2 soularr and other *arr bridges
TODO

## 5. Servers, emulators and test harnesses

### 5.1 Soulfind
TODO: can it be a local test server? how to run, Docker, limitations.

Already verified: `michel/soulseek-rs@20bd676` runs every e2e suite in CI against Soulfind built from source with
`dub build :server` (LDC) (`.github/workflows/ci.yml:74-140`), and its test harness spawns
`soulfind -p <free port> -d <tmp>/soulfind-e2e-<port>.db` per test, waits for the port, retries on a new port if the
child dies, and **skips** (passes) when no binary is available (`soulseek-rs-lib/tests/e2e.rs:1-16, 76-125`).
`docs/protocol-coverage.md` there records per-message which behaviours Soulfind exercises (e.g. 93 EmbeddedMessage:
"soulfind never sends one"; 102 PossibleParents / 71 HaveNoParent: e2e "a leaf adopting a parent").

### 5.2 Other emulators and harnesses
- Seeker `Common/MockSoulseekClient.cs` (3.1k lines, GPL-3.0-only): in-process fake of the whole client API, used by a
  "Debug Mock" build; search terms `n:N`/`t:N` set result count/timing, file-name tokens `speed:N`, `failat:N`, `stall`,
  `slots:N` script transfer behaviour (`jackBonadies/SeekerAndroid@ff1dcc6:DEVELOPMENT.md:28-30`,
  `MockSoulseekClient.cs:1091-1093, 2163-2172`). Idea only (licence).
- TODO: aioslsk mock server, nafsi, souls, soulsex, SunSeek.

### 5.3 Wireshark dissectors
TODO

## 6. Write our own protocol layer vs reuse/port

TODO (draft position: write our own; see TL;DR).

## 7. Per-subsystem reading list

TODO

## 8. GUI client UX comparison for a DJ workflow

TODO

## 9. Standing up a local offline Soulseek test network

TODO

## 10. Implications for Selecta

TODO

## 11. Open questions and things to verify on a local test server

TODO

## 12. Sources

TODO (all accessed 2026-10-19).
