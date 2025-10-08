#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>
#include <stdexcept>
#include <span>
#include "mio/mmap.hpp"
#include <iostream>

class MyGridTable : public wxGridTableBase {
public:
    static constexpr int ROWS = 100000000;
    static constexpr int COLS = 10;


    MyGridTable(std::string path) {
        mmap = mio::mmap_source(path);
        if(!mmap.is_open()) {
            throw std::runtime_error("Failed to open file: " + path);
        }

        rows = mmap.size() / sizeof(int32_t);
        auto ptr = reinterpret_cast<const int32_t*>(mmap.data()); 
        col = std::span<const int32_t>(ptr, rows);
    }

    int GetNumberRows() override { return ROWS; }
    int GetNumberCols() override { return COLS; }

    bool IsEmptyCell(int rowIdx, int colIdx) override { 
        return false; 
    }

    wxString GetValue(int rowIdx, int colIdx) override { 
        return wxString::Format("%d", col[rowIdx]); 
    }
    void SetValue(int, int, const wxString&) override { }

    wxString GetColLabelValue(int col) override { return wxString::Format("%d", col); }
    wxString GetRowLabelValue(int row) override { return wxString::Format("%d", row); }

    bool CanGetValueAs(int, int, const wxString& typeName) override {
        return typeName == "string" || typeName == "double" || typeName == "long";
    }
    bool CanSetValueAs(int, int, const wxString& typeName) override { return CanGetValueAs(0,0,typeName); }

    double GetValueAsDouble(int, int) override { return 69.0; }
    long GetValueAsLong(int, int) override { return 69; }
    void SetValueAsDouble(int, int, double) override { }
    void SetValueAsLong(int, int, long) override { }
private:
    mio::mmap_source mmap;
    int rows;
    std::span<const int32_t> col;
};

class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& filePath)
        : wxFrame(nullptr, wxID_ANY, "Modern Spreadsheet", wxDefaultPosition, wxSize(800, 600))
    {
        // Menu
        wxMenu* fileMenu = new wxMenu;
        fileMenu->Append(wxID_EXIT, "&Quit\tCtrl-Q", "Quit the application");
        wxMenuBar* menuBar = new wxMenuBar;
        menuBar->Append(fileMenu, "&File");
        SetMenuBar(menuBar);

        // Toolbar
        wxToolBar* toolbar = CreateToolBar();
        toolbar->AddTool(wxID_NEW, "New", wxArtProvider::GetBitmap(wxART_NEW));
        toolbar->AddTool(wxID_OPEN, "Open", wxArtProvider::GetBitmap(wxART_FILE_OPEN));
        toolbar->AddTool(wxID_SAVE, "Save", wxArtProvider::GetBitmap(wxART_FILE_SAVE));
        toolbar->Realize();

        // Status bar
        CreateStatusBar();
        SetStatusText("Booktabs-Inspired Spreadsheet");

        // Grid
        grid = new wxGrid(this, wxID_ANY);
        
        MyGridTable* table = new MyGridTable(filePath.ToStdString());
        grid->SetTable(table, true, wxGrid::wxGridSelectCells);
        
        // Make sure scrolling is enabled and the grid knows it has content
        grid->EnableScrolling(true, true);
        grid->SetRowLabelSize(80); 
        grid->ForceRefresh();      // force initial paint
        grid->Refresh();
        grid->Update();

        // --- Styling ---
        // Backgrounds
        SetBackgroundColour(wxColour(250, 250, 250));
        grid->SetDefaultCellBackgroundColour(wxColour(255, 255, 255));
        grid->SetDefaultCellTextColour(wxColour(30, 30, 30));

        // Remove vertical gridlines, keep subtle horizontal lines
        grid->EnableGridLines(true);
        grid->SetGridLineColour(wxColour(220, 220, 220));
        grid->EnableDragGridSize(false);
        grid->SetColMinimalAcceptableWidth(40);
        grid->SetRowMinimalAcceptableHeight(18);
        grid->EnableDragRowSize(false);
        grid->EnableDragColSize(false);

        // Header area
        grid->SetLabelBackgroundColour(wxColour(245, 245, 245));
        grid->SetLabelTextColour(wxColour(80, 80, 80));
        wxFont headerFont = grid->GetLabelFont();
        headerFont.SetWeight(wxFONTWEIGHT_NORMAL);
        grid->SetLabelFont(headerFont);

        // Thicker bottom border under headers (Booktabs "toprule")
        grid->SetLabelBackgroundColour(wxColour(245, 245, 245));
        grid->SetGridLineColour(wxColour(220, 220, 220));

        // Selection color
        grid->SetSelectionBackground(wxColour(51, 153, 255));
        grid->SetSelectionForeground(*wxWHITE);

        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(grid, 1, wxEXPAND);
        SetSizer(sizer);
    }
    private:
        wxGrid* grid;
};

class SpreadsheetApp : public wxApp {
    public:
    bool OnInit() override {
        wxCmdLineEntryDesc cmdLineDesc[] = {
            { wxCMD_LINE_PARAM, nullptr, nullptr, "File to open",
                wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
                { wxCMD_LINE_NONE }
            };
            
            wxCmdLineParser parser(cmdLineDesc, argc, argv);
            if(parser.Parse()) {
                return false;
            }
            
            if (parser.GetParamCount() == 0) {
                throw std::runtime_error("No file passed.");
            } 
            filePath = parser.GetParam(0);
            std::cout << filePath.ToStdString() << "\n";
            SpreadsheetFrame* frame = new SpreadsheetFrame(filePath);
            frame->Show(true);
            return true;
        }
    private:
        wxString filePath;
};

wxIMPLEMENT_APP(SpreadsheetApp);
