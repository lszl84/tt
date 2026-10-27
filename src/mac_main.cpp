// `tt` on macOS: the command line tool inside tt.app. The window and the menu
// bar item are the SwiftUI app next to it (mac/), which runs these commands
// for all its data. With no command, `tt` hands over to that app.

#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "cli.h"

namespace {

// Shared with mac/Sources/AppDelegate.swift.
constexpr const char* kPortName = "com.devmindscape.tt.app";
constexpr SInt32 kShowWindow = 1;
constexpr SInt32 kToggleWindow = 2;

// Ask a running app to show or toggle its window. False if none is running.
bool SendToApp(SInt32 message) {
    CFStringRef name = CFStringCreateWithCString(nullptr, kPortName,
                                                 kCFStringEncodingUTF8);
    CFMessagePortRef port = CFMessagePortCreateRemote(nullptr, name);
    CFRelease(name);
    if (port == nullptr) return false;
    SInt32 rc = CFMessagePortSendRequest(port, message, nullptr, 2.0, 0,
                                         nullptr, nullptr);
    CFRelease(port);
    return rc == kCFMessagePortSuccess;
}

// tt.app when this binary is its Contents/MacOS/tt, found through symlinks
// such as /usr/local/bin/tt.
std::filesystem::path EnclosingBundle() {
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(raw, resolved) == nullptr) return {};
    std::filesystem::path bundle =
        std::filesystem::path(resolved).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (bundle.extension() != ".app" ||
        !std::filesystem::exists(bundle / "Contents" / "Info.plist", ec)) {
        return {};
    }
    return bundle;
}

int LaunchApp() {
    std::filesystem::path bundle = EnclosingBundle();
    if (bundle.empty()) {
        execl("/usr/bin/open", "open", "-b", "com.devmindscape.tt", nullptr);
    } else {
        execl("/usr/bin/open", "open", bundle.c_str(), nullptr);
    }
    std::perror("tt: open");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (int rc = RunCli(argc, argv); rc >= 0) return rc;

    // --popup toggles the window (for a keyboard shortcut); --window and no
    // option show it.
    bool toggle = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--popup") == 0) toggle = true;
    }
    if (SendToApp(toggle ? kToggleWindow : kShowWindow)) return 0;
    return LaunchApp();
}
