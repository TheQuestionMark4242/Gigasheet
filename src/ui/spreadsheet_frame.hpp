#pragma once

#include <vector>

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "../model/mmapped_table.hpp"

class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& directory);

private:
    void OnOpenCsv(wxCommandEvent&);
    void OnSave(wxCommandEvent&);
    void OnAddColumn(wxCommandEvent&);
    void OnStatistics(wxCommandEvent&);
    void OnClose(wxCloseEvent&);
    void OnTimer(wxTimerEvent&);
    void OnGridSelectCell(wxGridEvent&);
    void OnFormulaEnter(wxCommandEvent&);
    void UpdateStatus();
    bool DoSave();

    // Mirror the given cell's value into the formula bar / cell-ref field.
    void ShowCellInFormulaBar(int row, int col);

    // Apply the flat, line-light visual styling to the grid.
    void StyleGrid();

    // Widen each column to fit its header label and the first `sampleRows` cells
    // (a bounded sample so it stays fast on multi-million-row tables).
    void AutoSizeColumns(int sampleRows = 10);

    // Push the active palette onto every widget.
    void ApplyTheme();
    // Switch to a built-in preset by index, apply it live, and remember it.
    void ApplyPreset(int index);
    // Pop the Appearance menu of presets from the nav bar.
    void OnAppearance();


    wxTimer memoryTimer;
    wxGrid* grid = nullptr;
    MmappedTable* table = nullptr;

    // Chrome widgets kept for live re-theming.
    wxPanel* topBar = nullptr;
    std::vector<wxStaticText*> navButtons;

    // Formula bar (above the grid): cell address + editable value field.
    wxStaticText* cellRefLabel = nullptr;
    wxTextCtrl* formulaBar = nullptr;

    // Display name of the loaded dataset, shown in the title bar.
    wxString sourceName;

    // Selected appearance preset (persisted next to the executable).
    wxString currentThemeName;
    wxString prefPath;
};
