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
| `&group=` | the Collection's grouping: `date`, `artist`, `genre`, `quality` |
| `&demo=1` | a mid-run state for screenshots: every kind of status on screen, something selected in each view |

Example: `index.html#view=inbox&theme=light&demo=1`.

## The look (P-003)

macOS 26 materials and standard macOS spacing, modelled on Strongbox: a source-list sidebar, a clean table, a quiet
detail panel. Fewer words and columns on screen; the details are in the inspector.

**Materials**, and what the Qt app uses for each (phase 2):

| Part | In the mock | In the app |
|---|---|---|
| Window behind the sidebar | glass: the desktop, blurred and tinted | `NSVisualEffectView`, material `.sidebar`, blending `.behindWindow`, through Objective-C++ like `MacNotifications.mm` |
| Sidebar | a floating rounded panel, 8 px in from the window edges | drawn by the sidebar on top of that view |
| Toolbar | no bar: items in glass capsules over the content | the toolbar's buttons painted as capsules |
| Content, inspector | opaque: `textBackgroundColor`; the inspector one step off it | Qt palette from the system colours |
| Player | its own bar; the waveform screen stays dark | as now |

**Spacing**: one scale, in `styles.css` (`--sp-1` … `--sp-6`): 4, 8, 12, 16, 20, 28 px. The window margin is 20 px;
related controls are 8 px apart; one-line rows are 28 px, two-line rows 44 px; controls are 28 px (22 px small). Views
use nothing else.

**Statuses**: an SF Symbol, tinted with the status colour, then a short word in the label colour. Colour says the kind:

| Kind | Colour | Symbols and words |
|---|---|---|
| Waiting | grey | `clock` Waiting, Waiting to search · `hourglass` Queued at the peer · `minus.circle.fill` No release |
| Working | blue | spinner Searching…, Looking up… · `arrow.down.circle.fill` Downloading 42 % · `arrow.triangle.2.circlepath` Converting |
| Release found | indigo | `opticaldisc.fill` Release found |
| Ready | green | `checkmark.circle.fill` Ready, Saved |
| Check | orange | `exclamationmark.triangle.fill` Check, Below your quality · `questionmark.circle.fill` Release unsure |
| Failed | red | `xmark.circle.fill` Not found · `exclamationmark.octagon.fill` Download failed |
| You have it | purple | `checkmark.seal.fill` You have it |
| In your Inbox | cyan | `tray.full.fill` In your Inbox |
| Upgrade | teal | `arrow.up.circle.fill` Upgrade |

**Shown as P-023 ships them**: "Save to Collection" in the Inbox; the sidebar's accent blue lighter in dark mode;
Download and Add to Wishlist pinned right in the search bar (in a narrow window the left controls shrink and the buttons
keep only their icons); inspector text that wraps; preview controls as standalone, centred glyphs.

**Collection** is P-010's cover wall: one tile per loose track, one per album (opens to its tracks); grouped by date
added, artist, genre or quality (worst first); a quality badge below 320 kbps and an upgrade mark; select covers, then
Search for Better Quality.

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
6. **Inbox.** Downloads are converted and tagged as they arrive. *Save to Collection* moves them into your collection
   under the names from Settings → File names.

## Other things to try

- **Search** (sidebar field) for digging: "neon rain" or "tidal memory". The release card ranks files by length and
  name. Same-name files of another length are hidden as "Not this release". Misspell a query and Selecta searches the
  network with the release's spelling and says so. **Search wider** drops words to catch misspelled file names.
  **Filters** reveals formats, bitrate, sample rate and the rest.
- **Waveform.** The Pioneer style follows rekordbox/CDJ 3Band, as described in `references/audio-preview-and-waveform.md`
  §2. It is the only style the app draws.
- **Albums and the Inbox rule.** In "neon rain", pick the album chip and download the album. "Sodium" is already in your
  Inbox as a single; when the album finishes, that single goes to the Trash, with Undo. Files already in your
  collection are only flagged, never touched.
- **Naming.** Settings → File names has the templates (`{artist} - {title}` for singles) and the name cleaner with a live
  preview. The cleaner keeps Unicode letters but turns spaces, quotes, `%`, `$`, `/`, `:` and the like into path-safe
  names.
- **Preview source.** Settings → Previews: YouTube (through your yt-dlp), else a 30-second clip; YouTube only; the clip
  only; or always the peer's file.
- **Fair use.** Activity → Peers shows your ratio and how much you took from each peer today.

## Design questions still open

1. Is the Pioneer waveform faithful enough? Are the zoom window (8 s) and the overview height right?
2. Should a *Check* line (length or version unsure) need a preview before Get, or just a tick?
3. What else can go: the Converter view, the Wishlist, the search filters?
4. (P-003) In search results the status is a symbol alone, its words in the tooltip and the inspector. Enough?
5. (P-003) Should the player and its waveform hide when nothing has played, to give the tables and the cover wall room?
