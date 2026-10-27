#include "macbar.h"
#include "data.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#include <libproc.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

NSString* const kShowWindowNotification = @"com.devmindscape.tt.show-window";
NSString* const kToggleWindowNotification = @"com.devmindscape.tt.toggle-window";

std::string Pad(long long n) {
    return (n < 10 ? "0" : "") + std::to_string(n);
}

// "1h 02m" / "12m".
std::string FormatShort(double seconds) {
    long long total = std::max(0LL, (long long)seconds);
    long long h = total / 3600, m = total % 3600 / 60;
    return h > 0 ? std::to_string(h) + "h " + Pad(m) + "m"
                 : std::to_string(m) + "m";
}

// "1:02", the label next to the icon.
std::string FormatClock(double seconds) {
    long long total = std::max(0LL, (long long)seconds);
    return std::to_string(total / 3600) + ":" + Pad(total % 3600 / 60);
}

NSString* Utf8(const std::string& text) {
    NSString* result = [NSString stringWithUTF8String:text.c_str()];
    return result != nil ? result : @"";
}

std::filesystem::path PidFilePath() {
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        return std::filesystem::path(home) / "Library" / "Caches" / "tt" /
               "menu-bar.pid";
    }
    return std::filesystem::path("/tmp") / "tt-menu-bar.pid";
}

bool ReadPid(pid_t* pid) {
    std::ifstream f(PidFilePath());
    long long value = 0;
    if (!(f >> value) || value <= 0) return false;
    *pid = (pid_t)value;
    return true;
}

bool IsTtProcess(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return false;
    std::string exe(path);
    size_t slash = exe.find_last_of('/');
    std::string base = slash == std::string::npos ? exe : exe.substr(slash + 1);
    return base == "tt";
}

void WritePidFile() {
    std::error_code ec;
    std::filesystem::path path = PidFilePath();
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (f) f << (long long)getpid() << "\n";
}

void RemovePidFile() {
    std::error_code ec;
    std::filesystem::remove(PidFilePath(), ec);
}

NSString* CommandNotification(MacBarCommand command) {
    return command == MacBarCommand::ToggleWindow ? kToggleWindowNotification
                                                  : kShowWindowNotification;
}

// A stopwatch for the menu bar. The SF Symbols image tints with the menu bar's
// appearance; the drawn one is a fallback for systems without it.
NSImage* TimerIcon(bool running) {
    NSImage* image = [NSImage imageWithSystemSymbolName:
                                 running ? @"stopwatch.fill" : @"timer"
                           accessibilityDescription:@"Time Tracker"];
    if (image == nil) {
        image = [[NSImage alloc] initWithSize:NSMakeSize(18, 18)];
        [image lockFocus];
        NSBezierPath* circle = [NSBezierPath
            bezierPathWithOvalInRect:NSMakeRect(2.5, 2.5, 13, 13)];
        circle.lineWidth = 1.5;
        if (running) [circle fill];
        else [circle stroke];
        [image unlockFocus];
    }
    [image setTemplate:YES];
    return image;
}

}  // namespace

@interface TtMenuBar : NSObject
- (instancetype)initWithDelegate:(MacBarDelegate*)delegate;
- (void)updateView;
- (void)showWindow;
- (void)toggleWindow;
- (void)shutdown;
@end

@implementation TtMenuBar {
    MacBarDelegate* _delegate;
    NSStatusItem* _statusItem;
    NSTimer* _timer;
    id<NSObject> _activity;
    bool _haveMtime;
    std::filesystem::file_time_type _mtime;
}

