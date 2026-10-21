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
| `MetaTypes.h` | `registerMetaTypes()`: every type carried by a signal |
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
- **Socket cap**: at most 512 (`protocol::kSocketsMax`) including `F` sockets. core/ lowers it to what the process's
  open-file limit leaves after a reserve for the rest of the app (`governor::limits::socketCapFor()`, P-022): sockets
  that take every descriptor make Qt abort on its next thread. At the cap new attempts fail with `SocketLimit`,
  relayed `ConnectToPeer` requests get `CantConnectToPeer`, and new inbound sockets are closed.
- **No report from inside the call that started the work**: a failure known at once (no session, the socket cap, a
  permit refused synchronously) is reported on the next event-loop turn, so a caller always holds the id it was
  given before hearing of it. `cancelFileConnection()` is silent: nothing more is heard of that id. `closeAll()` is
  the one call that reports (every cancellation) before it returns.
- **Tokens**: random start in [1, 2^32/1000], increment, wrap, skip 0; indirect-connection tokens are unique among
  pending ones (a `PierceFireWall` carries nothing else).
- **Peers on this machine**: an address the server gives that is one of this machine's own interface addresses is
  dialled as 127.0.0.1 (direct dials and dial-backs). Soulfind hands out the LAN address for clients that log in from
  127.0.0.1, and the macOS firewall in stealth mode silently drops connections to that address for programs it
  doesn't know; over loopback, clients on one machine reach each other and can listen on 127.0.0.1 alone.

## Transfers

Download: `Queued → Requested → Connecting → Transferring → Completed | Failed | Cancelled`.
Upload: `Requested → Connecting → Transferring → Completed | Failed | Cancelled | Declined`.

Every transfer has a `TransferId` (never 0, never reused by a manager) carried by every signal about it; asking again
for a file already being downloaded returns the existing id. A download is `Requested` once we allow the uploader's
`TransferRequest` (waiting for its `F` connection and token), `Connecting` once the token matched and our resume offset
is sent (waiting for the first bytes), `Transferring` from the first byte. A small file can go from `Connecting`
straight to `Completed`; states never go backwards.

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
- Inbound `F` connections (`PeerInit F`) normally carry a download (the uploader sends the token, within 20 s, or
  the socket is dropped; the download keeps waiting for a proper one until its own 60 s run out). If no download from
  that user is waiting but an upload to them is waiting for its `F` connection, the socket serves that upload (some
  downloaders open it themselves when ours is slow to arrive) and our own attempt is cancelled.
- Upload timers: 45 s from `TransferRequest` to the first byte (Nicotine+), 60 s without the downloader reading, 30 s
  for it to close once every byte is sent.
- Progress (`downloadProgress` / `uploadProgress`: id, bytes done including a resumed part, total) is coalesced: at
  most one signal per transfer every 250 ms (~4 a second) with the latest numbers, a trailing one when it stops
  moving, a final one on completion, none after the terminal state.
- Signals reach every receiver in the order of the changes, even when a slot calls back into the manager (cancels, or
  starts another transfer): a signal caused inside a slot is queued and delivered once the current one has reached
  every receiver. Code never assumes a transfer still exists after emitting.
- Destroying the manager closes open download devices through `DownloadStorage` and emits nothing.
- Previews (PREV): an ordinary download with `preview = true`; core/ hands out a growing-file writer as its device
  and plays from the file while it grows.

## Distributed network

See `DistributedNode.h`; protocol §8.

- After login: `HaveNoParent(true)`, `BranchRoot(self)`, `BranchLevel(0)`, `AcceptChildren(false)`, in that order.
- `PossibleParents`: a `D` connection to each candidate (at most 10 at a time, each asking the `PermitSource`,
  purpose `Distributed`). The first candidate that sends a search once its level and root are known becomes the
  parent (Nicotine+, SoulseekQt); a level-0 candidate is its own root. The others are closed, as is a candidate with
  a negative level. Then `HaveNoParent(false)`, `BranchLevel(parent + 1)`, `BranchRoot(parent's root)`, and later
  changes from the parent are passed on to the server and the children.
- Only the parent (and candidates) may send distributed messages; anyone else is closed. `Ping` and `ChildDepth` are
  ignored from anyone.
