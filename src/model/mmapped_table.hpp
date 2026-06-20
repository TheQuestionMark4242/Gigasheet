#pragma once 

#include <w/grid.h>

class MmappedTable : public wxGridTableBase {
public:
    /** 
     * Create a wxWidget data source from a memory mmapped file
     */
    MmappedTable(const std::string&);


    // Add a derived column by expression (e.g. "=A + B*2"). gridPtr is used to notify view.
    void AddDerivedColumn(const wxString&, wxGrid*); 
private:
    std::vector<mio::mmap_source> mmaps;   // keep mmaps alive
    std::vector<Column> columns;           // base + derived (in the same vector)
    std::vector<Column> derivedColumns;    // keep derived for metadata if needed
    int rows = 0;
};