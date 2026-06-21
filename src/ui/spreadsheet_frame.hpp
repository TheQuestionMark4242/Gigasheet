#pragma once 

#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "../model/mmapped_table.hpp"

class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& directory);

private:
    void OnAddColumn(wxCommandEvent&);

    wxGrid* grid = nullptr;
    MmappedTable* table = nullptr;
};