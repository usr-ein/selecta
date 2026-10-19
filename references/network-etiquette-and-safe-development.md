# Network etiquette and safe development

> Status: **draft — research in progress** (all sources accessed 2026-10-19). Items marked **TODO(verify)** are from
> memory or hearsay and have not yet been checked against a primary source. Do not turn them into constants until
> verified. Source repos were read at the commits listed in section 11.

## 1. TL;DR

- **The official rules only "tolerate" third-party clients that implement the full feature set** — "chat, search,
  wishlist, download, upload, and respect / recognition of privileges". Bots and "scripts otherwise failing to implement
  the full range of Soulseek features are not allowed to connect". Selecta's v1 non-goal "no chat rooms" is in tension
  with that wording (see §3.2 and the decision in §9).
- **Identify honestly with a reserved login version.** The community registry in Nicotine+'s protocol doc reserves
  major versions per client (157 official, 160 Nicotine+, 170 Soulseek.NET, 175 aioslsk, …) and **177 for
  experimental development**. Selecta should ask for its own number before any live use.
- **The server bans silently.** A banned login gets *no* login response (the `BANNED` reason is obsolete). Search
  flooding leads to a disconnect and a temporary ban; slsk-batchdl's author measured "around 34 searches every 220
  seconds" and reports 30-minute bans. That figure is one person's experiment, not a published limit.
- TODO: peer-side etiquette, previews, safe development summary.

## 2. Key findings

| # | Finding | Confidence | Source |
|---|---|---|---|
| K1 | Third-party clients are tolerated only if they implement chat, search, wishlist, download, upload and privileges; bots and partial scripts may not connect. | High (verbatim rule) | slsknet.org Rules |
| K2 | Max **5 clients from the same IP**. | High (verbatim rule) | slsknet.org Rules |
| K3 | ToS §12 forbids "overloading, flooding, spamming" and probing/testing the service without authorisation. | High (verbatim) | slsknet.org TOS |
| K4 | Login major versions are a de-facto registry; 177 is for experimental clients; random usernames are "unacceptable". | High (maintained by Nicotine+ devs; not an official Soulseek document) | Nicotine+ `doc/SLSKPROTOCOL.md` |
| K5 | Banned logins get no response at all (`BANNED` no longer sent). | Medium-high (protocol doc statement) | Nicotine+ `doc/SLSKPROTOCOL.md` |
| K6 | Search flooding → disconnect + ~30 min ban; slsk-batchdl default 34 searches / 220 s comes from its author's own testing (2023). | Medium (single-source empirical) | slsk-batchdl issue #12, README |
| K7 | Peers auto-detect "leechers" from the server-reported share counts and by browsing you; faking counts was publicly called "bad faith". | High | Nicotine+ leech_detector plugin; slsk-batchdl #165 |

TODO — more rows as sections are verified.

## 3. Official rules and terms

### 3.1 What exists

There is no API policy or developer programme. The binding texts are two pages on slsknet.org, plus community
conventions maintained by the Nicotine+ developers (who are the de-facto keepers of protocol documentation).

| Document | What it says that matters to Selecta |
|---|---|
| **Rules** (`slsknet.org/news/node/681`, undated) | "Access to the SoulSeek® server is a privilege; not a right. It may be revoked at any time, for any reason." · "Server or chatroom abuse will result in your connection being closed and your ISP notified." · "Spammers, automated clients (robot/bot), combinations of such, or scripts otherwise failing to implement the full range of Soulseek® features are not allowed to connect to the Soulseek® service." · "Alternative clients for platforms other than Microsoft(r) Windows(r) that implement the full range of features of the Soulseek® Network (including chat, search, wishlist, download, upload, and respect / recognition of privileges) are tolerated." · "Users are allowed to connect up to a maximum 5 clients from the same IP." · Administrators (nir, OrtaliGoddessOfPrivileges, Proteus93, arsenic_, sierracat) "can kill any connections without prior notice for any reason." |
| **Terms of Use** (`slsknet.org/news/node/682`, © 2007 Soulseek, LLC) | §2: you may not use someone else's account. §8: accounts may be terminated without notice for abuse or for "interfer[ing] with use of the Site or the Service by others". §12(b): no "attempting to probe, scan, or test the vulnerability of the Service … without proper authorization". §12(c): no "interfering … with service to any user, host, or network, including … overloading, 'flooding,' 'spamming' …". §3 forbids reverse-engineering *their software* (Selecta reads only open third-party code and docs — see §8.6). |
| **FAQ** (`slsknet.org/news/faq-page`) | Usernames of non-privileged accounts are recycled after 30 days without login. Admins' last resort against abusive users is "an IP ban". The official position on auto-banning non-sharers points to Nir's 2012 post (below). |
| **Nir, "Empty share warnings, fixes and more"** (`slsknet.org/news/node/523`, 2012-05-11) | The author of Soulseek refused an auto-ban for non-sharers as "too harsh", and instead added to SoulseekQt an option that **browses every downloading user in the background and sends them a message if their share comes up empty**. The same build started returning upload place-in-line to downloaders. |
| **Nicotine+ protocol doc** (`doc/SLSKPROTOCOL.md`, community, updated 2026-10-18) | Major-version registry (below). "It is unacceptable to use randomly generated usernames, as such automated scripting is disallowed by the official server rules." Recommends Soulfind as a test server. |

