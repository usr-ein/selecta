# Selecta

A native, quality-first Soulseek client for DJs and music collectors — C++/Qt 6, macOS-first, cross-platform, GPLv3.

> **Status:** pre-alpha. The app shell builds and runs; the Soulseek, metadata, audio and matching libraries are
> being built. See [docs/STATUS.md](docs/STATUS.md) and [docs/BUILDING.md](docs/BUILDING.md).

| What | Where |
|---|---|
| Product requirements (PRD) | [docs/PRD.md](docs/PRD.md) |
| Implementation plan | [docs/PLAN.md](docs/PLAN.md) |
| Interactive UI mock (HTML/JS/CSS) | [mock/index.html](mock/index.html) |
| Research notes | [references/](references/) |
| Building and inspecting | [docs/BUILDING.md](docs/BUILDING.md) |
| Progress | [docs/STATUS.md](docs/STATUS.md) |

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

Selecta is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version
(`GPL-3.0-or-later`). See [LICENSE](LICENSE).
