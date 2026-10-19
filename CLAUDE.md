# Selecta — notes for coding agents

Selecta is a GPLv3 Soulseek client in C++/Qt 6, macOS-first. Start here:

- Product requirements: `docs/PRD.md`
- Implementation plan and milestones: `docs/PLAN.md`
- Research notes (protocol, etiquette, audio, conversion, Qt, sharing, library matching): `references/`
- UI mock: `mock/index.html` — once validated by the maintainer, it is the reference for layout and interactions.

## Network safety rules (non-negotiable)

- Never connect to the live Soulseek server (`server.slsknet.org`) from tests, scripts, CI, or debugging sessions.
  Development and integration testing use a local test server (see "Safe development" in `docs/PLAN.md`).
- Never weaken or bypass the network governor (search rate limits, per-peer caps, reconnect backoff).
  Its ceilings are compile-time constants, not settings.
- Never run automated searches or downloads against the live network. Live-network sessions are run
  manually by the maintainer only.

## Working conventions

- Do not run `git add -A` / `git add .` — several agents may work in this tree at once; stage explicit paths.
- Secrets (API keys, client secrets, cookies) are never committed, logged, or copied into docs or tests.
  Development secrets live in `local/secrets.env` (git-ignored); read them from there.
- Selecta never downloads audio from streaming services (YouTube, YouTube Music, Spotify, …); see PRD D20.
- Code style, module layout and test commands will be defined in Phase 1 of `docs/PLAN.md`; update this file then.
