# Soulseek protocol — implementation reference for Selecta

> Status: **DRAFT in progress** (skeleton written from prior knowledge; items marked **TODO** are not yet verified
> against source code). Accessed/compiled 2026-10-19.

## TL;DR

- Soulseek is a **central-server + direct-peer** network. One TCP connection to the server (login, presence, search
  dispatch, address lookup, NAT traversal brokering, private messages). Everything else (search results, browsing,
  transfers) is **peer-to-peer over TCP**, opened directly or "indirectly" via a server-brokered reverse connection.
- **Searches are not answered by the server.** They propagate through a **distributed tree of clients** (the
  "distributed network"); each sharer matches locally and connects back to the searcher to deliver results.
- Framing is trivial: `uint32 length (LE)` + code + payload. Server/peer codes are `uint32`; peer-init and distributed
  codes are `uint8`. Strings are `uint32 length + bytes` (UTF-8 in practice, Latin-1 fallback on decode errors).
  Share lists, search results and folder contents are **zlib-compressed**.
- Downloads are **queue-based**: downloader sends `QueueUpload(filename)`; the uploader later sends
  `TransferRequest(upload)`; downloader answers `TransferResponse(allowed)`; the uploader opens an `F` connection and the
  raw bytes flow after `FileTransferInit(token)` / `FileOffset(offset)`.
- MVP subset for Selecta is roughly 25 server messages, ~12 peer messages, 4 distributed messages and the file-connection
  handshake. Chat rooms, interests/recommendations, tickers and private rooms can be ignored (decode-and-drop).

## Key findings

TODO — filled after source verification.

## Contents

1. Architecture overview
2. Wire format
3. Connections (P/F/D, PeerInit vs PierceFireWall, direct vs indirect, tokens, timeouts, NAT, obfuscation)
4. Server messages (full table)
5. Peer-init messages
6. Peer messages
7. File connections and transfers
8. Distributed network
9. Sequence diagrams
10. Search semantics
11. Keep-alives, timeouts and reconnect
12. Implications for Selecta (MVP subset, implementation order, defaults, decisions)
13. Open questions / verify on a local test server
14. Sources

---

## 1. Architecture overview

| Component | Transport | Purpose |
|---|---|---|
| Server (`server.slsknet.org`, port 2242 — **never contact from dev/test**) | 1 long-lived TCP connection | Login, status, user lookup, ConnectToPeer brokering, PMs, chat rooms, distributed-network control, search injection |
| Peer connection, type `P` | TCP, framed messages | Search results, browse (share list), folder contents, user info, transfer negotiation, queue position |
| File connection, type `F` | TCP, unframed after a tiny handshake | One file transfer (raw bytes) |
| Distributed connection, type `D` | TCP, framed (uint8 codes) | Search propagation tree (parent → children) |

TODO: verify port(s), describe server-side roles.

## 2. Wire format

TODO: verify all against Nicotine+ `slskmessages.py`.

| Type | Encoding |
|---|---|
| `uint8` | 1 byte |
| `uint16` | 2 bytes, little-endian |
| `uint32` | 4 bytes, little-endian |
| `int32` | 4 bytes, little-endian, signed |
| `uint64` | 8 bytes, little-endian |
| `bool` | 1 byte, 0/1 |
| `string` | `uint32` byte length + bytes (no terminator) |
| `bytes` | `uint32` length + raw bytes |
| `list<T>` | `uint32` count + items |

Message envelopes:

| Channel | Envelope |
|---|---|
| Server | `uint32 len` · `uint32 code` · payload (`len` counts code + payload) |
| Peer-init (first message on any peer socket) | `uint32 len` · `uint8 code` · payload |
| Peer (`P`) | `uint32 len` · `uint32 code` · payload |
| Distributed (`D`) | `uint32 len` · `uint8 code` · payload |
| File (`F`) | no envelope: `uint32 token` (FileTransferInit), `uint64 offset` (FileOffset), then raw bytes |

## 3. Connections

TODO.

## 4. Server messages

TODO.

## 5. Peer-init messages

TODO.

## 6. Peer messages

TODO.

## 7. File connections and transfers

TODO.

## 8. Distributed network

TODO.

## 9. Sequence diagrams

TODO.

## 10. Search semantics

TODO.

## 11. Keep-alives, timeouts and reconnect

TODO.

## 12. Implications for Selecta

TODO.

## 13. Open questions / verify on a local test server

TODO.

## 14. Sources

TODO (accessed 2026-10-19).
