# `slsk/` — design

The Soulseek protocol library: codec, server session, peer connections, transfers and the distributed network.
Contract: `docs/contracts/slsk.md`. Spec: `references/soulseek-protocol.md` (§12 above all), Nicotine+
`doc/SLSKPROTOCOL.md`, aioslsk `SOULSEEK.rst`. It holds no policy and no persistence: it never decides whether,
when or how often to send, never touches files, never decides what is shared, and never resolves host names.

## Layers

```
 core/ (services, governor)            ── permits, providers, devices ──┐
   │                                                                     │
   ▼                                                                     │
 Client (facade: search tokens, merged incoming searches)                │
   ├── ServerSession     server channel, login, keepalive, typed events  │
   ├── PeerNetwork       listener, connection race, P messages,      ◀───┤ PermitSource (PeerLookup)
   │                     F hand-off, D traffic                       ◀───┤ ShareProvider, UserInfoProvider
   ├── TransferManager   download/upload state machines over P + F   ◀───┘ DownloadStorage, upload QIODevices
   └── DistributedNode   parent/children/branch, raw search forwarding
 Codec (pure): Reader/Writer, zlib, message structs, framing
```

| File | What |
|---|---|
| `Protocol.h` | Codes (server, peer-init, peer, distributed), identity (177/1), caps, timers |
| `Codec.h` | `Reader`/`Writer` (bounds-checked LE, sticky failure flag), `decodeText` (UTF-8, Latin-1 fallback), `md5Hex`, `DecodeStatus`, the `SELECTA_SLSK_MESSAGE` convention |
| `Zlib.h` | `inflate` with an output cap, `inflatePrefix` (cheap search-result header), `deflate` |
| `Types.h` | `RemotePath` (raw bytes, echoed back), `FileEntry`, `FileAttributes` (missing VBR = unknown), share lists, `UserStats`, rejection classification |
| `ServerMessages.h` | `toserver::*` / `fromserver::*` structs, `decodeToServer` / `decodeFromServer` |
| `PeerMessages.h` | `peerinit::*`, `peer::*`, `file::*` (raw `F` handshake), `decodePeerInit` / `decodePeer` |
| `DistributedMessages.h` | `distrib::*`, `decodeDistrib` |
| `Frame.h` | `encodeFrame`, `frame(msg)`, `FrameReader` (per-frame caps checked on the header) |
| `ServerSession.h` | The server connection |
| `PermitSource.h` | Where PeerLookup permits come from (implemented by core/) |
| `Providers.h` | `ShareProvider`, `UserInfoProvider` (implemented by core/) |
| `PeerNetwork.h` | Peer connections |
| `TransferManager.h` | Transfers, `DownloadStorage` (implemented by core/) |
| `DistributedNode.h` | Distributed network membership |
| `Client.h` | The facade |
| `TokenGenerator.h` | Random-start, wrapping, never-0 tokens |
| `internal/` | Private implementation classes; not for core/ |

## Threading and ownership

- Every QObject in `slsk` lives on the engine thread chosen by `core/` and is used only from it. Nothing blocks:
  no `waitFor*`, no sleeps, no nested event loops. Every wait has a timer.
- Codec functions are pure and reentrant. Value types are copyable (Qt implicit sharing) and registered as
  metatypes (`Client::registerMetaTypes()`), so they can cross threads in queued signals.
- One exception to "everything on the engine thread": a share list larger than 1 MiB is inflated and parsed on a
  `QThreadPool` worker (pure codec code), and the result is posted back to the engine thread. A result that arrives
  after the `PeerNetwork` is gone is dropped (QPointer guard).
- `Client` owns the four components (QObject children). `PeerNetwork` owns its sockets until it hands an `F` socket
  to `TransferManager`, which then owns it. Providers, the `PermitSource`, the `DownloadStorage` and upload devices
  belong to `core/` and must outlive the objects they are set on (or be unset first).

## Error model

