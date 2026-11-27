#pragma once

#include <wx/dialog.h>

class wxListBox;
class wxStaticText;
class wxTextCtrl;

enum class HelpTopic {
    GettingStarted,
    MapAndPresets,
    Results,
    Calculation,
    Troubleshooting
};

class HelpDialog : public wxDialog {
public:
    explicit HelpDialog(wxWindow* parent);
    void SelectTopic(HelpTopic topic);

private:
    wxListBox* m_topics;
    wxStaticText* m_heading;
    wxTextCtrl* m_body;
};
