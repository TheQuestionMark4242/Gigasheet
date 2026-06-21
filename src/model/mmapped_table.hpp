#pragma once 

#include <string>
#include <vector>

#include <wx/grid.h>

#include "mio/mmap.hpp"

#include "column_types.hpp"

class MmappedTable : public wxGridTableBase {
public:
    /** 
     * Create a wxWidget data source from a memory mmapped file
     */
    MmappedTable(const std::string&);


    // Add a derived column by expression (e.g. "=A + B*2"). gridPtr is used to notify view.
    void AddDerivedColumn(const wxString&, wxGrid*); 

    // -------- wxGridTableBase overrides ----------
    int GetNumberRows() override;
    int GetNumberCols() override;
    bool IsEmptyCell(int row, int col) override;
    wxString GetValue(int row, int col) override;
    void SetValue(int, int, const wxString&) override;
    wxString GetColLabelValue(int col) override;
    wxString GetRowLabelValue(int row) override;

private:
    std::vector<mio::mmap_source> mmaps;   // keep mmaps alive
    std::vector<Column> columns;           // base + derived (in the same vector)
    std::vector<Column> derivedColumns;    // keep derived for metadata if needed
    int rows = 0;
};
