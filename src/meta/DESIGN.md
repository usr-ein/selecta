# `meta/` — metadata and preview services

Contract: [`docs/contracts/meta.md`](../../docs/contracts/meta.md). Spec:
[`references/metadata-and-preview-services.md`](../../references/metadata-and-preview-services.md) (cited as "the
note", §n). PRD: D19–D21, D26, D29, D30, META-1…8, PREV-11/12.

`meta` turns a track description into a canonical **release** and fetches what the arrival pipeline and the player
need: metadata, cover bytes, playlist and link imports, an official 30-second clip URL, and preview audio through the
user's own yt-dlp. It owns no UI and returns copyable value types; `core/` decides what to do with them.

## Classes

| Class | Header | Role |
|---|---|---|
| `Result<T>`, `MetaError`, `ErrorCode`, `Service` | `Result.h` | One error model for everything; `QFuture<Result<T>>` for async calls |
| `Release`, `CoverRef`, `PlaylistSummary`, `TrackList` | `Release.h` | Value types; nothing invented when a service is silent |
| `RequestPacer`, `PermitDecision`, `RequestRecord` | `RequestPacer.h` | Seam to the governor: one permit per outbound request; activity records |
| `ServicePolicy`, `k*Policy`, TTLs, bounds | `ServicePolicy.h` | Compile-time published limits per service |
| `MetaCache`, `MemoryMetaCache` | `MetaCache.h` | Key → bytes cache with TTLs, prefix purge (Spotify sign-out) |
| `TokenStore`, `InMemoryTokenStore` | `TokenStore.h` | Async secret store interface; in-memory for tests |
| `KeychainTokenStore` | `KeychainTokenStore.h` | macOS Keychain (`SecItem*`) on a private worker thread |
| `pkce::*` | `Pkce.h` | RFC 7636 S256 verifier/challenge, `state`, constant-time compare |
| `SpotifyAuth` | `SpotifyAuth.h` | PKCE sign-in state machine, loopback callback, refresh, 6-month expiry |
| `spotify::parseLink`, `parsePastedLinks` | `SpotifyLinks.h` | Track/album/playlist URLs and URIs |
| `SpotifyClient` | `SpotifyClient.h` | Search (≤ 10), track, album, playlists, Liked Songs, pasted links |
| `MusicBrainzClient` | `MusicBrainzClient.h` | ISRC → recordings → releases (label, catno) → genres; D30 `enrich()` |
| `CoverFetcher` | `CoverFetcher.h` | Largest linked image, then Cover Art Archive; bounded; disk cache |
| `DeezerClient` | `DeezerClient.h` | Keyless search, ISRC lookup, track, album (label, genres, UPC), preview URL |
| `ReleaseResolver` | `ReleaseResolver.h` | A line → ranked candidates with score, tier and reasons; Spotify first, Deezer fallback |
| `YtDlpRunner`, `YtDlpFetch` | `YtDlpRunner.h` | The user's own yt-dlp: discover and version-check, search, pick ±3 s, fetch one video's audio into the preview cache |
| `ClipSource` | `ClipSource.h` | Deezer's official 30-s clip: a freshly signed URL by Deezer ID, ISRC, else search |

Internal (`detail/`, not for `core/`): `HttpChannel` (one per service: pacing, permits, timeouts, bounded bodies,
manual redirects, retries, cache), `ServiceGate` (spacing / sliding window / in-flight / back-off scheduler, two
lanes), `Json` (strict optional-field accessors), `LoopbackReceiver` (one-shot `QTcpServer` for the sign-in answer),
`ImageHeader` (JPEG/PNG dimensions without decoding, bounded walk), `Identifiers` (ISRC, MBID, barcode, dates),
`TitleInfo` (a title read with `match/`: core key, version, featured artists, derivative markers, DJ-mix flag),
`SpotifyJson`, `DeezerJson` and `MusicBrainzJson` (strict mapping and the D30 choices), `Worker` (a `std::thread`
with mutex-guarded queues, so ThreadSanitizer sees the hand-offs that an uninstrumented Qt would hide), `ToolProcess`
(one external process: argument list, given environment, own session, bounded line-split output, idle and total
timeouts, group kill), `YtDlpOutput` (yt-dlp's command lines, strict readers of its output, the §12.6 error map).

## Threading

All `QObject`s live on one thread chosen by `core/` (the engine thread or a dedicated metadata thread) and are used
only there. Everything is asynchronous over an injected `QNetworkAccessManager` (same thread) or `QProcess`; nothing
blocks and nothing sleeps. Public async calls return `QFuture<Result<T>>`:

- the future finishes on the meta thread, **never inside the call** (cache hits and argument errors too), so callers
  never see re-entrant continuations; attach continuations with a context object (`future.then(ctx, …)`);
- a finished future always holds a `Result`; it is only *canceled* when the caller cancels it, which aborts the
  request in flight (a producer-side `QFutureWatcher` notices the cancel);
- the yt-dlp fetch is a job object (`YtDlpFetch`: `started(path)`, `progress`, `finished(Result)`, `cancel()`)
  because it has several events.

Value types are reentrant. `ReleaseResolver::score()` and `rank()` are pure static functions, callable from any
thread. `TokenStore` is thread-safe; its futures finish on a worker thread, so `SpotifyAuth` continues on its own
thread with a context object. The Keychain store runs `SecItem*` on a `detail::Worker` because a Keychain prompt can
block; the cover disk cache uses one too.

## Permit points

Every outbound request goes `ServiceGate` (published limit) → `RequestPacer::acquire(operation)` (governor) →
`Permit::consume()` → send. One permit per HTTP request, per redirect hop (redirects are followed by hand) and per
retry; one per yt-dlp process that talks to YouTube (`--version` is local and takes none).

| Service | Host(s) | Operation | Policy (meta's own pacing) |
|---|---|---|---|
| `SpotifyApi` | api.spotify.com | `SpotifyRequest` | 1/s, bursts of 5, 2 in flight; 429 → Retry-After |
| `SpotifyAccounts` | accounts.spotify.com | `SpotifyRequest` | ≥ 2 s apart, ≤ 6/min, 1 in flight |
| `CoverImages` | i.scdn.co, *.spotifycdn.com, *.dzcdn.net | `CoverArtRequest` | 2/s, 2 in flight, 20 MiB |
| `Deezer` | api.deezer.com | `DeezerRequest` | 2/s (published 50/5 s); quota code 4 → 5 s ×2 |
| `MusicBrainz` | musicbrainz.org | `MusicBrainzRequest` | ≥ 1.1 s apart, 1 in flight; 503 → 2 s ×2, 3 tries |
| `CoverArtArchive` | coverartarchive.org, archive.org | `CoverArtRequest` | 1/s, bursts of 5 (redirect chains) |
| `YtDlp` | YouTube via yt-dlp | `YtDlpRun` | 1 process, ≥ 2 s apart, ≤ 30/10 min, 60-min pause |

The numbers are `constexpr` in `ServicePolicy.h` with `static_assert`s; the governor can only slow meta further.
A back-off longer than the policy's `maxWait` (30–60 s) fails the request at once with `RateLimited` + `retryAfter`
instead of waiting, and later requests fail the same way until it ends: nothing hangs for hours (spotDL users saw
86 400-s `Retry-After`s).

## Cache TTLs

| Data | Where | TTL |
|---|---|---|
| Search answers (Spotify, Deezer, MusicBrainz search) | `MetaCache` | 24 h |
| Tracks, albums, MusicBrainz lookups | `MetaCache` | 30 days |
| "Not found" (ISRC unknown to MusicBrainz) | `MetaCache` | 24 h |
| The user's playlists, playlist items, Liked Songs | never cached | — |
| Spotify entries (`spotify/` prefix) | dropped on sign-out | — |
| Cover bytes | `CoverFetcher::Config::cacheDir`, one file per URL | 30 days |
| Deezer preview URLs | never used from the cache (signed, ~15 min): `ClipSource` asks with `fresh = true` | — |
| yt-dlp audio | the preview cache folder the caller passes (PREV-8 LRU is core's) | — |

## Integration points for `core/`

1. **Construct on the meta thread:** one `QNetworkAccessManager`, a `RequestPacer` over the Governor, a `MetaCache`
   (memory, or a persistent one), a `KeychainTokenStore(u"io.github.usr-ein.Selecta"_s)` (the bundle id; accounts
   are `spotify/<clientId>`), then `SpotifyAuth`,
   `SpotifyClient`, `DeezerClient`, `MusicBrainzClient`, `CoverFetcher`, `ReleaseResolver`, `ClipSource`,
   `YtDlpRunner`.
2. **Spotify settings:** `SpotifyAuth::setClientId()` from Settings → Integrations, then `restore()` at start-up;
   `signIn()` from the "Connect" button (`Config::openUrl` = `QDesktopServices::openUrl`); `signIn(playlistScopes())`
   before the first playlist import. Show `signInExpiresAt()` two weeks ahead. On `signedOut()` drop any Spotify
   data core keeps.
3. **Get tracks / search:** `ReleaseResolver::resolve(ResolveQuery, Priority)` per line, with `ResolveQuery` filled
   from `match::TracklistParser`'s line: `artist`, `title` with its version text kept, `durationMs` from
   `durationSec`, `label` from `labelHint` (and `isrc` when a file or playlist item has one). Core wraps it in a
   `std::function<QFuture<Result<Resolution>>(const ResolveQuery &)>`, runs at most 3 lines at once on
   `Priority::Background` for a pasted batch (`Interactive` for a search typed by hand), and cancels a line's future
   when the line is removed. Use the first candidate when `Resolution::confidence` is `Likely` or better, show the
   list for `Ambiguous`, and the line as typed for `None`; `sourceErrors` says which service failed while another
   answered (a resolution fails only when every source failed). The chosen candidate's `durationMs` drives the ±3 s
   Soulseek ranking (D23). Pasted Spotify links: `spotify::parsePastedLinks()` → `SpotifyClient::importLinks()`
   (background).
4. **Arrival pipeline (D30):** `SpotifyClient::album()` for disc totals when tagging, `MusicBrainzClient::enrich()` +
   `applied()` for label/genre, `CoverFetcher::fetch()` for cover bytes → convert library.
5. **Player:** `ClipSource::clipFor(release)` → stream `Clip::url` from memory before `expiresAt` (never store it:
   Deezer's terms), with the attribution and `pageUrl`. YouTube previews, on an explicit audition only:
   `YtDlpRunner::search(YtDlpRunner::searchText(release))` → `pick(candidates, release)` (nullopt: "No YouTube match
   within ±3 s", fall back to the clip or the peer's file) → `fetch(id, <preview cache>/yt)` → open
   `YtDlpFetch::started(path)` with the growing-file reader; `finished()` says when the file is complete. Delete the
   job (or `cancel()`) when the audition ends early. `AgeRestricted`, `GeoBlocked` and `Unavailable` mean "try the next
   candidate once"; `RateLimited`/`BotCheck` start the hour's pause (`paused()`, `pausedUntil()`); `ToolNotFound`,
   `ToolUnsafe`, `ToolTooOld`, `ToolMissingRuntime` carry the message for Settings. `probe()` from Settings shows the
   version, `stale` and whether Deno was found; `setExecutable()` takes the Settings path (empty: auto-discover).
6. **Activity log (SAFE-4, META-7):** `RequestPacer::report()` receives one scrubbed `RequestRecord` per request.
7. **Matching:** meta uses `match/`'s `Normalizer`, `VersionClassifier` and `FieldMatcher` directly: one comparison
   key and one version vocabulary for the whole app (`Candidate::version` is a `match::VersionCompatibility`).
8. **Endpoints for tests and isolated runs (`core::MetaEndpoints`):** every service's base URL is injectable —
   `SpotifyAuth::Config::accountsUrl`, `SpotifyClient`'s and `DeezerClient`'s `apiUrl`, `MusicBrainzClient`'s `apiUrl`,
   `CoverFetcher::Config::coverArtArchiveUrl` — and `ClipSource` only uses the `DeezerClient` it is given. Cover image
   URLs come from the answers and are limited to the services' CDN hosts. A non-HTTPS base (`http://127.0.0.1:1`) fails
   every request at once without opening a socket. For yt-dlp, an empty `Config::executable` means auto-discovery
   (the production default); an isolated run passes `YtDlpRunner::Config::disabled()`, which never finds or runs
   anything (every call fails with `NotConfigured`, whatever `setExecutable()` says later).
8a. **Shutdown:** destroying `YtDlpRunner` with a search, fetch or version check in flight kills the child's process
   group and reaps it before returning (a few milliseconds), deletes the partial file, cancels pending futures and
   finishes jobs with `Cancelled`.

## Priority lanes

Every gate has two lanes (`meta/Priority.h`): an `Interactive` request starts before any queued `Background` one, so a
100-line Get tracks batch (≈ 100–200 Spotify searches at 1/s, Deezer for the misses at 2/s, a few minutes in all)
never starves a preview, a typed search or the inspector. Defaults: resolver and catalogue calls `Interactive` unless
the caller says otherwise; `importLinks()` always `Background`; MusicBrainz enrichment (arrival pipeline) `Background`;
`ClipSource` and `CoverFetcher` `Interactive`. A lane changes the order, never the service's limits.

## Spotify endpoints

Only these Web API paths are called, all among the endpoints development mode still serves (February 2026 list; the
note §3.3 and §4.2). `detail::spotify::isDevelopmentModeEndpoint()` holds the list; a debug assertion guards every
request and a test checks every path the client requests.

| Path | Use | Cache |
|---|---|---|
| `GET /search?q=…&type=track&limit≤10` | track search with field filters; `q=isrc:…` | 24 h |
| `GET /tracks/{id}` | one track (pasted links one by one: no batch endpoint any more) | 30 days |
| `GET /albums/{id}`, `GET /albums/{id}/tracks` | album with all tracks, disc totals | 30 days |
| `GET /me/playlists` | the user's playlists | never |
| `GET /playlists/{id}?fields=name`, `GET /playlists/{id}/items` | own or collaborative playlists | never |
| `GET /me/tracks` | Liked Songs | never |

Errors: 401 → one refresh and one retry, then `SignInRequired`; 403 → `Forbidden` (playlists: a hint to paste the
tracks instead); 429 → Retry-After when short, else `RateLimited`; 429 with `reason: QUOTA_EXCEEDED` → `QuotaExceeded`
and no Spotify request until Retry-After (1 h when absent).

## MusicBrainz gap fill (D30, the note §5.3)

`MusicBrainzClient::enrich()` runs, one request after another at ≥ 1.1 s:

1. `GET /isrc/{isrc}?inc=artist-credits` (a 404 is remembered 24 h). Unknown ISRC, or none: one recording search
   `recording:"<title without version>" AND artist:"<main artist>" AND dur:[ms−3000 TO ms+3000]`, hits ≥ 90 only.
2. Keep recordings crediting the main artist (comparison keys) and, when both lengths are known, within ±3 s; take
   the closest.
3. `GET /release?recording=…&inc=labels+release-groups&limit=100`; choose barcode = UPC, else title and year
   (official or unknown status only: a same-titled bootleg is passed over), else the earliest official release of
   the same type (Spotify's "single" covers EPs), else the earliest official, else the earliest. Compilations and
   DJ mixes are never used unless the release is a compilation: their label is the compiler's.
4. Label and catalogue number from that release; MusicBrainz's `[no label]` and `[none]` are no answer.
5. Genres: release group, else recording, else artist; most votes first, zero-vote genres dropped. A failure here
   keeps the label and reports "genres unavailable"; everything else fails the enrichment (no half-truth label).

## Release resolution (M4, the note §4.4 and §14.5)

`ReleaseResolver::resolve()` asks as little as it can, one request at a time:

1. With an ISRC: Spotify `q=isrc:…`, then Deezer `/track/isrc:…`; an `Exact` candidate ends the line.
2. Spotify `track:"<core title>" artist:"<primary artist>"` (≤ 10 results). A `Likely` or better candidate whose
   version agrees ends the line. If the line names a version and no candidate of that version came back, one more
   search with the version's words (never after an empty first page: more words only narrow it).
3. Otherwise Deezer `"<primary artist> <core title>"` (25 results), and the same version search if needed.

A source that fails (rate limit, network, server) is not asked again for that line. At most 6 requests per line,
usually 1–2. Each release is scored once as it arrives; each step ranks the lot once.

Scoring (`score()`, the note §14.5's initial weights, tuned later by experiment X5):

- weighted over the fields both sides have: title core 0.40 (`match::FieldMatcher` on `Normalizer` keys, a length
  within ±3 s corroborating a fuzzy match), artist 0.30 (0.7 for the line's primary artists among the release's, 0.3
  for its featured artists, or for the share of the release's artists the line explains, the remixer included),
  length 0.20 (1 within ±3 s, then `exp(−0.1·(Δ−3))`; only when the versions agree), album 0.05, label 0.05;
- versions compared first (`match::VersionClassifier::compare`): another version scores 0 whatever its length; an
  unspecified line against a named version without a length ×0.9;
- penalties: DJ-mix segment ×0.3 and derivative uploads (slowed, sped up, nightcore, 8D, reverb, karaoke, tribute,
  cover, bass boosted) ×0.2 unless the line asks for them; the same title by another artist ×0.5. DJ versions
  (extended, club, dub, radio edit, remixes) are versions, never penalties;
- the ISRC rule: the line's ISRC on a release whose length (if known) is within ±3 s and whose title is not a
  different song → `Exact`.

Tiers: `Confident` ≥ 0.92 for the first candidate only, with a 0.05 margin over every other recording or version
(two versions that tie stay `Likely`); `Likely` ≥ 0.80; `Ambiguous` ≥ 0.60; else `None`. Copies of one recording
(same ISRC and version) from both services merge into one candidate showing Spotify's album (not a compilation's,
when Spotify has both), with Deezer's ID, label and covers added; two sources agreeing on the leading candidate's
ISRC raise it one tier. `rank()` keeps other versions in the list, last, with their reasons.

## Deezer

Keyless JSON API (`api.deezer.com`): `/search?q=&limit≤50`, `/track/isrc:{ISRC}`, `/track/{id}`, `/album/{id}` (label,
genres, UPC). Deezer reports errors as HTTP 200 with `{"error":{"code":…}}`: codes 4 (quota) and 700 (busy) back the
service off and retry, 800 (no data) is `NotFound`, anything else `BadResponse`; none of them is cached. IDs are
checked (`[1-9][0-9]{0,18}`) before they reach a path; queries lose quotes and colons (Deezer's advanced-search
syntax) and stop at 200 characters. Preview URLs are kept only over HTTPS from Deezer's hosts; `previewExpiry()`
reads the signed URL's `exp=`.

## yt-dlp previews (M5, D26, the note §12)

`YtDlpRunner` is the only place Selecta starts yt-dlp, and only on an explicit audition.

- **Discovery (§12.1):** the Settings path (absolute only), else `yt-dlp` in `/opt/homebrew/bin`, `/usr/local/bin`,
  `~/.local/bin`, then the absolute `PATH` entries; never the working directory. The first file found decides: it must
  be a regular executable after resolving symlinks, not writable by group or others, owned by the user or root
  (`ToolNotFound`, `ToolUnsafe`). Deno is looked up in the same directories and passed as
  `--js-runtimes deno:<path>` (the child's `PATH` may not include Homebrew).
- **Version:** `--version` (local, no permit) once per install; a changed file (size, time, path) or a newly found
  Deno probes again. Older than 2025.11.12 (the release that added `--js-runtimes` and `--no-remote-components`) is
  `ToolTooOld`, and so is "no such option" from an older tool; more than 90 days old sets `stale` ("run
  `brew upgrade yt-dlp`"). Selecta never updates yt-dlp.
- **Safety flags on every run (§12.5):** `--ignore-config --no-plugin-dirs --no-cookies --no-cookies-from-browser
  --no-remote-components`, `--cache-dir <Config::ytDlpCacheDir>` or `--no-cache-dir`, `--color never --socket-timeout
  15 --retries 2 --fragment-retries 2`; `--sleep-requests 0.75` for YouTube runs; `--` before the query or URL; the
  video as a full watch URL, never a bare ID. Never `-U`, `--exec`, cookies, accounts, config locations, batch files,
  `--impersonate` or `--extractor-args` (the fake script fails any run that breaks this).
- **Process hygiene:** `QProcess` with an argument list; environment `HOME`, `PATH` (yt-dlp's and Deno's folders,
  Homebrew, system), `LANG=en_US.UTF-8`, `TMPDIR` and nothing else; stdin `/dev/null`; descriptors above stderr closed
  and signal handlers reset in the child; its own session, so a kill (`SIGKILL` to the process group) also ends Deno
  or anything else it started.
- **One process at a time,** version checks included, in submission order. YouTube runs then pass the yt-dlp gate
  (≥ 2 s apart, ≤ 30 per 10 minutes; a wait beyond 15 s fails at once with `RateLimited` + `retryAfter`) and take one
  `YtDlpRun` permit each.
- **Search (§12.2):** `--flat-playlist -J -- ytsearch<N>:<text>` (N ≤ 20, text cleaned and ≤ 200 characters), 60 s
  timeout, stdout ≤ 4 MiB. Entries keep id (11 × `[A-Za-z0-9_-]`), title, channel, channel ID, duration, views, live
  status, verified flag; channels, playlists and broken IDs are dropped. Checked by hand once against the real tool
  (2026-10-20, one five-result search): the output has exactly this shape.
- **Pick (§12.3), pure:** length within ±3 s of the release (whole seconds; no release length, no pick); never live,
  upcoming or post-live; all the release's title words present; the version compatible (`match::VersionClassifier`);
  no DJ-mix segment or derivative upload the release doesn't have. Then the "<artist> - Topic" channel, a channel
  named like the artist or the label, the better version agreement, a verified channel, the closer length, the most
  views.
- **Fetch (§12.4):** `-f "bestaudio[ext=m4a]/bestaudio[acodec=opus]/bestaudio" -P <dir> -o "%(id)s.%(ext)s"
  --no-part --no-mtime --fixup never --force-overwrites --max-filesize 200M --no-playlist --abort-on-error -q
  --progress --newline` with `SELECTA-PROGRESS`, `SELECTA-FORMAT` (before_dl) and `SELECTA-FILE` (after_move) marker
  lines, read from both pipes. `started(path)` once the named file exists; `progress`; `finished` with the path,
  container, codec and size, after checking the file is the expected `<dir>/<id>.<ext>` and not empty. 30 s without
  output or 120 s in all → killed, `Timeout`. On any failure or cancel the partial file goes, and so does any
  `<id>.*` file changed during the run (yt-dlp may not have named its format yet); older previews stay.
- **Errors (§12.6, `YtDlpOutput::classifyFailure`):** rate limit / HTTP 429 → `RateLimited`; "not a bot", other
  sign-in requests, captcha → `BotCheck` (both pause yt-dlp for 60 minutes: queued runs fail, new ones fail at once
  with the time left, `probe()` still works); "confirm your age" → `AgeRestricted`; private, removed, DRM, members-only
  → `Unavailable`; region → `GeoBlocked`; missing format or failed challenge → `ToolMissingRuntime` without Deno, else
  `ToolTooOld`; HTTP 403 → `ToolTooOld`; network → `Network`; else `ToolFailed` with yt-dlp's reason (home folder
  shown as `~`).
- **Tests** run `tests/meta/fixtures/fake-yt-dlp` (POSIX sh) through a per-test wrapper: it checks the safety flags
  (exit 99) and that Selecta's environment didn't leak (exit 98), logs its arguments and process starts and ends,
  prints canned JSON, writes a file with progress, and fails in each §12.6 way or hangs with a child process.

## Clips (META-5, the note §9.1)

`ClipSource::clipFor(release)` asks Deezer for a fresh signed URL every time (they expire after about 15 minutes):
`/track/{deezerId}` when the release has one (a stale ID falls through), else `/track/isrc:{ISRC}`, else a search on
the main artist and core title whose best hit must agree on the version, fit ±3 s and score at least `Likely`
(`ReleaseResolver::score`), then `/track/{id}` for the signature (search answers are cached). `expiresAt` comes from
the URL's `exp=`, else ten minutes from now. No match or no preview → `NotFound`; other errors pass through.

## Smoke tool (M6, manual only)

`selecta-cli` commands in `tools/selecta-cli/commands/meta/`, for checking behaviour against the real services by hand,
sparingly; never from tests or CI:

- `meta-search "artist - title" […up to 20 lines]` resolves lines as Get tracks does (one Spotify sign-in for all of
  them); `--length`, `--label`, `--enrich` (MusicBrainz label and genres for each first candidate), `--all`.
- `meta-isrc <isrc>`: Spotify's and Deezer's releases with that ISRC, then MusicBrainz's label, catalogue number and
  genres (`--no-enrich` skips it).
- `meta-ytdlp-search "artist - title"`: probes the user's yt-dlp, finds the release (or takes `--length`), searches
  YouTube without downloading and prints the ±3 s pick; `--fetch <folder>` downloads the pick's audio there (delete the
  folder afterwards). `--ytdlp <path>` overrides discovery.
- Every command: `--verbose` prints each request's scrubbed activity-log line; `--no-spotify`; `--secrets <file>`.

`GovernorPacer` implements `RequestPacer` over a real `governor::Governor` on `base::SteadyClock`: one
`governor.request(op, {})` per acquire, the decision delivered on the next event-loop turn (never re-entrant), the
governor's explanation as the refusal text and its `retryAfter`. Spotify runs in client-credentials mode only when
`SPOTIFY_CLIENT_ID` and `SPOTIFY_CLIENT_SECRET` come from the environment or a secrets file (`--secrets`, else the
first `local/secrets.env` up from the working directory); the values are never printed, only where they came from.

### Smoke run, 2026-10-20 (real services, by hand)

Ten lines in one `meta-search` process (one token request, ten Spotify searches, about 0.2–0.4 s each):

| Line | First candidate | Tier |
|---|---|---|
| Bicep - Glue | Glue (single, 4:29) | Likely → Confident after the margin fix below |
| Daft Punk - One More Time (Radio Edit) | One More Time - Radio Edit (3:55) | Confident |
| Disclosure feat. Sam Smith - Latch | Latch (4:16), featured artist credited | Likely → Confident after the fix |
| Fred Again - Delilah (pull me out of this) | Fred again.., Delilah Montagu (4:11): the missing ".." didn't matter | Likely → Confident after the fix |
| Peggy Gou - (It Goes Like) Nanana | the unspecified 6:08 version, "Edit" second (×0.9) | Confident |
| RÜFÜS DU SOL - Innerbloom (What So Not Remix) | the remix (4:37); radio edit, other remixes and live version score 0 | Confident |
| Four Tet - Baby | Baby (4:24); a later remix second | Confident |
| Charlotte de Witte - Doppler | Formula EP version (7:14); a compilation copy titled "Doppler" is the 4:01 edit | Likely (right: two versions, no length) |
| Bonobo - Kerala | Kerala (3:58) | Confident |
| Avicii - Levels (Radio Edit) | Levels - Radio Edit (3:20); original, instrumental and remix score 0 | Confident |

What changed because of it: compilation copies of the same track under their own ISRCs had blocked the Confident
margin (Bicep, Disclosure, Fred again..); the margin now ignores the same title in a compatible version within ±3 s,
and an original release sorts before a compilation's copy at equal scores. A re-run of those three lines gave
Confident. "Streaming Mix" segments (Tomorrowland) scored ×0.9 instead of as DJ-mix segments; "streaming mix",
"dj mix", "continuous mix" and "mixed by" now mark DJ-mix segments.

Also checked: Deezer alone (`--no-spotify`) resolved "Bicep - Glue" and "Charlotte de Witte - Doppler" Confident;
MusicBrainz enrichment gave Ninja Tune / house, electronic, dance, techno and KNTXT / electronic, techno (3 requests
each, 1.1 s apart). `meta-isrc GBCFB1700229` merged Spotify's and Deezer's copies into one Exact candidate (score now
1.0 for an ISRC-only line) and MusicBrainz chose the EP release (Spotify's "single" type). yt-dlp 2026.08.19 with Deno
2.9.7: one 10-result search (2.1 s; the official video is 4:45, outside ±3 s of the 4:29 release; the pick was a
4:30 upload) and the one allowed download: 4.4 MB m4a (AAC) in 3.5 s, with progress and the marker lines as designed;
the file and its temp folder were deleted at once.

## Covers

`CoverFetcher::fetch()`: the largest linked image (only from Spotify's and Deezer's CDNs; any other host is skipped,
so a hostile answer can't point Selecta at a LAN address), then CAA `/release/{mbid}/front-1200`, then
`/release-group/{mbid}/front-1200`. Redirect hops to archive.org are paced and permitted. `CoverFetcher::inspect()`
reads the header with `detail::readImageHeader()` — JPEG SOFn after a bounded segment walk (4 MiB, 4096 segments) or
PNG IHDR — and refuses anything else, over 20 MiB, or over 10 000 px a side. Nothing in meta decodes images.

## Decisions taken here

| Decision | Why |
|---|---|
| `QFuture<Result<T>>` for request/response calls, a job object for the yt-dlp fetch | Standard Qt async type, cross-thread continuations for core; the fetch has several events |
| meta paces each service itself *and* asks the governor | Published limits hold whatever the governor's configuration; the governor can only slow down |
| Redirects followed by hand | One permit per outbound request, including each hop to archive.org |
| Own JPEG/PNG header parser for the 10 000-px check, no Qt Gui | The contract keeps meta free of Qt Gui; the decode-time `QImageReader::setAllocationLimit` belongs to the convert library's `ArtworkProcessor`, which decodes |
| `TokenStore` is asynchronous; Keychain calls on a worker | A Keychain prompt (ad-hoc signed rebuilds) would otherwise freeze the meta thread |
| Generic `TokenStore` (account → secret bytes) | Reusable for other credentials; `SpotifyAuth` serialises its own record |
| Loopback receiver handles several connections and ignores other paths | Browsers open speculative connections and ask for `/favicon.ico` |
| A wrong `state` ends the sign-in attempt | Safer than waiting on; the page tells the user to try again |
| Client-credentials mode kept in `SpotifyAuth` (development only) | PRD META-1 allows it; lets the smoke tool reach Spotify without a browser |
| Duration score: 1 within ±3 s, then `exp(−0.1·(Δ−3))` | spotDL's exponential, shifted so D23's ±3 s counts as "same" |
| Two priority lanes per gate | The head implementer's requirement: batches must not starve previews |
| Pasted links resolved one at a time by a self-owned importer | Pacing does the waiting; a fatal error (quota, sign-in) stops early and keeps what was found; destroying the client cancels it cleanly |
| Paging by offset and `total`, not by following `next` URLs | The client decides what it requests; a hostile `next` can't redirect it |
| Local files in playlists kept as text-only releases; episodes skipped and counted | They still make useful Get tracks lines (the note §14.6) |
| `Service::SpotifyImages` renamed `CoverImages` | The same channel fetches Deezer's CDN covers |
| Cover URLs restricted to the services' CDN hosts | Defence against a hostile or broken answer pointing at internal hosts |
| MusicBrainz defaults to the Background lane | It serves the arrival pipeline; interactive lookups pass `Priority::Interactive` |
| `match/` used directly; `TextMatcher` and `TextFold` dropped (M4, approved by the head implementer) | One normaliser and one version vocabulary for the app; no adapter layer to keep in step |
| A source that fails is not asked again for that line | The next request would fail the same way; a struggling service gets a break |
| No version search after an empty first page | More words only narrow a search; saves a request per miss |
| ISRC agreement raises the tier only for a candidate that leads by the margin | Both services carry every version of a track; agreement on one says nothing about which version the line meant |
| Merged candidates show Spotify's copy, an album or single before a compilation | The compilation's album, date and track number would be wrong for tagging |
| JSON APIs: a 200 body that isn't a JSON object fails `BadResponse` in the channel (`jsonInspection`) | A proxy page or truncated answer must never sit in the cache for 24 h or 30 days |
| yt-dlp minimum 2025.11.12; "no such option" also means too old | Every safety flag Selecta passes exists from that release; an older tool would reject the command line |
| The first yt-dlp found decides, even when unsafe | Silently running another copy would hide a problem the user should fix |
| Version checks share the one-process slot but take no permit | D26's "one process at a time"; `--version` never reaches YouTube |
| A yt-dlp gate wait beyond 15 s fails at once | A preview that starts minutes later is useless; the caller can retry after `retryAfter` |
| `--force-overwrites` on every fetch | A leftover partial file must never pass for a finished download; reuse is core's preview-cache decision |
| Job signals and the `paused` signal are delivered from the event loop | A receiver that deletes the job or starts another run never re-enters the runner mid-step |
| `ClipSource` always signs afresh, even after a cached search | The URL in a cached answer may already have expired |
| The Confident margin ignores copies of the same version within ±3 s, whatever their ISRC | Seen in the smoke run: compilations re-register tracks; a copy of the chosen track is no doubt about what to search for |
| At equal scores the original release sorts before a compilation's copy | Its album, date and track number are the ones to tag with |
| `meta-search` takes up to 20 lines | One Spotify sign-in for a whole smoke run instead of one per line |

## Progress

- **Done:**
  - Public headers (`1c7f454`).
  - M1 (merged as `07df505`): error model, policies, caches, token stores (Keychain verified once by hand), PKCE,
    `ServiceGate`, `HttpChannel`, `LoopbackReceiver`, `SpotifyAuth`.
  - M2 (merged): priority lanes, `SpotifyLinks`, `Identifiers`, `SpotifyJson`, `SpotifyClient`.
  - M3 (merged): `ImageHeader` (bounded; truncation/malformation cases and a 20 000-buffer random test),
    `MusicBrainzJson`, `MusicBrainzClient` (lookups, search, `enrich()`, `applied()`), `CoverFetcher` (CDN allowlist,
    CAA fallback through redirects, disk cache on a worker thread with TTL and size bound).
  - M4 (merged): `TextFold`/`TextMatcher` replaced by `match/`; `TitleInfo`, `DeezerJson`, `DeezerClient`,
    `ReleaseResolver` (the labelled table of tricky lines and the request flows), `jsonInspection` for the JSON APIs.
  - M5 (merged): `ToolProcess`, `YtDlpOutput`, `YtDlpRunner`/`YtDlpFetch` (discovery, version, search, pick, fetch,
    error map, pause, cleanup) against the fake yt-dlp script, `ClipSource`; `Config::disabled()` for isolated runs.
  - M6: `selecta-cli` `meta-search`, `meta-isrc`, `meta-ytdlp-search` over `GovernorPacer`; the manual smoke run
    above, and the resolver fixes it led to. 329 tests (+2 opt-in Keychain tests), green under `ctest -j8`.
- **Next:** nothing planned in meta. ADR-011 (previews through the user's own yt-dlp) is for the head implementer to
  write from the "yt-dlp previews" section.
- **Open questions:** none blocking.

### Notes for whoever resumes

- Tests drive time with `base::ManualClock`; gates poll real timers, so after `clock.advance()` call
  `detail::ServiceGate::pollAll()` (or use `testing::awaitAdvancing`).
- A flow's `LoopbackReceiver`/`QTimer` are deleted with `deleteLater()`: a flow can end inside their own signals.
- `QT_USE_QSTRINGBUILDER` is on: wrap `a + b` in `QByteArray(...)`/`QString(...)` before calling members on it.
- Process tests (`YtDlpRunnerTest`, `ToolProcessTest`) run real shells, so a loaded machine slows them: injected
  timeouts are seconds, not milliseconds, waits have 30-s ceilings and return as soon as their condition holds, and a
  test kills the fake only once its background child's pid file exists. Check changes with
  `ctest -j16 -R 'YtDlp|ToolProcess' --repeat until-fail:3`.
