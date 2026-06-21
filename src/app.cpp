#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "model/mmapped_table.cpp"

// ----------------- UI glue: SpreadsheetFrame -----------------
class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& dir)
        : wxFrame(nullptr, wxID_ANY, "Spreadsheet", wxDefaultPosition, wxSize(1000,700))
    {
        // toolbar
        wxToolBar* toolbar = CreateToolBar();
        toolbar->AddTool(wxID_ADD, "Add Column", wxArtProvider::GetBitmap(wxART_PLUS));
        toolbar->Realize();

        Bind(wxEVT_TOOL, &SpreadsheetFrame::OnAddColumn, this, wxID_ADD);

        // grid
        grid = new wxGrid(this, wxID_ANY);
        table = new MmappedTable(dir.ToStdString());
        grid->SetTable(table, true, wxGrid::wxGridSelectCells);

        wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
        s->Add(grid, 1, wxEXPAND);
        SetSizer(s);
    }

private:
    void OnAddColumn(wxCommandEvent&) {
        wxTextEntryDialog dlg(this, "Enter expression like =A + B*2", "Add Derived Column");
        if (dlg.ShowModal() == wxID_OK) {
            wxString expr = dlg.GetValue();
            table->AddDerivedColumn(expr, grid);
        }
    }

    wxGrid* grid = nullptr;
    MmappedTable* table = nullptr;
};

class SpreadsheetApp : public wxApp {
public:
    bool OnInit() override {
        // parse arg: directory
        wxString dir = ".";
        if (argc > 1) dir = wxString(argv[1]);
        SpreadsheetFrame* f = new SpreadsheetFrame(dir);
        f->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(SpreadsheetApp);
