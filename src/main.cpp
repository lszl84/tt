#include <wx/wx.h>
#include <cstdio>
#include <cstring>
#include "cli.h"
#include "macbar.h"
#include "omarchy.h"
#include "tt_frame.h"

class TTApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("tt");
        SetVendorName("tt");
#if defined(__APPLE__)
        // The window may close while tt keeps living in the menu bar.
        SetExitOnFrameDelete(false);
#endif
        auto* frame = new TTFrame();
        frame->Show(true);
        return true;
    }

#if defined(__APPLE__)
    int OnExit() override {
        MacBarShutdown();
        return wxApp::OnExit();
    }
#endif
};

#if defined(_WIN32)

wxIMPLEMENT_APP(TTApp);

#else

wxIMPLEMENT_APP_NO_MAIN(TTApp);

int main(int argc, char** argv) {
    if (int rc = RunCli(argc, argv); rc >= 0) return rc;

    // --window opens the window even where tt lives in a bar; --popup shows or
    // hides the bar widget's dropdown (on macOS it toggles the window). Drop
    // both before wx sees the arguments.
    bool forceWindow = false;
    bool togglePopup = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--window") == 0) {
            forceWindow = true;
        } else if (std::strcmp(argv[i], "--popup") == 0) {
            togglePopup = true;
        } else {
            continue;
        }
        std::memmove(&argv[i], &argv[i + 1], (argc - i) * sizeof(char*));
        --argc;
        --i;
    }

#if defined(__APPLE__)
    if (MacBarAvailable()) {
        // A running copy gets the request, so a second `tt` doesn't create a
        // second menu bar item. Without --popup, launching tt shows the
        // window, the way the Dock would bring an app forward.
        MacBarCommand command = (togglePopup && !forceWindow)
                                    ? MacBarCommand::ToggleWindow
                                    : MacBarCommand::ShowWindow;
        if (MacBarNotifyRunning(command)) return 0;
        return wxEntry(argc, argv);
    }
#endif

    if (!forceWindow && OmarchyShellAvailable()) {
        if (RunOmarchyLauncher(togglePopup)) return 0;
        std::fprintf(stderr, "tt: opening the window instead\n");
    }
    return wxEntry(argc, argv);
}

#endif
