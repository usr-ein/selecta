# Soulseek protocol — implementation reference for Selecta

> Status: **DRAFT in progress.** Sections 1–8 are written from the Nicotine+ protocol document and spot-checked
> against Nicotine+ code; items marked **TODO** are not yet verified. Compiled 2026-10-19.
> Primary source: Nicotine+ `doc/SLSKPROTOCOL.md` ("Last updated on August 27, 2026", repo commit `f3b9d4d`),
> cited below as **[N+doc]**; Nicotine+ code `pynicotine/*.py` at the same commit as **[N+]**.

## TL;DR

- Soulseek is a **central server + direct peer** network. One long-lived TCP connection to the server (login,
  presence, address lookup, NAT-traversal brokering, private messages, search injection). Everything else (search
  results, browsing, transfers) is **peer-to-peer over TCP**, opened directly or "indirectly" (the server asks the
  other peer to connect back to us).
- **The server does not answer searches.** A query is injected into a **distributed tree of clients**; every sharer
  matches it against its own files and, on a match, **connects to the searcher** and sends the results.
- Framing is trivial: `uint32 length (LE)` + code + payload. Server and peer codes are `uint32`; peer-init and
  distributed codes are `uint8`. Strings are `uint32 length + bytes` (UTF-8; decode falls back to Latin-1). Share
  lists, search results and folder contents are **zlib** streams. IPv4 only.
- Downloads are **queue-based**: downloader sends `QueueUpload(path)`; when a slot frees up the uploader sends
  `TransferRequest(direction=upload, token, path, size)`; the downloader answers `TransferResponse(token, allowed)`;
  the uploader opens an `F` connection, sends `token`, the downloader sends a `uint64` resume offset, raw bytes follow.
  **The downloader closes the `F` socket** when it has everything.
- **Client identification is a registry**: major version `157` = official, `160` = Nicotine+, `170` = Soulseek.NET,
  `175` = aioslsk, `176` = soulseek-rs, `169` = seeleseek; **`177` = experimental development**. Selecta must not
  reuse another client's number (see §4.2, §12).
- MVP subset: ~30 server messages, 13 peer messages, 4 distributed messages and the file handshake. Chat rooms,
  interests, tickers and private rooms can be decoded-and-dropped (framing makes skipping trivial).

## Key findings

TODO — filled after source verification (Soulseek.NET, aioslsk, Soulfind, Nicotine+ code).

## Contents

1. Architecture overview
2. Wire format
3. Connections (P/F/D, PeerInit vs PierceFireWall, direct vs indirect, tokens, timeouts, NAT, obfuscation)
4. Server messages (full table, login, MVP notes)
5. Peer-init messages
6. Peer messages (share lists, search results, user info, transfers)
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
| Server (official: `server.slsknet.org:2242` — **never contacted from dev/test/CI**; Soulfind locally) | 1 long-lived TCP connection per client | Login, status/stats, user address directory, `ConnectToPeer` brokering, private messages (stored until acked), chat rooms, interests, privileges, distributed-network management, search injection, parameters (wishlist interval, excluded phrases, parent speed rules) |
| Peer connection, type `P` | TCP, framed, `uint32` codes | Search results, browse (share list), folder contents, user info, transfer negotiation, queue position. **One active `P` connection per peer** [N+doc §Peer Messages]. |
| File connection, type `F` | TCP; 4-byte token, 8-byte offset, then raw bytes | Exactly one file transfer per connection |
| Distributed connection, type `D` | TCP, framed, `uint8` codes | Search propagation tree (parent → children). One active `D` connection per peer. |

Facts that shape the design:

- **The listening port matters.** A client that cannot accept incoming connections can still talk to *reachable*
  peers: it dials them directly, and when they want to reach it they ask the server (`ConnectToPeer`) and it dials
  back with `PierceFireWall`. Two unreachable peers cannot talk at all. Search results are delivered by the
  *responder* connecting to the searcher, so an unreachable searcher only gets results from reachable responders.
  See §3.
- **IPv4 only**: every address field is a `uint32`. There is no IPv6 in the protocol.
- **Server-side "username" namespace**: the pseudo-user `server` is used for server notices sent as private
  messages; Nicotine+ rejects peers claiming that name ([N+] `slskproto.py:354, 1718-1722`).
- The official server and client are proprietary; everything here is reverse-engineered [N+doc intro]. The N+ doc
  explicitly asks implementers not to extend the protocol without the Soulseek administrators' approval, and to
  prefer existing implementations because subtle bugs can harm the network.

## 2. Wire format

### 2.1 Primitive types [N+doc §Packing]

