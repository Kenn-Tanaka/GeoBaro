#include <wx/wx.h>
#include <wx/image.h>
#include "main_frame.h"

class App : public wxApp {
public:
    virtual bool OnInit() {
        SetAppName(wxT("GeoBaro"));
        wxImage::AddHandler(new wxPNGHandler);
        MainFrame* frame = new MainFrame(wxT("GeoBaro - 国土地理院・気象庁 標高＆気圧換算ビューア"));
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(App);
