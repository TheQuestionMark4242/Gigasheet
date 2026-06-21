#include "app.hpp"

bool SpreadsheetApp::OnInit() {
    wxString dir = ".";
    if (argc > 1) dir = wxString(argv[1]);
    SpreadsheetFrame* f = new SpreadsheetFrame(dir);
    f->Show(true);
    return true;
}