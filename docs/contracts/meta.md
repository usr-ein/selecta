# Contract: `meta/`, metadata and preview services

| | |
|---|---|
| **Owner** | meta library agent, branch `lib/meta` |
| **Integrator** | head implementer (`core/`, Get tracks, the Inbox arrival pipeline, the player) |
| **Spec** | `references/metadata-and-preview-services.md` (Spotify state and terms §3–4, spotDL lessons §7, preview sources, yt-dlp section) |
| **Decisions** | PRD D19, D21, D26 (yt-dlp previews), D29/D30 (tags from Spotify; label and genre from MusicBrainz), META-1…8, PREV-11/12; ADR-011 (to be written from your work) |

## Purpose and boundary

`meta` turns a track description into a canonical **release** and fetches what the arrival pipeline and the player
need:

- metadata and cover art;
- playlist and track-link imports;
- preview audio, from the user's own yt-dlp or an official 30-second clip.

It owns no UI. It returns value types; `core/` decides what to do with them.

- **No secrets in code, logs or the repository.** Spotify uses PKCE with the user's own Client ID. Tokens go to a
  `TokenStore`:
  - production: a macOS Keychain implementation using the Security framework (`SecItem*`), the only allowed
    dependency beyond Qt;
  - tests: an in-memory store.
  Development may read `local/secrets.env` (git-ignored) only in a manual smoke tool, never in tests.
- **Every outbound request needs a `governor::Permit`:** `SpotifyRequest`, `MusicBrainzRequest`, `CoverArtRequest`,
  `DeezerRequest`, `YtDlpRun`.
  - Add a `meta/RequestPacer.h` interface that `core/` implements with the governor; your tests use
    `tests/support/PermitFactory.h`.
  - Respect each service's published limits: MusicBrainz 1 request/s with a proper User-Agent; Spotify's 429
    `Retry-After`; Deezer's 50 requests per 5 s.
- **yt-dlp runs only as an external process:**
  - `QProcess` with an argument list, no shell;
  - `--ignore-config`, no cookies, no browser cookies, no account;
  - a timeout and one process at a time;
  - output only into a preview-cache directory that the caller passes in.
  The audio is never filed, shared, converted or tagged. Never bundle, download or reimplement yt-dlp (D26, CLAUDE.md).

## Threading

All QObjects live on one thread chosen by `core/` (the engine thread or a dedicated metadata thread). Everything is
asynchronous over `QNetworkAccessManager`, which is injected so tests can fake it; nothing blocks. Result types are
copyable values.

## Components and deliverables

### M1. Value types and Spotify authentication
- `Release`: title, artists, album, album artist, release date, track and disc numbers with totals, duration in ms,
  ISRC, Spotify id and URL, cover URLs with sizes, plus label and genre (filled later from MusicBrainz).
- `SpotifyAuth`:
  - Authorization Code with PKCE (RFC 7636, S256), with the code verifier and challenge per spec;
  - loopback redirect on `http://127.0.0.1:<random port>/callback` via a tiny `QTcpServer` that accepts one request,
    checks `state`, and answers a small "you can close this tab" page;
  - token exchange and refresh;
  - the 6-month refresh-token expiry (`400 invalid_grant` → `SignInRequired`), with a clean state machine;
  - "open this URL" is a callback the app supplies, so `meta` never needs Qt Gui.
- **Tests:** RFC 7636 appendix B vectors, a scripted fake token endpoint, callback edge cases (wrong `state`, user
  denied, port in use, timeout).

### M2. Spotify catalogue
- Track search by artist and title. Respect dev mode's `limit` ≤ 10 and build field filters correctly.
- Get track; get album with its tracks; parse track, album and playlist links and URIs.
- Playlist import:
  - playlists the user owns or collaborates on, Liked Songs, and albums;
  - for any other playlist, accept pasted track links (one lookup each). Check that the endpoints used are in dev
    mode's allowed set (§3.3).
- Map to `Release` without inventing missing data.
- **Tests:** recorded JSON fixtures (synthetic data shaped like real responses; no real user data); 429 with
  `Retry-After`; 401 and refresh; malformed JSON.

### M3. MusicBrainz and cover art
- `MusicBrainzClient`: recording lookup by ISRC, then label, genres or tags, and the release MBID. User-Agent
  `Selecta/<version> ( https://github.com/usr-ein/selecta )`; 1 request/s.
- Cover download: the largest Spotify image, with Cover Art Archive (by release MBID) as fallback. Bounded size
  (reject over 20 MB or over 10 000 px before decoding; use `QImageReader::setAllocationLimit`). Return the bytes; the
  convert library resizes them.
- **Tests:** fixtures, rate pacing, missing ISRC, multiple recordings.

### M4. Matching a line to a release
- `ReleaseResolver`: given artist, title, version and an optional duration or ISRC, return ranked candidates with a
  score and an explanation.
- Use the spotDL lessons (§7.5): ISRC first; per-field token-sort ratios via the `match/` library's normaliser once it
  lands (until then a local adapter you can swap); duration compared only after the versions agree; junk-word
  penalties ("slowed", "nightcore", …) but no penalties for DJ versions.
- Spotify first, Deezer when Spotify isn't configured or has no match (keyless; same `Release` type).
- **Tests:** a labelled table of tricky lines (remix versus original, featured artists, typos, radio edit versus
  extended).

### M5. Previews
- **`YtDlpRunner`:**
  - locate the binary: `PATH`, `/opt/homebrew/bin`, `/usr/local/bin`, or a user-set path; check the version;
  - search YouTube Music for "artist title" and return candidates with id, title, channel and duration (flat JSON,
    no download);
  - pick the candidate within ±3 s of the release length; when several fit, prefer official "Topic" channels;
  - fetch audio only for one id into the given cache directory, with progress if available and cancellation (kill
    the process);
  - map failures to clear errors: not installed, too old, HTTP 429, sign-in required or age-gated, unavailable,
    timeout.
  - **Tests:** a fake `yt-dlp` script in `tests/meta/fixtures/` that prints canned JSON, writes a small file, or fails
    in each way. Never run the real yt-dlp in tests.
- **`ClipSource`:** the official 30-s preview URL (Deezer by ISRC, else by search).

### M6. Smoke tool (manual only)
`selecta-cli` commands as new files in `tools/selecta-cli/commands/meta/`: `meta-search "artist - title"`,
`meta-isrc <isrc>`, `meta-ytdlp-search "artist - title"`. The maintainer, or you sparingly, runs it by hand to check
behaviour against the real services. It reads the client ID from the environment or `local/secrets.env` and never
prints secrets or tokens.

## Acceptance

- Unit tests with fakes cover every component and error path; dev and asan are clean; lint is clean.
- A manual smoke run resolves 10 real lines with sensible results. Note the results in `DESIGN.md`, without personal
  data.
- `DESIGN.md` describes the classes, threading, the permit points, cache TTLs (search 24 h, track and album 30 days,
  cover bytes in the cache directory) and the integration points for `core/`.
