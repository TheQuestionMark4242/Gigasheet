#pragma once

#include <wx/wx.h>

class MmappedTable;

// Modal dialog computing SUM/AVG/MIN/MAX/COUNT over a formula (same syntax
// as derived columns) and row range. A bare column reference is accelerated
// by the per-chunk stats index; other formulas scan the range.
class StatisticsDialog : public wxDialog {
public:
    StatisticsDialog(wxWindow* parent, MmappedTable* table, int initialCol);

private:
    void OnCompute(wxCommandEvent&);

    MmappedTable* table;
    wxTextCtrl* formulaCtrl = nullptr;
    wxTextCtrl* rowBeginCtrl = nullptr;
    wxTextCtrl* rowEndCtrl = nullptr;
    wxStaticText* resultText = nullptr;
};
