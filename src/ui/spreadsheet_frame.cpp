#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include "spreadsheet_frame.hpp"
#include "../storage/csv_importer.hpp"
#include "../storage/memory_usage.hpp"
#include "../model/mmapped_table.hpp"
namespace {

wxString FindCsvImporterPath() {
    wxFileName exeName(wxStandardPaths::Get().GetExecutablePath());
    const wxString exeDir = exeName.GetPathWithSep();
    const wxString windowsCandidate = exeDir + "csv_importer.exe";
    if (wxFileExists(windowsCandidate)) {
        return windowsCandidate;
    }

    const wxString unixCandidate = exeDir + "csv_importer";
    if (wxFileExists(unixCandidate)) {
        return unixCandidate;
    }

    return {};
}

wxString MakeOutputDirectoryPath(const wxString& csvPath) {
    wxFileName fileName(csvPath);
    wxString basePath = fileName.GetPathWithSep() + fileName.GetName() + "_gigasheet";
    wxString candidate = basePath;
    int suffix = 1;
    while (wxDirExists(candidate)) {
        candidate = wxString::Format("%s_%d", basePath, suffix++);
    }
    return candidate;
}

} 

SpreadsheetFrame::SpreadsheetFrame(const wxString& dir)
    : wxFrame(nullptr, wxID_ANY, "Spreadsheet", wxDefaultPosition, wxSize(1000,700)) {
    CreateStatusBar();
    SetStatusText("Loading...");
    
    // toolbar
    wxToolBar* toolbar = CreateToolBar();
    toolbar->AddTool(wxID_OPEN, "Open CSV", wxArtProvider::GetBitmap(wxART_FILE_OPEN));
    toolbar->AddTool(wxID_ADD, "Add Column", wxArtProvider::GetBitmap(wxART_PLUS));
    toolbar->Realize();

    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnOpenCsv, this, wxID_OPEN);
    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnAddColumn, this, wxID_ADD);

    // grid
    grid = new wxGrid(this, wxID_ANY);
    table = new MmappedTable(dir.ToStdString());
    grid->SetTable(table, true, wxGrid::wxGridSelectCells);
    
    SetStatusText(
        wxString::Format(
            "Rows: %d | Columns: %d | RAM: %.1f MB",
            table->GetNumberRows(),
            table->GetNumberCols(),
            get_memory_usage_MB()
        )
    );
    memoryTimer.SetOwner(this);

    Bind(
        wxEVT_TIMER,
        &SpreadsheetFrame::OnTimer,
        this);

    memoryTimer.Start(1000); // every second

    wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
    s->Add(grid, 1, wxEXPAND);
    SetSizer(s);
}

void SpreadsheetFrame::OnTimer(wxTimerEvent&) {
    SetStatusText(
        wxString::Format(
            "Rows: %d | Columns: %d | RAM: %.1f MB",
            table->GetNumberRows(),
            table->GetNumberCols(),
            get_memory_usage_MB()));
}

void SpreadsheetFrame::OnOpenCsv(wxCommandEvent&) {
    wxFileDialog dlg(
        this,
        "Open CSV",
        wxEmptyString,
        wxEmptyString,
        "CSV files (*.csv)|*.csv|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) {
        return;
    }

    const wxString csvPath = dlg.GetPath();
    const wxString outputDir = MakeOutputDirectoryPath(csvPath);

    try {
        wxBusyCursor busy;

        ImportCsv(
            csvPath.ToStdString(),
            outputDir.ToStdString());

        auto* frame = new SpreadsheetFrame(outputDir);
        frame->Show(true);
        frame->Raise();
    }
    catch (const std::exception& ex) {
        wxMessageBox(
            ex.what(),
            "CSV Import Failed",
            wxICON_ERROR | wxOK,
            this);
    }
}

void SpreadsheetFrame::OnAddColumn(wxCommandEvent&) {
    wxTextEntryDialog dlg(this, "Enter expression like =A + B*2 or =\"My Column\" + 5", "Add Derived Column");
    if (dlg.ShowModal() == wxID_OK) {
        wxString expr = dlg.GetValue();
        table->AddDerivedColumn(expr, grid);
    }
}
