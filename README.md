# Selecta

A native, quality-first Soulseek client for DJs and music collectors — C++/Qt 6, macOS-first, cross-platform, GPLv3.

> **Status:** pre-alpha — product definition and design phase. There is nothing to build or run yet.

| What | Where |
|---|---|
| Product requirements (PRD) | [docs/PRD.md](docs/PRD.md) |
| Implementation plan | [docs/PLAN.md](docs/PLAN.md) |
| Interactive UI mock (HTML/JS/CSS) | [mock/index.html](mock/index.html) |
| Research notes | [references/](references/) |

## Why another Soulseek client?

Selecta is built around one workflow: find the right file in the right quality, audition it instantly, and file it
into a clean library — without duplicating what you already own and without abusing the peers who share with you.

- Strict format/bitrate filters (e.g. "MP3 = 320 kbps CBR only", "lossless ≥ 16/44.1").
- Instant preview with a 3-band (low/mid/high) waveform.
- Knows what you already have: your collection, past downloads, and this session's downloads.
- Batch search from a pasted tracklist, safely paced.
- Built-in converter (FLAC ⇄ MP3 ⇄ WAV ⇄ AIFF) that keeps and completes metadata and artwork.
- Fairness built in: per-peer download counts, share/download ratio, conservative network limits.

## License

Selecta is free software, licensed under the GNU General Public License version 3. See [LICENSE](LICENSE).
