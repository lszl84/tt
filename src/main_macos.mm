// Time Tracker — macOS platform layer.
//
// NSWindow + NSOpenGLView (3.3 core profile) + NSEvent dispatch into the
// shared App (same contract as the Wayland/X11 paths in main.cpp).
//
// OpenGL is deprecated on macOS but still ships with the OS and runs our
// 3.3 core renderer unchanged. We silence the deprecation noise and keep
// the renderer/font/app layers portable.

#define GL_SILENCE_DEPRECATION 1

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <chrono>
#include <cstdio>
#include <cstdint>

#include "app.h"

namespace {

// App code uses X11-style keysyms for navigation keys (0xFF52/0xFF54) and
// ASCII for printable control keys (0x08 BS, 0x0D CR, 0x1B ESC, 0x20 SP).
// Map the NSEvent virtual keycode to the same convention so OnKey stays
// backend-agnostic. Returns 0 for unmapped keys.
uint32_t MapKey(unsigned short vk) {
    switch (vk) {
        case 0x24: return 0x0D; // kVK_Return
        case 0x4C: return 0x0D; // kVK_ANSI_KeypadEnter
        case 0x30: return 0x09; // kVK_Tab
        case 0x31: return 0x20; // kVK_Space
        case 0x33: return 0x08; // kVK_Delete (Backspace)
        case 0x35: return 0x1B; // kVK_Escape
        case 0x7E: return 0xFF52; // kVK_UpArrow
        case 0x7D: return 0xFF54; // kVK_DownArrow
        default:   return 0;
    }
}

} // namespace

// ============================================================================
// GL view
// ============================================================================

@interface TTView : NSOpenGLView {
    App* _app;
    NSTimer* _tickTimer;
    std::chrono::steady_clock::time_point _lastPaint;
    BOOL _initialized;
}
- (instancetype)initWithFrame:(NSRect)frame app:(App*)app;
@end

@implementation TTView

- (instancetype)initWithFrame:(NSRect)frame app:(App*)app {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize,     24,
        NSOpenGLPFAAlphaSize,     8,
        NSOpenGLPFADepthSize,     0,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0
    };
    NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!pf) {
        std::fprintf(stderr, "Failed to create NSOpenGLPixelFormat\n");
        return nil;
    }
    self = [super initWithFrame:frame pixelFormat:pf];
    if (!self) return nil;

    _app = app;
    _initialized = NO;
    [self setWantsBestResolutionOpenGLSurface:YES];
    return self;
}

// Flip the coordinate system so mouse events and app layout agree on
// "0 is at the top" — matches how the Wayland/X11 paths feed coords in.
- (BOOL)isFlipped { return YES; }

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder   { return YES; }

- (void)prepareOpenGL {
    [super prepareOpenGL];

    GLint one = 1;
    [[self openGLContext] setValues:&one forParameter:NSOpenGLCPSwapInterval];

    [[self openGLContext] makeCurrentContext];
    _app->scale = (int)[[self window] backingScaleFactor];
    _app->Init();
    _initialized = YES;

    // 60Hz timer drives setNeedsDisplay: when the app has work to do
    // (animating panel, active timer second boundary, cursor blink).
    // Idle frames are skipped entirely — no wasted GL work when the UI
    // is static.
    _lastPaint = std::chrono::steady_clock::now();
    _tickTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                                  target:self
                                                selector:@selector(tick:)
                                                userInfo:nil
                                                 repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:_tickTimer forMode:NSRunLoopCommonModes];
}

- (void)tick:(NSTimer*)t {
    if (!_initialized) return;
    auto now = std::chrono::steady_clock::now();

    bool redraw = false;
    if (_app->IsAnimating()) {
        redraw = true;
    }
    if (_app->activeTask >= 0) {
        auto ms      = std::chrono::duration_cast<std::chrono::milliseconds>(now        - _app->tasks[_app->activeTask].steadyStart).count();
        auto last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_lastPaint - _app->tasks[_app->activeTask].steadyStart).count();
        if (ms / 1000 != last_ms / 1000) redraw = true;
    }
    if (_app->inputFocused) {
        auto ms      = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        auto last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(_lastPaint.time_since_epoch()).count();
        if ((ms / 500) != (last_ms / 500)) redraw = true;
    }
    if (redraw) [self setNeedsDisplay:YES];
}

- (void)reshape {
    [super reshape];
    [self setNeedsDisplay:YES];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_initialized) {
        _app->scale = (int)[[self window] backingScaleFactor];
        [self setNeedsDisplay:YES];
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    if (!_initialized) return;
    [[self openGLContext] makeCurrentContext];

    NSSize logical = [self bounds].size;
    NSSize backing = [self convertRectToBacking:[self bounds]].size;

    _app->winW  = (int)logical.width;
    _app->winH  = (int)logical.height;
    _app->bufW  = (int)backing.width;
    _app->bufH  = (int)backing.height;
    _app->scale = (int)[[self window] backingScaleFactor];
    if (_app->scale < 1) _app->scale = 1;

    _app->Paint();

    [[self openGLContext] flushBuffer];
    _lastPaint = std::chrono::steady_clock::now();
}

