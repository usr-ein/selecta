// SPDX-License-Identifier: GPL-3.0-or-later
// The macOS system accent colour in light or dark (Appearance.h), and the status colours (Status.h). Compiled with ARC.
#include "app/Appearance.h"
#include "app/Status.h"

#import <AppKit/AppKit.h>

namespace selecta::app {
namespace {

// How far toward the system's highlight (white) the accent goes on Dark Aqua. controlAccentColor is the same in both
// appearances, and thin symbols drawn in it on a dark sidebar read as too dark a blue (beta notes, 2026-10-20).
constexpr CGFloat kDarkHighlightLevel = 0.3;

// An NSColor resolved under Aqua or Dark Aqua (system colours are dynamic), in sRGB, as a QColor; invalid if AppKit
// can't give it.
QColor resolved(NSColor *(^make)(void), bool dark)
{
    NSAppearance *const appearance =
        [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    if (appearance == nil) {
        return {};
    }
    __block NSColor *color = nil;
    [appearance performAsCurrentDrawingAppearance:^{
        color = [make() colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    }];
    if (color == nil) {
        return {};
    }
    return QColor::fromRgbF(static_cast<float>(color.redComponent), static_cast<float>(color.greenComponent),
                            static_cast<float>(color.blueComponent), static_cast<float>(color.alphaComponent));
}

} // namespace

QColor status::macStatusColor(int kind, bool dark)
{
    switch (static_cast<Kind>(kind)) {
    case Kind::Waiting:
        return resolved(^{ return NSColor.systemGrayColor; }, dark);
    case Kind::Working:
        return resolved(^{ return NSColor.systemBlueColor; }, dark);
    case Kind::Release:
        return resolved(^{ return NSColor.systemIndigoColor; }, dark);
    case Kind::Ready:
        return resolved(^{ return NSColor.systemGreenColor; }, dark);
    case Kind::Check:
        return resolved(^{ return NSColor.systemOrangeColor; }, dark);
    case Kind::Failed:
        return resolved(^{ return NSColor.systemRedColor; }, dark);
    case Kind::Have:
        return resolved(^{ return NSColor.systemPurpleColor; }, dark);
    case Kind::Inbox:
        return resolved(^{ return NSColor.systemCyanColor; }, dark);
    case Kind::Upgrade:
        return resolved(^{ return NSColor.systemTealColor; }, dark);
    }
    return {};
}

QColor macSystemAccentColor(bool dark)
{
    NSAppearance *const appearance =
        [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
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