| Type | Encoding |
|---|---|
| `uint8` | 1 byte |
| `uint16` | 2 bytes, little-endian (only used for the obfuscated port in `GetPeerAddress`) |
| `uint32` | 4 bytes, little-endian |
| `int32` | 4 bytes, little-endian, signed (`SetStatus` status, `DistribBranchLevel`) |
| `uint64` | 8 bytes, little-endian (file sizes, file offset) |
| `bool` | 1 byte, 0 or 1 (decode: non-zero = true) |
| `string` | `uint32` byte length + bytes, no terminator |
| `bytes` | `uint32` length + raw bytes (user picture). **Exception:** in `EmbeddedMessage` the "bytes" field is the *rest of the message with no length prefix* ([N+] `slskmessages.py:2127-2129` reads `uint8 code` then `message[offset:]`). |
| `ip` | `uint32` LE whose value is the IPv4 address in host order: wire bytes `04 03 02 01` = `1.2.3.4`. In Qt: `QHostAddress(quint32(readU32LE()))` ([N+] `slskmessages.py:361-364` reverses the 4 bytes before `inet_ntoa`). |
| list | `uint32` count, then items (no list type of its own; every list in the doc is "uint32 n, iterate n") |

### 2.2 Strings and encodings

- Send UTF-8. Decode UTF-8, and on invalid UTF-8 fall back to Latin-1 ([N+] `slskmessages.py:340-354`).
- Old clients (Soulseek NS) use Latin-1/ANSI paths. Nicotine+ keeps a per-transfer `legacy_attempt` flag and
  re-sends `QueueUpload`, `PlaceInQueueRequest` and `FolderContentsRequest` with the path **encoded as Latin-1**
  when the peer answers `File not shared.` or when the upload fails once ([N+] `slskmessages.py:3628, 3852, 3948`;
  `downloads.py:1261-1270, 1306-1316`).
- **Design consequence for Selecta:** keep every remote path as the **original raw bytes** received from the peer
  (plus a decoded display string), and echo the raw bytes back in requests. That removes most encoding round-trip
  bugs; the Latin-1 retry then becomes a fallback only for paths typed or reconstructed locally.