- Searches go to every child as the raw bytes received (unknown trailing fields travel on), then to core/; the same
  search twice in a row is dropped. `EmbeddedMessage` (93) wrapping a search, from the server (only without a peer
  parent: we are then a branch root) or from the parent, is unpacked the same way; anything else inside is ignored.
- Children: while we have a parent or are a root, up to `min(avgspeed / ParentSpeedRatio / 100, 10)` once our
  server-side average speed (from `WatchUser`/`GetUserStats` on ourselves) reaches `ParentMinSpeed` (Nicotine+), and
  only if `setChildrenAllowed()`. A child gets our level and root at once and on every change. `AcceptChildren`
  follows whether there is room; dial-back requests for `D` are accepted on the same rule.
- Losing the parent: back to step one; children are kept and hear level 0. `ResetDistributed`: parent, candidates
  and children all go. A new login, or the session ending, starts the tree over.

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
6. Answering: implement `ShareProvider` and `UserInfoProvider` (browse, folder contents, user info) and set them
   on `peers()`. `Client::searchRequestReceived(IncomingSearch)` carries every search from both routes (server code
   26, distributed); skip your own username, apply excluded phrases, caps and the ≥ 3-character rule, look the
   query up in the share index, and answer with `Client::answerSearch(search, hits, {slotFree, averageSpeed,
   queueLength})`.
7. Transfers: implement `DownloadStorage`; `transfers().download(request, permit)` returns the stable id to map to
   a Get tracks line; `downloadChanged` (state, error, rejection) and `downloadProgress(id, done, total)` (~4/s)
   update it. Feed the governor from `downloadChanged` (queued/started/finished/withdrawn, `rejection` →
   `peerDenied`). Uploads: own the queue, answer `uploadRequested` with `denyUpload()` or
   `startUpload(request, device)`; close the device after the terminal `uploadChanged`.
8. Distributed: nothing to do; optionally `setChildrenAllowed(false)` from a setting.
9. Kill switch (SAFE-5): `server().disconnectFromServer()`, `peers().stopListening()`, `peers().closeAll()`.

## Local test network: `selecta-devnet`

`tools/devnet/` builds `selecta-devnet` on the test fixtures: a `FakeServer` and one `FakePeer` per subfolder of a
library folder (the peer is named after the folder and shares everything below it, with attributes read from the
MP3/FLAC/WAV/AIFF headers), all on 127.0.0.1 and nowhere else. It prints the `127.0.0.1:<port>` to put in Selecta's
settings and accepts any login (or only `--user`/`--password`).

- `--generate <dir>` first writes a small synthetic corpus with ffmpeg, the same for the same `--seed`: invented
  artists and generated tones (never music); peers with different habits (FLAC 16/44.1 and 24/96 with cue sheets and
  rip logs, scene-style MP3 320, V0 with disc folders, a 192 kbps "Unsorted" folder of loose files, AIFF and WAV
  extended mixes); overlapping releases, so one search finds a track in several qualities; the naming mess real
  shares have. Next to the peers it writes `tracklist.txt` (every shared "Artist - Title", for a Get tracks list),
  `tracklist-missing.txt` (tracks nobody shares) and `devnet.conf`.
- `devnet.conf` (or `--queue <peer>=<s>` / `--refuse <peer>[=<reason>]`) makes peers misbehave on purpose: in the
  corpus, `queue_quinn` keeps each download queued 45 s (no free slot advertised, place-in-queue answered) and
  `refusing_rita` refuses every one with "Too many files"; each alone shares one release, so the queue and failure
  paths can't be avoided for those tracks.
- Peers answer searches after a random delay (whole-word matching, `-word` exclusions, `*part` partials) with their
  free slot, speed and queue; answer browses, user info and folder requests; queue download requests, start them
  after a random wait (`--queue-wait`) or refuse a share of them with real reasons (`--deny`); and stream the local
  files at their own speed (25–100 % of `--speed`). Every message both ways is logged unless `--quiet`.
- It is hooked from `tests/slsk/CMakeLists.txt` (it needs the fixtures), so it builds with the tests.

## Command line: `selecta-cli slsk-*`

