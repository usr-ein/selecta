# Network etiquette and safe development

> Status: **skeleton — research in progress** (accessed 2026-10-19). Items marked **TODO(verify)** are from memory or
> hearsay and have not yet been checked against a primary source. Do not turn them into constants until verified.

## 1. TL;DR

- The Soulseek server enforces limits silently or with a disconnect; there is no documented, machine-readable "you are
  rate-limited" message. The best-known limit is on **search frequency**: tools that searched too fast report
  temporary bans (commonly quoted as ~30 minutes). slsk-batchdl ships a limiter of **34 searches per 220 s** —
  TODO(verify) its origin and whether it is measured or guessed.
- Selecta must never be the client that trips these limits: one **network governor**, conservative defaults, hard
  `constexpr` ceilings, and a development workflow that never touches the live server (local Soulfind server + test
  peers).
- Peer-side etiquette matters as much as the server: share back, do not hammer a single peer, respect queue and
  denial signals, do not mass-queue entire shares, and keep previews cheap for the uploader.

## 2. Key findings

TODO — filled in as sections are verified.

## 3. Official rules and terms

TODO(verify): official rules page on slsknet.org; stance on third-party clients and client identification; bots,
multiple accounts, sharing requirements.

## 4. Server-side limits and bans

TODO(verify): search rate limits (slsk-batchdl 34/220 s and its source; reported ~30-minute bans), login/reconnect
limits, IP bans, ban signals and durations, wishlist interval, how other clients throttle, issue-tracker reports.

## 5. Peer-side etiquette

TODO(verify): leeching culture, common uploader rules, how clients enforce them (Nicotine+, slskd, SoulseekQt),
denial strings, annoying downloader behaviour, recommended self-limits.

## 6. Previews: cancel mid-transfer or finish?

TODO: is cancelling a transfer mid-way rude/harmful versus finishing it?

## 7. Privileges, sharing and reputation, excluded search phrases

TODO.

## 8. Safe development

TODO: local server (Soulfind), test peers, no official test server, record/replay, code guardrails, first controlled
live tests.

## 9. Implications for Selecta

### 9.1 Recommended default limits

TODO — table: searches per window, batch pacing, wishlist, reconnect backoff, per-peer and global download
concurrency, queued files per peer, per-peer daily cap, browse rate, upload slots, preview behaviour.

### 9.2 Never do this

TODO.

### 9.3 Developer safety checklist

TODO.

## 10. Open questions and things to verify on a local test server

TODO.

## 11. Sources

TODO (all accessed 2026-10-19).
