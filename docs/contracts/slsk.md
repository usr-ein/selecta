# Contract: `slsk/`, the Soulseek protocol library

| | |
|---|---|
| **Owner** | slsk library agent, branch `lib/slsk` |
| **Integrator** | head implementer (`core/` and `app/` use it) |
| **Spec** | `references/soulseek-protocol.md` (§12 above all), Nicotine+ `doc/SLSKPROTOCOL.md`, aioslsk `SOULSEEK.rst` |
| **Decisions** | ADR-002 (own library), ADR-006 (governor), ADR-013 (test network), PRD NET-1…10, SRCH-1/16, DL-1/2, SHARE-2/3, D32 (rooms) |
| **Licence rule** | Port logic only from GPL-3.0-or-later or MIT sources (Nicotine+, aioslsk, soulseek-rs, seeleseek), with notices. **Never** copy or translate code from Soulseek.NET, slskd, Seeker, sockseek or soulseekcpp (ADR-010) |

## Purpose and boundary

`slsk` speaks the Soulseek protocol and nothing else. It holds no policy and no persistence:

- **It never decides whether, when or how often to send.** Budgeted operations require a `governor::Permit`; the
  caller (`core/`) asks the governor.
- **It never touches files directly.** Downloads write to, and uploads read from, `QIODevice`s that `core/` provides.
- **It never decides what is shared.** Answering a search or a browse calls interfaces that `core/` implements.
- **It never resolves host names or chooses servers.** It connects to a `QHostAddress` and port given by the caller,
  and the caller has already checked `governor::addressAllowed()`.

It depends only on Qt Core, Qt Network, zlib and `selecta_base` / `selecta_governor`; no Widgets, no Gui.

## Threading model (mandatory)

- Every `slsk` QObject lives on one thread, the engine thread chosen by `core/`, and is used only from it.
- Everything is asynchronous (signals and slots). Nothing blocks: no `waitFor*`, no sleeps, no nested event loops.
- Value types (messages, results) are copyable and can cross threads in queued signals. Register them with
  `Q_DECLARE_METATYPE` / `qRegisterMetaType` where they travel in signals.
- Codec functions are pure and reentrant.

## Error model

- Decoding never throws and never crashes on any input. A failed parse drops that message (logged under category
  `selecta.slsk.*`); framing violations and oversize frames close the connection. Fuzz tests prove it.
- Connection-level failures surface as typed terminal states with reasons, never as silent stalls. Every wait has a
  timeout (numbers in `src/governor/Limits.h` and protocol §12.4).
- Rejection strings from peers keep their exact text (`File not shared.`, `Too many files`, `Banned`, `Queued`, …)
  and are classified (`retryable`, `never`, `banned`) for `core/`.

## Components and deliverables

Deliver in this order; each step is useful on its own and reviewed when done.

### S1. Codec (pure)
- Bounds-checked little-endian `Reader` and `Writer` over `QByteArray`:
  - integer widths per protocol §2;
  - length-prefixed strings that keep raw bytes, with a decode helper (UTF-8, Latin-1 fallback);
  - a failure flag instead of exceptions.
- zlib inflate and deflate with output caps (never `qUncompress`).
- Message structs and encode/decode for the P0 subset of protocol §12.1 on every channel, plus the minimal rooms
  subset (D32: room list, join/leave, say, user joined/left; tickers and private rooms are decoded and dropped). Unknown
  codes are skipped cleanly.
- `RemotePath { QByteArray raw; QString display() const; }`, always echoed back raw.
- File attributes (bitrate, duration, VBR, sample rate, bit depth) decoded faithfully. A missing VBR flag means
  "unknown", not CBR.
- Login with major version **177** (experimental, never 157/160/169/170/175/176) and minor version 1, as named
  constants.
- **Tests:**
  - golden vectors, including the login example in Nicotine+'s doc;
  - round-trip tests for every message;
  - truncation and garbage tests;
  - one libFuzzer target per decoder family under `tests/fuzz/slsk/`, built only by a fuzz configuration. Apple clang
    has no libFuzzer, so use Homebrew LLVM. Document how to run it.

### S2. Framing and the server session
- A frame reader with per-channel caps (protocol §12.4: server 16 MiB, peer-init 16 KiB, peer 1 MiB, …).
- `ServerSession` (QObject):
  - `connectToServer(QHostAddress, quint16, governor::Permit)` with a `Login` permit, then `login(username,
    password)`;
  - waits for the login reply before sending anything else;
  - after login, sends its listen port, shared-file counts (values given by the caller) and status;
  - keeps the connection alive with TCP keepalive plus a ping every 300 s.
