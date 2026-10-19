# References

Research notes behind Selecta's design decisions. Each note ends with **Implications for Selecta** and a list of
sources. They are inputs to the architecture decision records (`docs/adr/`, written in Phase 1), not decisions
themselves.

| File | Topic |
|---|---|
| [soulseek-protocol.md](soulseek-protocol.md) | Wire protocol: server, peer, file and distributed messages; connection and transfer flows |
| [soulseek-ecosystem.md](soulseek-ecosystem.md) | Existing libraries, clients, servers and tools; licences and reuse options |
| [network-etiquette-and-safe-development.md](network-etiquette-and-safe-development.md) | Rate limits, bans, peer etiquette, recommended default limits, safe development |
| [audio-preview-and-waveform.md](audio-preview-and-waveform.md) | Decoding, progressive preview, 3-band waveform analysis and rendering, quality analysis |
| [conversion-and-metadata.md](conversion-and-metadata.md) | Transcoding, tag mapping (Vorbis/ID3/MP4/RIFF/AIFF), cover art, quality warnings |
| [qt-app-architecture.md](qt-app-architecture.md) | Qt 6 stack, native macOS UI, repo layout, dependencies, CI, packaging, licensing |
| [sharing-and-uploads.md](sharing-and-uploads.md) | Share index, answering searches, upload queue and slots |
| [library-matching-and-organization.md](library-matching-and-organization.md) | Matching results to the local collection, download organisation, single→album supersede |
