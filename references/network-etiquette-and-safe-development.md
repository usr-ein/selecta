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

The official server is closed source and publishes no limits. Everything below comes from what clients observe, from
the server's own ban message, and from Soulfind (an open-source server whose maintainers — the Nicotine+ developers —
deliberately mimic the official server's behaviour; their comments are cited as such).

### 4.1 The automatic flood ban (verbatim)

When a client does "too many operations at once", the server sends a **private message from the pseudo-user
`server`** and drops the connection. The text, as reported verbatim by two independent users (slskd web UI "Chat"
screen, soularr discussion #142, 2025-11-10; Lidarr.Plugin.Slskd issue #19, 2026-10-03):

> System Message: You have been banned for 30 minutes. This is usually the result of doing too many operations at once.
> Do not flood chat rooms or private chat with many messages. Do not repeat the same text several times in a row. Do
> not quickly repeat a search. Do not connect and disconnect quickly. Do not connect many clients at once from the same
> IP. This list is by no mean exhaustive. You will be able to reconnect to the service in 30 minutes. NOTE: This is an
> automatic message, dont try to answer it.

This is the closest thing to an official rate-limit policy. It names five triggers — message flooding, repeated text,
**quickly repeated searches**, **rapid connect/disconnect**, **many clients per IP** — and a fixed **30-minute**
duration. Nicotine+ already special-cases private messages from the user `server` (`privatechat.py`,
`SERVER_USERNAME = "server"`); Soulfind sends its admin replies the same way (`defines.d`: `server_username = "server"`).

### 4.2 Ban and disconnect signals

| Situation | What the client sees | Correct client reaction | Confidence · source |
|---|---|---|---|
| Automatic flood ban | PM from `server` starting "System Message: You have been banned for 30 minutes…", then disconnect | Parse it; stop *all* automatic network activity; schedule the next login no earlier than **30 min + jitter**; surface it in the UI and the activity log | High · soularr #142, Lidarr.Plugin.Slskd #19 |
| Logging in while banned | **No login response at all**; Soulfind closes the socket after 60 s + 0–15 s jitter, "the official server doesn't send a response when a user is banned" | Treat a login timeout as a *possible ban*: back off exponentially from minutes, not seconds | High · N+ `SLSKPROTOCOL.md` ("`BANNED` … No longer sent, server omits login response instead"); Soulfind `src/server/user.d` |
| Same account logs in elsewhere | `Relogged` (server code 41), then disconnect | **Never reconnect automatically** (PRD NET-4) | High · N+ `slskproto.py` sets `_manual_server_disconnect`; slskd `KickedFromServerException` path does not start the watchdog |
| Login rejected | `LoginResponse` failure with `INVALIDUSERNAME` (+ detail), `EMPTYPASSWORD`, `INVALIDPASS`, `INVALIDVERSION`, `SVRFULL`, `SVRPRIVATE` | No automatic retry except `SVRFULL` (long backoff). `INVALIDVERSION` means the version fields are wrong — never "try other numbers" | High · N+ `slskmessages.py` `LoginRejectReason`, Soulfind `messages.d` |
| Admin kill | Plain disconnect, no reason | Normal reconnect backoff; if it repeats, stop and tell the user | High (rule text: admins "can kill any connections without prior notice") |
| Global admin notice | `AdminMessage` (server code 66) | Show it | High · N+ doc |
| Search returns nothing for a popular query | Silence: no error, no results | Do not "work around" (see §7.3); say in the UI that the query may be filtered | Medium · sockseek #128; Soulfind models *server-side* phrase filtering (`is_search_query_filtered` silently drops the search) |
| Longer / IP-level bans | Logins keep timing out across accounts on one IP | Stop; manual only | Low–medium · sockseek #95 (user switched IP after "the one I used before got banned"); a Google Groups user says a "30 minute" search ban lasted months (hearsay, 2023); FAQ mentions IP bans as the admins' last resort |

Admins also operate escalations we cannot observe (account termination, ISP notification per the Rules). There is
no documented appeal path.

### 4.3 Search rate: what is actually known

| Evidence | What it says | Implied rate | Confidence |
|---|---|---|---|
| slsk-batchdl/sockseek author, issue #12 (2023-11): "You get banned for 30 minutes if you search too many times in a short timespan. I don't know the exact rate limits though." … "From my own testing it seems to be around **34 searches every 220 seconds**." | Became the defaults `SearchesPerTime = 34`, `SearchRenewTime = 220` (`Sockseek.Core/Settings/EngineSettings.cs`) | ≈ 9.3/min, but see the window caveat below | Medium: one person's black-box test, 3 years old, threshold not re-verified since |
| Same release added `--random-login` "in case you are still getting banned" | Evasion by throw-away accounts — explicitly what the Nicotine+ doc calls "unacceptable" | — | High (as a "never do") |
| sockseek issue #95 (2025-04): user banned after 10–20 downloads; "workaround" was **1 search per minute** plus a new IP | Even 34/220 s can be too much in practice (that tool runs up to 4 searches per track in `--desperate` mode and 2 concurrently) | < 9/min sustained is not safe | Medium (single report) |
| soularr discussion #142 (2025-11): banned "regularly" with sequential album searches, 5 s search timeout + 5 s sleep and no cooldown; soularr later added `minimum_search_interval = 5` and a result cache | Back-to-back sequential searches (~6/min) for long runs trigger bans | ~6/min sustained, many repeats of the same queries across runs | Medium |
| The ban message itself: "Do not quickly repeat a search" | **Repeating the same query** is a named trigger, independent of the global rate | Cache and de-duplicate | High |
| Nicotine+ (`search.py`) | No outgoing limiter for manual searches (human-paced); wishlist sends **one** term per server interval | — | High |
| Soulseek.NET (`SoulseekClientOptions`) | `maximumConcurrentSearches = 2`, `searchTimeout = 15000` ms, `responseLimit = 250`, `fileLimit = 25000` | ≤ 8/min if a caller chains searches | High (code) |

**Caveat on sockseek's limiter** (`Sockseek.Core/Search/RateLimitedSemaphore.cs`): it is a *fixed window* — all 34
permits come back at once when the window expires, and the window restarts at the moment of the refill. 34 searches at
the end of one window plus 34 at the start of the next can go out within seconds. Selecta should use a token bucket
or sliding window, so that no 220 s span (or any other span) ever exceeds the budget.

**Server-side search mechanics worth knowing** (Soulfind `src/server/server.d`, `defines.d`; modelled on the official
server, not guaranteed identical): queries longer than **256** characters are dropped; queries matching *server-side*
filter phrases are dropped silently; searches are batched and fanned out every **650 ms**.

### 4.4 Wishlist interval

Server code 104 (`WishlistInterval`) tells the client how often it may run a wishlist search: "almost always 12
minutes, or 2 minutes for privileged users" (N+ `SLSKPROTOCOL.md`); Soulfind uses the same values
(`wish_interval = 12.minutes`, `wish_interval_privileged = 2.minutes`). Nicotine+ schedules a repeating timer at that
interval and sends **one** wishlist term per tick, round-robin (`_do_next_wishlist_search`: "Search for a maximum of 1
item at each search interval"), using `WishlistSearch` (code 103). A 30-item wishlist therefore cycles every 6 h for
a normal user. This is the convention Selecta should follow (PRD NET-6, SRCH-14).

### 4.5 Connection and reconnect behaviour

| Client | First retry | Growth | Cap | Jitter | After `Relogged` | Source |
|---|---|---|---|---|---|---|
| Nicotine+ | random **5–15 s** ("to spread out connection attempts from Nicotine+ clients in case server goes down") | ×2 | stops doubling once ≥ 300 s (so 300–480 s) | initial value only | no auto-reconnect | `slskproto.py` `_set_server_timer` |
| slskd | ~0 s (attempt 1), then `floor((2^n − 1)/2)` s: 1, 3, 7, 15, 31, 63, 127, 255 | ×2 | **300 s** | + U(0, 1 s) every attempt | no auto-reconnect | `ConnectionWatchdog.cs`, `Common/Compute.cs` |
| Soulfind (server side) | — | — | a `kick` bans for 2 min by default; banned logins time out after 60–75 s | 0–15 s on login timeout "to spread out reconnect attempts" | — | `defines.d`, `user.d` |

"Do not connect and disconnect quickly" and "Do not connect many clients at once from the same IP" (ban message) plus
"a maximum 5 clients from the same IP" (Rules) mean: one session per machine, a first retry of several seconds, and no
test harness that spins up many logins against the live server.

### 4.6 Private-message and chat flooding

Repeated identical private messages trigger the same 30-minute ban. Users of Nicotine+'s auto-"please share" message
were banned several times a day for "sending the same message too often" (Google Groups, 2025-04-12; a 2025-05
thread describes trolls exploiting this). Nir Arbel's answer (2025-05-05) is the reference behaviour: SoulseekQt sends
its empty-share notice **once per user**, only when that user tries to download. Nicotine+'s leech-detector plugin
warns "too many message lines may get you temporarily banned for spam", and its `ResponseThrottle` (plugin chat
replies) refuses to answer the same request from the same user within 6 min, or any reply in the same room within
30 s — "some of the throttle logic is guesswork as server code is closed source". Selecta v1 sends no automatic
messages; if auto-replies are ever added, they need a per-user once-per-session rule.

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
