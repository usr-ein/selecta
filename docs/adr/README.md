# Architecture decision records

Short records of decisions that shape the code. Each ADR has a status, its inputs and the decision. Change a decision
by editing its ADR and saying why in the commit. Background: [../RESEARCH-SYNTHESIS.md](../RESEARCH-SYNTHESIS.md).

| ADR | Decision | Status |
|---|---|---|
| [001](ADR-001-ui-toolkit.md) | Qt Widgets with an Objective-C++ mac shell | Accepted (toolbar after the M1 spike) |
| [002](ADR-002-protocol-library.md) | Own Soulseek protocol library | Accepted |
| [003](ADR-003-audio-stack.md) | FFmpeg decode, QAudioSink then CoreAudio, libmp3lame direct, TagLib | Accepted |
| [004](ADR-004-dependencies.md) | Qt 6.12.x, Homebrew for dev, vcpkg for release, CI matrix, signing | Accepted |
| [005](ADR-005-persistence.md) | Own pinned SQLite, WAL, migrations, in-memory hot paths | Accepted |
| [006](ADR-006-network-governor.md) | Token buckets, constexpr ceilings, 2 searches/min | Accepted |
| [007](ADR-007-waveform.md) | 3-band analysis, cache, Pioneer look, QPainter tiles | Accepted (calibration pending) |
| [008](ADR-008-matching.md) | Path parsing, length-first scoring, typo tolerance, supersede guards | Accepted |
| [009](ADR-009-file-safety.md) | FileOps, conversion on arrival, filing by copy → verify | Accepted |
| [010](ADR-010-licence-policy.md) | GPL-3.0-or-later, allowed and forbidden sources | Accepted |
| 011 | Metadata and preview services | Waiting for `references/metadata-and-preview-services.md` |
| [012](ADR-012-sharing-uploads.md) | ShareIndex and UploadManager | Accepted |
| [013](ADR-013-test-network.md) | Soulfind devenv, fakes, counting proxy | Accepted |
