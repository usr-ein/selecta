// SPDX-License-Identifier: GPL-3.0-or-later
// The macOS 26 window chrome (WindowChrome.h): AppKit, compiled with ARC.
#include "app/WindowChrome.h"

#include <QGuiApplication>
#include <QVariant>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

namespace selecta::app::chrome {
namespace {

const char *const kWantsGlass = "selectaWantsGlass";
const char *const kHasGlass = "selectaHasGlass";
NSString *const kGlassIdentifier = @"selecta.glass";
NSString *const kToolbarIdentifier = @"selecta.titlebar";

// Only Qt's Cocoa platform has NSViews behind its windows (tests run on the offscreen one).
bool onCocoa()
{
    return QGuiApplication::platformName() == u"cocoa";
}

NSWindow *nativeWindow(const QWidget *window)
{
    if (!window || !onCocoa() || !window->testAttribute(Qt::WA_WState_Created)) {
        return nil;
    }
    // A native widget's WId is its NSView, by Qt's contract.
    // NOLINTNEXTLINE(bugprone-casting-through-void,performance-no-int-to-ptr)
    auto *const view = (__bridge NSView *)reinterpret_cast<void *>(window->winId());
    return view.window;
}

// The content view never moves the window on its own: a translucent view would otherwise answer YES (NSView's default
// for a view that isn't opaque), and a click on a toolbar button, under the title bar, could drag the window instead.
// Empty space moves it explicitly (titleBarPressed).
BOOL cannotMoveWindow(id /*self*/, SEL /*command*/)
{
    return NO;
}

void keepClicksForTheContent(NSView *content)
{
    const SEL selector = @selector(mouseDownCanMoveWindow);
    auto *const inherited = class_getInstanceMethod([NSView class], selector);
    auto *const viewClass = object_getClass(content);
    if (inherited == nil || viewClass == nil) {
        return;
    }
    // Adds the method to Qt's view class (it doesn't define it); a no-op once added.
    class_addMethod(viewClass, selector, reinterpret_cast<IMP>(cannotMoveWindow), method_getTypeEncoding(inherited));
}

void installGlass(QWidget *window, NSWindow *ns)
{
    NSView *const content = ns.contentView;
    NSView *const frame = content.superview;
    if (frame == nil) {
        return;
    }
    for (NSView *existing in frame.subviews) {
        if ([existing.identifier isEqualToString:kGlassIdentifier]) {
            window->setProperty(kHasGlass, true);
            return;
        }
    }
    NSVisualEffectView *const glass = [[NSVisualEffectView alloc] initWithFrame:frame.bounds];
    glass.identifier = kGlassIdentifier;
    glass.material = NSVisualEffectMaterialSidebar;
    glass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    glass.state = NSVisualEffectStateFollowsWindowActiveState;
    glass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [frame addSubview:glass positioned:NSWindowBelow relativeTo:content];
    window->setProperty(kHasGlass, true);
}

} // namespace

bool contentUnderTitleBar()
{
    return onCocoa();
}

void prepare(QWidget *window, bool glass)
{
    if (!window || !onCocoa()) {
        return;
    }
    // Qt sets NSWindowStyleMaskFullSizeContentView and a transparent title bar for these.
    window->setWindowFlag(Qt::ExpandedClientAreaHint, true);
    window->setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    // The layout covers the whole window, title bar included: the sidebar and the toolbar leave the traffic lights
    // their room themselves (Qt would otherwise push the content below the title bar's safe area).
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    window->setProperty(kWantsGlass, glass);
    if (glass) {
        // Where the widgets paint nothing, the window is clear, and the glass under them shows.
        window->setAttribute(Qt::WA_TranslucentBackground, true);
    }
}

bool install(QWidget *window)
{
    NSWindow *const ns = nativeWindow(window);
    if (ns == nil) {
        return false;
    }
    // An empty unified toolbar makes the title bar as tall as Selecta's toolbar and centres the traffic lights in it.
    // It holds no items: clicks in the title bar reach the content under it (the toolbar's buttons).
    if (ns.toolbar == nil || ![ns.toolbar.identifier isEqualToString:kToolbarIdentifier]) {
        NSToolbar *const toolbar = [[NSToolbar alloc] initWithIdentifier:kToolbarIdentifier];
        toolbar.allowsUserCustomization = NO;
        toolbar.displayMode = NSToolbarDisplayModeIconOnly;
        ns.toolbar = toolbar;
    }
    ns.toolbarStyle = NSWindowToolbarStyleUnified;
    ns.titleVisibility = NSWindowTitleHidden;
    ns.titlebarAppearsTransparent = YES;
    keepClicksForTheContent(ns.contentView);
    if (window->property(kWantsGlass).toBool()) {
        installGlass(window, ns);
    }
    return hasGlass(window);
}

bool hasGlass(const QWidget *window)
{
    return window && window->property(kHasGlass).toBool();
}

void titleBarPressed(const QWidget *window)
{
    if (window && window->windowHandle()) {
        window->windowHandle()->startSystemMove();
    }
}

void titleBarDoubleClicked(const QWidget *window)
{
    NSWindow *const ns = nativeWindow(window);
    if (ns == nil) {
        return;
    }
    // System Settings → Desktop & Dock → "Double-click a window's title bar to": Zoom (Fill), Minimize or Do Nothing.
    NSString *const action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"None"]) {
        return;
    }
    if ([action isEqualToString:@"Minimize"]) {
        [ns performMiniaturize:nil];
        return;
    }
    [ns performZoom:nil];
}

} // namespace selecta::app::chrome