- (instancetype)initWithDelegate:(MacBarDelegate*)delegate {
    self = [super init];
    if (self == nil) return nil;
    _delegate = delegate;

    _statusItem = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSVariableStatusItemLength];

    NSStatusBarButton* button = _statusItem.button;
    button.imagePosition = NSImageLeft;
    button.imageScaling = NSImageScaleProportionallyDown;
    button.font = [NSFont monospacedDigitSystemFontOfSize:
                              [NSFont systemFontSizeForControlSize:NSControlSizeSmall]
                                                   weight:NSFontWeightRegular];
    button.target = self;
    button.action = @selector(clicked:);
    [button sendActionOn:(NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp)];
    [button setAccessibilityLabel:@"Time Tracker"];

    // The status item is a menu bar extra: no Dock icon. The window can still
    // be shown, from the icon or with `tt --window`.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    // Main run loop common modes keeps the clock ticking while a menu is open.
    _timer = [NSTimer timerWithTimeInterval:1.0
                                     target:self
                                   selector:@selector(tick:)
                                   userInfo:nil
                                    repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];

    NSDistributedNotificationCenter* center =
        [NSDistributedNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(handleShowWindow:)
                   name:kShowWindowNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(handleToggleWindow:)
                   name:kToggleWindowNotification
                 object:nil];

    WritePidFile();
    [self updateView];

    return self;
}

- (void)dealloc {
    [self shutdown];
}

- (void)shutdown {
    [_timer invalidate];
    _timer = nil;
    if (_activity != nil) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    if (_statusItem != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
        _statusItem = nil;
    }
    pid_t pid = 0;
    if (ReadPid(&pid) && pid == getpid()) RemovePidFile();
}

- (void)handleShowWindow:(NSNotification*)notification {
    [self showWindow];
}

- (void)handleToggleWindow:(NSNotification*)notification {
    [self toggleWindow];
}

// A click shows the window, or hides it when it is the window in use. The
// tasklist behaves the same: a click on the window already in front hides it.
- (void)clicked:(id)sender {
    NSEvent* event = [NSApp currentEvent];
    bool contextMenu =
        event.type == NSEventTypeRightMouseUp ||
        (event.type == NSEventTypeLeftMouseUp &&
         (event.modifierFlags & NSEventModifierFlagControl) != 0);
    if (contextMenu) {
        [self showContextMenu];
        return;
    }
    [self toggleWindow];
}

- (NSWindow*)window {
    NSWindow* window = (__bridge NSWindow*)_delegate->MacBarNativeWindow();
    // Showing the window brings it to the current Space instead of switching
    // to the Space it was last on.
    window.collectionBehavior =
        (window.collectionBehavior &
         ~NSWindowCollectionBehaviorCanJoinAllSpaces) |
        NSWindowCollectionBehaviorMoveToActiveSpace;
    return window;
}

// Hide the window if it is on the current Space; otherwise bring it here.
- (void)toggleWindow {
    NSWindow* window = [self window];
    if (window.isVisible && !window.miniaturized && window.onActiveSpace) {
        _delegate->MacBarHideWindow();
        return;
    }
    [self showWindow];
}

- (void)showWindow {
    NSWindow* window = [self window];
    // A window still showing on another Space stays there when ordered
    // front; hiding it first makes it reappear on this one.
    if (window.isVisible && !window.onActiveSpace) {
        _delegate->MacBarHideWindow();
    }
    [NSApp activateIgnoringOtherApps:YES];
    _delegate->MacBarShowWindow();
}

// Without a Dock icon this is the way to quit while the window is hidden.
- (void)showContextMenu {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"tt"];
    NSMenuItem* open = [[NSMenuItem alloc] initWithTitle:@"Open Window"
                                                  action:@selector(openWindow:)
                                           keyEquivalent:@""];
    open.target = self;
    [menu addItem:open];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit tt"
                                                  action:@selector(quit:)
                                           keyEquivalent:@""];
    quit.target = self;
    [menu addItem:quit];
    [NSMenu popUpContextMenu:menu
                   withEvent:[NSApp currentEvent]
                     forView:_statusItem.button];
}

- (void)openWindow:(id)sender {
    [self showWindow];
}

- (void)quit:(id)sender {
    _delegate->MacBarQuit();
}

