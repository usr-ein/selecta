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
- Files Selecta keeps (Inbox, collection, shares) come only from Soulseek peers (PRD D20). Preview audio may come from
  the user's own installed yt-dlp, run as an external process with no cookies or accounts. That audio stays in the
  preview cache and is never filed, shared or tagged. Never bundle, download or reimplement yt-dlp (PRD D26, PREV-11).
  Tests use a fake yt-dlp script, never the real one.
- Code style, module layout and test commands will be defined in Phase 1 of `docs/PLAN.md`; update this file then.
