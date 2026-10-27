#include <wx/wx.h>
#include <cstdio>
#include <cstring>
#include "cli.h"
#include "omarchy.h"
#include "tt_frame.h"

class TTApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("tt");
        SetVendorName("tt");
        auto* frame = new TTFrame();
        frame->Show(true);
        return true;
    }
};

#if defined(_WIN32)

wxIMPLEMENT_APP(TTApp);

#else

wxIMPLEMENT_APP_NO_MAIN(TTApp);

int main(int argc, char** argv) {
    if (int rc = RunCli(argc, argv); rc >= 0) return rc;

    // --window opens the window even where the Omarchy bar widget is
    // available; --popup shows or hides the widget's dropdown (for a keyboard
    // shortcut). Drop both before wx sees the arguments.
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

    if (!forceWindow && OmarchyShellAvailable()) {
        if (RunOmarchyLauncher(togglePopup)) return 0;
        std::fprintf(stderr, "tt: opening the window instead\n");
    }
    return wxEntry(argc, argv);
}

#endif
