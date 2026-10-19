# ADR-002: Own Soulseek protocol library (`slsk/`)

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/soulseek-protocol.md` §12; `references/soulseek-ecosystem.md` §6, §10; RESEARCH-SYNTHESIS §1.1–1.2

## Context

No maintained, licence-clean C++ Soulseek library exists. Soulseek.NET and slskd now carry GPLv3 §7 additional
terms, which make code reuse impractical. Nicotine+ (GPL-3.0-or-later) maintains the de-facto protocol document.

## Decision

- **Write `src/slsk/` in C++20 on Qt Core + Network + zlib.** It has no UI and no policy; every outbound request is
  gated by the governor (ADR-006).
- **Specification:**
  - Nicotine+ `doc/SLSKPROTOCOL.md` and aioslsk `SOULSEEK.rst`.
  - Behaviour is checked against Nicotine+, aioslsk and soulseek-rs.
  - Code may be ported only from GPL-3.0-or-later or MIT sources, with notices (ADR-010).
- **Scope (protocol §12.1):**
  - The MVP subset: server, peer-init, peer, distributed and file channels.
  - Distributed-network membership is P0.
  - Minimal chat rooms are P1 (D32).
  - Everything else is decoded and dropped.
- **Identity:** major 177 with a non-zero minor during development. A reserved number is requested before M9.
- **Codec:**
  - Bounds-checked little-endian reader and writer.
  - zlib `inflate` with output caps (never `qUncompress`).
  - A failed parse drops the message, not the connection; framing violations close it.
  - Raw path bytes are kept and echoed back.
- **Sessions:**
  - The server session's terminal states are distinct: `Relogged`, rejection, instant EOF and login timeout.
  - Peer connections race the direct and indirect paths.
  - Transfers are explicit state machines, with a preview flag.
- **Fixtures:** `FakeServer` and `FakePeer` cover the distributed network, which Soulfind lacks.

## Consequences

- `slsk/` is a library with a clean boundary, built by its own agent against a header contract (PLAN §9).
- Interop is proven on the local test network (ADR-013), never on the live server.