// ---------- Mouse ----------

- (NSPoint)mouseLocal:(NSEvent*)event {
    return [self convertPoint:[event locationInWindow] fromView:nil];
}

- (void)mouseDown:(NSEvent*)event {
    NSPoint p = [self mouseLocal:event];
    _app->mouseDown = true;
    _app->mx = p.x;
    _app->my = p.y;
    _app->OnClick(p.x, p.y);
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent*)event {
    _app->mouseDown = false;
    [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent*)event {
    NSPoint p = [self mouseLocal:event];
    _app->mx = p.x;
    _app->my = p.y;
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent*)event {
    NSPoint p = [self mouseLocal:event];
    _app->mx = p.x;
    _app->my = p.y;
    [self setNeedsDisplay:YES];
}

- (void)mouseEntered:(NSEvent*)event { [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent*)event  { [self setNeedsDisplay:YES]; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* a in [self trackingAreas]) [self removeTrackingArea:a];
    NSTrackingAreaOptions opts = NSTrackingMouseMoved | NSTrackingActiveInKeyWindow
                               | NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                                        options:opts
                                                          owner:self
                                                       userInfo:nil];
    [self addTrackingArea:area];
}

// ---------- Keyboard ----------

- (void)keyDown:(NSEvent*)event {
    uint32_t key = MapKey([event keyCode]);
    NSEventModifierFlags mods = [event modifierFlags];
    uint32_t modMask = 0; // App's OnKey doesn't inspect mods today.
    if (key) _app->OnKey(key, modMask);

    // Skip text insertion when command/control is held so Cmd-Q etc.
    // don't leak a literal 'q' into the input field. Also skip if the
    // keyDown already mapped to a control key (backspace/enter/escape/tab).
    if (key == 0x08 || key == 0x0D || key == 0x1B) {
        [self setNeedsDisplay:YES];
        return;
    }
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) {
        [self setNeedsDisplay:YES];
        return;
    }

    NSString* chars = [event characters];
    NSUInteger len = [chars length];
    for (NSUInteger i = 0; i < len; ) {
        unichar c0 = [chars characterAtIndex:i];
        uint32_t cp = 0;
        NSUInteger consumed = 1;
        if (c0 >= 0xD800 && c0 <= 0xDBFF && i + 1 < len) {
            unichar c1 = [chars characterAtIndex:i + 1];
            if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(c0 - 0xD800)) << 10) + (uint32_t)(c1 - 0xDC00);
                consumed = 2;
            } else {
                cp = c0;
            }
        } else {
            cp = c0;
        }
        if (cp >= 0x20 && cp != 0x7F) _app->OnChar(cp);
        i += consumed;
    }

    [self setNeedsDisplay:YES];
}

- (void)keyUp:(NSEvent*)event {}

@end

// ============================================================================
// App delegate — ties window close to NSApp termination.
// ============================================================================

@interface TTAppDelegate : NSObject<NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) TTView*   view;
@end

@implementation TTAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}
- (void)windowWillClose:(NSNotification*)n {
    [NSApp terminate:nil];
}
@end

// ============================================================================
// Entry point (called from main.cpp)
// ============================================================================

int run_macos(App& app) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        TTAppDelegate* delegate = [[TTAppDelegate alloc] init];
        [NSApp setDelegate:delegate];

        NSRect frame = NSMakeRect(0, 0, 520, 640);
        NSWindowStyleMask mask = NSWindowStyleMaskTitled
                               | NSWindowStyleMaskClosable
                               | NSWindowStyleMaskMiniaturizable
                               | NSWindowStyleMaskResizable;
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:mask
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setTitle:@"Time Tracker"];
        [win setReleasedWhenClosed:NO];
        [win setContentMinSize:NSMakeSize(320, 560)];
        [win center];
        [win setDelegate:delegate];

        TTView* view = [[TTView alloc] initWithFrame:frame app:&app];
        if (!view) return 1;
        [win setContentView:view];
        [win makeFirstResponder:view];

        delegate.window = win;
        delegate.view   = view;

        // Match the dark background used by Paint() so the window doesn't
        // flash light-grey before the first GL frame lands.
        [win setBackgroundColor:[NSColor colorWithSRGBRed:0.11 green:0.12 blue:0.15 alpha:1.0]];

        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        std::fprintf(stderr, "Using macOS backend\n");
        [NSApp run];
    }
    return 0;
}
