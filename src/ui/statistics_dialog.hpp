#pragma once

#include <memory>

#include <wx/wx.h>

#include "formula_autocomplete.hpp"

class MmappedTable;

// Modal dialog computing SUM/AVG/MIN/MAX/COUNT over a formula (same syntax
// as derived columns). Row ranges are part of the formula, Excel-style:
// =MAX(A1:A100). A bare column reference is accelerated by the per-chunk
// stats index; per-row formulas scan all rows.
class StatisticsDialog : public wxDialog {
public:
    StatisticsDialog(wxWindow* parent, MmappedTable* table, int initialCol);

private:
    void OnCompute(wxCommandEvent&);

    MmappedTable* table;
    wxTextCtrl* formulaCtrl = nullptr;
    wxStaticText* resultText = nullptr;
    std::unique_ptr<FormulaAutocomplete> autocomplete;
};
