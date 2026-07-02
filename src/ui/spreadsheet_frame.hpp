#pragma once 

#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "../model/mmapped_table.hpp"

class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& directory);

private:
    void OnOpenCsv(wxCommandEvent&);
    void OnSave(wxCommandEvent&);
    void OnAddColumn(wxCommandEvent&);
    void OnStatistics(wxCommandEvent&);
    void OnClose(wxCloseEvent&);
    void OnTimer(wxTimerEvent&);
    void UpdateStatus();
    bool DoSave();


    wxTimer memoryTimer;
    wxGrid* grid = nullptr;
    MmappedTable* table = nullptr;
};
