# ADR-001: UI toolkit — Qt Widgets with an Objective-C++ mac shell

- **Status:** Accepted. The native `NSToolbar` part waits for the M1 chrome spike
- **Date:** 2026-10-19
- **Inputs:** `references/qt-app-architecture.md` §2–4; RESEARCH-SYNTHESIS §1.4; PRD UI-1…8

## Context

Selecta must feel like a native Mac app (UI-1, D9). Its core screens are large, sortable, streaming tables, which
VoiceOver must read as tables (UI-5). Its waveform needs smooth 60–120 Hz animation.

- Qt Quick's macOS style has no header view, toolbar or tab bar.
- Qt Quick has no accessible table implementation.
- Qt Widgets' item views bridge to `NSAccessibilityTableRole`, and its controls are drawn by AppKit, already adapted
  to Liquid Glass.

## Decision

- **Qt Widgets for the whole UI.** No Qt Quick or QML in v1.
- **A small Objective-C++ layer behind `PlatformShell` interfaces** for what Qt doesn't do natively:
  - native context menus (`QMenu::toNSMenu()`);
  - notifications and the Dock badge;
  - a display-link (vsync) ticker;
  - possibly a native `NSToolbar` and sidebar material, decided by the M1 chrome spike, which the maintainer judges.
- **The waveform** is a plain `QWidget` that paints cached tiles with QPainter, ticked by the display link.
  `QRhiWidget` is the upgrade path, only if profiling demands it.
- **Appearance** uses `QStyleHints::setColorScheme()` for the System / Light / Dark setting and palette roles
  everywhere. The waveform screen is the exception: it stays dark (UI-3).

## Consequences

- Mature tables and accessibility come for free.
- Apple's yearly chrome changes (Liquid Glass 26 → 27) reach AppKit-drawn chrome automatically. Qt-drawn chrome is
  the risk, and the Objective-C++ shell contains it.
- Non-mac builds use the plain Qt implementations of `PlatformShell`.