- Remote paths use `\` as separator (Windows style) regardless of the peer's OS (TODO: confirm with [N+]
  `shares.py`). Never trust them: they are display data and lookup keys, never local paths.

### 2.3 Message envelopes

| Channel | Envelope | Notes |
|---|---|---|
| Server | `uint32 len` · `uint32 code` · payload | `len` counts code + payload (not itself) |
| Peer-init (first message on every peer socket) | `uint32 len` · `uint8 code` · payload | Code 0 `PierceFireWall`, 1 `PeerInit` |
| Peer (`P`) | `uint32 len` · `uint32 code` · payload | |
| Distributed (`D`) | `uint32 len` · `uint8 code` · payload | |
| File (`F`) | none: `uint32 token` (uploader → downloader), `uint64 offset` (downloader → uploader), then raw bytes | |

Unknown codes are skipped using `len`; an implementation never needs to understand a message to stay in sync.

### 2.4 Compression

`SharedFileListResponse` (peer 5), `FileSearchResponse` (peer 9) and `FolderContentsResponse` (peer 37): the entire
payload after the code is a **zlib stream** (RFC 1950 header + deflate, i.e. `compress()`/`uncompress()` format, not
raw deflate, not gzip). Nicotine+ compresses at level 4 (`ZLIB_COMPRESSION_LEVEL = 4`, [N+] `slskmessages.py:42`).
Use streaming inflate with a hard output cap; reject trailing garbage or truncated streams
(`decompressor.eof` check, [N+] `slskmessages.py:3385-3392`).

### 2.5 Size limits (bounded allocation — NFR-4)

Nicotine+'s inbound limits ([N+] `slskproto.py:340-353, 1530-1540, 1745-1757, 1910-1935, 2545-2556`;
`slskmessages.py:3387, 3509, 3677`):

| Channel / message | Max framed size | Max decompressed |
|---|---|---|
| Server, any message | 448 MiB (then close + no auto-reconnect) | — |
| Peer-init | 16 KiB | — |
| Peer, default | 1 MiB | — |
| Peer `FileSearchResponse`, `FolderContentsResponse`, unknown codes | 16 MiB | 128 MiB |
| Peer `SharedFileListResponse`, `UserInfoResponse` | 448 MiB, **only if we requested it** (unsolicited ones close the connection) | 2 GiB (share list) |
| Distributed | 16 KiB | — |
| Username in `PeerInit` | 256 bytes (real limit 30 chars) | — |

Selecta should copy this structure (per-message caps, "solicited-only" for big responses) with possibly tighter
values; the parser must never pre-allocate from a length or count field without checking it against the bytes
remaining.

## 3. Connections

### 3.1 Connection types and the init handshake

Every peer socket starts with exactly one peer-init message from the side that *opened* the TCP connection:

| Message | Sent by | Meaning |
|---|---|---|
| `PeerInit(own_username, type, token=0)` | the side that initiates a **direct** connection | "I am `own_username`, this socket is of type `P`/`F`/`D`." The token is always 0 today (it used to pair with the obsolete `SendConnectToken`) [N+doc §Peer Init Code 1]. |
| `PierceFireWall(token)` | the side answering an **indirect** request | "You asked the server for me with this token." The receiver looks up the token it sent in `ConnectToPeer` to learn the username and type. |

After the init message the socket carries messages of its type (`P`, `D`) or the file handshake (`F`).

### 3.2 Direct vs indirect ("modern" order) [N+doc §Modern Peer Connection Message Order]

Used by SoulseekQt, Nicotine+ ≥ 3.2.1 and Soulseek.NET clients. User A wants to reach user B:

1. A sends `ConnectToPeer(token, B, type)` to the server **and** `GetPeerAddress(B)` — at the same time.
2. Server answers `GetPeerAddress(B, ip, port, …)`; A dials B and sends `PeerInit`.
3. Server forwards `ConnectToPeer(A, type, A.ip, A.port, token, privileged, …)` to B.
4. If B already got A's `PeerInit`, the direct connection wins. Otherwise B dials A and sends `PierceFireWall(token)`.
5. If B can't reach A either, B sends `CantConnectToPeer(token, A)` to the server, which relays
   `CantConnectToPeer(token)` to A. **Do not rely on it**: keep a local timeout [N+doc §1001].

Legacy order (Soulseek NS, Nicotine+ ≤ 3.2.0, Museek+): direct first, `ConnectToPeer` only after the direct
attempt fails; NS retries the indirect request every 20 s up to three times [N+doc §Legacy].

Rules that follow:

- A `GetPeerAddress` reply with **port 0 / IP 0** means "user offline/unknown" (TODO verify in code).
- Whoever has data to send opens the connection; if a usable `P` connection to that user already exists (either
  direction), reuse it.
- Incoming `ConnectToPeer` from the server = a *request to connect out*; answer with `PierceFireWall` on a new
  outbound socket, or `CantConnectToPeer` if it fails.
- Nicotine+ timers: `CONNECTION_INIT_TIMEOUT = 2` s, `INDIRECT_REQUEST_TIMEOUT = 20` s, idle peer connection closed
  after `CONNECTION_MAX_IDLE = 60` s (10 s for "ghost" connections), cached user address TTL 30 min ([N+]
  `slskproto.py:340-344`). TODO: confirm exact semantics of each timer.

### 3.3 Tokens

`uint32`, chosen by the client, unique among its outstanding requests. Used for `ConnectToPeer`/`PierceFireWall`,
searches (`FileSearch`, `UserSearch`, `WishlistSearch`; echoed in `FileSearchResponse`), transfers
(`TransferRequest` → `FileTransferInit`) and `FolderContentsRequest`. Tokens are not authenticators: a peer
can echo any token. Validate against (username, token, expected state).

### 3.4 NAT, port forwarding, obfuscation

- Default listen port **2234** [N+doc §2]; reported with `SetWaitPort` after login.
- Reachability decides connectivity: reachable↔anyone works; unreachable↔unreachable fails. UPnP/NAT-PMP (NET-3)
  and a port check therefore matter for result counts and upload contribution.
- **Obfuscation** (type 1, "rotated") is a SoulseekQt-only scheme for a second, obfuscated listening port, advertised
  as optional fields of `SetWaitPort` and returned in `GetPeerAddress`/`ConnectToPeer`. Nicotine+ deliberately does
  not support it (no evidence of ISP throttling today; one forwarded port is simpler) [N+doc §2]. Selecta: do not
  implement; parse and ignore the fields. TODO: check Soulseek.NET/aioslsk for an implementation.

## 4. Server messages

### 4.1 Table

Legend — **Dir**: `→` client to server, `←` server to client. **Selecta**: `P0` = MVP, `P1` = v1.0, `parse` = decode
(to stay robust / log) but no feature, `drop` = skip by length, never send. Field notation: `str`, `u8`, `u32`, `i32`,
`u64`, `bool`, `ip`, `[ … ]` = list (u32 count + items). Source: [N+doc §Server Messages] unless noted.

| Code | Name | Dir | Fields | Purpose / notes | Status | Selecta |
|---|---|---|---|---|---|---|
| 1 | Login | → ← | → `str user, str pass, u32 major, str md5hex(user+pass), u32 minor`; ← `bool ok`; ok: `str greet(MOTD), ip own_ip, str md5hex(pass), bool supporter`; fail: `str reason` [+ `str detail` if `INVALIDUSERNAME`] | First message. See §4.2 | active | P0 |
| 2 | SetWaitPort | → | `u32 port` [opt: `u32 obfuscation_type, u32 obfuscated_port`] | Our listening port | active | P0 |
| 3 | GetPeerAddress | → ← | → `str user`; ← `str user, ip, u32 port, u32 obfuscation_type, u16 obfuscated_port` | Address lookup | active | P0 |
| 5 | WatchUser | → ← | → `str user`; ← `str user, bool exists`; exists: `u32 status, u32 avgspeed, u32 uploadnum, u32 unknown, u32 files, u32 dirs`; if status away/online: `str countrycode` | Subscribe to status changes (`GetUserStatus` pushes). Stats are a snapshot, not updated later. `uploadnum` rolls over; incremented by `SendUploadSpeed` | active | P0 |
| 6 | UnwatchUser | → | `str user` | | active | P0 |
| 7 | GetUserStatus | → ← | → `str user`; ← `str user, u32 status, bool privileged` | status 0 offline, 1 away, 2 online | active | P0 |
| 11, 12 | IgnoreUser, UnignoreUser | | `str user` | no longer functional | obsolete | drop |
| 13 | SayChatroom | → ← | → `str room, str msg`; ← `str room, str user, str msg` | chat rooms | active | drop (non-goal) |
| 14 | JoinRoom | → ← | → `str room, u32 private`; ← room + users + statuses + stats + slotsfull + countries [+ owner, operators] | chat rooms | active | drop |
| 15–17 | LeaveRoom, UserJoinedRoom, UserLeftRoom | | | chat rooms | active | drop |
| 18 | ConnectToPeer | → ← | → `u32 token, str user, str type`; ← `str user, str type, ip, u32 port, u32 token, bool privileged, u32 obfuscation_type, u32 obfuscated_port` | Indirect connection request / solicitation (§3.2) | active | P0 |
| 22 | MessageUser | → ← | → `str user, str msg`; ← `u32 id, u32 timestamp, str user, str msg, bool new` | Private message. `new=false` = re-delivery of an offline/unacked message | active | P0 receive+ack, P1 UI |
| 23 | MessageAcked | → | `u32 id` | **Must ack** or the server keeps re-sending | active | P0 |
| 25 | FileSearchRoom | → | `u32 token, u32 room_id, str query` | superseded by 120 | obsolete | drop |
| 26 | FileSearch | → ← | → `u32 token, str query`; ← `str user, u32 token, str query` | Send: global search. Receive: a `UserSearch`/`RoomSearch` aimed at us, delivered outside the distributed network | active | P0 |
| 28 | SetStatus | → | `i32 status` (1 away, 2 online) | Server echoes `GetUserStatus` only when going away | active | P0 |
| 32 | ServerPing | → | empty | Keep-alive, "at most once per minute"; server no longer replies; Nicotine+ uses TCP keepalive instead | active | P0 (or TCP keepalive) |
| 33 | SendConnectToken | → ← | `str user, u32 token` | forwarded by server, used by nobody; re-adding it would isolate the client | obsolete | drop |
| 34 | SendDownloadSpeed | → | `str user, u32 speed` | superseded by 121 | obsolete | drop |
| 35 | SharedFoldersFiles | → | `u32 dirs, u32 files` | Our share counts (shown to others) | active | P0 |
| 36 | GetUserStats | → ← | → `str user`; ← `str user, u32 avgspeed, u32 uploadnum, u32 unknown, u32 files, u32 dirs` | On request, or pushed for room co-members | active | P0 parse, P1 request |
| 40 | UploadSlotsFull | → ← | `u32 slotsfull` | | obsolete | drop |
| 41 | Relogged | ← | empty | Someone logged in with our name; server disconnects us. **No auto-reconnect** (NET-4) | active | P0 |
| 42 | UserSearch | → | `str user, u32 token, str query` | Search one user's shares; target receives code 26 | active | P1 |
| 50 | SimilarRecommendations | | | server sends empty list | obsolete | drop |
| 51, 52 | AddThingILike, RemoveThingILike | → | `str item` | interests | active | drop |
| 54, 56 | Recommendations, GlobalRecommendations | → ← | lists of `(str, i32)` | interests | active | drop |
| 55 | MyRecommendations | | | | obsolete | drop |
| 57 | UserInterests | → ← | `str user` → likes/hates lists | interests | active | drop |
| 58 | AdminCommand | | | | obsolete | drop |
| 59, 60 | PlaceInLineRequest/Response | | | superseded by peer 51/44 | obsolete | drop |
| 62, 63 | RoomAdded, RoomRemoved | | | | obsolete | drop |
| 64 | RoomList | → ← | 3 room lists with user counts | Sent unsolicited after login (rooms ≥ 5 users) | active | parse (skip) |
| 65 | ExactFileSearch | | | no results even with official client | obsolete | drop |
| 66 | AdminMessage | ← | `str message` | Global admin notice | active | P0 (show in activity log / banner) |
| 67, 68 | GlobalUserList, TunneledMessage | | | | obsolete | drop |
| 69 | PrivilegedUsers | ← | `[str user]` | Donors; they get upload-queue priority | active | P0 (upload queue) |
| 71 | HaveNoParent | → | `bool no_parent` | Ask for / stop `PossibleParents` (§8) | active | P0 |
| 73 | ParentIP | → | `ip` | NS only | deprecated | drop |
| 83 | ParentMinSpeed | ← | `u32 speed` | Min upload speed to be a parent | active | P0 parse |
| 84 | ParentSpeedRatio | ← | `u32 ratio` | max children = upload speed ÷ ratio | active | P0 parse |
| 86–88, 90, 91 | ParentInactivityTimeout, SearchInactivityTimeout, MinParentsInCache, DistribPingInterval, AddToPrivileged | ← | `u32` / `str` | no longer sent | obsolete | drop |
| 92 | CheckPrivileges | → ← | → empty; ← `u32 seconds_left` | Our remaining privileges | active | P0 (cheap) |
| 93 | EmbeddedMessage | ← | `u8 distrib_code` + raw distributed message (rest of payload, no length) | Only `DistribSearch`. Receiving it means **we are a branch root**: forward the *unpacked* distributed message to children | active | P0 |
| 100 | AcceptChildren | → | `bool accept` | Whether we take distributed children | active | P0 |
| 102 | PossibleParents | ← | `[str user, ip, u32 port]` (≤ 10) | Sent periodically while we report `HaveNoParent(true)` | active | P0 |
| 103 | WishlistSearch | → | `u32 token, str query` | One wishlist query per interval | active | P1 |
| 104 | WishlistInterval | ← | `u32 seconds` | "Almost always 12 minutes, or 2 minutes for privileged users" | active | P0 parse, P1 use |
| 110–112 | SimilarUsers, ItemRecommendations, ItemSimilarUsers | | | interests | active | drop |
| 113–116 | RoomTickers, RoomTickerAdded/Removed, SetRoomTicker | | | chat rooms | active | drop |
| 117, 118 | AddThingIHate, RemoveThingIHate | → | `str item` | interests | active | drop |
| 120 | RoomSearch | → | `str room, u32 token, str query` | Search a room's members | active | drop (no rooms) |
| 121 | SendUploadSpeed | → | `u32 bytes_per_s` | After a finished upload; feeds our `avgspeed` (which decides parent eligibility and child count) and `uploadnum` | active | P0 |
| 122 | UserPrivileged | → ← | `str user` → `str user, bool` | use 5/7 | deprecated | drop |
| 123 | GivePrivileges | → | `str user, u32 days` | | active | drop |
| 124, 125 | NotifyPrivileges, AckNotifyPrivileges | | | | deprecated | drop |
| 126 | BranchLevel | → | `u32 level` | Our depth in the distributed tree | active | P0 |
| 127 | BranchRoot | → | `str root_user` | Our branch root | active | P0 |
| 129 | ChildDepth | → | `u32 depth` | SoulseekQt sends garbage; server computes it itself | obsolete | drop |
| 130 | ResetDistributed | ← | empty | Drop parent and children, restart parent search | active | P0 |
| 133–141, 143–148 | private rooms (members, operators, ownership, invitations) | | | chat rooms | active | drop |
| 142 | ChangePassword | → ← | `str pass` | Response only if changed | active | P2 |
| 149 | MessageUsers | → | `[str user], str msg` | broadcast PM | active | drop |
| 150–152 | JoinGlobalRoom, LeaveGlobalRoom, GlobalRoomMessage | | | public room feed | active | drop |
| 153 | RelatedSearch | | | server sends empty list | obsolete | drop |
| 160 | ExcludedSearchPhrases | ← | `[str phrase]` | **Paths containing a phrase must not be returned in search results** (NET-6, SHARE-2) | active | P0 |
| 1001 | CantConnectToPeer | → ← | → `u32 token, str user`; ← `u32 token` | Indirect connection failed (§3.2) | active | P0 |
| 1003 | CantCreateRoom | ← | `str room` | | active | drop |

### 4.2 Login in detail

- `hash` = lowercase hex MD5 of `username + password` (concatenated strings). The success reply returns
  `md5hex(password)` (Nicotine+ does not need it) [N+doc §1].
- Rejection reasons in use: `INVALIDUSERNAME` (+ detail: `Nick empty.`, `Nick too long.` (max 30 chars),
  `Invalid characters in nick.` (printable ASCII only), `No leading and trailing spaces allowed in nick.`),
  `EMPTYPASSWORD`, `INVALIDPASS`, `INVALIDVERSION`, `SVRFULL`, `SVRPRIVATE` (registrations closed). `BANNED` is
  obsolete: **a banned client gets no login response at all** — so a login timeout must be treated as a possible
  ban, not retried aggressively [N+doc §Constants].
- There is no password reset; logging in with a new username **registers** it. Randomly generated usernames are
  forbidden by the official server rules (automated scripting) [N+doc §1, citing slsknet.org/news/node/681].
- **Version numbers** [N+doc §Major/Minor Versions]: the server "uses the major and minor versions to differentiate
  between clients". Reserved majors: `157` Soulseek NS/SoulseekQt (`158`/`159` reserved for SoulseekQt development),
  `160` Nicotine+, `169` seeleseek, `170` Soulseek.NET, `175` aioslsk, `176` soulseek-rs, `177` "experimental
  development and testing". Obsolete: ≤ 156 pre-NS, 178/182 Museek+, 180 PySoulSeek, 181/198/200 old Nicotine.
  Minor version: any non-zero `uint32`; Nicotine+ uses 1/2/3 by capability level (3 = 3.4.0+); Soulseek.NET
  requires each downstream app to pick a unique minor (100 legacy, 760 slskd, 9999 testing); SoulseekQt currently
  `101` (2024-02 onwards). "Experimental clients may use major version 177 and any minor version number they choose
  for each project."
- What the server sends right after a successful login, and in which order: TODO (Soulfind + client code).

## 5. Peer-init messages

| Code | Name | Fields | Notes |
|---|---|---|---|
| 0 | PierceFireWall | `u32 token` | Answer to a server-forwarded `ConnectToPeer`; token from that message |
| 1 | PeerInit | `str own_username, str type ("P"/"F"/"D"), u32 token (=0)` | Direct connection |

Validation on accept (from [N+] `slskproto.py:1714-1730`): username length 1–256 bytes, printable, not `server`;
type ∈ {P, F, D}; otherwise close. A `PierceFireWall` whose token doesn't match a pending `ConnectToPeer` is closed.

## 6. Peer messages

### 6.1 Table

| Code | Name | Dir (from the peer that sends it) | Fields | Selecta |
|---|---|---|---|---|
| 1 | Private message | | | obsolete, drop |
| 4 | GetShareFileList | requester → sharer | empty | P1 send (browse), P0 answer |
| 5 | SharedFileListResponse | sharer → requester | zlib: `[dir]`, `u32 unknown=0`, `[private dir]` (see 6.2) | P1 receive, P0 send |
| 8 | FileSearchRequest | | `u32 token, str query` | obsolete, drop |
| 9 | FileSearchResponse | responder → searcher | zlib: `str user, u32 token, [file]`, `bool slotfree, u32 avgspeed, u32 queue_len, u32 unknown=0`, `[private file]` | P0 both |
| 10, 14 | Room invitation, Cancelled queued transfer | | | obsolete, drop |
| 15 | UserInfoRequest | → | empty | P1 send, P0 answer |
| 16 | UserInfoResponse | ← | `str description, bool has_pic, [bytes pic], u32 total_uploads, u32 queue_size, bool slots_free` [opt: `u32 upload_permitted`] | P1 receive, P0 send |
| 33, 34 | Send connect token, Move download to top | | | obsolete, drop |
| 36 | FolderContentsRequest | → | `u32 token, str folder` | P0 both (album download needs it) |
| 37 | FolderContentsResponse | ← | zlib: `u32 token, str folder, [str dir, [file]]` (folder **with all subfolders**) | P0 both |
| 40 | TransferRequest | uploader → downloader | `u32 direction (0 download, 1 upload), u32 token, str path` [+ `u64 size` if direction 1] | P0 both |
| 41 | TransferResponse | ← | `u32 token, bool allowed`; allowed & answering a legacy download request: `u64 size`; not allowed: `str reason` | P0 both |
| 42 | PlaceholdUpload | | `str path` | obsolete, drop |
| 43 | QueueUpload | downloader → uploader | `str path` | P0 both |
| 44 | PlaceInQueueResponse | uploader → downloader | `str path, u32 place` | P0 both |
| 46 | UploadFailed | uploader → downloader | `str path` | Sent when the `F` connection of an active upload closes (NS also on read errors); downloader re-queues or ignores if done | P0 both |
| 47–49 | Exact/indirect file search, queued downloads | | | obsolete, drop |
| 50 | UploadDenied | uploader → downloader | `str path, str reason` | Reject a `QueueUpload` or a queued file | P0 both |
| 51 | PlaceInQueueRequest | downloader → uploader | `str path` | P0 both |
| 52 | UploadQueueNotification | | empty | NS only, deprecated, drop |

### 6.2 File entries, share lists and attributes

A **file entry** (used in share lists, search results and folder contents):

```
u8   code = 1
str  filename      // search results: full virtual path; share list / folder contents: bare name within its dir
u64  size
str  extension     // SoulseekQt always sends "", NS needs "mp3" to show attributes → derive from filename
u32  n_attributes
  u32 attr_code, u32 attr_value   × n
