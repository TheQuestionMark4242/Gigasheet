#pragma once

#include <string>
#include <utility>
#include <vector>

#include <wx/grid.h>

// A tiny, read-only in-memory table used to paint a preview of a CSV (its first
// N rows, every value as a string) the instant it's opened, while the real
// columnar dataset is imported and type-inferred in the background. Once the
// import finishes the grid swaps this out for the real MmappedTable.
class InMemoryTable : public wxGridTableBase {
public:
    InMemoryTable(std::vector<wxString> labels,
                  std::vector<std::vector<wxString>> rows)
        : labels_(std::move(labels)), rows_(std::move(rows)) {}

    int GetNumberRows() override { return static_cast<int>(rows_.size()); }
    int GetNumberCols() override { return static_cast<int>(labels_.size()); }
    bool IsEmptyCell(int, int) override { return false; }

    wxString GetValue(int row, int col) override {
        if (row < 0 || row >= static_cast<int>(rows_.size())) return {};
        const auto& r = rows_[row];
        if (col < 0 || col >= static_cast<int>(r.size())) return {};
        return r[col];
    }

    // Preview is read-only; edits are ignored until the real dataset loads.
    void SetValue(int, int, const wxString&) override {}

    wxString GetColLabelValue(int col) override {
        if (col < 0 || col >= static_cast<int>(labels_.size())) return {};
        return labels_[col];
    }
    wxString GetRowLabelValue(int row) override {
        return wxString::Format("%d", row + 1);
    }

private:
    std::vector<wxString> labels_;
    std::vector<std::vector<wxString>> rows_;
};
