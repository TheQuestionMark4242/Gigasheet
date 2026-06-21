#pragma once 
#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include "spreadsheet_frame.hpp"

class SpreadsheetApp : public wxApp {
public:
    bool OnInit() override;

};