- Terminal states are distinct and typed: logged out by the user, `Relogged`, login rejected (with the server's reason
  text), login timeout (possible silent ban), immediate EOF (possible IP throttle), connection lost, protocol violation.
  The ban PM from `server` ("You have been banned for 30 minutes…") is recognised and reported as such. **`slsk` never
  reconnects on its own**; `core/` does, through the governor.
- Typed signals for incoming server messages: searches to answer, PMs, room events, privileged users, excluded search
  phrases, wishlist interval, parent-candidate lists, and so on.
- Typed send methods. Budgeted ones take a `Permit`: `FileSearch` and the other searches (`Search`), PMs
  (`PrivateMessage`), room posts (`RoomMessage`), `GetPeerAddress` (`PeerLookup`).

### S3. Peer connections
- A listener on the configured port, plus direct and indirect connection setup raced as in protocol §3: `PeerInit`,
  `PierceFireWall`, `ConnectToPeer` via the server, `CantConnectToPeer` sent when our dial-back fails.
- Every new outbound connection needs a `PeerLookup` permit.
- Tokens start at a random value, increment, wrap and skip 0.
- Matching is against `{token → (user, type, deadline)}` with deadlines (direct 10 s, indirect 20 s).
- One `P` and one `D` connection per user.
- Idle timeouts (60 s), and a global socket cap.
- Peer messages:
  - P0 subset: search results in, shared-file lists, folder contents, user info, transfer requests and responses,
    queue upload, place in queue, upload failed/denied.
  - Requesting a browse, user info or folder contents needs the matching permit.
  - Answering is unbudgeted and done through interfaces: `ShareProvider` gives browse blobs, folder contents and
    search hits; `UserInfoProvider`.

### S4. Transfers
- Explicit state machines for downloads (queued at the peer → requested → connecting `F` → transferring → done,
  failed, cancelled) and uploads, as in protocol §7.
- Downloads:
  - accept the `F` connection from either direction;
  - send the resume offset from the size of the caller's `QIODevice`;
  - close after `size` bytes;
  - can be cancelled at any time by closing `F`.
- The `UploadFailed` echo that follows our own cancel is ignored.
- Queued-but-unstarted downloads are withdrawn by answering a later `TransferRequest` with `Cancelled`.
- Legacy downloader-initiated `TransferRequest` (direction 0) is answered `Queued` when we upload.
- Uploads read from a `QIODevice` that `core/` opens after an exact `ShareProvider` lookup, and validate the offset.
- `QueueUpload` needs a `QueueDownload` permit.
- Progress signals are throttled (at most ~10/s per transfer).
- A preview is an ordinary download with a flag.

### S5. Distributed network
`DistributedNode`, as in protocol §8:
- parent candidates from the server, parent selection (the Nicotine+ rule);
- branch level and root reported to the server and to children;
- raw forwarding of `DistribSearch` to children;
- accepting children when eligible (≤ 10);
- `ResetDistributed`; code 93 from server or parent handled identically.
- Searches that reach us become a signal for `core/` to answer.

### S6. Fixtures and the local test network
- In-repo **`FakeServer`** and **`FakePeer`**, test-only, scriptable, in-process on loopback ports. Together they cover
  every flow above, including the distributed network, which Soulfind lacks.
- **`devenv/`** (you own it): scripts that fetch a pinned Soulfind release (verify its SHA-256) and run it on
  `127.0.0.1` with a throwaway database, plus a README.
  - Integration tests against it carry the CTest label `devenv` and run only when `SELECTA_DEVENV=1` is set.
  - Reference peers (Nicotine+ headless, slskd in Docker) are optional extras; document them, but don't make the
    suite depend on them.
- **Never** point anything at `server.slsknet.org` or any non-local address. `governor::ServerGuard` refuses them, and
  your tests must not try.

### S7. Command-line exercise
Add `selecta-cli` subcommands that exercise the library against the local network only, in new files under
`tools/selecta-cli/`: `slsk-login`, `slsk-search`, `slsk-browse`, `slsk-download`. Register them in
`tools/selecta-cli/main.cpp` with a minimal change. Each refuses non-local servers through `ServerGuard`. Permits in
the CLI come from a small local `Governor` stand-in that enforces the real limits from `Limits.h`; ask the head
implementer for the real `Governor` API when you get there.

## Acceptance

1. All unit tests and fuzz smoke runs (60 s per target) pass; dev, asan and tsan are clean; lint is clean.
2. Against `FakeServer`/`FakePeer`:
   - login, search out and in, distributed search forwarding, browse both ways, folder contents and user info;
   - download with resume after a kill, cancel mid-transfer, upload with limits, denial strings;
   - room join, post and leave.
3. With `SELECTA_DEVENV=1`, against Soulfind: two `selecta-cli` instances log in, one searches and the other answers,
   and a file downloads end to end over the direct path and over the forced indirect path.
4. `DESIGN.md` explains the classes, threading and states, and the integration points for `core/`.
