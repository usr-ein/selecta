# Selecta UI mock

An interactive HTML/CSS/JS mock of Selecta's interface. It exists to decide **what goes where** in the Qt app, in the
visual language of macOS. Everything is simulated with sample data: no network, no real files. The artists, peers and
tracks are fictional.

Open it with `open mock/index.html`; any modern browser works. There are no build steps or dependencies:
`index.html` (structure and icons), `styles.css` (macOS light/dark tokens), `app.js` (state, simulation, views).

## Deep links

Append to the URL: `#view=search|batch|inbox|collection|history|transfers|peers|messages|shares|wishlist|converter|settings`,
`&theme=light|dark`, `&search=<query>`. Example: `index.html#view=inbox&theme=light`.

## Things to try

- **Search:** type in the sidebar field (⌘F) and press Return. Results stream in from simulated peers. Change the preset
  or the format chips and watch the "Hidden by filters" breakdown. Searching the same thing twice opens the cache and
  costs no search; the toolbar gauge shows the search budget (34 per 220 s).
- **Audition:** select a result and press **Space**. The preview downloads and starts playing once enough is buffered;
  the not-yet-downloaded part of the waveform is hatched. Then:
  - ↑/↓ moves through the results; Space previews the new selection
  - ←/→ seeks 5 s (30 s with ⇧)
  - drag the zoomed waveform like a jog wheel, or click/drag the overview
  - **K** keeps the preview (moves it to the Inbox without downloading again); **⌫** discards it
  - skipping a preview cancels its transfer
- **Waveform styles:** Layered (Pioneer-like: lows behind, highs in front), Stacked (cumulative: blue centre, orange,
  white outside), Lanes (one band per lane). The style is remembered.
- **Albums and the supersede rule:** in "neon rain", "Download album" on a folder. "Sodium" is already in the Inbox as a
  single; when the album finishes, that single goes to the Trash, with Undo. In "salt flats" the album is flagged
  because "Tidewater" is already filed in your Singles; filed files are never touched.
- **Ownership badges:** Owned, Upgrade (you own a lower quality), Likely, Other mix, Inbox, This session, Earlier,
  Queued. Try "kassia moreau" and "tidal memory".
- **Fair use:** the peer column shows how many files you took from that peer today; flac_archivist is near the limit
  and dubplate_dan is at it (try downloading from them). See Peers & ratio.
- **Batch:** Batch search → Start searching. The demo clock ×10 makes pacing visible: one search every 8.5 s, keeping 8
  searches free. Review the matches, approve, then download; nothing downloads without approval.
- **Inbox → File:** each item shows what filing will do ("Converts to MP3 320 CBR", "Already MP3 320: moves as-is",
  "below target"). File one and watch it land in Collection.
- **Converter:** switch the target format to see the quality warnings and the Vorbis → ID3v2.3 tag mapping, including
  the cover.jpg that gets added.
- **Appearance:** the toolbar's half-circle button cycles System / Light / Dark.

## Design questions to answer while reviewing

1. Which waveform style is right: Layered, Stacked or Lanes? Are the zoom window (8 s default) and the overview height right?
2. Filter bar: is two rows of filters plus the "Hidden by filters" line the right density? Anything missing or unneeded?
3. Results: are Folders grouping as the default, and the column set (status, quality, peer · today, availability), right?
4. Inspector: is the content and its order (ownership, quality check, file, peer and daily limit) what you want to see?
5. Player: bottom placement, full width, controls. Should Keep be the main action?
6. Inbox: is the per-item "what filing will do" column clear? Should filing ask for confirmation?
7. Batch: is review-first with "Approve confident" the right flow?
8. Navigation: does the sidebar grouping (Searches / Library / Network / Tools) match how you think about the app?
