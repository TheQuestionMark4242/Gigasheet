#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "../model/mmapped_table.hpp"

class SpreadsheetFrame : public wxFrame {
public:
    // Open a prepared dataset directory (or an empty window if it has no data).
    SpreadsheetFrame(const wxString& directory);

    // Open a CSV progressively: paint a preview of its first rows immediately,
    // then import + type-infer the full file in the background and swap in the
    // real dataset when ready. The bool tag distinguishes this from the
    // directory constructor above.
    SpreadsheetFrame(const wxString& csvPath, bool fromCsv);

    ~SpreadsheetFrame() override;

private:
    // Build the shared chrome (nav bar, formula bar, grid, timers, theme) around
    // whichever table is shown first (preview or real).
    void BuildUi(wxGridTableBase* initialTable);

    // Kick off the background import of pendingCsvPath. When it finishes the
    // worker posts a wxThreadEvent back to this frame (OnLoadDone), which swaps
    // the preview out for the real dataset on the GUI thread.
    void StartBackgroundLoad();
    void OnLoadDone();
    void FinishBackgroundLoad();

    void OnOpenCsv(wxCommandEvent&);
    void OnSave(wxCommandEvent&);
    void OnAddColumn(wxCommandEvent&);
    void OnStatistics(wxCommandEvent&);
    void OnClose(wxCloseEvent&);
    void OnTimer(wxTimerEvent&);
    void OnGridSelectCell(wxGridEvent&);
    void OnFormulaEnter(wxCommandEvent&);

    // Toggle filtering mode: shows/hides the per-column filter affordance on
    // the column headers and clears active filters when turned off.
    void OnToggleFiltering();
    // Open the filter dropdown for a single column (numeric range or text
    // match), with Apply / Remove buttons.
    void OnColumnLabelClick(wxGridEvent&);
    void ShowColumnFilterPopup(int col);
    // Refresh the "▼" filter markers on the column labels.
    void RefreshFilterMarkers();
    // Recompute filters after a cell edit, then repaint (no-op if unfiltered).
    void RecomputeFiltersAfterEdit(int col);
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
    // The Enable/Disable Filtering nav button (label toggles).
    wxStaticText* filterButton = nullptr;

    // Formula bar (above the grid): cell address + editable value field.
    wxStaticText* cellRefLabel = nullptr;
    wxTextCtrl* formulaBar = nullptr;

    // Display name of the loaded dataset, shown in the title bar.
    wxString sourceName;

    // Directory of the loaded dataset (holds metadata.bin, *.bin, source.txt).
    wxString datasetDir;
    // Absolute path of the original CSV this dataset was imported from, so Save
    // can write edits back to it. Empty if unknown (e.g. empty window).
    wxString originalCsvPath;

    // Selected appearance preset (persisted next to the executable).
    wxString currentThemeName;
    wxString prefPath;

    // -------- Progressive (preview -> full) load state --------
    bool previewMode = false;      // true while showing the CSV preview
    bool closing = false;          // set in OnClose so a late swap is skipped
    wxString pendingCsvPath;       // CSV being imported in the background
    std::thread loaderThread;      // does the import off the GUI thread
    std::atomic<bool> loaderCancel{false};
    std::string loaderResultDir;   // dataset dir produced by the loader
    std::string loaderError;       // non-empty if the import failed
};
