# Selecta UI mock

An interactive HTML/CSS/JS mock of Selecta's interface, used to decide **what goes where** in the Qt app in the visual
language of macOS. Its design language and layout are approved (PRD D25). Everything is simulated with sample data: no
network, no real files, no real Spotify calls. Artists, peers and tracks are fictional.

Open it with `open mock/index.html`. There are no build steps or dependencies:

| File | Contents |
|---|---|
| `index.html` | Structure and icons |
| `styles.css` | macOS light/dark tokens |
| `app.js` | State, simulation and views |

## Deep links

Append these to the URL:

| Parameter | Values |
|---|---|
| `#view=` | `search`, `batch`, `inbox`, `collection`, `history`, `transfers`, `peers`, `messages`, `shares`, `wishlist`, `converter`, `settings` |
| `&theme=` | `light`, `dark` |
| `&search=` | any query |

Example: `index.html#view=inbox&theme=light`.

## Things to try

- **Release first.** Search "neon rain" or "tidal memory". Selecta first shows the matching release (from Spotify when
  signed in, otherwise Deezer), then ranks the files on the network against it. Length within ±3 s gets a ✓; same-name
  files of another length (another artist's song, a radio edit) are hidden as "Not this release". Pick another release
  from the chips, or "Any (name only)".
- **Audition.**
  - **Space** plays a 30-second clip from Deezer, which costs the peer nothing.
  - **⇧Space** previews the full file from the peer, with the 3-band waveform filling in as it downloads.
  - ↑/↓ moves through results; ←/→ seeks (⇧ for 30 s).
  - Drag the zoomed waveform like a jog wheel.
  - **K** keeps the preview (a clip: downloads the file); **⌫** discards it.
  - Settings → Preview swaps what Space plays.
- **Waveform styles:**
  - **Pioneer** (default) follows rekordbox/CDJ 3Band, as documented in `references/audio-preview-and-waveform.md` §2.
    The zoomed view is mirrored: brown where lows and mids overlap, then the larger band in blue or amber, with white
    highs on top. The overview is a one-sided stack from the bottom.
  - **Layered**, **Stacked** and **Lanes** are there for comparison.
- **Albums and the Inbox rule.** In "neon rain", pick the album chip and download the album. "Sodium" is already in your
  Inbox as a single; when the album finishes, that single goes to the Trash, with Undo. Files already filed in your
  collection are only flagged, never touched.
- **Batch.** Every line shows its release before any network search. Paste a Spotify playlist link to import a list.
  "Start searching" paces the network searches: the demo clock ×10 makes one search every 8.5 s visible, keeping 8
  searches free. Then review, approve and download.
- **Inbox → File.** Each item shows what filing will do and the exact file name it will get.
- **Naming.** Settings → Filing has the templates (`{artist} - {title}` for singles) and the name cleaner with a live
  preview. The cleaner keeps Unicode letters but turns spaces, quotes, `%`, `$`, `/`, `:` and the like into path-safe
  names.
- **Integrations.** Settings → Integrations has the Spotify client ID and secret fields (empty and fillable). Sign-in
  needs only the client ID. The mock never contacts Spotify.
- **Fair use.** The peer column counts what you took from each peer today. See Peers & ratio.

## Design questions still open

1. Is the Pioneer waveform faithful enough? Are the zoom window (8 s) and the overview height right?
2. Should Space play the 30-second clip (current default) or the full file?
3. Is the release card the right size, and should "Only this release" be on by default?
4. What else should start hidden (filters, columns, inspector sections) to keep each screen to the essentials?
