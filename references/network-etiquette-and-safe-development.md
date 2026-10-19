# Network etiquette and safe development

> Status: **complete draft v1** (2026-10-19; all sources accessed 2026-10-19). Remaining **TODO(verify)** items are
> listed in §10. Source repositories were read at the commits listed in §11.
> Confidence labels: **H** = code or official text, **M** = several consistent reports, **L** = judgement.

## 1. TL;DR

- **The rules only "tolerate" full-featured third-party clients**: "chat, search, wishlist, download, upload, and
  respect / recognition of privileges". Bots and "scripts otherwise failing to implement the full range of Soulseek
  features are not allowed to connect". Selecta's v1 gaps against that list: chat rooms (non-goal) and privileged-first
  uploads (not in the PRD). Both are cheap to close (§9.4 E1, E3).
- **The server's flood ban is explicit about its triggers.** Its automatic PM (from user `server`): "You have been
  banned for 30 minutes. This is usually the result of doing too many operations at once … Do not quickly repeat a
  search. Do not connect and disconnect quickly. Do not connect many clients at once from the same IP." A banned login
  then gets **no response at all**.
- **The only search-rate figure is empirical**: slsk-batchdl's author measured "around 34 searches every 220 seconds"
  (2023) and made it the default; automated tools at or near that rate still report bans, and one (weakly sourced)
  client puts the trigger at ~28 searches per 10 min. Selecta's proposed default is **2 searches/min with a burst of
  6** (≤ 13 per 220 s, ≤ 26 per 10 min), one shared budget for every search source, no user setting to loosen it, and
  no repeat of the same query within 10 min.
- **Identify honestly**: the community version registry reserves login major versions per client; **177** is for
  experimental clients. Ask for a Selecta number before going live; never use 157 (official) or another client's.
- **Peers enforce etiquette themselves**: one upload per user at a time, per-user queue limits (Nicotine+ 100 files),
  leech detectors that check your share counts *and browse you*, exact denial strings (`Too many files` is retryable;
  `Banned`/`File not shared.` are not). Selecta: 1 active and ≤ 20 queued files per peer, a daily per-peer cap,
  true share counts reported before any download is queued.
- **Previews: cancel early.** Cancelling frees the uploader's slot for the next person (Nicotine+ starts the next
  upload at once); finishing unwanted files is worse. But slskd counts cancelled uploads against optional failure
  limits, and auto-preview bursts look like flooding: debounce, one preview at a time, free-slot results only.
- **There is no official test server.** Develop against Soulfind (which mimics the official ban behaviour but has no
  flood detection and no distributed network) plus reference peers, with a counting proxy as a "referee" in CI.

## 2. Key findings

| # | Finding | Conf. | Source |
|---|---|---|---|
| K1 | Third-party clients are tolerated only if they implement chat, search, wishlist, download, upload and privileges; bots and partial scripts may not connect. | H (verbatim rule) | slsknet.org Rules |
| K2 | Max **5 clients from the same IP**; admins can kill connections "for any reason". | H | Rules |
| K3 | ToS §12 forbids "overloading, flooding, spamming" and probing/testing the service without authorisation. | H | Terms of Use |
| K4 | The automatic ban is **30 minutes**, announced by a PM from `server` listing its triggers (repeated searches, connect/disconnect churn, many clients per IP, chat/PM flooding, repeated text). | H (two independent verbatim reports, 2025–2026) | soularr #142; Lidarr.Plugin.Slskd #19 |
| K5 | Banned logins get no response at all (`BANNED` is no longer sent); Soulfind copies this. | H | N+ `SLSKPROTOCOL.md`; Soulfind `user.d` |
| K6 | slsk-batchdl's 34 searches / 220 s is one developer's black-box measurement (2023), implemented as a burst-prone fixed window; tools near that rate still get banned; one weak source says ~28 per 10 min. | M | slsk-batchdl #12, #95; soularr #142; Orbit-pure notes |
| K7 | Login major versions form a registry (157 official, 160 N+, 169 seeleseek, 170 Soulseek.NET, 175 aioslsk, 176 soulseek-rs, **177 experimental**); random usernames are "unacceptable". | H (community doc, not official) | N+ `SLSKPROTOCOL.md` |
| K8 | Wishlist interval comes from the server: "almost always 12 minutes, or 2 minutes for privileged users"; clients send one term per interval. | H | N+ doc and `search.py`; Soulfind `defines.d` |
| K9 | Uploaders serve one file per user at a time and cap per-user queues (N+ 100 files / 10 000 MiB; SoulseekQt per-user limit on by default; slskd optional queued/daily/weekly/failure limits). | H | N+ `config.py`/`uploads.py`; Qt changelog; slskd docs |
| K10 | Leech detection uses server-reported share counts and then browses you; faking counts was called "bad faith" by the Soulseek.NET author. | H | N+ leech_detector; slsk-batchdl #165 |
| K11 | Two content filters exist: client-side excluded phrases (code 160, since ~2024, results must be filtered) and silent server-side query filtering (substring match, unpublished list). slsk-batchdl evades the latter with wildcards — Selecta must not. | H / M | Soulseek.NET README; N+ `search.py`; sockseek `Searcher.cs`, #128; Soulfind |
| K12 | Cancelling a transfer: N+ uploader marks it cancelled, sends `UploadFailed` (to be ignored if intentional) and immediately serves the next user; slskd records it as failed. | H | N+ `uploads.py`; slskd `TransferStateCategories.cs` |
| K13 | No official or public test server exists; Soulfind is the recommended local server; logging in with a new username creates an account. | H | N+ maintainer (2025-09); N+ doc; Soulfind |

## 3. Official rules and terms

### 3.1 What exists

There is no API policy or developer programme. The binding texts are two pages on slsknet.org, plus community
conventions maintained by the Nicotine+ developers (who are the de-facto keepers of protocol documentation).

| Document | What it says that matters to Selecta |
|---|---|
| **Rules** (`slsknet.org/news/node/681`, undated) | "Access to the SoulSeek® server is a privilege; not a right. It may be revoked at any time, for any reason." · "Server or chatroom abuse will result in your connection being closed and your ISP notified." · "Spammers, automated clients (robot/bot), combinations of such, or scripts otherwise failing to implement the full range of Soulseek® features are not allowed to connect to the Soulseek® service." · "Alternative clients for platforms other than Microsoft(r) Windows(r) that implement the full range of features of the Soulseek® Network (including chat, search, wishlist, download, upload, and respect / recognition of privileges) are tolerated." · "Users are allowed to connect up to a maximum 5 clients from the same IP." · Administrators (nir, OrtaliGoddessOfPrivileges, Proteus93, arsenic_, sierracat) "can kill any connections without prior notice for any reason." |
| **Terms of Use** (`slsknet.org/news/node/682`, © 2007 Soulseek, LLC) | §2: you may not use someone else's account. §8: accounts may be terminated without notice for abuse or for "interfer[ing] with use of the Site or the Service by others". §12(b): no "attempting to probe, scan, or test the vulnerability of the Service … without proper authorization". §12(c): no "interfering … with service to any user, host, or network, including … overloading, 'flooding,' 'spamming' …". §3 forbids reverse-engineering *their software*; Selecta's protocol work relies only on open third-party code and documentation. |
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

