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

// A system colour resolved under Aqua or Dark Aqua (system colours are dynamic), in sRGB, as a QColor; invalid if
// AppKit can't give it.
QColor resolved(NSColor *color, bool dark)
{
    NSAppearance *const appearance =
        [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    if (appearance == nil || color == nil) {
        return {};
    }
    __block NSColor *srgb = nil;
    [appearance performAsCurrentDrawingAppearance:^{
        srgb = [color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    }];
    if (srgb == nil) {
        return {};
    }
    return QColor::fromRgbF(static_cast<float>(srgb.redComponent), static_cast<float>(srgb.greenComponent),
                            static_cast<float>(srgb.blueComponent), static_cast<float>(srgb.alphaComponent));
}

NSColor *systemColor(status::Kind kind)
{
    using status::Kind;
    switch (kind) {
    case Kind::Waiting:
        return NSColor.systemGrayColor;
    case Kind::Working:
        return NSColor.systemBlueColor;
    case Kind::Release:
        return NSColor.systemIndigoColor;
    case Kind::Ready:
        return NSColor.systemGreenColor;
    case Kind::Check:
        return NSColor.systemOrangeColor;
    case Kind::Failed:
        return NSColor.systemRedColor;
    case Kind::Have:
        return NSColor.systemPurpleColor;
    case Kind::Inbox:
        return NSColor.systemCyanColor;
    case Kind::Upgrade:
        return NSColor.systemTealColor;
    }
    return nil;
}

} // namespace

QColor status::macStatusColor(int kind, bool dark)
{
    return resolved(systemColor(static_cast<status::Kind>(kind)), dark);
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
