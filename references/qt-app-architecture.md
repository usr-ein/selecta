# Qt 6 application architecture for Selecta (toolkit, build, dependencies, CI, packaging, licensing)

> Status: **draft in progress** (research started 2026-10-19). Items marked **TODO(verify)** are from prior knowledge
> and have not yet been checked against a primary source. Confidence levels: **high** = checked in source/docs,
> **medium** = consistent secondary sources or strong prior knowledge, **low** = inference.

## TL;DR

- **Toolkit: Qt Widgets for the whole app, plus a thin Objective-C++ "mac shell" for the window chrome** (native
  `NSToolbar`, sidebar material, notifications, Dock, Keychain). Qt Quick is *not* recommended for v1: its macOS
  style covers only basic controls, it has no native-looking sortable table/outline view, and dense, keyboard-driven,
  accessible tables are Selecta's core surface. The waveform is a custom widget (QPainter with cached tiles first;
  `QRhiWidget` or the new Qt 6.11 `QCanvasPainterWidget` if profiling demands GPU). (Confidence: medium-high; details below.)
- **Qt version:** build against **Qt 6.11.x** (installed locally: 6.11.1 via Homebrew). Qt 6.11's CMake config
  declares **macOS 13** as the minimum supported runtime (checked in `Qt6ConfigExtras.cmake`), but the Homebrew bottle
  binaries are built with `minos 14.0` (checked with `otool`). TODO(verify): Qt 6.12 status and LTS policy.
- **Build:** CMake ≥ 3.28 with `qt_standard_project_setup(REQUIRES 6.11)`, `CMakePresets.json`, C++20, Ninja,
  warnings-as-errors in CI, ASan/UBSan and TSan presets, clang-format/clang-tidy from Homebrew LLVM, ccache.
- **Dependencies:** Homebrew for day-to-day macOS development; a **minimal, audio-only FFmpeg** and pinned deps for
  release builds (TODO: vcpkg vs self-built — section 6). FetchContent only for small CMake-native deps.
- **Packaging:** `.app` via `qt_generate_deploy_app_script`/`macdeployqt`, hardened runtime, DMG; ad-hoc signing for
  "me first" builds, Developer ID + notarisation if the maintainer gets an Apple account. TODO(verify): Homebrew cask
  policy on unsigned/un-notarised apps.
- **Licensing:** every planned dependency is GPLv3-compatible when Selecta is GPL-3.0-or-later; watch FFmpeg build
  flags (no `--enable-nonfree`) and never bundle SF Symbols images or SF fonts.

## Key findings

1. (high) Qt 6.11.1 exposes the APIs Selecta needs for appearance: `QStyleHints::setColorScheme()` (in-app
   light/dark override), `QPalette::Accent`, `QIcon::ThemeIcon` / `QIcon::fromTheme()`, `Qt::ExpandedClientAreaHint`
   and `Qt::NoTitleBarBackgroundHint` (content under the title bar), `QWindow::safeAreaMargins()`, `QRhiWidget`.
   (Checked in local headers.) `QMacNativeWidget`/`QMacCocoaViewContainer` are gone in Qt 6 (no headers).
2. (high) Qt 6.11 ships new modules relevant to us: **Qt Canvas Painter** (`QCanvasPainterWidget`,
   `QCanvasPainterItem` — GPU 2D painting via QRhi) and **Qt TaskTree** (async task composition). TODO(verify)
   their support status (tech preview?) and licences.
3. TODO: macOS 26 "Liquid Glass" and Qt — how QMacStyle and the Qt Quick macOS style look on macOS 26/27.
4. TODO: Qt release landscape and LTS terms for open-source users.

## 1. Qt 6 release landscape

TODO(verify) — expected content: latest stable, LTS versions and their open-source terms, release cadence, minimum
macOS per version, Homebrew/aqtinstall availability.

| Item | Value | Confidence / source |
|---|---|---|
| Installed locally | Qt 6.11.1 (Homebrew bottle, `/opt/homebrew/opt/qt`) | high — `qtpaths --qt-version` |
| Min macOS declared by Qt 6.11 | 13 (SDK ≥ 14, Xcode ≥ 15) | high — `lib/cmake/Qt6/Qt6ConfigExtras.cmake` |
| Homebrew bottle `minos` | 14.0 (built with SDK 26.4) | high — `otool -l QtCore` |
| Local toolchain | macOS 27.0.1, Xcode 27.0, Apple clang 21, CMake 4.3.4, Ninja | high — local |

## 2. Qt Widgets vs Qt Quick vs hybrid (decision: ADR-001)

### 2.1 Requirements that drive the decision

UI-1…UI-5 and NFR-2: dense sortable tables (10 000+ streaming rows, grouped by folder), a source-list sidebar, a
unified toolbar with a search field, the standard menu bar, segmented controls, VoiceOver, light/dark + accent, SF
Symbols, vibrancy, and a custom 60/120 fps waveform.

### 2.2 Comparison

TODO: fill the table from verification (Qt source + docs).

| Need | Qt Widgets (QMacStyle) | Qt Quick Controls (macOS style) |
|---|---|---|
| Sortable dense tables / outline | Mature `QTreeView`/`QTableView`/`QHeaderView` | `TableView`/`TreeView`/`HorizontalHeaderView` — lower level |
| Menu bar, context menus | Native global menu bar | TODO(verify) native menus since 6.8 |
| Toolbar | `setUnifiedTitleAndToolBarOnMac` (Qt-drawn) | none |
| VoiceOver | Mature for standard widgets incl. tables | TODO(verify) |
| Light/dark, accent | Yes | Yes |
| SF Symbols | TODO(verify) `QIcon::fromTheme` Apple icon engine | same |
| Waveform | QPainter / `QRhiWidget` / `QCanvasPainterWidget` | `QQuickItem` + scene graph / `QCanvasPainterItem` |

### 2.3 Recommendation

Qt Widgets + Objective-C++ mac shell. TODO: justify with verified facts; list of native details to get right.

## 3. Native macOS details to get right (checklist)

TODO.

## 4. Waveform rendering options in a Widgets app

TODO.

## 5. Modern CMake structure

TODO.

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

TODO.

## 12. Proposed repository tree and dependency graph

TODO.

## Implications for Selecta

TODO.

## Open questions and experiments

TODO.

## Sources

Accessed 2026-10-19.

- Local Homebrew Qt 6.11.1 install, `/opt/homebrew/opt/qt` (headers, CMake config) — API availability, min macOS.
