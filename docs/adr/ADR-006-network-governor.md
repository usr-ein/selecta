# ADR-006: Network governor

- **Status:** Accepted
- **Date:** 2026-10-19
- **Inputs:** `references/network-etiquette-and-safe-development.md` §4, §9; `references/soulseek-protocol.md` §11–12; RESEARCH-SYNTHESIS §3; PRD D31, SAFE-1…5

## Decision

- **Every outbound operation asks `governor/` first:** searches from every source, peer lookups and connections,
  browse, user info, folder contents, queue requests, previews, downloads, private messages and room posts, and
  reconnects. A test proves each send path is unreachable without a permit.
- **Rates are token buckets or sliding windows, never fixed windows.** Searches use one shared bucket: burst 6,
  one more every 30 s (D31).
- **Defaults and ceilings are the table in RESEARCH-SYNTHESIS §3.**
  - Every default has a `constexpr` ceiling and `static_assert(default <= ceiling)`.
  - Settings can only make limits stricter (SAFE-2).
- **Ban handling:** the 30-minute ban PM from `server` pauses the network and asks the user. A login timeout is
  treated as a possible silent ban. Never reconnect after `Relogged`.
- **The governor takes an injectable clock.** Pacing tests never sleep.
- **Every decision is written to the activity log (SAFE-4):** permit, delay or refusal, with its reason.

## Consequences

- Get tracks searches about 2 lines a minute, so 50 new tracks take about 25 minutes. The UI says why.
- Raising any number needs new evidence (M9) and a change to this ADR. Never above 3 searches a minute.
