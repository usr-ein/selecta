# ADR-011: Previews through the user's own yt-dlp

- **Status:** Accepted
- **Date:** 2026-10-20
- **Inputs:** PRD D26 and PREV-11; `src/meta/DESIGN.md` ("yt-dlp previews"); the maintainer's direction "Selecta is
  not breaking any term, it simply calls yt-dlp that is already on my machine."

## Context

Get tracks lets the user hear a track before committing to it (D27). Deezer's clips are short and often missing for
club music. Previews from Soulseek peers depend on a peer being online with a free slot.

## Decision

- **Run the user's own yt-dlp, as an external process only.** Selecta never bundles, downloads or updates it.
  - Discovery checks the file. The minimum version is 2025.11.12. Deno is passed explicitly.
- **Run the child in a contained way.**
  - The safety flags are always passed, with `--` before the query or URL.
  - The child gets a minimal environment and its own session; a kill reaches its whole process group.
- **Pace it.**
  - One process at a time.
  - Launches are at least 2 s apart, and at most 30 happen in 10 minutes.
  - Every YouTube run takes one `YtDlpRun` permit from the governor.
  - A rate limit or bot check pauses yt-dlp for 60 minutes.
- **Search and pick.** The search uses flat JSON. The pick must fall within ±3 s of the release's length,
  preferring Topic, artist or label channels.
- **Fetch.** bestaudio goes into the preview cache with `--no-part`, so the growing-file reader can follow it.
  - Timeouts: 30 s without output, 120 s in all.
  - A failure leaves no partial file.
- **Keep the audio apart.** It is never filed, shared, converted or tagged, and it is labelled as lossy.

## Consequences

- It works only with the user's own install.
- The maintainer accepted the YouTube terms risk for personal use.
- Throttling is handled by the pause.
- Tests use a fake yt-dlp script and never the real one. Isolated runs pass `YtDlpRunner::Config::disabled()`, so
  nothing is found or run.
