// SPDX-License-Identifier: GPL-3.0-or-later
// The macOS system accent colour in light or dark (Appearance.h). Compiled with ARC.
#include "app/Appearance.h"

#import <AppKit/AppKit.h>

namespace selecta::app {
namespace {

// How far toward the system's highlight (white) the accent goes on Dark Aqua. controlAccentColor is the same in both
// appearances, and thin symbols drawn in it on a dark sidebar read as too dark a blue (beta notes, 2026-10-20).
constexpr CGFloat kDarkHighlightLevel = 0.3;

} // namespace

QColor macSystemAccentColor(bool dark)
{
    NSAppearance *appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    if (appearance == nil) {
        return {};
    }
    // Resolved under the appearance asked for (system colours are dynamic), into sRGB, as Qt's colours are.
    __block NSColor *resolved = nil;
    [appearance performAsCurrentDrawingAppearance:^{
      NSColor *accent = [NSColor controlAccentColor];
      if (dark) {
          accent = [accent highlightWithLevel:kDarkHighlightLevel];
      }
      resolved = [accent colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    }];
    if (resolved == nil) {
        return {};
    }
    return QColor::fromRgbF(static_cast<float>(resolved.redComponent), static_cast<float>(resolved.greenComponent),
                            static_cast<float>(resolved.blueComponent), static_cast<float>(resolved.alphaComponent));
}

} // namespace selecta::app
