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
| `TextMatcher`, `LocalTextMatcher` | `TextMatcher.h` | Keys, versions, similarity; swappable for `match/` |
| `ReleaseResolver` | `ReleaseResolver.h` | Ranked candidates with score, tier and reasons |
| `YtDlpRunner`, `YtDlpFetch` | `YtDlpRunner.h` | Discover/version-check yt-dlp, search, pick ±3 s, fetch into the preview cache |
| `ClipSource` | `ClipSource.h` | Deezer 30-s clip URL by ISRC, else by search |

Internal (`detail/`, not for `core/`): `HttpChannel` (one per service: pacing, permits, timeouts, bounded bodies,
manual redirects, retries, cache), `ServiceGate` (spacing / sliding window / in-flight / back-off scheduler),
`Json` (strict optional-field accessors), `LoopbackReceiver` (one-shot `QTcpServer` for the sign-in answer),
`ImageHeader` (JPEG/PNG dimensions without decoding).

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

Value types are reentrant. `TextMatcher` implementations are immutable and shareable across threads.
`TokenStore` is thread-safe; its futures finish on a worker thread, so `SpotifyAuth` continues on its own thread
with a context object. The Keychain store runs `SecItem*` on a one-thread pool because a Keychain prompt can block.

## Permit points

Every outbound request goes `ServiceGate` (published limit) → `RequestPacer::acquire(operation)` (governor) →
`Permit::consume()` → send. One permit per HTTP request, per redirect hop (redirects are followed by hand) and per
retry; one per yt-dlp process that talks to YouTube (`--version` is local and takes none).

| Service | Host(s) | Operation | Policy (meta's own pacing) |
|---|---|---|---|
| `SpotifyApi` | api.spotify.com | `SpotifyRequest` | 1/s, bursts of 5, 2 in flight; 429 → Retry-After |
| `SpotifyAccounts` | accounts.spotify.com | `SpotifyRequest` | ≥ 2 s apart, ≤ 6/min, 1 in flight |
| `SpotifyImages` | i.scdn.co | `CoverArtRequest` | 2/s, 2 in flight, 20 MiB |
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
| Deezer preview URLs | never cached (signed, ~15 min) | — |
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
3. **Get tracks / search:** `ReleaseResolver::resolve()` per line — `Priority::Background` for a pasted batch,
   `Priority::Interactive` for a search typed by hand; the chosen candidate's `durationMs` drives the ±3 s Soulseek
   ranking (D23). Pasted Spotify links: `spotify::parsePastedLinks()` → `SpotifyClient::importLinks()` (background).
4. **Arrival pipeline (D30):** `SpotifyClient::album()` for disc totals when tagging, `MusicBrainzClient::enrich()` +
   `applied()` for label/genre, `CoverFetcher::fetch()` for cover bytes → convert library.
5. **Player:** `ClipSource::clipFor()` → stream the URL from memory; `YtDlpRunner::probe()` / `search()` / `pick()` /
   `fetch()` → open `YtDlpFetch::started(path)` with the growing-file reader.
6. **Activity log (SAFE-4, META-7):** `RequestPacer::report()` receives one scrubbed `RequestRecord` per request.
7. **Matching:** pass an adapter over `match/` as the `TextMatcher` once that library lands.

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

## Progress

- **Done:**
  - Public headers (`1c7f454`).
  - M1 (merged as `07df505`): `Result`/`MetaError`, `ServicePolicy`, `MemoryMetaCache`, `InMemoryTokenStore`,
    `KeychainTokenStore` (verified once against the real login Keychain with a throwaway service name,
    `SELECTA_TEST_KEYCHAIN=1`), PKCE, `detail::ServiceGate`, `detail::HttpChannel`, `detail::Json`,
    `detail::LoopbackReceiver`, `SpotifyAuth` (PKCE + loopback, refresh, rotation, 6-month expiry, client credentials).
  - M2: priority lanes, `SpotifyLinks`, `detail::Identifiers`, `detail::SpotifyJson`, `SpotifyClient` (search, ISRC,
    track, album paging, playlists, Liked Songs, pasted links). 174 tests (+2 opt-in Keychain).
- **Next:** M3 — `MusicBrainzClient` (ISRC → recordings → releases → genres, `enrich()`), `CoverFetcher` with
  `detail::ImageHeader` (bounded JPEG/PNG parser, fuzz-style test), disk cache on a worker thread.
- **Open questions:** none blocking. Merge main before M4 to adapt `match/`'s `Normalizer` / `VersionClassifier`.

### Notes for whoever resumes

- Tests drive time with `base::ManualClock`; gates poll real timers, so after `clock.advance()` call
  `detail::ServiceGate::pollAll()` (or use `testing::awaitAdvancing`).
- A flow's `LoopbackReceiver`/`QTimer` are deleted with `deleteLater()`: a flow can end inside their own signals.
- `QT_USE_QSTRINGBUILDER` is on: wrap `a + b` in `QByteArray(...)`/`QString(...)` before calling members on it.
