#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include "spreadsheet_frame.hpp"
#include "statistics_dialog.hpp"
#include "formula_autocomplete.hpp"
#include "../storage/csv_importer.hpp"
#include "../storage/memory_usage.hpp"
#include "../model/mmapped_table.hpp"
namespace {

const int kStatisticsToolId = wxID_HIGHEST + 1;

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
    toolbar->AddTool(wxID_SAVE, "Save", wxArtProvider::GetBitmap(wxART_FILE_SAVE));
    toolbar->AddTool(wxID_ADD, "Add Column", wxArtProvider::GetBitmap(wxART_PLUS));
    toolbar->AddTool(kStatisticsToolId, "Statistics", wxArtProvider::GetBitmap(wxART_REPORT_VIEW));
    toolbar->Realize();

    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnOpenCsv, this, wxID_OPEN);
    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnSave, this, wxID_SAVE);
    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnAddColumn, this, wxID_ADD);
    Bind(wxEVT_TOOL, &SpreadsheetFrame::OnStatistics, this, kStatisticsToolId);
    Bind(wxEVT_CLOSE_WINDOW, &SpreadsheetFrame::OnClose, this);

    // grid
    grid = new wxGrid(this, wxID_ANY);
    table = new MmappedTable(dir.ToStdString());
    grid->SetTable(table, true, wxGrid::wxGridSelectCells);
    grid->EnableEditing(true);

    UpdateStatus();
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
    UpdateStatus();
}

void SpreadsheetFrame::UpdateStatus() {
    wxString text = wxString::Format(
        "Rows: %d | Columns: %d | RAM: %.1f MB",
        table->GetNumberRows(),
        table->GetNumberCols(),
        get_memory_usage_MB());

    const size_t unsaved = table->UnsavedCellCount();
    if (unsaved > 0) {
        text += wxString::Format(" | Unsaved edits: %zu", unsaved);
    }
    SetStatusText(text);
    SetTitle(unsaved > 0 ? "Spreadsheet *" : "Spreadsheet");
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

void SpreadsheetFrame::OnSave(wxCommandEvent&) {
    DoSave();
}

bool SpreadsheetFrame::DoSave() {
    if (!table->HasUnsavedChanges()) {
        SetStatusText("No unsaved changes.");
        return true;
    }
    // Commit any in-progress cell editor so its value is saved too.
    grid->SaveEditControlValue();

    const size_t count = table->UnsavedCellCount();
    wxBusyCursor busy;
    std::string error;
    if (!table->SaveOverrides(error)) {
        wxMessageBox(error, "Save Failed", wxICON_ERROR, this);
        UpdateStatus();
        return false;
    }
    UpdateStatus();
    SetStatusText(wxString::Format("Saved %zu cell edit(s).", count));
    return true;
}

void SpreadsheetFrame::OnClose(wxCloseEvent& event) {
    if (table && table->HasUnsavedChanges() && event.CanVeto()) {
        const int rc = wxMessageBox(
            "There are unsaved cell edits. Save them before closing?",
            "Unsaved Changes",
            wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (rc == wxCANCEL || (rc == wxYES && !DoSave())) {
            event.Veto();
            return;
        }
    }
    event.Skip();
}

void SpreadsheetFrame::OnStatistics(wxCommandEvent&) {
    StatisticsDialog dlg(this, table, grid->GetGridCursorCol());
    dlg.ShowModal();
}

void SpreadsheetFrame::OnAddColumn(wxCommandEvent&) {
    wxDialog dlg(this, wxID_ANY, "Add Derived Column",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto* formulaCtrl = new wxTextCtrl(&dlg, wxID_ANY, "=",
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    auto* hint = new wxStaticText(&dlg, wxID_ANY,
        "e.g. =A + B*2 or =CONCATENATE(Name, ' - ', A) — type a letter for suggestions");

    wxArrayString labels;
    for (int i = 0; i < table->GetNumberCols(); ++i) {
        labels.Add(table->GetColLabelValue(i));
    }
    FormulaAutocomplete autocomplete(
        formulaCtrl, formulahint::BuildFormulaCandidates(labels));
    // Enter confirms the dialog once the suggestion popup is closed.
    formulaCtrl->Bind(wxEVT_TEXT_ENTER,
        [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_OK); });

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(new wxStaticText(&dlg, wxID_ANY, "Expression:"), 0,
        wxLEFT | wxRIGHT | wxTOP, 10);
    top->Add(formulaCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    top->Add(hint, 0, wxEXPAND | wxALL, 10);
    top->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxALL, 6);
    dlg.SetSizerAndFit(top);
    dlg.SetMinSize(wxSize(480, dlg.GetMinSize().y));
    dlg.SetSize(wxSize(480, -1));
    formulaCtrl->SetFocus();
    formulaCtrl->SetInsertionPointEnd();

    if (dlg.ShowModal() == wxID_OK) {
        wxString expr = formulaCtrl->GetValue();
        table->AddDerivedColumn(expr, grid);
    }
}