- (void)updateView {
    if (_delegate == nullptr) return;
    MacBarSnapshot snapshot = _delegate->MacBarState();

    // Keep the clock ticking in the menu bar instead of letting App Nap
    // throttle a hidden app.
    if (snapshot.running && _activity == nil) {
        _activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                              reason:@"Tracking time"];
    } else if (!snapshot.running && _activity != nil) {
        [[NSProcessInfo processInfo] endActivity:_activity];
        _activity = nil;
    }

    const long long now = (long long)std::time(nullptr);
    const double elapsed =
        snapshot.running ? std::max(0.0, (double)(now - snapshot.activeSince))
                         : 0.0;

    NSStatusBarButton* button = _statusItem.button;
    button.image = TimerIcon(snapshot.running);
    button.imagePosition = snapshot.running ? NSImageLeft : NSImageOnly;
    button.title = snapshot.running ? Utf8(FormatClock(elapsed)) : @"";

    button.accessibilityLabel =
        snapshot.running
            ? [NSString stringWithFormat:@"Time Tracker, %@, %@",
                                         Utf8(snapshot.activeName),
                                         Utf8(FormatClock(elapsed))]
            : @"Time Tracker, not tracking";

    NSString* tooltip =
        snapshot.running
            ? [NSString stringWithFormat:@"%@ · %@",
                                         Utf8(snapshot.activeName),
                                         Utf8(FormatDuration(elapsed))]
            : @"Not tracking";
    tooltip = [tooltip
        stringByAppendingFormat:@"\n%@ today\nClick to show or hide tt · "
                                "right-click to quit",
                                Utf8(FormatShort(snapshot.todayTotal))];
    button.toolTip = tooltip;
}

- (void)tick:(NSTimer*)timer {
    if (_delegate == nullptr) return;
    MacBarSnapshot snapshot = _delegate->MacBarState();

    // Another tt process (the CLI, or a second window) changed the state.
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(snapshot.dataPath, ec);
    if (!ec) {
        if (_haveMtime && mtime != _mtime) _delegate->MacBarReloadState();
        _mtime = mtime;
        _haveMtime = true;
    }
    [self updateView];
}

@end

namespace {

TtMenuBar* gMenuBar = nil;

}  // namespace

bool MacBarAvailable() { return true; }

void MacBarInstall(MacBarDelegate* delegate) {
    if (gMenuBar != nil || delegate == nullptr) return;
    gMenuBar = [[TtMenuBar alloc] initWithDelegate:delegate];
}

void MacBarUpdate() {
    [gMenuBar updateView];
}

bool MacBarNotifyRunning(MacBarCommand command) {
    pid_t pid = 0;
    if (!ReadPid(&pid) || pid == getpid() || !IsTtProcess(pid)) return false;
    [[NSDistributedNotificationCenter defaultCenter]
        postNotificationName:CommandNotification(command)
                      object:nil
                    userInfo:nil
          deliverImmediately:YES];
    return true;
}

void MacBarShutdown() {
    [gMenuBar shutdown];
    gMenuBar = nil;
}

int MacTableColumnSpacing(void* view) {
    id object = (__bridge id)view;
    NSTableView* table = nil;
    if ([object isKindOfClass:[NSScrollView class]]) {
        table = ((NSScrollView*)object).documentView;
    } else if ([object isKindOfClass:[NSTableView class]]) {
        table = object;
    }
    if (![table isKindOfClass:[NSTableView class]]) return 0;
    return (int)std::ceil(table.intercellSpacing.width *
                          (CGFloat)table.numberOfColumns);
}

#else

bool MacBarAvailable() { return false; }
void MacBarInstall(MacBarDelegate*) {}
void MacBarUpdate() {}
bool MacBarNotifyRunning(MacBarCommand) { return false; }
void MacBarShutdown() {}
int MacTableColumnSpacing(void*) { return 0; }

#endif