```

Attribute codes [N+doc §Constants]: `0` bitrate (kbps), `1` duration (s), `2` VBR (0/1), `3` encoder (obsolete),
`4` sample rate (Hz), `5` bit depth (bits). Combinations seen in the wild:

| Sender | Lossy | Lossless |
|---|---|---|
| NS, SoulseekQt ≤ 2015-02, Nicotine+, Museek+, slskd | `{0 bitrate, 1 duration, 2 VBR}` | Nicotine+/slskd: `{1 duration, 4 sample rate, 5 bit depth}` |
| SoulseekQt ≥ 2015-06 | `{0, 1}` for MP3/OGG/WMA/M4A — **no VBR flag** | `{1, 4, 5}` for FLAC/WAV/APE; `{0, 1, 4, 5}` for WV |

Consequences for Selecta's strict filters (SRCH-3/6): "CBR 320" cannot be proven from SoulseekQt results (no VBR
attribute); VBR files often report an *average* bitrate (e.g. 245–260 for V0) — TODO verify typical values; any
attribute can be missing. Strict mode must treat "no VBR attribute" as "unknown CBR/VBR", not as CBR.

`SharedFileListResponse` body (after inflate): `u32 n_dirs, (str dir, u32 n_files, file×n)×n_dirs, u32 unknown (0),
u32 n_private_dirs, (dir…)×n` — the second list holds folders the peer shares only with some users (buddy-only /
"private"); they are visible but may be locked (TODO: what the official client does when requesting a private file).
`FileSearchResponse` trailing private-results list likewise. Older clients omit the trailing fields: parse them only
if bytes remain (TODO confirm in [N+] parsers).

### 6.3 Search response flow

The responder connects to the searcher (P connection, direct or indirect) and sends `FileSearchResponse` with the
search token. `slotfree`, `avgspeed` (bytes/s as reported by the server stats — TODO verify unit) and `queue_len` are
the responder's current upload state; they feed Selecta's "free slot / queue / speed" filters and ranking (SRCH-5,
SRCH-10).

### 6.4 Transfer negotiation (queue-based, current)

1. Downloader → `QueueUpload(path)`. The uploader adds it to its queue (or answers `UploadDenied(path, reason)`).
2. Downloader may poll `PlaceInQueueRequest(path)` → `PlaceInQueueResponse(path, place)`.
3. When a slot frees, uploader → `TransferRequest(direction=1, token, path, size)`.
4. Downloader → `TransferResponse(token, allowed=true)` (or `false` + reason, e.g. `Cancelled` if no longer wanted).
5. Uploader opens an `F` connection (§7).

Legacy request: `TransferRequest(direction=0, token, path)` sent by the *downloader* (still used by slskd and Seeker,
so Selecta must understand it when acting as uploader). Accepting it directly is discouraged because the peer could be
spoofed; instead answer `TransferResponse(token, false, "Queued")`, queue the file, and start the upload normally
[N+doc §41a].

### 6.5 Transfer rejection reasons [N+doc §Transfer Rejection Reasons]

| String (exact, case and trailing dot matter) | Meaning |
|---|---|
| `Banned` | Uploader banned us (SoulseekQt sends `File not shared.` instead) |
| `Cancelled` | Transfer cancelled (by either side) |
| `Complete` | Already complete |
| `File not shared.` | Path unknown — or a ban in disguise (SoulseekQt); may also be an encoding mismatch (retry once as Latin-1) |
| `File read error.` | Uploader couldn't read the file |
| `Pending shutdown.` | Uploader is quitting |
| `Queued` | Normal answer to a legacy download request: it's queued |
| `Too many files` | Per-user queued-file limit reached |
| `Too many megabytes` | Per-user queued-size limit reached |
| Deprecated: `Blocked country`, `Disallowed extension` (NS), `File not shared` (no dot, old N+), `Remote file error` (NS, legacy requests), `User limit of x megabytes exceeded`, `User limit of x files exceeded` (old N+) | Recognise when receiving, never send |

Nicotine+ handling as downloader ([N+] `downloads.py:1250-1288`): `File not shared.` → retry once with a Latin-1
path; `Too many files` / `Too many megabytes` / `User limit of…` → show as **queued**, remember a per-user limit of
`max(5, queued − 1)` files and re-queue later automatically. Selecta's FAIR-4 should be at least as polite: treat
these as "back off from this peer", never as "retry now".

## 7. File connections and transfers

1. The **uploader** opens the `F` connection (direct `PeerInit(type F)` or, if that fails, the downloader receives a
   `ConnectToPeer(type F)` and connects back with `PierceFireWall`).
2. Uploader sends `FileTransferInit`: raw `u32 token` (the `TransferRequest` token).
3. Downloader sends `FileOffset`: raw `u64 offset` = bytes it already has (0 for a fresh download).
4. Uploader streams bytes from `offset` until `size`.
5. **The downloader closes the connection when it has received `size − offset` bytes; the uploader must not close
   it** [N+doc §Example Flow]. If the connection drops early, the uploader sends `UploadFailed(path)` on the `P`
   connection and the downloader re-queues.

Notes:
- The offset is for **resume only**. Do not use it to fetch chunks or ranges: no chunk hashing, every request holds an
  upload slot, and an aborted transfer shows as failed for the uploader [N+doc §File Offset]. This matters for
  previews (PREV-6): cancelling a preview mid-transfer looks like a failed upload on the other side (see etiquette
  note).
- Soulseek NS sends offset `-1` (as `u64`, i.e. 0xFFFF…) when resuming beyond 2 GB — treat as invalid.
- Downloader-side cancel mid-transfer: close the `F` socket; the uploader sends `UploadFailed` (TODO: verify what
  Nicotine+/Soulseek.NET send when the *downloader* cancels, e.g. `TransferResponse`/`UploadDenied` `Cancelled`).
- There is no integrity check in the protocol: verify size, and decode the audio afterwards (DL-10).

## 8. Distributed network

Purpose: deliver every search to every sharer without the server fanning out to all users itself.

| Concept | Meaning |
|---|---|
| Branch root | Client at level 0; receives searches from the server as `EmbeddedMessage` (93) and forwards them |
| Parent | The peer that forwards searches to us over a `D` connection |
| Children | Peers we forward searches to; max = our upload speed ÷ `ParentSpeedRatio` (server 84); eligibility needs ≥ `ParentMinSpeed` (server 83) |
| Branch level | Parent's level + 1 (0 for a root); reported to the server (126) and to children (distrib 4) |
| Branch root name | Reported to the server (127) and children (distrib 5) |

Distributed messages [N+doc §Distributed Messages]:

| Code | Name | Fields | Notes |
|---|---|---|---|
| 0 | DistribPing | empty | NS only, deprecated |
| 3 | DistribSearch | `u32 identifier (always 49 = '1'), str username, u32 token, str query` | Forward the **raw message unchanged** (including unknown attributes) to all children. Reject other identifiers [N+] `slskmessages.py:4084-4113` |
| 4 | DistribBranchLevel | `i32 level` | Sent by parent on connect; level 0 ⇒ parent is the root |
| 5 | DistribBranchRoot | `str root` | Sent by parent on connect; a root sends its own name (SoulseekQt fixed this early 2026) |
| 7 | DistribChildDepth | `u32 depth` | obsolete |
| 93 | DistribEmbeddedMessage | `u8 code` + raw message | Only from older SoulseekQt roots (bug fixed early 2026); unpack, forward if code 3, else ignore |

Joining (outline; details and timers TODO from code):

1. After login send `HaveNoParent(true)` (and `AcceptChildren`, `BranchRoot`/`BranchLevel` — TODO exact order).
2. Server sends `PossibleParents` (≤ 10 candidates with higher upload speed than ours) periodically while we have no
   parent. If there are no candidates, we become a branch root and start receiving `EmbeddedMessage`.
3. Connect `D` to candidates; each sends `DistribBranchLevel` + `DistribBranchRoot`. Adopt as parent the first one
   that sends a `DistribSearch` *in addition to* level/root (older clients adopt on level/root alone) [N+doc §3].
   Close the others.
4. Tell the server `HaveNoParent(false)`, `BranchLevel(parent+1)`, `BranchRoot(root)`; tell children the same via
   distrib 4/5.
5. On parent loss or `ResetDistributed` (130): drop children, `HaveNoParent(true)`, repeat.

**What a sharer must do to receive searches:** have a parent (or be a root) — a client with no parent receives no
global searches at all, only `UserSearch`/`RoomSearch` via server code 26. Then match each `DistribSearch` locally,
skip our own username, and on a hit connect to `username` and send `FileSearchResponse(token)`. Forward every
search to children regardless of local matches.

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
