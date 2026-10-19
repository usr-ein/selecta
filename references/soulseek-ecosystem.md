# Soulseek ecosystem: libraries, clients, servers and tools

> Research note for Selecta (GPLv3 C++/Qt 6 Soulseek client). Status: **DRAFT — in progress** (2026-10-19).
> Items marked **TODO(verify)** are from prior knowledge and not yet checked against a primary source.

## TL;DR

- **No reusable, maintained C++ Soulseek library exists.** The only C++ implementation is museek+ (2000s, unmaintained,
  GPL). Selecta should **write its own protocol layer in C++/Qt**, porting logic (not code structure) from the three
  well-documented, actively maintained GPL implementations: **Nicotine+** (Python, the de-facto protocol reference),
  **Soulseek.NET** (C#, powers slskd / slsk-batchdl / Seeker) and **aioslsk** (Python asyncio, best-documented library).
  All are GPL-3.0(-or-later)-family, so reading and porting is licence-compatible with a GPLv3 Selecta. TODO(verify licences).
- **Soulfind** (open-source Soulseek server, maintained alongside Nicotine+) is the candidate local test server.
  TODO(verify): build/run, Docker, which messages it supports, how it routes searches (no distributed network?).
- **slsk-batchdl (sldl)** is the closest prior art to Selecta's batch search; its search limiter defaults
  (34 searches / 220 s — TODO(verify) and origin) and its filter/ranking options are documented below.
- **SoulseekQt** (official, closed source, Qt) is the UX baseline; Nicotine+ and slskd are the open-source baselines.

## Key findings

TODO — filled in as sections are verified.

## 1. Comparison table

| Project | Kind | Language | Licence (SPDX) | Activity | Protocol coverage | Reuse for Selecta |
|---|---|---|---|---|---|---|
| SoulseekQt | Official client | C++/Qt (closed) | Proprietary freeware | TODO | Full (reference behaviour) | UX reference only |
| Nicotine+ | Client (GTK) | Python 3 | GPL-3.0-or-later TODO(verify) | Very active TODO | Full incl. distributed network, uploads | Protocol reference + port logic |
| Soulseek.NET | Library | C# | GPL-3.0 TODO(verify) | Active TODO | Full client side | Port logic (state machines, transfers) |
| slskd | Daemon + web UI | C# + React | AGPL-3.0 TODO(verify) | Active TODO | via Soulseek.NET | Design ideas (limits, groups, API) |
| aioslsk | Library | Python asyncio | GPL-3.0 TODO(verify) | Active TODO | Full TODO | Port logic; docs; mock server for tests? |
| museek+ / museekd | Daemon + Qt client | C++ | GPL-2.0-or-later TODO(verify) | Dead | Old protocol subset | Historical only |
| Seeker | Android client | C# (Xamarin/.NET) | GPL-3.0 TODO(verify) | TODO | via Soulseek.NET fork | Mobile UX ideas |
| slsk-batchdl (sldl) | Batch CLI | C# | GPL-3.0 TODO(verify) | Active TODO | via Soulseek.NET | Batch/matching/rate-limit prior art |
| soularr | Lidarr↔slskd bridge | Python | TODO | TODO | via slskd API | Matching/selection ideas |
| Soulfind | Server | D | GPL-3.0-or-later TODO(verify) | Active TODO | Server side, subset | **Local test server** |
| Rust / Go / JS libs | Libraries | various | TODO | TODO | TODO | TODO |
| Wireshark dissector | Tool | C / Lua | GPL-2.0-or-later TODO | TODO | TODO | Debugging local captures |

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

### 3.5 museek+ / museekd
TODO

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

### 5.2 Other emulators and harnesses
TODO

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
