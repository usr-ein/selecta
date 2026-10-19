# Selecta UI mock

An interactive HTML/CSS/JS mock of Selecta's interface, used to decide **what goes where** in the Qt app in the visual
language of macOS. Its design language and layout are approved (PRD D25). Everything is simulated with sample data: no
network, no real files, no real Spotify or yt-dlp calls. Artists, peers and tracks are fictional.

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
| `#view=` | `batch` (Get tracks, the default), `search`, `inbox`, `collection`, `history`, `transfers` (Activity), `messages`, `shares`, `wishlist`, `converter`, `settings` |
| `&theme=` | `light`, `dark` |
| `&search=` | any query |

Example: `index.html#view=inbox&theme=light`.

## The main flow: Get tracks

The app opens on **Get tracks** (PRD D27). You give it a list; at the end every track is in your Inbox as MP3 320,
tagged from Spotify (ID3v2.3) with the cover embedded.

1. **Add tracks.** Paste lines like `Artist - Title`, or a Spotify playlist or album link. Typos are fine:
   `Okuma Drfit - Neon Rian` still finds *Okuma Drift — Neon Rain*.
2. **The release comes first.** Each line shows the release it matched (title and length) before any network search.
3. **Find them.** Searches are paced by the network governor: up to 6 at once, then one every 30 s, shared with your
   own searches. The demo clock runs ×10. Each line ends as *Ready* (a file within ±3 s of the release length),
   *Check*, *Not found* or *Have it*.
4. **Preview.** Select a line and press **Space**. The full track plays through your installed yt-dlp with its 3-band
   waveform, from the preview cache only: it's never filed or shared. **⇧Space** streams the actual file from the
   Soulseek peer instead.
5. **Get.** Ready lines are ticked; untick what you don't want and press **Get N tracks**.
6. **Inbox.** Downloads are converted and tagged as they arrive. *File* moves them into your collection under the
   names from Settings → Filing.

## Other things to try

- **Search** (sidebar field) for digging: "neon rain" or "tidal memory". The release card ranks files by length and
  name. Same-name files of another length are hidden as "Not this release". Misspell a query and Selecta searches the
  network with the release's spelling and says so. **Search wider** drops words to catch misspelled file names.
  **Filters** reveals formats, bitrate, sample rate and the rest.
- **Waveform styles.**
  - **Pioneer** (default) follows rekordbox/CDJ 3Band, as described in `references/audio-preview-and-waveform.md` §2.
  - **Layered**, **Stacked** and **Lanes** are there for comparison.
- **Albums and the Inbox rule.** In "neon rain", pick the album chip and download the album. "Sodium" is already in your
  Inbox as a single; when the album finishes, that single goes to the Trash, with Undo. Files already in your
  collection are only flagged, never touched.
- **Naming.** Settings → Filing has the templates (`{artist} - {title}` for singles) and the name cleaner with a live
  preview. The cleaner keeps Unicode letters but turns spaces, quotes, `%`, `$`, `/`, `:` and the like into path-safe
  names.
- **Preview source.** Settings → Preview: automatic (yt-dlp, else a 30-second clip), yt-dlp only, the clip only, or
  always the peer's file.
- **Fair use.** Activity shows your ratio, transfers and how much you took from each peer today.

## Design questions still open

1. Is the Pioneer waveform faithful enough? Are the zoom window (8 s) and the overview height right?
2. Should a *Check* line (length or version unsure) need a preview before Get, or just a tick?
3. What else can go: the Converter view, the Wishlist, the search filters?
