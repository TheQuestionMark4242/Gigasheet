#include "statistics_dialog.hpp"

#include <chrono>

#include <wx/valnum.h>

#include "../model/mmapped_table.hpp"

StatisticsDialog::StatisticsDialog(wxWindow* parent, MmappedTable* tablePtr, int initialCol)
    : wxDialog(parent, wxID_ANY, "Column Statistics",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      table(tablePtr)
{
    const int numCols = table->GetNumberCols();
    const int numRows = table->GetNumberRows();

    wxArrayString labels;
    for (int c = 0; c < numCols; ++c) {
        labels.Add(table->GetColLabelValue(c));
    }

    columnChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, labels);
    if (numCols > 0) {
        columnChoice->SetSelection(
            initialCol >= 0 && initialCol < numCols ? initialCol : 0);
    }

    wxIntegerValidator<int> rowValidator;
    rowValidator.SetRange(0, numRows > 0 ? numRows - 1 : 0);

    rowBeginCtrl = new wxTextCtrl(this, wxID_ANY, "0",
        wxDefaultPosition, wxDefaultSize, 0, rowValidator);
    rowEndCtrl = new wxTextCtrl(this, wxID_ANY,
        wxString::Format("%d", numRows > 0 ? numRows - 1 : 0),
        wxDefaultPosition, wxDefaultSize, 0, rowValidator);

    auto* computeBtn = new wxButton(this, wxID_ANY, "Compute");
    computeBtn->Bind(wxEVT_BUTTON, &StatisticsDialog::OnCompute, this);

    resultText = new wxStaticText(this, wxID_ANY, "");

    auto* form = new wxFlexGridSizer(2, wxSize(8, 6));
    form->AddGrowableCol(1);
    form->Add(new wxStaticText(this, wxID_ANY, "Column:"), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(columnChoice, 1, wxEXPAND);
    form->Add(new wxStaticText(this, wxID_ANY, "First row:"), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(rowBeginCtrl, 1, wxEXPAND);
    form->Add(new wxStaticText(this, wxID_ANY, "Last row:"), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(rowEndCtrl, 1, wxEXPAND);

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(form, 0, wxEXPAND | wxALL, 10);
    top->Add(computeBtn, 0, wxALIGN_CENTER | wxBOTTOM, 6);
    top->Add(resultText, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    top->Add(CreateSeparatedButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 6);
    SetSizerAndFit(top);
    SetMinSize(wxSize(380, 300));
}

void StatisticsDialog::OnCompute(wxCommandEvent&) {
    const int col = columnChoice->GetSelection();
    if (col == wxNOT_FOUND) {
        return;
    }

    long rowBegin = 0, rowEnd = 0;
    if (!rowBeginCtrl->GetValue().ToLong(&rowBegin) ||
        !rowEndCtrl->GetValue().ToLong(&rowEnd))
    {
        resultText->SetLabel("Invalid row range.");
        return;
    }
    if (rowEnd < rowBegin) {
        resultText->SetLabel("Last row must be >= first row.");
        return;
    }

    wxBusyCursor busy;
    const auto start = std::chrono::steady_clock::now();
    const StatsResult r = table->ComputeColumnStats(
        col, static_cast<int>(rowBegin), static_cast<int>(rowEnd));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    wxString text;
    if (!r.valid) {
        // CHARBUF column (or empty range): only COUNT is defined
        text = wxString::Format(
            "COUNT: %llu\n\n(text column: numeric statistics not available)",
            static_cast<unsigned long long>(r.count));
    } else {
        const wxString sumStr = r.intSum
            ? wxString::Format("%lld", static_cast<long long>(r.isum))
            : wxString::Format("%.6f", r.sum);
        text = wxString::Format(
            "SUM:   %s\nAVG:   %.6f\nMIN:   %.6f\nMAX:   %.6f\nCOUNT: %llu",
            sumStr,
            r.count > 0 ? r.sum / static_cast<double>(r.count) : 0.0,
            r.min,
            r.max,
            static_cast<unsigned long long>(r.count));
    }

    text += wxString::Format(
        "\n\n%s, %lld us",
        r.usedIndex ? "chunk index used" : "full scan",
        static_cast<long long>(elapsed.count()));
    resultText->SetLabel(text);
    Layout();
}
