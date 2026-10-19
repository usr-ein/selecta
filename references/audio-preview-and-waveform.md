# Audio preview and waveform — decoding, progressive playback, 3-band waveform, quality analysis

> Status: **draft in progress** (2026-10-19). Items marked **TODO** / *unverified* are from memory and not yet checked
> against a primary source. Confidence levels: **high** = read in source code/spec; **medium** = documentation or
> consistent secondary sources; **low** = inference or hearsay.

## TL;DR

- **Pioneer 3Band, answered (§2):** the **overview** is a single-sided **cumulative stack** (blue low at the bottom,
  amber mid on top of it, a white high crest on top). The **zoomed detail** view is **mirrored** about a centre line,
  with **overlaid** envelopes: each band is measured from the centre, the low∩mid overlap is drawn **brown**, and
  white highs go **in front at ≈ 85 % opacity**. Measured rekordbox colours: blue `#0055E1`, amber `#FFA600`, brown
  `#B36607`, white. Crossovers and height scaling are not public; Selecta should calibrate them against rekordbox 7.
- Rest: TODO (filled in at the end).

## 1. Key findings

- Pioneer stores 8 bits per band, 150 detail columns per second and a fixed 1,200-column overview (§2.3). That is a good
  model for Selecta's analysis cache.
- Further findings: TODO (filled in as sections are verified).

## 2. Pioneer "3Band" waveform (rekordbox / CDJ-3000) — how it looks and is layered

**Answer (priority question).** Pioneer layers the three bands differently in its two views:

| View | Shape | Layering | Draw order (back → front) | Confidence |
|---|---|---|---|---|
| **Overview** (whole track, "preview"; 1,200 columns) | **Single-sided**: grows up from a baseline at the bottom | **Cumulative stack**: blue spans `0…L`, amber spans `L…L+M`, white spans `L+M…L+M+H` | white (total height) → amber (`L+M`) → blue (`L`) — or simply draw three adjacent segments | **High** |
| **Detail** (zoomed, scrolling; 150 columns/s) | **Mirrored** about a horizontal centre line | **Overlaid**: each band is its own envelope measured from the centre line (not added) | larger of low/mid in its own colour → the overlap `0…min(L,M)` in a **dark amber/brown** → high in **white, slightly translucent, on top** | **Medium–high** (layering); **low** (exact scaling) |

So the PRD's "stacked" is right for the overview only. The detail view is the mock's "Layered" mode, with two
refinements: the low∩mid overlap gets its own brown colour (it is not simply amber covering blue), and white is drawn
in front at ~85 % opacity.

### 2.1 Evidence

- **Deep Symmetry analysis docs** (crate-digger `doc/modules/ROOT/pages/anlz.adoc`, commit `7c9d535`, mirrored in
  dysentery `track_metadata.adoc`, commit `f62a24b`): `PWV6` (3-band preview, in `ANLZ0000.2EX`) has 1,200 entries of 3
  bytes: "one-byte height values representing the mid-range, high, and low frequencies, in that order … they seem to
  be drawn stacked on top of each other, with the lows in dark blue, the mid-range in amber, and the highs in white."
  `PWV7` (3-band detail) has the same entry layout, one entry per half-frame = **150 entries per second**, and "a
  different kind of scaling … it seems to be non-linear."
- **dysentery issue #33** (GitHub, 2021–2025): nudge (2021-02-15) decoded `PWV6`: byte 0 = mid, byte 1 = high,
  byte 2 = low; "in the preview format, the bands appear to be stacked on one another", with fixed height coefficients
  low × 0.49, mid × 0.32, high × 0.25 (gist `eca9f07f…`, which draws blue `0…L`, orange `L…L+M`, white
  `L+M…L+M+H`, bottom-anchored). James Elliott (brunchboy, 2025-03-31) on the detail view: instead of stacking opaque
  colours as in the preview, draw "the mid first, opaque, then the high over that translucently, so we get mixing as
  seen in rekordbox, and then finally the low", then "I ended up having to do my own version of blending the low and
  mid-range colors and drawing the overlap separately"; "there is non-linear scaling of some sort involved for the
  detail waveforms" (unsolved).
- **beat-link** (`src/main/java/org/deepsymmetry/beatlink/data/`, commit `855d0bd`, 2026-10-07) — the most complete
  open implementation, rendered against CDJ-3000s:
  - `WaveformFinder.ThreeBandLayer`: LOW `rgb(32,83,217)` "dark blue"; LOW_AND_MIO (sic, = low+mid overlap)
    `rgb(169,107,39)` "brown"; MID `rgb(242,170,60)` "amber"; HIGH `rgb(255,255,255)`.
  - `WaveformPreview.segmentHeight()`: `low = b[2]·0.49`, `mid = low + b[0]·0.32`, `high = mid + b[1]·0.25`
    (cumulative). `WaveformPreviewComponent.updateWaveform()` draws each column bottom-up: HIGH to the total height, then
    MID, then LOW over it.
  - `WaveformDetail.segmentHeight()`: per band, not cumulative: `low = b[2]·0.4`, `mid = b[0]·0.3`, `high = b[1]·0.06`,
    averaged over `scale` entries per pixel column when zoomed out. `WaveformDetailComponent` (around line 1024) draws
    each column symmetric about `axis = height/2`: if `low > mid`, blue `±low` then brown `±mid`; else amber `±mid`
    (if different) then brown `±low`; finally white `±high`.
