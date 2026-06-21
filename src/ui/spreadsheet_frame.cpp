#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "spreadsheet_frame.hpp"
#include "../model/mmapped_table.hpp"

SpreadsheetFrame::SpreadsheetFrame(const wxString& dir)
    : wxFrame(nullptr, wxID_ANY, "Spreadsheet", wxDefaultPosition, wxSize(1000,700)) {
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


void SpreadsheetFrame::OnAddColumn(wxCommandEvent&) {
    wxTextEntryDialog dlg(this, "Enter expression like =A + B*2", "Add Derived Column");
    if (dlg.ShowModal() == wxID_OK) {
        wxString expr = dlg.GetValue();
        table->AddDerivedColumn(expr, grid);
    }
}