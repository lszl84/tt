#include <wx/wx.h>
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

wxIMPLEMENT_APP(TTApp);