- **A real rekordbox 6 screenshot in 3Band mode** (attached to dysentery #33 by nudge, 2021). Sampling its pixels
  (screenshot colour space, so ±a few units):

  | Element | Measured colour |
  |---|---|
  | Overview low (blue) | `#0055E1` |
  | Overview mid (amber) | `#FFA600` |
  | Overview high | `#FFFFFF` |
  | Detail low (blue) | `#0053E1` |
  | Detail low∩mid overlap (brown) | `#B36607` |
  | Detail high | `#FFFFFF` where opaque; `#F5EBD7` over brown and `#FFF0D7` over amber, i.e. white at **≈ 85 % alpha** |

  In the detail view, kicks show as brown cores with blue flanks (low > mid); off-beat hats/claps as large white
  shapes; the bands are mirrored about the centre line. In the overview, blue sits at the bottom, a thinner amber band
  above it and a thin white crest on top.
- **Vendor material**: Pioneer's CDJ-3000 page says "3Band Waveform" shows the volume of each frequency band in
  different colours. AlphaTheta's FAQ lists which hardware can show BLUE / RGB / 3Band (CDJ-3000, OPUS-QUAD, XDJ-RX3,
  OMNIS-DUO, DDJ-FLX10, …). rekordbox sets it in Preferences › View › Waveform › Color. Tracks analysed before
  rekordbox 6 lack 3Band data until "Add New Analysis Data". The colour names (blue/amber/white) are community
  descriptions; no official colour values were found.

### 2.2 What is not known

- **Crossover frequencies and band filters.** Not documented anywhere. nudge's sine-chirp test (dysentery #33) shows
  broad, overlapping band responses: low falls off early, mid is wide, high rises slowly. The chirp's parameters are not
  stated, so no numbers can be derived. Selecta can measure them (see §12).
- **Height scaling.** Preview coefficients (0.49 / 0.32 / 0.25) are visual fits. Detail scaling is non-linear and
  unsolved. In the rekordbox screenshot white shapes are as tall as kicks, whereas beat-link's `high × 0.06` makes white
  a thin core. Treat beat-link's detail scaling as wrong for highs.
- **Value semantics.** Whether bytes are peak, RMS or loudness-weighted is unknown. rekordcrate (`src/anlz.rs`,
  `14d54ed`) names the `PWV6`/`PWV7` bytes bottom/mid/top third in file order, which contradicts the mid/high/low order
  above. Deep Symmetry's order is the one verified visually; rekordcrate's naming is probably wrong (medium confidence).
- **`PWVC`** (6 bytes, `.2EX`): rekordcrate calls it "per-band gain calibration" (`low_gain`, `mid_gain`,
  `high_gain`); dysentery calls it a vocal-detection configuration. Conflict unresolved. If it is a gain, rekordbox
  normalises band heights per track.

### 2.3 Reference: rekordbox ANLZ waveform tags

| Tag | File | Style | Entries | Entry format |
|---|---|---|---|---|
| `PWAV` | `.DAT` | Blue preview | 400 | 1 byte: height (5 bits) + whiteness (3 bits) |
| `PWV2` | `.DAT` | Tiny blue preview (CDJ-900) | 100 | 1 byte |
| `PWV3` | `.EXT` | Blue detail | 150/s | 1 byte: height (5 bits) + whiteness (3 bits) |
| `PWV4` | `.EXT` | RGB preview | 1,200 | 6 bytes |
| `PWV5` | `.EXT` | RGB detail | 150/s | 2 bytes: R, G, B (3 bits each) + height (5 bits) |
| `PWV6` | `.2EX` | 3-band preview | 1,200 | 3 bytes: mid, high, low |
| `PWV7` | `.2EX` | 3-band detail | 150/s | 3 bytes: mid, high, low |
| `PWVC` | `.2EX` | ? (per-band gain or vocal config) | — | 3 × u16 |

Takeaways for Selecta's analysis format: Pioneer stores **8 bits per band**, **150 columns per second** for the
detail view (one column = 1/150 s ≈ 294 samples at 44.1 kHz) and a **fixed 1,200-column** overview, and computes the
pixel heights at draw time.

## 3. Decoding options

TODO: FFmpeg (LGPL/GPL), Qt Multimedia FFmpeg backend, libsndfile, dr_libs/miniaudio, libmpg123, libFLAC,
AudioToolbox, Mixxx SoundSource.

## 4. Playback engine and accurate position reporting

TODO: QAudioSink vs CoreAudio (AUHAL / AVAudioEngine) vs miniaudio; latency compensation; playhead interpolation.

## 5. Progressive preview of growing files

TODO: per format (MP3, FLAC, WAV/AIFF, M4A moov position, Ogg/Opus); seeking in the downloaded range; VBR seeking;
safe growing-file reader design (blocking reads, cancellation, FFmpeg AVIOContext pitfalls such as tail reads for
ID3v1/APE/moov).

## 6. Mixxx waveform implementation (filtered / RGB / stacked)

TODO: files, crossovers, filters, summarisation, storage; GPLv2+ reuse.

## 7. Recommended analysis pipeline for Selecta

TODO: crossovers, filters, peak vs RMS, perceptual scaling, overview vs zoom resolution, cache format, < 300 ms for a
6-minute track on Apple Silicon, progressive analysis.

## 8. Rendering in Qt (Widgets / Qt Quick / QRhi) at 60–120 fps

TODO.

## 9. Fake-lossless / transcode detection and spectrograms

TODO.

## 10. Optional BPM / key detection libraries and licences

TODO.

## 11. Implications for Selecta

TODO.

## 12. Open questions and things to verify experimentally (local test server)

TODO.

## 13. Sources

All accessed 2026-10-19. TODO.
