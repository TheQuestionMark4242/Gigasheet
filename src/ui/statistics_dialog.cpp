#include "statistics_dialog.hpp"

#include <chrono>
#include <string>

#include "../model/mmapped_table.hpp"

StatisticsDialog::StatisticsDialog(wxWindow* parent, MmappedTable* tablePtr, int initialCol)
    : wxDialog(parent, wxID_ANY, "Statistics",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      table(tablePtr)
{
    const int numCols = table->GetNumberCols();

    wxString initialFormula;
    if (initialCol >= 0 && initialCol < numCols) {
        initialFormula = "=" + formulahint::FormulaName(table->ColumnLabel(initialCol));
    } else if (numCols > 0) {
        initialFormula = "=" + formulahint::FormulaName(table->ColumnLabel(0));
    }

    formulaCtrl = new wxTextCtrl(this, wxID_ANY, initialFormula,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    formulaCtrl->Bind(wxEVT_TEXT_ENTER, &StatisticsDialog::OnCompute, this);

    wxArrayString labels;
    for (int i = 0; i < numCols; ++i) {
        labels.Add(table->ColumnLabel(i));
    }
    autocomplete = std::make_unique<FormulaAutocomplete>(
        formulaCtrl, formulahint::BuildFormulaCandidates(labels));

    auto* computeBtn = new wxButton(this, wxID_ANY, "Compute");
    computeBtn->Bind(wxEVT_BUTTON, &StatisticsDialog::OnCompute, this);

    auto* hint = new wxStaticText(this, wxID_ANY,
        "e.g. =SUM(A1:A100)");
    resultText = new wxStaticText(this, wxID_ANY, "");

    auto* form = new wxFlexGridSizer(2, wxSize(8, 6));
    form->AddGrowableCol(1);
    form->Add(new wxStaticText(this, wxID_ANY, "Formula:"), 0, wxALIGN_CENTER_VERTICAL);
    form->Add(formulaCtrl, 1, wxEXPAND);

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(form, 0, wxEXPAND | wxALL, 10);
    top->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    top->Add(computeBtn, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 6);
    top->Add(resultText, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    top->Add(CreateSeparatedButtonSizer(wxCLOSE), 0, wxEXPAND | wxALL, 6);
    SetSizerAndFit(top);
    SetMinSize(wxSize(440, 320));
}

void StatisticsDialog::OnCompute(wxCommandEvent&) {
    const wxString formula = formulaCtrl->GetValue();
    if (formula.Strip(wxString::both).IsEmpty()) {
        resultText->SetLabel("Enter a formula.");
        return;
    }

    wxBusyCursor busy;
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    const StatsResult r = table->ComputeFormulaStats(
        formula.ToStdString(),
        0, table->GetNumberRows() - 1, error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    if (!error.empty()) {
        resultText->SetLabel("Error: " + wxString(error));
        Layout();
        return;
    }

    wxString text;
    if (r.scalar) {
        text = wxString::Format("Result: %.6f", r.value);
    } else if (!r.valid) {
        if (r.count == 0) {
            resultText->SetLabel("Empty range.");
            Layout();
            return;
        }
        // Text-valued formula or CHARBUF column: only COUNT is defined
        text = wxString::Format(
            "COUNT: %llu\n\n(text values: numeric statistics not available)",
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
