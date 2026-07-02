#pragma once

#include <wx/wx.h>

class MmappedTable;

// Modal dialog computing SUM/AVG/MIN/MAX/COUNT over a column and row range,
// accelerated by the per-chunk stats index when the column has one.
class StatisticsDialog : public wxDialog {
public:
    StatisticsDialog(wxWindow* parent, MmappedTable* table, int initialCol);

private:
    void OnCompute(wxCommandEvent&);

    MmappedTable* table;
    wxChoice* columnChoice = nullptr;
    wxTextCtrl* rowBeginCtrl = nullptr;
    wxTextCtrl* rowEndCtrl = nullptr;
    wxStaticText* resultText = nullptr;
};
