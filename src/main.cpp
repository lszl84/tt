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
    // available. Drop it before wx sees the arguments.
    bool forceWindow = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--window") == 0) {
            forceWindow = true;
            std::memmove(&argv[i], &argv[i + 1], (argc - i) * sizeof(char*));
            --argc;
            --i;
        }
    }

    if (!forceWindow && OmarchyShellAvailable()) {
        if (RunOmarchyLauncher()) return 0;
        std::fprintf(stderr, "tt: opening the window instead\n");
    }
    return wxEntry(argc, argv);
}

#endif
