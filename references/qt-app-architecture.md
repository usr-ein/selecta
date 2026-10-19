# Qt 6 application architecture for Selecta (toolkit, build, dependencies, CI, packaging, licensing)

> Status: **draft in progress** (research 2026-10-19). Items marked **TODO(verify)** come from prior knowledge and have
> not yet been checked against a primary source. Confidence: **high** = checked in source code or official docs;
> **medium** = consistent secondary sources or strong prior knowledge; **low** = inference.
> Source paths like `qtbase@v6.12.0:src/...` refer to shallow clones in `local/research-clones/qt/` (read only).

## TL;DR

- **Toolkit (ADR-001): Qt Widgets for the whole UI, plus a small Objective-C++ "mac shell"** for what Qt does not
  draw natively: an `NSToolbar` (search field, segmented controls, status items), native context menus
  (`QMenu::toNSMenu()`), notifications, vsync ticks, optionally the sidebar material. **No Qt Quick in v1.**
  Decisive facts (all checked in Qt 6.12 source): Qt Quick has **no accessible table/tree implementation** (VoiceOver
  cannot navigate a Quick `TableView` as a table), and its macOS style has **no header view, toolbar or tab bar**;
  Qt Widgets has mature sortable item views that bridge to `NSAccessibilityTableRole`/`OutlineRole`, and its controls
  are drawn by AppKit, already adapted to **Liquid Glass** metrics in 6.11/6.12. Qt's weak spot on macOS 26/27 is the
  **window chrome** (toolbar, sidebar, menus' material), not the controls — that is where the Objective-C++ goes.
- **Qt version: Qt 6.12 LTS (6.12.0 released 2026-09-30; 6.12.1 planned 2026-11-12).** Open-source users get no LTS
  patch stream beyond the ~6-month standard window (6.12 open-source support ends ≈ 2027-04-01), so plan to move to
  each new minor (6.13 ≈ spring 2027). Qt 6.12 requires **macOS 14.4+** at runtime, **Xcode 16 / macOS 15 SDK+** to
  build, CMake ≥ 3.25. Local machine: Homebrew Qt **6.11.1** (6.11 is already out of open-source support).
- **Liquid Glass:** an app built with the macOS 26+ SDK runs with Liquid Glass; the `UIDesignRequiresCompatibility`
  opt-out only works on macOS 26, and Qt assumes it is ignored on macOS 27 (the maintainer's OS). Design for glass.
- **Build:** CMake ≥ 3.28 + Ninja + `CMakePresets.json`, `qt_standard_project_setup(REQUIRES 6.12)`, C++20, warnings as
  errors in CI, ASan/UBSan and TSan presets, clang-tidy/clang-format from Homebrew LLVM, ccache. (Sections 5–6 TODO.)
- **Licensing:** Qt Widgets/Core/Gui/Network/Sql/Multimedia are `LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`;
  Qt Canvas Painter, Charts, Graphs and HTTP Server are **`GPL-3.0-only`** for open-source users — fine for a GPLv3 app,
  but it pins distributed binaries to GPLv3 terms (relevant to PRD OQ-7). Never bundle SF Symbols images or SF fonts.

## Key findings

1. **(high) Qt 6.12 LTS is the current release**; Qt 6.12's `.cmake.conf` sets `QT_SUPPORTED_MIN_MACOS_VERSION 14.4`,
   min SDK 15, max SDK 27, min Xcode 16, "max macOS tested" 26 (`qtbase@v6.12.0:.cmake.conf`). Qt 6.11 was macOS 13+.
   The Homebrew Qt 6.11.1 bottle on this machine is built with `minos 14.0` (`otool -l QtCore`).
2. **(high) Liquid Glass detection** lives in `qt_apple_runningWithLiquidGlass()` (`qtbase@v6.12.0:src/corelib/kernel/qcore_mac.mm`):
   true when the binary's SDK ≥ 26 and the OS ≥ 26; the Info.plist opt-out is honoured only below macOS 27
   ("Word on the street is that the opt out will only work for macOS 26"). `QMacStyle` has ~33 Liquid Glass branches
   (button/text-field/tab/slider metrics) in both 6.11.1 and 6.12.0. Qt's official word (blog, 2025-09-05): "most"
   style issues ironed out, glass effects for app developers still "being looked into".
3. **(high) Unified toolbar in Qt 6.12** (`QMainWindow::setUnifiedTitleAndToolBarOnMac`) is now built on
   `Qt::ExpandedClientAreaHint` + `Qt::NoTitleBarBackgroundHint`; before Tahoe Qt adds a titlebar `NSVisualEffectView`,
   on Tahoe+ the title bar is fully transparent with no material (`qmainwindowlayout.cpp`). Toolbar buttons remain
   Qt-drawn `QToolButton`s, so they **do not get the Liquid Glass toolbar capsules** that native `NSToolbar` items get.
   Its doc still warns it is unsupported with OpenGL content "including QOpenGLWidget" (`qmainwindow.cpp`).
4. **(high) Context menus:** `QMenu::popup()/exec()` draws a Qt widget (`PE_PanelMenu` fills `palette.window()` in a
   rounded path — no system material). The menu **bar** is native. Public `QMenu::toNSMenu()` (macOS only) returns a
   synced native `NSMenu`, so native context menus need only a few lines of Objective-C++. Qt Quick's macOS style
   defaults menus to `Popup.Native`.
5. **(high) SF Symbols:** `QIcon::fromTheme(name)` on macOS goes through `QAppleIconEngine`; freedesktop names are mapped
   to SF Symbols and **any other name is passed straight to `+[NSImage imageWithSystemSymbolName:]`**, so
   `QIcon::fromTheme("waveform")` works. Native menu items receive the `NSImage` itself (crisp, AppKit-configured).
6. **(high) Appearance APIs:** `QStyleHints::setColorScheme()/unsetColorScheme()` (in-app light/dark override),
   `QPalette::Accent` = `NSColor.controlAccentColor`, `Highlight` = `selectedContentBackgroundColor` (inactive:
   `unemphasizedSelectedContentBackgroundColor`), `AlternateBase` from `alternatingContentBackgroundColors`
   (`qcocoatheme.mm`). `QGuiApplication::setBadgeNumber()` exists (Dock badge). `QMessageBox` uses `NSAlert`.
7. **(high) Accessibility:** Widgets' `QAccessibleTable`/`QAccessibleTree` (`src/widgets/accessible/itemviews_p.h`)
   are bridged to `NSAccessibilityTableRole`/`OutlineRole` with synthesized rows and cells
   (`qcocoaaccessibilityelement.mm`). `qtdeclarative@v6.12.0:src/quick/accessible/` implements only items, Flickable,
   ListView and text — **no table interface for Quick `TableView`/`TreeView`**.
8. **(high) Qt Quick macOS style** (`qtdeclarative@v6.12.0:src/quickcontrols/macos/`) provides buttons, fields,
   sliders, menus, `SearchField`, `TreeViewDelegate`, `SelectionRectangle` — **no `HorizontalHeaderView`, `ToolBar`,
   `TabBar` or `SplitView`** (they fall back to the Basic style). `QtQuick.Controls.Native` (new in 6.12) is just an
   alias that resolves to the macOS style on macOS.
9. **(high) New modules:** Qt Canvas Painter (GPU 2D painter over QRhi; `QCanvasPainterWidget` is a `QRhiWidget`
   subclass) left Technology Preview in 6.12 — licence `GPL-3.0-only` for open source. Qt TaskTree (async task
   composition, LGPL/GPL) is present since 6.11.
10. **(high) Widget repaint pacing:** `QWidget::update()` posts a low-priority `UpdateRequest`
    (`qwidgetrepaintmanager.cpp`); it is not tied to the display link. `QWindow::requestUpdate()` is
    `CVDisplayLink`-driven on macOS (`qcocoascreen.mm`), but on a `QWidgetWindow` it repaints the whole top-level
    widget (`qwidgetwindow.cpp`). A smooth 120 Hz waveform therefore needs its own vsync source (section 4).

## 1. Qt 6 release landscape

| Item | Value | Confidence / source |
|---|---|---|
| Latest stable | **Qt 6.12.0 LTS**, released 2026-09-30 | high — Qt blog "Qt 6.12 LTS Released!", wiki Qt_6.12_Release |
| Next patches | 6.12.1 2026-11-12, 6.12.2 2027-01-14, 6.12.3 2027-03-04 (planned) | high — wiki Qt_6.12_Release |
| LTS terms | New LTS every 2 years, 5 years of maintenance — **for commercial licensees**. Open-source users "are expected to update to every new minor release" (~6 months). Commercial LTS code is opened "within 12 months". | high (blog) / medium (endoflife.date summary) |
| Open-source support windows | 6.8: ended 2025-04-02 · 6.10: ended 2026-04-07 · 6.11: ended 2026-09-30 · **6.12: until ≈ 2027-04-01** | medium — endoflife.date (updated 2026-10-04) |
| Latest 6.11 patch | 6.11.2 (local Homebrew has 6.11.1) | medium — endoflife.date |
| Qt 6.12 macOS runtime | **macOS 14.4 or later** ("including macOS 26"); arm64, x86_64, x86_64h; universal via `CMAKE_OSX_ARCHITECTURES` | high — doc.qt.io/qt-6/macos.html + `.cmake.conf` |
| Qt 6.12 build env | Xcode 16 (macOS 15 SDK) or later; SDK ≤ 27 supported | high — same |
| Qt 6.11 (installed) | min macOS 13, SDK ≥ 14, Xcode ≥ 15 | high — `/opt/homebrew/opt/qt/lib/cmake/Qt6/Qt6ConfigExtras.cmake` |
| CMake to use Qt 6.12 | ≥ 3.25 | high — whatsnew612, `.cmake.conf` |
| Homebrew bottle | Qt 6.11.1, `minos 14.0`, SDK 26.4 | high — local `otool -l` |
| Local toolchain | macOS 27.0.1, Xcode 27.0, Apple clang 21, CMake 4.3.4, Ninja | high — local |

Notes:
- macOS 27 is not yet in Qt 6.12's "tested" list (max tested 26), but Qt now states it is "generally
  forward-compatible" with new OS versions (blog 2025-09-05; macos.html: new OS *features* are "not typically
  included in patch releases").
- Qt 6.12 additions useful to Selecta: `QRangeModel` gains `sort()` with a collator, `match()`, header data and
  drag-and-drop; `QSortFilterProxyModel::dataChangeRelevanceFlags()` lets a proxy skip re-sorting/re-filtering on
  irrelevant `dataChanged` (useful while results stream in); `QTreeView::itemDecorationRect()`; Qt Test benchmarks
  report variance/stddev; `QAudioSink::setNativePeriodFrameCount()`; the unified toolbar rework above.
  (high — doc.qt.io/qt-6/whatsnew612.html)

## 2. Qt Widgets vs Qt Quick vs hybrid (decision: ADR-001)

### 2.1 What "the visual language of macOS" means in late 2026

The maintainer runs **macOS 27** (local `sw_vers`: 27.0.1). Since macOS 26 the system look is **Liquid Glass**:
rounder, taller controls; toolbar items grouped in glass capsules; a sidebar that floats as a glass panel inset from
the window edge; glass menus and popovers. An app gets this look by using AppKit controls and AppKit window chrome
(`NSToolbar`, `NSSplitViewController` sidebar/inspector items) built with the 26+ SDK. Opting out
(`UIDesignRequiresCompatibility`) is a stopgap that Qt itself treats as ineffective on macOS 27. (Search results
also mention a Liquid Glass "revamp" announced at WWDC 2026 for macOS 27 — hearsay, low; it would mean another round
of style adjustments in Qt.)

So "native" decomposes into three layers with different Qt answers:

| Layer | Examples in Selecta | Qt Widgets | Qt Quick (Controls, macOS style) |
|---|---|---|---|
| **Controls** | buttons, pop-up buttons, checkboxes, sliders, text fields, scroll bars, table headers | Drawn by AppKit cells through `QMacStyle` → genuinely native, Liquid-Glass-aware (high) | Drawn by a fork of `QMacStyle` (`quicknativestyle/qstyle/mac/qquickmacstyle_mac.mm`) — same quality for the controls it covers; **no header view, toolbar, tab bar** (high) |
| **Content views** | 10 000-row sortable grouped results, source list, transfer list, inspector forms | `QTreeView`/`QTableView`/`QHeaderView`: sorting, resizable/movable/hideable columns, multi-selection, type-ahead, keyboard navigation, uniform row heights, persistent editors — mature (high) | `TableView`/`TreeView`/`HorizontalHeaderView`: fast and virtualised but lower level; sort indicators, column moving/hiding, header context menus and native look are DIY (medium-high) |
| **Window chrome** | unified toolbar + search field, floating sidebar, inspector, menus, context menus | Menu bar native; context menus Qt-drawn unless `toNSMenu()`; toolbar Qt-drawn on a transparent title bar; no sidebar material (high) | Menus native (`Popup.Native`); no toolbar, no sidebar material (high) |

### 2.2 Requirement-by-requirement comparison

| Need (PRD) | Qt Widgets | Qt Quick | Winner |
|---|---|---|---|
| Dense sortable tables, grouped by folder, 10 k streaming rows (SRCH-10, NFR-2) | `QTreeView` + own model; `QHeaderView` sort indicators drawn natively | `TableView`/`TreeView` + custom header and delegates | Widgets |
| Keyboard audition ↑/↓, Space, ←/→, Return (PREV-4) | Item views have complete keyboard models; override keys in the view | Possible, more manual focus handling | Widgets |
| VoiceOver for tables, badges, waveform (UI-5) | Table/outline bridged with rows/cells; custom `QAccessibleInterface` for waveform | No table/tree accessibility interface in 6.12 | **Widgets (decisive)** |
| Source-list sidebar | `QTreeView` + custom delegate (counts, SF Symbols, rounded selection) | `ListView` + delegate | Tie (custom either way) |
| Unified toolbar with search, budget, ratio, status | `setUnifiedTitleAndToolBarOnMac` (Qt-drawn items) or native `NSToolbar` via Obj-C++ | Draw into the title bar with `ExpandedClientAreaHint`; or native `NSToolbar` | Tie; native `NSToolbar` best for both |
| Menu bar, shortcuts, Settings… role merging | Native, `QAction::MenuRole`, `QKeySequence::StandardKey` | Native `MenuBar` | Tie |
| Context menus | Qt-drawn; native via `QMenu::toNSMenu()` | Native by default | Quick (Widgets fixable cheaply) |
| Segmented controls | No native control; `QTabBar` is drawn with clipped `NSButton`s ("NSSegmentedControl and NSSegmentedCell are letting us down", `qmacstyle_mac.mm`) | None | Tie (native `NSSegmentedControl` in the toolbar via Obj-C++) |
| Light/dark + in-app override + accent (UI-2) | `QStyleHints::setColorScheme`, palette roles | Same | Tie |
| SF Symbols | `QIcon::fromTheme("<symbol>")` | `icon.name` → same engine | Tie |
| Vibrancy / glass behind sidebar | Obj-C++ (`NSVisualEffectView`/`NSGlassEffectView` behind a transparent widget) | Same | Tie |
| Custom 60/120 fps waveform (PREV-2) | QPainter + tile cache; `QRhiWidget`/`QCanvasPainterWidget` for GPU; vsync via Obj-C++ display link | Scene graph item, vsync-driven render loop — natural fit | Quick |
| Native dialogs (open panel, alerts, sheets) | `QFileDialog`/`QMessageBox` native; window-modal dialogs as sheets | `FileDialog`/`MessageDialog` native | Tie |
| Team/agent productivity, examples, tests | C++ only; `QAbstractItemModelTester`, Qt Test widgets helpers | C++ + QML, two languages, QML tooling | Widgets |

### 2.3 Recommendation

**Use Qt Widgets for the entire v1 UI; do not use Qt Quick (no hybrid) unless a later spike proves the waveform
cannot be made smooth in Widgets.** Confidence: high for "Widgets over Quick"; medium for how much native chrome is
worth building (decide with the spike in 2.4).

Why: Selecta's core surface is dense, sortable, keyboard-driven, accessible tables. Widgets is the only Qt toolkit that
gives native-drawn table headers, a complete item-view keyboard model and VoiceOver table semantics out of the box.
Qt Quick's only real advantage here — vsync-driven GPU animation — matters for one widget (the waveform), which can be
done in Widgets (section 4). Hybrid `QQuickWidget` embedding adds a second language and a render-to-texture composition
path that also conflicts with the unified toolbar (finding 3).

Add a **mac shell** (`src/app/platform/mac/*.mm`, Objective-C++, behind small C++ interfaces with plain-Qt fallbacks for
Linux/Windows):

| Piece | Why | Effort | Priority |
|---|---|---|---|
| Native `NSToolbar` attached to the main window's `NSWindow` (search field as `NSSearchToolbarItem`, `NSSegmentedControl` items, budget/ratio/connection status as small native views, "Pause network", sidebar/inspector toggles) | Toolbar is the most visible Liquid Glass surface; Qt's toolbar gets no glass capsules | M (port ideas from Qt 5's `QMacToolBar` in qtmacextras 5.15 — TODO(verify) licence/state) | P0 polish, after the M1 spike |
| Native context menus via `QMenu::toNSMenu()` + `-[NSMenu popUpMenuPositioningItem:atLocation:inView:]` | Glass/material menus, SF Symbol images, native keyboard handling | S | P0 |
| Vsync ticker (`-[NSView displayLinkWithTarget:selector:]`, macOS 14+) driving waveform `update()` | Judder-free 120 Hz scrolling | S | P0 (M5) |
| Notifications (`UNUserNotificationCenter`) | No Qt API | S | P1 (UI-6) |
| Sidebar material (`NSVisualEffectView` / `NSGlassEffectView` behind a transparent `QTreeView`) | Floating glass sidebar look | M, fragile | P2 — spike only; accept a solid sidebar until then |
| Keychain (via QtKeychain), Trash (`QFile::moveToTrash`), quarantine xattr | Platform services | S | P0 |

Fallbacks: on Linux/Windows the same interfaces map to `QToolBar`, `QMenu::exec`, `QTimer`, `QSystemTrayIcon`.

Rejected alternatives:
- **Qt Quick (+ macOS style)** — no table accessibility, no native header/toolbar, QML + C++ split; Quick's strengths
  (animation, GPU) are needed in one place only.
- **Hybrid (Widgets + `QQuickWidget` for the waveform/player)** — keeps the option open, but costs a QML stack and a
  composition mode change for one view. Keep as fallback B for the waveform only.
- **AppKit window shell hosting Qt content** (`NSSplitViewController` with sidebar/inspector items, Qt widgets embedded
  as child `NSView`s; Qt 6.12 "now manages embedding into a foreign NSView on its own") — the most faithful Liquid Glass
  chrome, but focus traversal, shortcuts, drag-and-drop and accessibility across the native/Qt boundary are untested
  territory and the Linux/Windows builds need a second shell. Revisit only if the toolbar spike is not native enough.
- Third-party **qt-liquid-glass** (MIT per README, 109 stars, uses private `NSGlassEffectView` variants and `_setPath:`)
  — a useful reference for where to insert effect views, not a dependency (private APIs, early stage).

### 2.4 Spike to run in M1 (one day, decides how much mac shell to build)

On macOS 27, light and dark, accent colours blue/graphite/pink: (a) `QMainWindow` + `QToolBar` with
`setUnifiedTitleAndToolBarOnMac(true)`; (b) the same window with an `NSToolbar` attached through Obj-C++; (c) a
`QTreeView` sidebar on a transparent background over an `NSVisualEffectView`. Screenshot against Finder/Music/Mail and
let the maintainer choose. Also check: `QMenu::toNSMenu()` actions trigger correctly; a `QRhiWidget` in a unified-toolbar
window renders correctly (finding 3 warns about GL content).

## 3. Native macOS details to get right (checklist for M1 and every view)

Window and chrome
- [ ] Unified toolbar (Qt or native, per spike); window title + `NSWindow.subtitle` (e.g. current search) via Obj-C++.
- [ ] Content under the title bar where it helps (`Qt::ExpandedClientAreaHint`, `QWindow::safeAreaMargins()` to clear
      the traffic lights; `WA_ContentsMarginsRespectsSafeArea`).
- [ ] Single main window; closing it keeps transfers running (Music-style); Dock click reopens it; ⌘Q with active
      transfers asks in a sheet. Disable automatic window tabbing (single-window app) — TODO(verify) Qt default.
- [ ] Inspector toggle: PRD says ⌘I; macOS convention for "Show Inspector" is ⌥⌘I (Finder) — decide.
- [ ] Settings window in macOS style: toolbar-tab panes, instant apply, no OK/Cancel, ⌘, (`QAction::PreferencesRole`,
      label it "Settings…" as on macOS 13+).

Menus and shortcuts
- [ ] Set `QAction::MenuRole` explicitly (About/Settings/Quit) — Qt's default `TextHeuristicRole` moves items by
      matching their text.
- [ ] Qt only inserts a *hidden* Window menu (for the Dock window list, `qcocoamenubar.mm`); add a visible Window menu
      (Minimize ⌘M, Zoom, Bring All to Front) and a Help menu (AppKit adds the search field).
- [ ] Use `QKeySequence::StandardKey` where one exists; Space/←/→ overrides in item views must not break type-ahead.
- [ ] Native context menus (`toNSMenu`), with SF Symbol icons only where macOS apps use them (Tahoe menus do show
      symbols for common actions).

Controls and content
- [ ] Never hard-code control sizes or margins: Tahoe changed control heights/radii (QMacStyle's glass branches); use
      style metrics and layout defaults; test with and without Liquid Glass (Xcode-16-built or macOS 26 opt-out build).
- [ ] Tables: no grid, alternating rows from the palette, uniform row heights, sort indicator on click, header context
      menu to hide/show columns, persistent column state, type-ahead, tabular digits for numbers
      (`QFont::setFeature("tnum")` — TODO(verify) API name in 6.7+), right-aligned numeric columns, ellipsis middle for
      paths.
- [ ] Selection colours from the palette at paint time (accent changes live); unemphasized selection when the view or
      window is inactive.
- [ ] Sidebar: section headers in secondary label colour, SF Symbol icons tinted with the accent colour, count badges,
      rounded inset selection, row height following System Settings › Appearance › Sidebar icon size — TODO(verify)
      how to read it (`NSUserDefaults` `NSTableViewDefaultSizeMode`).
- [ ] Search field: `QLineEdit` with leading `magnifyingglass` action, clear button, ⌘F focus — or `NSSearchToolbarItem`.
- [ ] Segmented choices (Grouped/Flat, Any/=/≥/≤): native `NSSegmentedControl` in the toolbar; in content use
      `QTabBar`-style or radio-like `QToolButton` groups.
- [ ] Overlay scroll bars follow the system setting (QMacStyle transient scroll bars) — TODO(verify).
- [ ] System font everywhere (`QFontDatabase::systemFont`); no bundled fonts; SF Mono only for paths/logs if at all.
- [ ] Dark waveform "screen" in both appearances (UI-3); everything else follows the palette.
- [ ] Colour is never the only signal (UI-4): symbol + label in badges.

Platform integration
- [ ] VoiceOver: accessible names for toolbar items and icon-only buttons; `QAccessibleValueInterface` for the
      waveform (position/duration) and budget/ratio meters; `QAccessibleAnnouncementEvent` (present in 6.11 headers) for
      "download finished"-type events.
- [ ] Keyboard: Full Keyboard Access / Tab focus behaviour from `QStyleHints::tabFocusBehavior`.
- [ ] Drag and drop: `.txt` tracklists into Batch; Inbox files out to Finder/rekordbox (file URLs).
- [ ] Dock badge (`QGuiApplication::setBadgeNumber`), notifications (Obj-C++), `QFile::moveToTrash` for Inbox/cache.
- [ ] High-DPI: all custom painting at `devicePixelRatio`; cached pixmaps carry `setDevicePixelRatio`.
- [ ] Local Network privacy prompt (macOS 15+) when connecting to LAN peers/test servers: `NSLocalNetworkUsageDescription`
      in Info.plist — TODO(verify) whether loopback/Docker is exempt; see section 10.

## 4. Waveform rendering in a Widgets app

Targets (PRD PREV-2, NFR-2): zoomed scrolling view + overview, 60 fps (120 on ProMotion), dark background, buffered
range overlay. Analysis (bands, peaks) is the audio note's job; this section is only about getting pixels on screen.

| Option | How | Pros | Cons | Verdict |
|---|---|---|---|---|
| A. `QWidget` + QPainter (raster) | Pre-render the 3-band waveform into tiles (`QImage`/`QPixmap`, e.g. 512 px wide at device pixels); each frame blit the visible tiles at the playhead offset, draw playhead/cue overlays | Simple, deterministic, works with the unified toolbar, no GPU state; blitting a ~2400×300 device-pixel area per frame is cheap on Apple Silicon | CPU path; tile cache must be rebuilt on zoom/resize/appearance change | **Default** |
| B. `QRhiWidget` (6.7+) | Metal through QRhi: vertex buffers of band envelopes, scrolling by a uniform | GPU, scales to huge zoom ranges | The whole top-level window switches to render-to-texture composition once a texture child exists (`textureChildSeen` in `qwidgetrepaintmanager.cpp`); unified toolbar docs warn against GL content (TODO(verify) for RHI) | Fallback if A misses budget |
| C. `QCanvasPainterWidget` (6.11; supported in 6.12) | QPainter-like vector API on the GPU (a `QRhiWidget` subclass) | Familiar API, GPU | Same composition caveat as B; `GPL-3.0-only`; young module | Watch |
| D. `QQuickWidget`/window container with a scene-graph item | Quick render loop, vsync animation | Smoothest animation model | Second stack (QML), composition caveats; a `createWindowContainer` child is an opaque native view (fine for a dark screen) with focus/a11y quirks | Fallback for the waveform only |

Frame pacing: `update()` is not vsync-aligned (finding 10). Drive the playhead animation from a display link —
on macOS 14+ `-[NSView displayLinkWithTarget:selector:]` (CADisplayLink for AppKit; Qt 6.12's minimum macOS 14.4
guarantees it) — calling `update()` on the waveform only, and compute the playhead position from the audio clock at
paint time (never accumulate per-frame deltas). Fallback elsewhere: `QTimer` at `QScreen::refreshRate()` with
`Qt::PreciseTimer`. TODO(verify experimentally): ProMotion actually presents at 120 Hz for a raster-backed Qt window.

## 5. Modern CMake structure

TODO (next commit).

## 6. Dependency management

TODO.

## 7. Architecture patterns

TODO.

## 8. Testing

TODO.

## 9. CI

TODO.

## 10. macOS packaging and distribution

TODO.

## 11. GPLv3 licensing hygiene and dependency compatibility

Qt module licences (open-source side, from SPDX headers of the local 6.11.1 install):

| Qt module(s) | SPDX |
|---|---|
| Core, Gui, Widgets, Network, Sql, Concurrent, Test, Multimedia, Svg, Quick, QuickControls2, QuickWidgets, StateMachine, TaskTree | `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only` |
| CanvasPainter, Charts, Graphs, HttpServer | `LicenseRef-Qt-Commercial OR GPL-3.0-only` |

TODO: dependency table.

## 12. Proposed repository tree and dependency graph

TODO.

## Implications for Selecta

TODO (see TL;DR for the toolkit decision).

## Open questions and experiments

TODO.

## Sources

Accessed 2026-10-19.

- Local Homebrew Qt 6.11.1, `/opt/homebrew/opt/qt` (headers, CMake config, `otool`) — API availability, licences, min macOS.
- qtbase v6.12.0 and v6.11.1 (github.com/qt/qtbase, shallow clones) — `.cmake.conf`; `src/corelib/kernel/qcore_mac.mm`
  (Liquid Glass detection); `src/plugins/styles/mac/qmacstyle_mac.mm` (glass metrics, menus, tabs);
  `src/widgets/widgets/qmainwindow.cpp`, `qmainwindowlayout.cpp` (unified toolbar); `src/widgets/widgets/qmenu.cpp`,
  `qmenu_mac.mm` (popup vs `toNSMenu`); `src/gui/platform/darwin/qappleiconengine.mm` (SF Symbols);
  `src/plugins/platforms/cocoa/qcocoatheme.mm` (palette/accent), `qcocoamenubar.mm` (Window menu),
  `qcocoaaccessibility*.mm` (VoiceOver roles), `qcocoascreen.mm` (display link);
  `src/widgets/kernel/qwidgetrepaintmanager.cpp`, `qwidgetwindow.cpp` (repaint pacing);
  `src/widgets/accessible/itemviews_p.h`.
- qtdeclarative v6.12.0 (sparse clone) — `src/quickcontrols/macos/`, `src/quickcontrols/native/CMakeLists.txt`,
  `src/quicktemplates/qquickmenu.cpp`, `src/quick/accessible/`.
- https://www.qt.io/blog/qt-6.12-released — 6.12 LTS, LTS cadence/terms, Canvas Painter status.
- https://wiki.qt.io/Qt_6.12_Release — release and patch dates.
- https://doc.qt.io/qt-6/whatsnew612.html — 6.12 feature list (unified toolbar rework, QRangeModel, Canvas Painter, CMake ≥ 3.25).
- https://doc.qt.io/qt-6/macos.html — supported macOS (14.4+), build requirements, architectures, forward compatibility.
- https://endoflife.date/qt — open-source vs commercial support windows (secondary source).
- https://www.qt.io/blog/qt-on-macos-26-tahoe — Liquid Glass status and workarounds (2025-09-05, Tor Arne Vestbø).
- https://github.com/fsalinas26/qt-liquid-glass — third-party glass effect views for Qt (reference only).
- https://github.com/opencloud-eu/desktop/issues/721 — a Qt Widgets app enabling Liquid Glass with Qt 6.10.2/6.11.