The server does act on these numbers: a zero major or minor is rejected with `INVALIDVERSION` (Soulfind), and the
aioslsk server notes record that a too-old client version gets a PM from `server`: "Your connection is restricted: You
cannot search or chat. Your client version is too old. You need to upgrade to the latest version." (threshold unknown;
`docs/source/SOULSEEK.rst`). Selecta must recognise that message and stop, not retry with other numbers.

**Is batch search a "bot"?** The Rules ban "automated clients (robot/bot)". Selecta's batch and wishlist are
user-initiated features of a full client (Nicotine+ and SoulseekQt have wishlists), they are paced by the governor and
nothing downloads without the user's approval (BATCH-5/6). Keeping "review first" and conservative pacing is what keeps
these features on the client side of that line. (Inference.)

## 4. Server-side limits and bans

The official server is closed source and publishes no limits. Everything below comes from what clients observe, from
the server's own ban message, and from Soulfind (an open-source server whose maintainers — the Nicotine+ developers —
deliberately mimic the official server's behaviour; their comments are cited as such).

### 4.1 The automatic flood ban (verbatim)

When a client does "too many operations at once", the server sends a **private message from the pseudo-user
`server`** and disconnects the client (the text says "You will be able to reconnect … in 30 minutes"; the order of
message and disconnect is not documented). The text, as reported verbatim by two independent users (slskd web UI "Chat"
screen, soularr discussion #142, 2025-11-10; Lidarr.Plugin.Slskd issue #19, 2026-10-03):

> System Message: You have been banned for 30 minutes. This is usually the result of doing too many operations at once.
> Do not flood chat rooms or private chat with many messages. Do not repeat the same text several times in a row. Do
> not quickly repeat a search. Do not connect and disconnect quickly. Do not connect many clients at once from the same
> IP. This list is by no mean exhaustive. You will be able to reconnect to the service in 30 minutes. NOTE: This is an
> automatic message, dont try to answer it.

This is the closest thing to an official rate-limit policy. It names five triggers — message flooding, repeated text,
**quickly repeated searches**, **rapid connect/disconnect**, **many clients per IP** — and a fixed **30-minute**
duration. Nicotine+ already special-cases private messages from the user `server` (`privatechat.py`,
`SERVER_USERNAME = "server"`); Soulfind sends its admin replies the same way (`defines.d`: `server_username = "server"`);
aioslsk's server notes: "The server will send private chat messages to report errors and information back to the
client". **Conflict:** one small client (MeshDigital/Orbit-pure, `Services/SoulseekAdapter.cs`) assumes the ban text
arrives as a global `AdminMessage` (code 66). The slskd screenshots show it in the private chat with `server`, which
is the stronger evidence; Selecta's ban detector should match the text in **both** channels.

### 4.2 Ban and disconnect signals

| Situation | What the client sees | Correct client reaction | Confidence · source |
|---|---|---|---|
| Automatic flood ban | PM from `server` starting "System Message: You have been banned for 30 minutes…", then disconnect | Parse it; stop *all* automatic network activity; schedule the next login no earlier than **30 min + jitter**; surface it in the UI and the activity log | High · soularr #142, Lidarr.Plugin.Slskd #19 |
| Logging in while banned | **No login response at all**; Soulfind closes the socket after 60 s + 0–15 s jitter, "the official server doesn't send a response when a user is banned" | Treat a login timeout as a *possible ban*: back off exponentially from minutes, not seconds | High · N+ `SLSKPROTOCOL.md` ("`BANNED` … No longer sent, server omits login response instead"); Soulfind `src/server/user.d` |
| Same account logs in elsewhere | `Relogged` (server code 41), then disconnect | **Never reconnect automatically** (PRD NET-4) | High · N+ `slskproto.py` sets `_manual_server_disconnect`; slskd `KickedFromServerException` path does not start the watchdog |
| Login rejected | `LoginResponse` failure with `INVALIDUSERNAME` (+ detail), `EMPTYPASSWORD`, `INVALIDPASS`, `INVALIDVERSION`, `SVRFULL`, `SVRPRIVATE` | No automatic retry except `SVRFULL` (long backoff). `INVALIDVERSION` means the version fields are wrong — never "try other numbers" | High · N+ `slskmessages.py` `LoginRejectReason`, Soulfind `messages.d` |
| Admin kill | Plain disconnect, no reason | Normal reconnect backoff; if it repeats, stop and tell the user | High (rule text: admins "can kill any connections without prior notice") |
| Restricted connection (old client version) | PM from `server`: "Your connection is restricted: You cannot search or chat. Your client version is too old…" | Stop searching and chatting; show it; fix the version fields | Medium · aioslsk `docs/source/SOULSEEK.rst` ("The server will send private chat messages to report errors and information back to the client") |
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
| MeshDigital/Orbit-pure (C#, 2026), `DOCS/memory/network_resilience_and_library_reconciliation.md`: the ban comes "when the client fires too many searches in a short window (~28 searches per 10 min)" | Lowest figure found; no method or evidence given (an AI-agent memory file in a one-star repo) | ~2.8/min over 10 min | Low — but cheap to respect |
| The ban message itself: "Do not quickly repeat a search" | **Repeating the same query** is a named trigger, independent of the global rate | Cache and de-duplicate | High |
| Nicotine+ (`search.py`) | No outgoing limiter for manual searches (human-paced); wishlist sends **one** term per server interval | — | High |
| Soulseek.NET (`SoulseekClientOptions`) | `maximumConcurrentSearches = 2`, `searchTimeout = 15000` ms, `responseLimit = 250`, `fileLimit = 25000` | ≤ 8/min if a caller chains searches | High (code) |
| slskd (`SearchesController.cs`) | No rate limit: the API only refuses a *concurrent* start request (HTTP 429, "Only one concurrent operation is permitted") | Whatever the automation in front of it sends — hence the Lidarr/soularr ban reports | High (code) |

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
| Nicotine+ | random **5–15 s** ("to spread out connection attempts from Nicotine+ clients in case server goes down") | ×2 | stops doubling once ≥ 300 s (final wait 300–600 s, depending on the random start) | initial value only | no auto-reconnect | `slskproto.py` `_set_server_timer` |
| slskd | ~0 s (attempt 1), then `floor((2^n − 1)/2)` s: 1, 3, 7, 15, 31, 63, 127, 255 | ×2 | **300 s** | + U(0, 1 s) every attempt | no auto-reconnect | `ConnectionWatchdog.cs`, `Common/Compute.cs` |
| Soulfind (server side) | — | — | a `kick` bans for 2 min by default; banned logins time out after 60–75 s | 0–15 s on login timeout "to spread out reconnect attempts" | — | `defines.d`, `user.d` |

"Do not connect and disconnect quickly" and "Do not connect many clients at once from the same IP" (ban message) plus
"a maximum 5 clients from the same IP" (Rules) mean: one session per machine, a first retry of several seconds, and no
test harness that spins up many logins against the live server.

### 4.6 Private-message and chat flooding

Repeated identical private messages trigger the same 30-minute ban. A user of an automatic "please share" warning
was banned several times a day for "sending the same message too often" (Google Groups, 2025-04-12); a 2025-05
thread (Nicotine+ user) describes trolls exploiting this by repeatedly triggering the warning. Nir Arbel's answer (2025-05-05) is the reference behaviour: SoulseekQt sends
its empty-share notice **once per user**, only when that user tries to download. Nicotine+'s leech-detector plugin
warns "too many message lines may get you temporarily banned for spam", and its `ResponseThrottle` (plugin chat
replies) refuses to answer the same request from the same user within 6 min, or any reply in the same room within
30 s — "some of the throttle logic is guesswork as server code is closed source". Selecta v1 sends no automatic
messages; if auto-replies are ever added, they need a per-user once-per-session rule.

## 5. Peer-side etiquette

### 5.1 Why it matters more on Soulseek than on BitTorrent

Soulseek has no swarming: every file comes from **one** uploader, start to finish, through that uploader's limited
upload slots. Every file Selecta takes from a peer is a slot-time that peer's other downloaders wait for. Most
clients also give **one active upload per user** at a time (Nicotine+ removes a user from the round-robin while one of
their uploads is active — `uploads.py` `_activate_transfer`/`_update_user_counter`; Soulseek.NET
`MaximumConcurrentUploadsPerUser = 1`), so asking one peer for several files at once only lengthens *their* queue.

The culture has a long "leecher" grievance: the official FAQ concedes that "Soulseek has a reputation for mean,
pedantic users that have a great list of 'dos and don'ts' in their userinfo". The author refused automatic bans of
non-sharers as "too harsh" (2012) and built a polite warning instead; uploaders built the bans themselves.

### 5.2 How uploaders enforce their rules (what Selecta will run into)

| Client (as uploader) | Slots | Per-user queue limits | Order | Privileged users | Leecher handling | Source |
|---|---|---|---|---|---|---|
| **Nicotine+** (defaults) | 3 upload slots (`useupslots`, `uploadslots = 3`); alternative mode: accept new uploads while total < 50 KiB/s | **100 files** (`filelimit`) and **10 000 MiB** (`queuelimit`) per user → `Too many files` / `Too many megabytes`; buddies can be exempt | Round-robin by "user who waited longest since their last download" (`fifoqueue = False`) | First, ahead of everyone (server list + optionally prioritised buddies) | Optional *Leech detector* plugin: threshold ≥ 1 file in ≥ 1 folder; checks server stats, **browses you** to verify ("SoulseekQt only sends the number of shared files/folders to the server once on startup"), then private-messages you after the transfer. Third-party *Ban-SoulseekLeechers* plugin bans outright. Custom ban text default: "Banned, don't bother retrying" | `pynicotine/config.py`, `uploads.py`, `plugins/leech_detector` |
| **slskd** (defaults) | 10 global slots; unlimited group slots | None by default; group limits for *queued*, *daily*, *weekly* files / MB and **failed transfers** | Groups by priority (privileged = 0), round-robin inside a group | Priority 0, FIFO, "not subject to any limits" | Built-in `leechers` group = fewer than 1 shared file or 1 directory. The docs' example config: **1 slot, 100 KiB/s, 15 queued files / 150 MB, 30 files / 300 MB / 10 failures per day, 150 files / 1.5 GB / 30 failures per week** | `src/slskd/Core/Options.cs`, `docs/config.md`, `Application.cs` |
| **SoulseekQt** (official; behaviour from the changelog) | User-set | "Maximum uploads per user limit on by default to prevent mass queuing" (2014-02-14); "Maximum queued MB per user setting" (2013-04-10) | Place in line returned to downloaders (2012) | Privileged first (donation page) | Optional background browse of every downloader + one warning message if the share is empty (2012; "only gets sent once per user", only on a download attempt — Nir, 2025-05-05) | slsknet.org changelog, node/523, Google Groups |

Example values in slskd's docs are illustrative, not defaults, but they show the scale uploaders think in: **tens of
files per day** for a stranger who shares nothing, **~150 queued files** for an ordinary user.

### 5.3 Denial strings and what Selecta must do with each

| Reason (exact string) | Meaning | Retryable? | Selecta reaction | Source |
|---|---|---|---|---|
| `Queued` | Accepted, waiting for a slot | — | Show place in queue; poll it at most every 5 min (Nicotine+ does every 300 s) | N+ `downloads.py` |
| `Too many files` / `Too many megabytes` | Per-user queue limit hit | **Yes** — "these messages are retryable, while anything else is not" (slskd) | Keep the rest *locally* queued; learn the limit (Nicotine+ uses `max(5, queued − 1)`); send more only as earlier files finish. SoulseekQt does the same (2015-06-12) | slskd `Application.cs`; N+ `_upload_denied`; Qt changelog |
| `File not shared.` | Not shared — **also what SoulseekQt sends instead of `Banned`** | No (one latin-1 retry for very old clients, as N+ does) | Fail the item; count it in the peer's record | N+ `SLSKPROTOCOL.md` |
| `Banned` (or any custom text such as "Banned, don't bother retrying", "Sorry, your country is blocked") | You are banned by this peer | **Never** | Stop all requests to that peer (queue, browse, user info); mark the peer "banned you" with the date; demote/hide its results (FAIR-4) | N+ `config.py` |
| `Pending shutdown.` | Uploader is closing | Later | Retry after ≥ 15 min (N+ retries in its 3-minute cycle; Selecta should be gentler) | N+ `TransferRejectReason` |
| `Cancelled` | Uploader cancelled | Not automatically | Mark failed; user may retry once | N+ |
| `File read error.` | Uploader-side I/O error | Rarely | Retry at most a few times, ≥ 15 min apart (N+: every 15 min) | N+ |
| `Disallowed extension`, `Remote file error`, `Blocked country`, `User limit of x …` | Legacy (NS / old N+) | No (`User limit …` = treat like "Too many …") | As above | N+ `SLSKPROTOCOL.md` |
| `UploadFailed` (peer code 46) after *we* closed the connection | Uploader asks us to re-queue an abruptly ended transfer | Only if we did not cancel on purpose — "If the transfer was intentionally cancelled, the peer should ignore this message." | Ignore it for cancelled previews | N+ `uploads.py` `_file_connection_closed` |

When an uploader later offers a file we no longer want (peer `TransferRequest`, direction = upload), answer at once
with `TransferResponse(allowed = false, reason = "Cancelled")` — or `"Complete"` if we already have it, as SoulseekQt
does — so the uploader moves on to the next user (N+ `_transfer_request_downloads`).

### 5.4 Downloader behaviour that annoys uploaders

| Behaviour | Why it is a problem | Evidence |
|---|---|---|
| Sharing nothing, or faking share counts | Triggers leech detectors and bans; faking was called "bad faith" by the Soulseek.NET author and pushed back by Nicotine+ maintainers | slsk-batchdl #165 (2026-03); N+ leech detector |
| Mass-queuing a whole share or discography | Monopolises the uploader; why SoulseekQt turned a per-user limit on by default | Qt changelog 2014-02-14; FAQ "dos and don'ts" |
| Re-queuing immediately after a denial or a failure | Endless requeue cycles; SoulseekQt caps automatic retries at "three times every five minutes" | Qt changelog 2013-10-05 |
| Several concurrent requests to one peer | Most uploaders serve one file per user at a time anyway; extra requests just pile up in their queue | N+ `uploads.py`; Soulseek.NET option |
| Polling place-in-queue or user info aggressively | Pointless load; N+ polls every 5 min | N+ `downloads.py` |
| Private messages asking for slots / auto-messages | Repeated identical PMs earn a 30-min server ban | §4.6 |
| Queue-jumping (cancel and re-queue, or multiple accounts) | Explicit PRD non-goal; multiple accounts also hit the 5-per-IP rule | PRD §4; Rules |
| Sending `QueueUpload` before our own shares are loaded | The uploader's leech check sees 0 files. Nicotine+ holds queue messages locally "until our shares have initialized, to prevent invalid messages about not sharing any files" | N+ `downloads.py` `_enqueue_transfer` |

### 5.5 Self-limits Selecta should impose

Proposed values are in the table in §9.1. The principles: one active transfer per peer; a small number of files
queued per peer (well under the 15-file "leecher" example and far under Nicotine+'s 100); a daily per-peer cap that
keeps Selecta's footprint on any one person modest; no automatic retries after non-retryable denials; learn and
respect each peer's queue limit; global concurrency kept low enough that Selecta never holds many uploaders' slots at
once.

## 6. Previews: cancel mid-transfer or finish?

**Answer: cancel early — it is the considerate choice — but keep previews rare, serial and debounced.**

Facts:
- A preview is indistinguishable from a download on the wire: the protocol has no range or "preview" request, only a
  start offset for resuming. It takes a queue position and an upload slot like any download (so PREV-7 is right).
- Cancelling an active transfer means closing the file connection. Nicotine+ as uploader then marks the upload
  `Cancelled`, sends `UploadFailed` (which the downloader must ignore if it cancelled on purpose) and **immediately
  starts the next user's upload** (`uploads.py` `_file_connection_closed` → `_check_upload_queue`). The slot is freed
  sooner than if Selecta had finished a file it does not want; the bytes not sent are bandwidth saved.
- slskd records such an upload in a *failed* state — reading Soulseek.NET's upload loop, a remote disconnect
  surfaces as an exception and ends as `Completed | Errored`; `Cancelled`, `Errored` and `Aborted` are all in
  `TransferStateCategories.Failed` — and **counts it against optional per-user daily/weekly failure limits** (none by
  default; the docs' example allows a leecher 10 failures per day). Many cancelled previews from one slskd peer can
  therefore get Selecta refused by that peer for a day or a week.
- The mirror case matters too: when a download (or a kept preview) *completes*, close the file connection promptly.
  Uploaders wait for the receiver to disconnect — Soulseek.NET: "ideally the receiving end disconnects; this way we
  know they've gotten all of the data", lingering up to `MaximumLingerTime` otherwise (`SoulseekClient.cs`).
- No source found treats an early cancel as rude; no client bans on it. The complaints in the community are about
  hoarding slots and mass-queuing, which finishing unwanted files makes *worse*. (Confidence: medium — absence of
  evidence, plus the mechanics above.)

Risks specific to Selecta's keyboard audition (PREV-4):
- **Bursts.** Holding ↓ with auto-preview on would fire a peer connection (via `GetPeerAddress`/`ConnectToPeer` on
  the server) and a `QueueUpload` for every row. Server-side operations in bursts are exactly what the flood ban
  punishes ("too many operations at once"). Debounce and cap.
- **Queue pollution.** A preview requested from a busy peer sits in their queue after the user has moved on. There is
  no "dequeue" message; when the uploader offers it, Selecta must answer `Cancelled` immediately (§5.3).
- **Churn on one peer.** Previewing many files from the same peer in a row (start, cancel, start, cancel) looks like
  slot-hogging and accumulates slskd failures. Prefer one preview per peer at a time and count previews in the
  per-peer budget.

Recommended preview policy (defaults; see §9.1): auto-preview starts only after the selection has rested
**≥ 600 ms**; at most **1 active preview transfer** (plus the user's normal downloads); a preview of a peer that is busy
with another of our transfers is not started (show "peer busy"); previews count in the per-peer daily cap and in a
global **preview budget (≤ 30 per 10 min)**; cancel the transfer as soon as the user moves on (PREV-6 default stays
**cancel**), unless more than ~85 % is already received, in which case finishing costs the uploader almost nothing
and saves a later re-download if the user keeps it. These numbers are judgement calls (confidence: low–medium) and
should be tuned on the local test network, then in the M9 live beta.

Keep the partial data of a cancelled preview in the preview cache. If the user later presses Keep or Download, Selecta
re-queues the file and **resumes from the cached offset** (the protocol's start offset exists for exactly this), so
the uploader never sends the same bytes twice.

## 7. Privileges, sharing and reputation, excluded search phrases

### 7.1 Privileges

- Bought by donating. The official donation page (quoted by a Nicotine+ maintainer, 2025-09-09): for $5 or more
  "your username will become permanent", and "you'll receive a minimum of one month of privileges. While your
  privileges are active, your downloads will be queued ahead of those of non-privileged users." 7–10 days per dollar;
  days can be gifted.
- On the protocol: the server sends the list of privileged users (`PrivilegedUsers`, code 69) and updates;
  `CheckPrivileges` returns our remaining time; privileged users get a **2-minute** wishlist interval instead of 12.
- "Respect / recognition of privileges" is one of the six features the Rules require of third-party clients.
  **Selecta's upload queue must serve privileged users first** (Nicotine+: privileged ahead of everyone; slskd:
  priority 0, FIFO, no limits). This is missing from the PRD (SHARE-3) and should be added.
- Selecta must never pretend to be privileged or reorder anyone's queue; it can show the user's own privilege status.

### 7.2 Sharing and reputation

- Uploaders judge you by (a) the share counts *you* report to the server (`SharedFoldersFiles`), (b) browsing your
  share, (c) your behaviour in their queue. Report **true counts**, as soon as the share index is loaded, and never
  queue downloads before then (§5.4).
- Answer browse and folder-contents requests reliably even for users you are downloading from: SoulseekQt and the
  Nicotine+ leech detector both browse downloaders to check them.
- The PRD's ratio display (FAIR-3) is the right social lever: there is no global reputation system on Soulseek, only
  each uploader's local view of you.
- Ignoring/banning is per client; there is no network-wide blacklist. Be careful with automatic bans of others (Nir's
  argument: many non-sharers are new, or share privately).
- Carry your weight in the distributed search network too: join it (needed to receive searches at all), and accept
  children up to what the server allows — the server's `ParentSpeedRatio` (code 84) sets "the number of children we
  can have" from our upload speed; Soulseek.NET/slskd cap at 25 children by default (`distributedChildLimit`).
  Details belong to the protocol and sharing notes.

### 7.3 Excluded search phrases and filtered queries

Two different mechanisms exist; both silently remove results:

| Mechanism | How it works | What Selecta must do |
|---|---|---|
| **Client-side excluded phrases** (server code 160 `ExcludedSearchPhrases`, sent at login, "starting around the beginning of 2024" per the Soulseek.NET README) | Each responding client must leave out any file whose *path* contains a listed phrase (case-insensitive substring; Nicotine+ checks `phrase in file_path.lower()` in `search.py`). The Soulseek.NET author: outgoing results "*must* be filtered". | Filter our own search responses (SHARE-2). On the search side, warn when a query contains an excluded phrase — compliant peers will return nothing — and do not spend search budget on it. |
| **Server-side filtered queries** | The server silently drops searches that contain certain substrings — even inside other words ("Eclipse" contains a duo's name, "NANORAY" contains a film title) according to slsk-batchdl's author (issue #128, 2025-08: "The soulseek server is blocking searches which contain certain substrings … to appease copyright trolls"). Soulfind models this as "server-side phrases". The list is not published. | Accept it. When a query returns zero results, say "No results (some queries are filtered by the server)". |

**Never evade either mechanism.** sockseek ships a hard-coded `bannedTerms` list and rewrites matching queries by
replacing the first letter with a wildcard (`*eatles`) to slip past the server filter ("copyright is joke", comment in
`Searcher.cs`). That is circumvention of a server rule, which the PRD excludes (§4 non-goals) and which risks the
account. Selecta must not do it, nor relay results that it knows match an excluded phrase.

## 8. Safe development

### 8.1 There is no official test server

None exists, and no other public Soulseek-compatible server is known: "The client can connect to any Soulseek®
compatible server (however none are presently known to exist other than the official one)" (Nicotine+ maintainer
slook, 2025-09-09). The protocol doc points developers to **Soulfind**: "an open source implementation (unrelated to
the official proprietary Soulseek server). It handles the protocol well enough for development and testing of client
implementations." Soulfind's own README: it "exists for local testing, and should not be used in production."

Also note: **logging in with an unknown username creates an account** (Soulfind registers on first login unless in
private mode, which answers `SVRPRIVATE`; the official server behaves the same way, and recycles unused names after
30 days per the FAQ). A typo, or a test that generates usernames, would create accounts on the live server — the
"randomly generated usernames" the protocol doc calls unacceptable.

### 8.2 The local test network (`devenv/`)

| Component | Role | What it models | What it does **not** model | Source |
|---|---|---|---|---|
| **Soulfind** (D, GPL-3.0-or-later; `ghcr.io/soulfind-dev/soulfind`, port 2242; `soulsetup` CLI; admin commands by PM to user `server`) | The server | Login and registration, rooms, PMs, user/room/wishlist search; search fan-out to all users (server code 26, batched every 650 ms); queries > 256 chars dropped; **client-side excluded phrases** and **server-side query filters** (configurable in `soulsetup` → "Search filters"); "unsearchable" users; privileges; **bans** (no login response, timeout 60 s + 0–15 s), **kick [minutes]** (default 2 min), `kickall`; wishlist interval 12 / 2 min; `Relogged` | **Distributed network** (parent/child messages are accepted but "unused in Soulfind"); **automatic flood bans** (no rate limiting found in the source); any undocumented heuristics of the official server | Soulfind @ 5a256f5: `README.md`, `src/defines.d`, `src/server/{server,user,msghandler,cmdhandler}.d` |
| **aioslsk mock server** (Python, GPL-3.0-or-later, `tests/e2e/mock/`) | Second server for distributed-network tests | Pluggable distributed strategies (`EveryoneRootStrategy`, potential-parent jobs), excluded phrases, wishlist interval | Same as above for flood bans | aioslsk @ a90ad82 |
| **Reference peers**: Nicotine+ (configurable server host:port), slskd (Docker; `soulseek.address`/`port`), scripted aioslsk peers | Interop and adversarial behaviour | Real queue/slot/denial behaviour (Nicotine+ 3 slots, 100-file limit; slskd groups and failure limits); scripted peers can deny with each reason string, stall, disconnect mid-transfer, send `UploadFailed`, run a leech check | Official SoulseekQt (closed, cannot be pointed at another server reliably — TODO(verify)) | §5.2 |
| **`selecta-cli` instances** | Load and multi-peer scenarios | Everything Selecta does | — | PLAN M1/M2 |
| **Topology tricks** | NAT and failure paths | A peer with its listener disabled forces indirect connections (`ConnectToPeer` / `CantConnectToPeer`); optional `tc netem` for latency and bandwidth | — | inference |

Pin every image by digest; keep the whole network on an internal Docker network with no route to the internet if
possible (`internal: true` in Compose), so even a misconfigured peer cannot reach `server.slsknet.org`.

### 8.3 A "referee" in tests, because no local server punishes flooding

Neither Soulfind nor the mocks reproduce the official flood ban, so a test can pass while Selecta would get banned
live. Add two independent checks:

1. **Governor audit stream.** Every outbound server and peer message passes the governor, which emits
   `(timestamp, kind, peer)` events. Property tests (fake clock) assert that no rolling window ever exceeds the budget.
2. **Counting proxy.** A small TCP proxy between Selecta and Soulfind decodes message codes and fails the scenario if,
   for example, more than *N* `FileSearch`/`WishlistSearch` messages pass in any 220 s, more than one identical search
   passes within the cache TTL, more than *N* logins occur in 10 min, or an identical PM is repeated. It is
   independent of Selecta's own code, so a bug that bypasses the governor still fails CI.

The same proxy **records** sessions (codes + payloads, with usernames and paths replaced) as fixtures for codec
round-trip tests and fuzz seeds, and **replays** them for regression tests. Fixtures come only from the local network
(PLAN §5.3).

### 8.4 Code guardrails (in addition to PLAN §5.2)

- **Live-server lock checked after DNS resolution.** Debug/test builds allow only loopback, RFC 1918, link-local and
  ULA addresses; check the *resolved* address, not the hostname, so an `/etc/hosts` alias or an IP literal cannot
  slip through. As defence in depth, also refuse the known official endpoints by name: `server.slsknet.org:2242`
  (Nicotine+, Soulseek.NET, aioslsk, Seeker defaults), `server.slsknet.org:2416` (SoulseekQt, per aioslsk docs) and
  `vps.slsknet.org:2271` (slskd default) — the official service has several names and ports, so a name list alone is
  not enough. Release builds need a one-time explicit confirmation that names the Rules.
- **One instance per account and machine** (lock file): protects against "connect many clients at once from the same
  IP" and against `Relogged` ping-pong.
- **Ban detector → automatic pause.** Recognise the `server` PM ("You have been banned for …"), login timeouts and
  repeated disconnects; switch the global network pause on (SAFE-5), log it (SAFE-4), and require a user action after
  the cool-down to resume.
- **Identity constants in one file**: major/minor version, client name in user info; 177 in development until a number
  is reserved; never 157/160/170/175.
- **Explicit account creation.** The login screen distinguishes "log in" from "create account" and warns that an
  unknown username creates a new account; no code path ever invents a username.
- **No automatic messages** in v1 (no auto-replies, no leech warnings); PM sending goes through the governor with a
  per-recipient and global rate limit and an identical-text check.
- **Kill switch and ceilings are tested**: `static_assert(default <= ceiling)` for every limit; a test that every
  outbound message type is routed through the governor (e.g. the codec's send function is only reachable from it).

### 8.5 First controlled live tests (M9, run manually by the maintainer)

| Step | What | Pass criterion |
|---|---|---|
| 0 | Pre-flight: reserved version numbers (or 177 with the maintainers' blessing), true share counts, shares loaded, activity log on, all limits at their *minimum* defaults, network pause reachable | Checklist §9.3 complete |
| 1 | Log in, stay idle 30 min (pings, status, parent acquisition, incoming searches answered) | No disconnects; no `server` PMs |
| 2 | 1 manual search, then 5 manual searches over 10 min | Results arrive; budget indicator correct |
| 3 | 1 download from a peer with a free slot; 1 preview cancelled at ~30 % | Peer shows normal behaviour; no re-download after `UploadFailed` |
| 4 | A 10-line batch at default pacing | Finishes within the computed ETA; no denials beyond normal |
| 5 | Two weeks of normal use, daily log review | Zero bans (PRD §10). Raise at most one limit at a time, never above the ceiling |

Any ban message: stop, keep the logs, find the cause, halve the relevant budget, wait at least 24 h before trying
again. A ban is an IP-level event in practice; a second "test account" does not protect the maintainer's main account.

## 9. Implications for Selecta

### 9.1 Recommended default limits (input to ADR-006)

"Default" is what ships; "Ceiling" is the `constexpr` bound that settings can never exceed (PRD SAFE-2, CLAUDE.md:
"ceilings are compile-time constants, not settings"). This table assumes settings may loosen a limit from its default
up to the ceiling; under the strictest reading of SAFE-2 ("settings can only make limits stricter") the ceiling simply
equals the default — decision E8. All search sources (manual, batch, wishlist, user/room search, refresh) draw from
**one** search budget. Rates are enforced
as token buckets or sliding windows — never fixed windows (see §4.3). Confidence: **H** = backed by code or official
text, **M** = consistent with several reports, **L** = judgement, tune on the local network and in M9.

| Limit | Default | Ceiling | Justification | Source | Conf. |
|---|---|---|---|---|---|
| **Network searches** (all sources) | Token bucket: burst **6**, refill **1 per 30 s** (2/min; ≤ 13 in any 220 s, ≤ 26 in any 10 min) | Burst 8, refill ≥ 30 s (≤ 15 per 220 s, ≤ 28 per 10 min); not user-adjustable (E8) | 34/220 s is the *ban edge* measured once in 2023; soularr was banned at ~6/min sustained; a user still banned at 34/220 s switched to 1/min; one weak source says ~28 per 10 min. The default satisfies all of them. A human digging session rarely needs more than 2/min sustained, and the burst covers quick re-typing | §4.3 | M |
| **Same query repeated** | Not re-sent within **10 min** (cache answers; "Refresh" also obeys this) | ≥ 5 min | Ban message: "Do not quickly repeat a search" | §4.1 | H (trigger) / L (value) |
| **Query hygiene** | Drop 1-character tokens; refuse queries < 3 characters; refuse > 256 characters; warn on excluded phrases and do not send | Same | SoulseekQt drops one-character tokens (2015); Soulseek.NET `removeSingleCharacterSearchTerms`; N+ answers only ≥ 3 chars; Soulfind drops > 256 | §4.3, §7.3 | H |
| **Batch pacing** | Uses the search bucket at its refill rate, keeping **2 tokens reserved** for manual searches; one search per line; cached lines are free; no automatic fallback queries (a "broaden unresolved lines" pass is a user action and costs budget). 100 lines ≈ 50 min; cached lines are instant | Same bucket | slsk-batchdl's multi-query `--desperate` mode multiplies load; bans with automated tools are the main report | §4.3 | M |
| **Per-search collection cap** (SRCH-16) | Stop after **15 s**, 300 responses or 10 000 files | 60 s / 1 000 / 50 000 | Soulseek.NET defaults 15 s / 250 / 25 000; SoulseekQt 10 000 results; N+ shows 2 500 | §4.3, Qt changelog | H (refs) / L (values) |
| **Wishlist** (SRCH-14) | One term per **server-provided interval** (`WishlistInterval`, usually 12 min; 2 min if privileged), round-robin; nothing if the server sent no interval | Never more often than the server interval, never < 120 s | N+ behaviour; protocol doc | §4.4 | H |
| **Login / reconnect** (NET-4) | First retry random **10–30 s**, then ×2 with ±25 % jitter, cap **10 min**. After the ban PM: wait **30 min + 1–5 min**, auto-pause, ask the user. After a login timeout (possible silent ban): ≥ 5 min, ×2, cap 60 min. No auto-reconnect after `Relogged` or any login rejection except `SVRFULL` (≥ 5 min) | First retry ≥ 5 s; cap ≥ 5 min; ≤ **10 login attempts per hour** | "Do not connect and disconnect quickly"; N+ 5–15 s ×2 to ~5 min; slskd 1 s ×2 to 5 min + jitter; Soulfind jitters to spread reconnects | §4.2, §4.5 | H (shape) / M (values) |
| **Sessions** | **1** Selecta session per account per machine (lock file) | 1 | Rules: ≤ 5 clients per IP; ban message: "many clients at once from the same IP"; `Relogged` | §3.1 | H |
| **Peer lookups / new peer connections** (`GetPeerAddress`, `ConnectToPeer`) | ≤ **10 in flight**, ≤ **30 per minute** | ≤ 60/min | They are server operations; "too many operations at once" is the ban trigger. No public figure | §4.1 | L |
| **Active downloads per peer** (FAIR-2) | **1** | 2 | N+ and Soulseek.NET serve one upload per user at a time; extra requests only lengthen the peer's queue | §5.1 | H |
| **Queued files per peer** (FAIR-2) | **20** (an album download counts its files; larger albums queue the rest locally) | 50 | Below every observed uploader limit: N+ 100 files; slskd example 150 (default group) / 15 (leechers); SoulseekQt has a per-user limit on by default (value unknown) | §5.2 | M |
| **Per-peer daily cap** (FAIR-2) | Soft **20 files or 1 GB** (warning); hard **50 files or 3 GB** (blocked unless overridden per item); 7-day hard cap 150 files | Hard ≤ 100 files / 6 GB per day | slskd's example limits (30/day for a leecher, 1 500/week for an ordinary user) show uploaders think in tens of files per day for a stranger | §5.2 | L |
| **Global concurrent downloads** | **4** active + 1 preview | 10 | Each active transfer holds one uploader's slot (N+ default: 3 slots in total); slsk-batchdl's 20 concurrent jobs are in the ban reports | §5.2, §4.3 | M |
| **Global queued downloads** | 200 files | 1 000 | Bounds `QueueUpload` traffic and queue-position polling | — | L |
| **Place-in-queue polling** | Head of our queue at each peer only, **every 10 min** (and on demand when the user opens the transfer) | ≥ 2 min per file | N+ polls every queued file every 5 min | §5.3 | M |
| **Automatic retries** (DL-2) | Connection failures: **3** retries at 3, 10 and 30 min, then "failed". `Too many files/megabytes`: re-queue only as earlier files finish. `Banned`, `File not shared.`, unknown strings: **never** | ≤ 5 retries; ≥ 3 min first delay | SoulseekQt: max 3 retries every 5 min; slskd: 3 attempts; N+: every 3 min | §5.3 | H (refs) / M (values) |
| **Peer denial back-off** (FAIR-4) | `Banned`/custom ban text: no requests to that peer for **30 days** (and flag it); repeated `File not shared.`: 24 h; `Pending shutdown.`: ≥ 15 min | — | Peers' explicit signals | §5.3 | M |
| **Browse requests** (BRWS-2) | Burst 3, then **1 per 100 s** (≤ 6 per 10 min); same user not re-browsed within the 24 h cache TTL unless "Refresh" (≥ 10 min apart) | ≤ 30 per hour | A browse makes the peer compress and send its whole share list; crawling looks like a bot | — | L |
| **User info / folder contents** | User info: ≤ 1 per user per 10 min, ≤ 30/h. Folder contents: ≤ 1 per folder per cache TTL, burst 5, ≤ 20 per 10 min | 2× default | Same reasoning | — | L |
| **Private messages** (NET-7) | User-typed only; ≤ 1 per 2 s, identical text to the same user not repeated within 10 min; no automatic messages in v1 | Same | Repeated identical PMs → 30-min ban; N+ ResponseThrottle | §4.6 | M |
| **Previews** (PREV-*) | Auto-preview after the selection rests **≥ 600 ms**; **only for results whose peer reports a free slot** (others need an explicit Space and queue like a download); **1** active preview; not started if we already have an active transfer with that peer; ≤ **30 previews per 10 min**; counted in per-peer caps (PREV-7); on skip **cancel** unless ≥ 85 % received; answer later offers with `Cancelled`; ignore `UploadFailed` for cancelled previews | ≤ 2 active; ≤ 60 per 10 min | §6 | §6 | M (cancel) / L (values) |
| **Answering others' searches** (SHARE-2, belongs to the sharing note) | Answer only queries with ≥ 3 characters; at most **300 files** per response; never files matching an excluded phrase | ≤ 500 files | N+ `maxresults = 300`, `min_search_chars = 3`; slskd `ResponseFileLimit = 500`; Soulseek.NET README on excluded phrases | §7.3 | H |
| **Upload side** (SHARE-3, belongs to the sharing note) | **3** slots, **1** active upload per user, round-robin, **privileged users first**, per-user queue limits 100 files / 10 GiB answered with exactly `Too many files` / `Too many megabytes`, `File not shared.` for anything not in the index | Slots ≥ 1 while sharing | N+ defaults; slskd interop note; Rules require respect of privileges | §5.2, §7.1 | H |

Two numbers deserve a UI explanation, because users will want them higher: the search budget (show "2 searches per
minute keeps you clear of Soulseek's 30-minute search ban") and the per-peer cap (show the peer's counters and why).

### 9.2 Never do this

1. Connect to `server.slsknet.org` from tests, CI, scripts or debug builds; run automated searches or downloads on
   the live network (CLAUDE.md).
2. Use another client's login version numbers (157 official, 160 Nicotine+, 170 Soulseek.NET, 175 aioslsk, …), or
   try other numbers after `INVALIDVERSION`.
3. Create accounts automatically, invent usernames, or rotate accounts or IP addresses to dodge a ban (sockseek's
   `--random-login` is the anti-pattern).
4. Report fake share counts or fake user stats (`SharedFoldersFiles`, user info).
5. Evade server-side query filters or excluded phrases (no wildcard rewriting like sockseek's `bannedTerms`); return
   results whose paths contain an excluded phrase.
6. Reconnect automatically after `Relogged` or a login rejection; reconnect in a tight loop; log in during a ban window.
7. Repeat the same search within minutes, or fan one user query out into several network searches automatically.
8. Send automatic or repeated private messages (auto-replies, "please share" warnings, mass messages).
9. Queue a whole share, exceed the per-peer caps, re-queue after `Banned` / `File not shared.`, or queue-jump
   (cancel-and-requeue tricks, second accounts).
10. Run more than one session per account, or more than one Selecta per machine.
11. Send `GlobalUserList` (code 67 — "the official server disconnects the user", Soulfind comment), admin commands,
    or obsolete/experimental messages to the live server; fuzz or probe it (ToS §12(b)); try to measure its limits.
12. Bypass the governor "just once" (for a retry, a refresh, a background task or a test helper).
13. Send `QueueUpload` before the share index is loaded and true counts are reported.
14. Re-download a preview because the uploader sent `UploadFailed` after we cancelled it.

### 9.3 Developer safety checklist

For every change that touches networking:

- [ ] Every new outbound message goes through the governor, and a test proves it (the send path is unreachable
      otherwise).
- [ ] Every new limit has a `constexpr` ceiling, `static_assert(default <= ceiling)`, and settings can only tighten it.
- [ ] Pacing tests use the injectable fake clock; no test sleeps or touches a real network.
- [ ] Integration scenarios run on `devenv` only, through the counting proxy (§8.3), and pass its budgets.
- [ ] A test shows debug builds refuse a public (resolved) server address.
- [ ] The new action is written to the activity log (SAFE-4) with its outcome and any denial reason.
- [ ] Failure, denial and ban paths are specified: what happens on `Banned`, `Too many files`, timeouts, disconnects.
- [ ] Any retry has a backoff, jitter and a maximum attempt count.
- [ ] Fixtures were captured on the local network and scrubbed of usernames and paths.

Before any live session (maintainer, M9 onwards):

- [ ] Build from a tagged commit; version fields are the reserved (or agreed experimental) numbers.
- [ ] No other client is logged in with this account; only one Selecta is running.
- [ ] Shares are loaded and their counts reported; uploads enabled with privileged-first queueing.
- [ ] Limits at defaults or stricter; activity log visible; "Pause network activity" within reach.
- [ ] After the session: read the activity log for denials, disconnects and `server` messages; record them.

### 9.4 Decisions for the maintainer

| # | Decision | Recommendation |
|---|---|---|
| E1 | The Rules tolerate third-party clients that implement "chat, search, wishlist, download, upload, and respect / recognition of privileges". Chat rooms are a v1 non-goal. | Add a **minimal chat-room view (join, read, send) as P1**, next to private messages — cheap compared with the risk of being a "partial client". Alternatively record the accepted risk in ADR-006. |
| E2 | Login identity | Use major **177** (experimental) during development on the local network; **ask the Nicotine+ maintainers to reserve a major version for Selecta** (they keep the registry in `doc/SLSKPROTOCOL.md`) before M9. Put "Selecta <version>" in user info. |
| E3 | Privileges | Add to SHARE-3 (P0): privileged users are served first. |
| E4 | Preview policy | Accept §6/§9.1: auto-preview only for free-slot results; cancel on skip (≥ 85 % finishes). Confirms PREV-6 / OQ-5. |
| E5 | Search budget | Accept 2/min sustained with a burst of 6 (OQ-4); expose remaining budget and refill time (SRCH-13). Revisit only with M9 evidence, and never above 3/min. |
| E6 | Per-peer caps | Accept 1 active / 20 queued / 20–50 files per day (OQ-4). |
| E7 | Excluded phrases on outgoing searches | Warn and do not send (no override): compliant peers would return nothing anyway. |
| E8 | May settings loosen a limit above its default (up to the ceiling), or only tighten it? | Allow loosening up to the ceiling for the per-peer and concurrency limits (a privileged or very active user may want it). For the **search budget, reconnect and PM limits** — the ones the server bans for — expose no loosening setting in v1; their ceilings then only bound future changes of the defaults. |

## 10. Open questions and things to verify on a local test server

Never verify a limit by approaching it on the live network. Local-network experiments only, unless stated.

| # | Question | How to settle it |
|---|---|---|
| Q1 | Does the official server count `WishlistSearch`, `UserSearch` and `RoomSearch` in the same flood budget as `FileSearch`? | Unknown and not testable safely. Assume yes (one shared budget). |
| Q2 | Do bursts of `GetPeerAddress` / `ConnectToPeer` contribute to the flood ban? | Unknown. Keep the L-confidence peer-lookup bucket; watch M9 logs. |
| Q3 | Is "34 per 220 s" (or "~28 per 10 min") the real edge in 2026, and is there more than one window? Does the ban escalate for repeat offenders? Does a ban disconnect, or silently block searches (Orbit-pure says searches "returned zero results silently")? | Unknown; never probe. Defaults stay far below. Record any ban in M9 with full context. |
| Q4 | Does the server send the ban PM before closing the connection, and are logins during the ban silently ignored (as Soulfind assumes)? | Only observable if it happens; the ban detector must handle both orders. |
| Q5 | What does slskd record when the downloader closes mid-transfer (code reading says `Errored`), and does it count toward failure limits? | devenv: slskd peer with `failures` limits set; preview-cancel scenario. |
| Q6 | How do Nicotine+ and slskd react to many short previews from one downloader (queue position, `UploadFailed`, bans)? | devenv scenario with both as uploaders. |
| Q7 | How do Nicotine+, slskd and SoulseekQt react to `TransferResponse(false, "Cancelled")` for a queued item? | devenv for N+/slskd; SoulseekQt: TODO(verify) whether it can use a custom server — otherwise observe only in normal M9 use. |
| Q8 | SoulseekQt's default per-user upload limits (files/MB) and slot count. | TODO(verify): official client options, not documented on slsknet.org. |
| Q9 | Does Selecta filter its own responses with Soulfind's client-side phrases, and show the "may be filtered" hint for Soulfind server-side phrases? | devenv test with `soulsetup` → Search filters. |
| Q10 | Distributed-network behaviour (parents, branch level, `DistribSearch`) — Soulfind does not implement it. | aioslsk mock server or a scripted parent peer; then M9 observation. |
| Q11 | Are any of the private-message rate values (§9.1) too strict for normal conversation? | M9 usage. |

## 11. Sources

All accessed 2026-10-19. Repositories were shallow-cloned read-only into `local/research-clones/etiquette/`.

### Official (Soulseek, LLC)

| Source | Used for |
|---|---|
| https://www.slsknet.org/news/node/681 — Rules | Third-party client clause, bots/scripts, 5 clients per IP, admin powers, "privilege; not a right" |
| https://www.slsknet.org/news/node/682 — Terms of Use | §2 accounts, §8 termination, §12(b) no probing/testing, §12(c) no flooding/spamming |
| https://www.slsknet.org/news/faq-page — FAQ | Username recycling after 30 days, IP bans, non-sharer policy, "dos and don'ts" quote |
| https://www.slsknet.org/news/node/523 — Nir, "Empty share warnings, fixes and more" (2012-05-11) | Refusal of auto-bans, background browse + warning, place in line |
| https://www.slsknet.org/news/changelog — SoulseekQt changelog (2012–2026) | Per-user upload limit on by default, MB-per-user limit, retry policy, 10 000-result cap, one-character tokens, too-many-files handling |

### Issue trackers, discussions, forums

| Source | Used for |
|---|---|
| https://github.com/fiso64/sockseek/issues/12 (2023-11) | Origin of 34 searches / 220 s; 30-minute bans; `--random-login` |
| https://github.com/fiso64/sockseek/issues/95 (2025-04) | Bans despite the limiter; 1 search/min and IP change as "workaround" |
| https://github.com/fiso64/sockseek/issues/128 (2025-08) | Server-side substring filtering of queries |
| https://github.com/fiso64/sockseek/issues/163, https://github.com/fiso64/sockseek/issues/165 (2026-03) | Share-count spoofing dispute; Rules quoted by Nicotine+ maintainers; "bad faith" |
| https://github.com/mrusse/soularr/discussions/142 (2025-11-10) | Verbatim ban message; regular bans of an automated searcher |
| https://github.com/allquiet-hub/Lidarr.Plugin.Slskd/issues/19 (2026-10-03) | Verbatim ban message (second independent report) |
| https://github.com/nicotine-plus/nicotine-plus/discussions/3475 (2025-09-09) | Official donation/privileges text; no other known servers; forum moved to Google Groups |
| https://github.com/nicotine-plus/nicotine-plus/issues/3342, /3425, /2002 | Maintainers on temporary bans ("hammering the server"), PM-spam bans from a leecher plugin |
| https://groups.google.com/g/soulseek-discussion/c/N6L0qwMNFYg (2023-11-25) | Unanswered request for the search limits |
| https://groups.google.com/g/soulseek-discussion/c/t_L_SwzPbzk (2023-04) | Hearsay: "30-minute" search ban that lasted months |
| https://groups.google.com/g/soulseek-discussion/c/AktBq4ehYGI (2025-04) | Repeated bans for automatic warning PMs |
| https://groups.google.com/g/soulseek-discussion/c/yH8YGNcxH84 (2025-05) | Nir Arbel: empty-share message once per user, only on download attempts |

### Source code and project documentation

| Repository @ commit (date) | Files | Used for |
|---|---|---|
| nicotine-plus/nicotine-plus @ f3b9d4d (2026-10-18), GPL-3.0-or-later | `doc/SLSKPROTOCOL.md`; `pynicotine/config.py`, `search.py`, `slskproto.py`, `users.py`, `uploads.py`, `downloads.py`, `transfers.py`, `privatechat.py`, `pluginsystem.py`, `plugins/leech_detector/__init__.py` | Version registry, rejection reasons, denial strings, wishlist and excluded phrases, reconnect backoff, upload defaults and queue logic, queue-limit learning, `UploadFailed`/`Cancelled` handling, leech detection, chat throttling |
| fiso64/sockseek (formerly slsk-batchdl / sldl) @ ff9a3ac (v3.0.7, 2026-10-15) | `README.md`; `Sockseek.Core/Settings/EngineSettings.cs`; `Sockseek.Core/Search/RateLimitedSemaphore.cs`, `Searcher.cs` | 34/220 s defaults, fixed-window limiter, concurrency, `bannedTerms` evasion |
| slskd/slskd @ 6a450e4 (2026-10-18) | `src/slskd/Core/Options.cs`, `Core/ConnectionWatchdog.cs`, `Common/Compute.cs`, `Application.cs`, `Transfers/Types/TransferStateCategories.cs`, `Transfers/Uploads/UploadService.cs`, `Search/API/Controllers/SearchesController.cs`; `docs/config.md` | Reconnect backoff, upload groups and limits, failure counting, retry defaults, search API behaviour, default server endpoint |
| jpdillingham/Soulseek.NET @ c07c187 (2026-10-11) | `README.md`; `src/Options/SoulseekClientOptions.cs`, `src/Options/SearchOptions.cs`; `src/SoulseekClient.cs` | Excluded phrases statement, concurrency and search defaults, per-user upload concurrency |
| JurgenR/aioslsk @ a90ad82 (2026-10-11), GPL-3.0-or-later | `src/aioslsk/client.py`, `settings.py`; `docs/source/SOULSEEK.rst`; `tests/e2e/mock/` | Version 175/1, server notification PMs, restricted-connection message, endpoints, cancel flow, mock server |
| soulfind-dev/soulfind @ 5a256f5 (2026-10-15), GPL-3.0-or-later | `README.md`; `src/defines.d`; `src/server/{server,user,msghandler,cmdhandler,messages}.d`; `src/setup/setup.d` | Local server capabilities, ban/kick semantics, search filters, intervals, limits, official-server comments |
| mrusse/soularr @ 2037399 (2026-10-06) | `config.ini`, `soularr.py`, `README.md` | `minimum_search_interval`, search pacing |
| jackBonadies/SeekerAndroid @ ff1dcc6 (2026-10-11) | `Seeker/SeekerApplication.cs` | Default server host |