### 3.2 Reading the "full range of features" clause for Selecta

- The clause is the only official statement about third-party clients. It is old (it predates SoulseekQt on macOS
  and Linux) and loosely enforced: Nicotine+, slskd and others coexist with the server. But it is the text Nicotine+
  maintainers quote when they push back on partial clients (slsk-batchdl issue #165, 2026-03: "third-party clients
  are only 'tolerated' if they implement the full range of features … scripts otherwise failing … are not allowed").
- Selecta P0/P1 already covers search, wishlist (P1), download and upload. **Gaps against the clause:** chat rooms
  (v1 non-goal; private messages are P1) and "respect / recognition of privileges" (not yet in the PRD: privileged
  users must get priority in Selecta's upload queue).
- "Platforms other than Microsoft Windows": the official client (SoulseekQt) now exists on Windows, macOS and Linux,
  so this clause is anachronistic. Selecta is macOS-first; Windows builds are CI-only in v1, which keeps us on the
  safe side of the literal text. **Inference**, not an official interpretation.
- Admins can disconnect anyone for any reason; there is no appeal process documented. A ban during development costs
  the maintainer his account and possibly his IP (K2/K5) — hence the safe-development rules in §8.

### 3.3 Client identification (login version fields)

The login message carries a major and a minor version. The server "uses the major and minor versions to
differentiate between clients" (Nicotine+ protocol doc). The doc maintains a registry so that clients do not
impersonate each other:

| Major | Client | Notes |
|---|---|---|
| 157 | Soulseek NS and SoulseekQt (official) | Never use: it impersonates the official client. Qt minor versions 100/101. |
| 158–159 | Reserved for SoulseekQt development | |
| 160 | Nicotine+ | Minor 1/2/3 by capability level (3 = ≥ 3.4.0). |
| 169 | seeleseek | |
| 170 | Soulseek.NET (and slskd) | Minor chosen per downstream project: 100 legacy, 760 slskd, 9999 experimental. |
| 175 | aioslsk | `CLIENT_VERSION = 175`, `MINOR_VERSION = 1` in `aioslsk/client.py`. |
| 176 | soulseek-rs | |
| **177** | **Experimental development and testing** | "Experimental clients may use major version 177 and any minor version number they choose for each project." |
| 156 or less, 178, 180–182, 198, 200 | Obsolete (pre-NS, Museek+, PySoulSeek, old Nicotine) | Do not reuse. |

Recommendation (detailed in §9): Selecta logs in as **177 / <Selecta minor>** until it has a reserved number, and asks
the Nicotine+ maintainers (who keep this table) to reserve one before the first live session (M9).

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

All accessed 2026-10-19.

| Source | Used for |
|---|---|
| https://www.slsknet.org/news/node/681 (Rules) | Third-party client clause, bots, 5 clients per IP, admin powers |
| https://www.slsknet.org/news/node/682 (Terms of Use) | Flooding/probing prohibitions, account termination |
| https://www.slsknet.org/news/faq-page (FAQ) | Username recycling, IP bans, non-sharer policy |
| https://www.slsknet.org/news/node/523 (Nir, 2012) | SoulseekQt empty-share warning, refusal of auto-bans |
| nicotine-plus/nicotine-plus @ f3b9d4d, `doc/SLSKPROTOCOL.md` | Version registry, login rejection reasons, wishlist interval, excluded phrases, denial strings |
| fiso64/sockseek (formerly slsk-batchdl/sldl) @ ff9a3ac, README + issues #12, #95, #128, #163, #165 | Search limiter origin, ban reports, banned terms, share-count spoofing dispute |
| aioslsk @ a90ad82, `src/aioslsk/client.py` | Login version 175/1 |
