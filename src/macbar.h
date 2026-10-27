#pragma once

#include <string>

// macOS menu bar integration. tt lives in the menu bar as a status item: a
// timer icon with the running task's elapsed time. A click hides the window if
// it is on the current Space, and otherwise brings it there; a right-click offers Open Window and
// Quit. It is the macOS counterpart of the Omarchy bar widget and the Xfce
// panel plugin.

struct MacBarSnapshot {
    std::string dataPath;  // watched for changes made by another process
    std::string activeName;
    bool running = false;
    long long activeSince = 0;  // wall-clock start of the running session
    double todayTotal = 0;
};

// Implemented by TTFrame. All calls happen on the main thread.
class MacBarDelegate {
public:
    virtual ~MacBarDelegate() = default;
    virtual MacBarSnapshot MacBarState() = 0;
    // The window's NSWindow, so a click can tell which Space it is on.
    virtual void* MacBarNativeWindow() = 0;
    virtual void MacBarShowWindow() = 0;
    virtual void MacBarHideWindow() = 0;
    virtual void MacBarReloadState() = 0;
    virtual void MacBarQuit() = 0;
};

// True where the menu bar item can be created (macOS).
bool MacBarAvailable();

// Create the menu bar item for `delegate`. Only the first call per process
// has an effect. The delegate must outlive the item.
void MacBarInstall(MacBarDelegate* delegate);

// Update the icon, elapsed time and tooltip from the delegate's state.
void MacBarUpdate();

enum class MacBarCommand { ShowWindow, ToggleWindow };

// Ask a tt menu bar process that is already running to show or toggle its
// window. Used by a second `tt`, `tt --window` or `tt --popup` so it doesn't
// start a second copy. Returns false if no running instance was found.
bool MacBarNotifyRunning(MacBarCommand command);

// Remove this process's pid file on exit.
void MacBarShutdown();

// Width a native table view (wxDataViewCtrl's NSScrollView, `view`) adds
// around its columns: the cell spacing of every column. Column widths set
// through wx don't include it, so a list whose widths add up to its client
// width clips its last column.
int MacTableColumnSpacing(void* view);
