# ADR-007: Waveform analysis, cache and rendering

- **Status:** Accepted (crossovers to be calibrated against rekordbox)
- **Date:** 2026-10-19
- **Inputs:** `references/audio-preview-and-waveform.md` §2, §6–8, §11; PRD PREV-2, PREV-8

## Decision

- **Analysis:**
  - Mono, LR4 crossovers at 200 Hz and 2.5 kHz (named constants).
  - Peak and RMS per band at 150 columns/s, √-companded to `uint8`.
  - The overview is derived from the detail.
  - Decode and analysis run chunked and in parallel (target < 300 ms for 6 minutes), and progressively while a
    preview downloads.
- **Cache:** a versioned binary blob keyed by content hash. The ~3.6 KB overview is kept long-term; the full detail
  (~0.1–0.3 MB per track) lives in an LRU.
- **Look (Pioneer 3Band):**
  - The overview is a single-sided cumulative stack.
  - The zoomed view is mirrored and layered: low `#0055E1`, mid `#FFA600`, overlap `#B36607`, high white at 85 %, on
    black.
  - Display range −48…0 dBFS; about 8 s across the zoomed view.
  - One pure layering function serves the CPU renderer, a future GPU renderer and the tests.
- **Rendering:** QPainter tiles in a `QWidget`, ticked by the display link (ADR-001).
- **Calibration:** the maintainer analyses a synthetic file in rekordbox 7 and exports it. Selecta then fits its
  crossovers and scaling to it (RESEARCH-SYNTHESIS §6).

## Consequences

- The detail cache is larger than PREV-8's first estimate. The PRD has been updated.