`tools/selecta-cli/commands/slsk/` adds `slsk-login`, `slsk-search`, `slsk-browse`, `slsk-download` (resumes an
existing file) and `slsk-share` (answers searches, browses and folder requests from a folder, uploads on request;
`--advertise-port` plays a peer that can only be reached indirectly). `SlskSession` holds what they share: options for
the server, account and ports; `governor::ServerGuard` (a public address or a host name is refused, and names are
never resolved); a real `governor::Governor` on `base::SteadyClock` for the Login, Search, Browse, QueueDownload and
PeerLookup permits (PeerLookup through a `PermitSource` that delivers on a later turn, as core/ will); one
`slsk::Client` listening on 127.0.0.1; and an event-loop wait. Output is line by line and flushed, so scripts and
tests can read it as it comes.

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
| 9 | `TransferManager` queues signals raised inside a slot until the current one has reached every receiver | core/ will connect several receivers (a controller that cancels, a model that displays); without the queue a receiver connected after the controller would see `Cancelled` before the `Transferring` that caused it |
| 10 | Failures are never reported from inside the call that started the work (`PeerNetwork`) | The caller holds the id first; no re-entrant callback while its own state is half-updated (the crash this fixed: a synchronous `fileConnectionFailed` finishing the upload being adopted) |
| 11 | A peer at one of this machine's own addresses is dialled over loopback | Found against Soulfind: with the firewall in stealth mode, two local clients couldn't reach each other at the LAN address Soulfind gives out; loopback always reaches the same listener |

Ported behaviour (not code) follows Nicotine+ (GPL-3.0-or-later) `pynicotine/slskproto.py`, `slskmessages.py`,
`uploads.py` at `f3b9d4d`: caps, parent selection and child limit, connection replacement, upload close handling.
No code from Soulseek.NET, slskd, Seeker, sockseek or soulseekcpp was read or used.

## Progress

- **Done** (contract S1–S7):
  - S1 codec: `Codec`, `Zlib`, `Types`, the three message families, `Frame`; five libFuzzer targets
    (`tests/fuzz/slsk/`, 60 s smoke clean at S1 and again at S7). Merged as `9f95706`.
  - S2 `ServerSession` with `FakeServer` (`tests/slsk/fixtures/`). Merged.
  - S3 `PeerNetwork` with `FakePeer` and `TestPermits`: listener, direct/indirect race, dial-backs, requests and
    answers, caps, timers, socket cap, `F` hand-off and `D` traffic; `MetaTypes` registers every signal type. Merged.
  - S4 `TransferManager`: downloads (resume from the device's size, size-0 fallback, token matching, both `F`
    directions, denials classified, withdrawn and unsolicited requests answered `Cancelled`, `UploadFailed` echo
    ignored), uploads (TransferRequest, offset validation, streaming under a shared speed limit, linger,
    `SendUploadSpeed`, `UploadFailed`/`UploadDenied`, downloader-opened `F` connections), stable ids, coalesced
    progress, ordered signals, every wait timed. The fixtures became the static library `selecta_slsk_testing`
    (`#include "fixtures/FakeServer.h"` …) for core/'s end-to-end tests. Merged as `974a017`.
  - `selecta-devnet` (see above): `FakePeer` streams uploads from files at a set speed, with a search hook and queue
    control; `FakeServer` takes a port and an optional single account.
  - S5 `DistributedNode` and `Client`: parent selection, branch reports, raw forwarding to children with the
    duplicate guard, branch roots, child limits, reset; the facade with search tokens, both search routes merged into
    `IncomingSearch`, and `answerSearch()`.
  - S6 `devenv/`: Soulfind pinned by GitHub asset id and SHA-256 (`soulfind.pin`), `fetch-soulfind.sh` (by hand
    only), `run-soulfind.sh --all-interfaces` (throwaway database), README with the safety notes and the optional
    reference peers; `tests/slsk/devenv/` (label `devenv`, built only with `SELECTA_SOULFIND_TESTS=ON`, run only
    with `SELECTA_DEVENV=1`). devnet, S5 and S6 merged as `bb81648`.
  - S7 `selecta-cli slsk-*` (see above), tested as processes against the fakes and, opt-in, two of them against
    Soulfind over the direct and the indirect path (contract acceptance 3).
  - Tests: 284 in `selecta_slsk_tests` (dev, ASan and TSan clean), 5 opt-in devenv tests against a real Soulfind
    (green here).
- **Next:** nothing left in the contract; review feedback.
- **Open questions:** none blocking.
