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
- slskd records such an upload in a *failed* state (`Cancelled`, `Errored` or `Aborted` are all in
  `TransferStateCategories.Failed`) and **counts it against optional per-user daily/weekly failure limits** (none by
  default; the docs' example allows a leecher 10 failures per day). Many cancelled previews from one slskd peer can
  therefore get Selecta refused by that peer for a day or a week.
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
  slip through. Release builds need a one-time explicit confirmation that names the Rules.
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