- Decoding never throws or crashes. A malformed message is dropped and logged (`selecta.slsk.*`); the connection
  stays. Framing violations (a length over the channel's cap, or too short to hold the code) close the connection.
  Unknown codes are skipped by length. Optional trailing fields are read only if bytes remain; extra trailing bytes
  are ignored. Lists are checked against the bytes that remain before anything is reserved.
- Caps (protocol §12.4): server 16 MiB; peer-init 16 KiB; peer 1 MiB; search results, folder contents and unknown
  peer codes 16 MiB (128 MiB inflated); share list 256 MiB (1 GiB inflated) and user info 16 MiB **only while we
  wait for one from that user**, otherwise the connection closes; distributed 16 KiB.
- Connection-level failures are typed terminal states: `ServerSession::EndReason`, `PeerError`, `TransferError`.
- Rejection strings keep their exact text and are classified (`Retryable`, `Never`, `Banned`) by
  `classifyRejection()`.

## Server session

States: `Disconnected → Connecting → Connected → LoggingIn → LoggedIn`, back to `Disconnected` through `ended()`,
exactly once per session.

- `connectToServer(address, port, Login permit)`, then `login(user, password)`. Nothing else is sent before the
  login reply (Soulfind leaves messages sent earlier unread).
- After a successful reply, the burst: `SetWaitPort`, `SetStatus`, `SharedFoldersFiles` (values from the caller),
  `CheckPrivileges`, `WatchUser(self)` (our own average speed, for the child limit), `EnableRoomInvitations(false)`.
  `DistributedNode` then sends `HaveNoParent(true)`, `BranchRoot(self)`, `BranchLevel(0)`, `AcceptChildren(false)`.
- Keepalive: TCP keepalive (60 s idle, 10 s interval, 6 probes where the platform allows tuning) plus `ServerPing`
  every 300 s.
- Private messages are acknowledged (`MessageAcked`) as they arrive, then signalled. PMs from `server` and
  `AdminMessage`s are also signalled as `ServerNotice`s, with the ban text and the restricted-version text
  recognised.
- Rooms (D32): join (always public), leave, say, room list, user joined/left; events for rooms we didn't join are
  dropped; an unsolicited `JoinRoom` reply is answered with `LeaveRoom`; our own `UserJoinedRoom` before the reply
  is ignored. Tickers and private-room messages are recognised and dropped.

`EndReason` and the governor's `ReconnectPolicy::Event`:

| EndReason | Event | Notes |
|---|---|---|
| `LoggedOut` | — | `disconnectFromServer()`; the user chose it |
| `Relogged` | `Relogged` | never reconnect automatically |
| `LoginRejected` | `LoginRejected` | `loginRejection()` has the reason and detail |
| `ServerFull` | `ServerFull` | rejection reason `SVRFULL` |
| `LoginTimeout` | `LoginTimeout` | no reply in 30 s: possible silent ban |
| `InstantEof` | `InstantEof` | closed before the login reply: possible IP throttle |
| `BanNotice` | `BanNotice` | the ban PM arrived (the server then disconnects) |
| `ConnectionLost` | `ConnectionLost` | a logged-in session dropped |
| `ConnectFailed` | — (suggest: ordinary back-off) | TCP connect refused, unreachable or timed out |
| `ProtocolViolation` | — (suggest: needs the user) | oversize or impossible frame from the server |

## Peer connections

- **Outbound** (we have something to send and no connection): ask the `PermitSource` for a PeerLookup permit
  (purpose: `Request`, `SearchResponse`, `Transfer` or `Distributed`), then race the direct path (`GetPeerAddress`,
  dial with a 10 s timeout, `PeerInit`) against the indirect one (`ConnectToPeer`; the peer dials back with
  `PierceFireWall`, 20 s). The first to complete wins; the loser is closed. `0.0.0.0` in the address reply means the
  user is offline (immediate `UserOffline`); port 0 or > 65535 means indirect only. Messages queue on the attempt
  and are flushed when it succeeds, or failed with its error.
- **Inbound**: an accepted socket must send `PeerInit` (username 1–256 bytes, printable, not `server`; type P/F/D)
  or `PierceFireWall` with a token we issued, within 10 s; otherwise it is closed.
- **Dial-backs**: a relayed `ConnectToPeer` makes us connect out and send `PierceFireWall`, or
  `CantConnectToPeer` if that fails. `P`: always (if we don't already have a connection to that user). `F`: only if
  `TransferManager` expects a transfer from that user. `D`: only if `DistributedNode` wants it. Dial-backs send no
  lookup and are not budgeted (they answer someone else's lookup); they count against the socket cap.
- **One connection we send on per user and type** (`P`, `D`). When a newer connection for the same user and type is
  established (an inbound `PeerInit`, our dial-back, a late `PierceFireWall`), it becomes the one we send on; the
  older one is kept for reading until it closes or idles out, so nothing the peer already sent on it is lost.
- **Timers**: idle 60 s (bytes in or out reset it); 10 s for an inbound socket that never identifies itself; a
  request with no answer and no traffic from that user for 60 s fails with `Timeout`.
- **Short-lived search connections**: a `P` connection a peer opened only to deliver search results is closed once
  they're read (if nothing else is pending with that user); the one we open to deliver our answer is closed once it
  is written.
- **Socket cap**: 512 (`protocol::kSocketsMax`) including `F` sockets; new attempts fail with `SocketLimit` and new
  inbound sockets are closed at the cap.
- **Tokens**: random start in [1, 2^32/1000], increment, wrap, skip 0; indirect-connection tokens are unique among
  pending ones (a `PierceFireWall` carries nothing else).

## Transfers

Download: `Queued → Requested → Connecting → Transferring → Completed | Failed | Cancelled`.
Upload: `Requested → Connecting → Transferring → Completed | Failed | Cancelled | Declined`.

- Download: `QueueUpload` (QueueDownload permit). The uploader's `TransferRequest` (matched by user and raw path) is
  allowed when the download is still queued and `DownloadStorage::openDownload()` gives a device; the `F`
  connection is accepted from either direction; the token must match; the resume offset is the device's size; the
  connection is closed after `size − offset` bytes. Size: the uploader's if non-zero, else the search result's.
  `UploadDenied` fails a download with the classified rejection; `UploadFailed` fails an active one
  (`RemoteFailed`), and is ignored for queued, cancelled or finished ones (the echo of our own cancel).
  Unsolicited and withdrawn `TransferRequest`s are answered `Cancelled`.
- Upload: core/ keeps the queue; `uploadRequested()` signals `QueueUpload` and legacy `TransferRequest(0)` (already
  answered `Queued`). `startUpload()` sends `TransferRequest(1)`, waits up to 45 s for the answer and the `F`
  connection, sends the token, validates the offset (`> size` fails the upload; `== size` sends nothing), streams,
  then waits up to 30 s for the downloader to close. Completed uploads report their average speed to the server
  (`SendUploadSpeed`). A broken upload sends `UploadFailed`; a cancel by core/ sends `UploadDenied(reason)` or
  `UploadFailed`. An optional global upload speed limit is shared by active uploads.
- Inbound `F` connections (`PeerInit F`) normally carry a download (the uploader sends the token). If none is
  expected from that user but an upload to them is waiting for its `F` connection, the socket is used for that
  upload (Soulseek.NET downloaders open it themselves after 10 s).
- Progress signals at most every 100 ms per transfer, plus every state change.

## Distributed network

See `DistributedNode.h`. Parent selection is the Nicotine+/SoulseekQt rule (P-4); children ≤ 10 by Nicotine+'s
formula (P-5); `DistribSearch` with an identifier other than 49 is rejected (Nicotine+); forwarding uses the raw
payload; code 93 from the server or a parent is unpacked identically.

## Integration points for core/

1. Implement `PermitSource` on top of `governor::Governor` (`Operation::PeerLookup`, `Request{peer = username}`;
   the `ConnectPurpose` lets the governor budget inbound-triggered connections separately).
2. Create one `Client` on the engine thread; call `Client::registerMetaTypes()` once if any signal crosses threads.
3. Login: `governor.request(Login, {server, liveEnabledByUser})` → `server().connectToServer(addr, port, permit)` →
   `login()`. Map `ended(reason)` to `governor.serverEvent()` with the table above; `loggedIn` →
   `LoginSucceeded`. Call `setListenPort` / `setSharedCounts` / `setStatus` before login and on change.
4. `peers().listen(port)` before login; on failure, tell the user.
5. Searches: `Client::search(query, permit)` with `Decision::cleanedQuery`; results via
   `peers().searchResponseReceived`; `stopSearch(token)` when the SRCH-16 window ends.
6. Answering: implement `ShareProvider` and `UserInfoProvider`; answer `Client::searchRequestReceived` with
   `peers().sendSearchResponse()` (apply excluded phrases, caps and the ≥ 3-character rule in core/).
7. Transfers: implement `DownloadStorage`; `transfers().download(request, permit)`; feed the governor with
   `downloadChanged` (queued/started/finished/withdrawn, `rejection` → `peerDenied`). Uploads: own the queue,
   answer `uploadRequested` with `denyUpload()` or `startUpload(request, device)`.
8. Distributed: nothing to do; optionally `setChildrenAllowed(false)` from a setting.
9. Kill switch (SAFE-5): `server().disconnectFromServer()`, `peers().stopListening()`, `peers().closeAll()`.

## Decisions taken here (for review)

| # | Decision | Why |
|---|---|---|
| 1 | PeerLookup permits come from a pull-style, asynchronous `PermitSource`; operation permits (Search, Browse, …) are passed explicitly | Connections are opened by slsk's own mechanics (queued sends, parent candidates, upload `F` connections); asking core/ for each through signals would leak connection state into core/. The callback can be delayed, so the governor can grant later instead of only yes/no |
| 2 | Search answers go through `searchRequestReceived` + `sendSearchResponse()`, not `ShareProvider` | Each answer opens an outbound, budgeted connection and core/ keeps a bounded work queue (ADR-012); a synchronous provider call would hide both |
| 3 | Dial-backs are not budgeted | They send no lookup; they answer someone else's. Bounded by the socket cap and by type rules (`F` only expected, `D` only wanted) |
| 4 | Newest connection is the one we send on; older ones are read until idle | Avoids losing messages a peer already sent when both directions connect at once |
| 5 | `TransferManager` sends `SendUploadSpeed` itself after a completed upload | Honest reporting is protocol duty (SHARE-3, §7), not policy |
| 6 | Downloads get their device from `DownloadStorage` when the uploader is ready, not at queue time | A queued download may wait hours; holding 200 open files would waste descriptors |
| 7 | Share lists over 1 MiB are parsed on a worker thread | A 1 GiB inflate would stall the engine thread for seconds |
| 8 | `ConnectFailed` and `ProtocolViolation` are distinct end reasons | The governor's event list has no counterpart; reported to the head implementer |

Ported behaviour (not code) follows Nicotine+ (GPL-3.0-or-later) `pynicotine/slskproto.py`, `slskmessages.py`,
`uploads.py` at `f3b9d4d`: caps, parent selection and child limit, connection replacement, upload close handling.
No code from Soulseek.NET, slskd, Seeker, sockseek or soulseekcpp was read or used.

## Progress

- **Done:**
  - Public headers and this design (S1–S5 API, `Client`).
  - S1 codec: `Codec`, `Zlib`, `Types`, the three message families, `Frame`; 125 tests; five libFuzzer targets
    (`tests/fuzz/slsk/`, 60 s smoke clean). Merged into main as `9f95706`.
  - S2 `ServerSession` with `FakeServer` (`tests/slsk/fixtures/`): login, burst, keepalive, every end reason, notices,
    PM acks, rooms, budgeted sends; 34 tests (the connect-timeout one skips on macOS, which completes connections
    past a full backlog).
- **Next:** S3 `PeerNetwork` (implementation written, kept out of the build until its tests with `FakePeer` pass),
  then S4 transfers, S5 distributed, S6 devenv, S7 CLI.
- **Open questions:** none blocking.
