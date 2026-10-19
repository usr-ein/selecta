# Metadata matching and quick-preview services

> Status: **draft — research in progress** (all sources accessed 2026-10-19). Items marked **TODO(verify)** come from
> memory and have not yet been checked against a primary source; do not turn them into constants until verified.
> Source repos are read at the commits listed in §14.
>
> Scope: canonical track metadata (Spotify and alternatives), matching a Soulseek result or a batch line to a
> catalogue entry, and 30-second previews from official sources. **Out of scope by decision:** downloading or ripping
> audio from YouTube, YouTube Music, Spotify or any other streaming service. spotDL is described only at the level of
> "where does metadata come from, where does audio come from", plus its matching and tagging logic.

## 1. TL;DR

- TODO — written last.

## 2. Key findings

| # | Finding | Confidence | Source |
|---|---|---|---|
| — | TODO | | |

## 3. Spotify Web API (state on 2026-10-19)

TODO — auth flows, endpoints, rate limits, development mode vs extended quota, Nov 2024 and later restrictions.

## 4. Spotify Developer Terms and Developer Policy vs a Soulseek client

TODO — quotes and analysis.

## 5. Full-length listening through the user's own Spotify app

TODO — Web API playback (Premium, PKCE, Connect devices), URI deep links, macOS AppleScript.

## 6. C++/Qt libraries for the Spotify Web API

TODO — spotify-qt `lib`, others; minimal Qt client sketch.

## 7. spotDL: architecture and matching logic

TODO — licence, query building, scoring, thresholds, tagging, what transfers to Selecta.

## 8. Official 30-second preview sources

TODO — Deezer, iTunes Search API, Spotify preview_url, Apple Music API, Beatport, Traxsource, Bandcamp.

## 9. Alternative and complementary metadata sources

TODO — MusicBrainz + Cover Art Archive, Discogs, Deezer.

## 10. Deep links and purchases into the Inbox

TODO

## 11. Comparison table

TODO

## 12. Implications for Selecta

TODO — MetadataService + PreviewService design, matching pipeline with confidence scoring, governor budgets,
caching/TTLs, privacy, terms/legal risk table.

## 13. Open questions and things to verify

TODO

## 14. Sources

TODO
