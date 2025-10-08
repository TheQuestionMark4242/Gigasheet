#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>
#include <stdexcept>
#include <span>
#include "mio/mmap.hpp"
#include <iostream>
#include <vector>
#include <variant>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using char_buf = std::array<char, 64>;

enum class ColumnType : uint8_t {
    INT32  = 0,
    DOUBLE = 1,
    CHARBUF = 2
};

using Column = std::variant<
    std::span<const int32_t>,
    std::span<const double>,
    std::span<const char_buf>
>;

class MmappedTable : public wxGridTableBase {
public:
    MmappedTable(const std::string& dirPath) {
        auto metaPath = fs::path(dirPath) / "metadata.bin";
        std::ifstream meta(metaPath, std::ios::binary);
        if (!meta)
            throw std::runtime_error("Failed to open metadata: " + metaPath.string());

        int32_t numCols;
        meta.read(reinterpret_cast<char*>(&numCols), sizeof(numCols));
        if (!meta)
            throw std::runtime_error("Failed to read number of columns");

        std::vector<ColumnType> types(numCols);
        meta.read(reinterpret_cast<char*>(types.data()), numCols * sizeof(ColumnType));
        if (!meta)
            throw std::runtime_error("Failed to read column type list");

        for (int i = 0; i < numCols; ++i) {
            auto colPath = fs::path(dirPath) / (std::to_string(i) + ".bin");

            mio::mmap_source mmap(colPath.string());
            if (!mmap.is_open())
                throw std::runtime_error("Failed to mmap: " + colPath.string());

            colMmappers.emplace_back(std::move(mmap));
            const auto& mmapRef = colMmappers.back();

            const char* data = mmapRef.data();
            size_t bytes = mmapRef.size();

            switch (types[i]) {
                case ColumnType::INT32: {
                    size_t n = bytes / sizeof(int32_t);
                    auto ptr = reinterpret_cast<const int32_t*>(data);
                    cols.emplace_back(std::span<const int32_t>(ptr, n));
                    rows = n;
                    break;
                }
                case ColumnType::DOUBLE: {
                    size_t n = bytes / sizeof(double);
                    auto ptr = reinterpret_cast<const double*>(data);
                    cols.emplace_back(std::span<const double>(ptr, n));
                    rows = n;
                    break;
                }
                case ColumnType::CHARBUF: {
                    size_t n = bytes / sizeof(char_buf);
                    auto ptr = reinterpret_cast<const char_buf*>(data);
                    cols.emplace_back(std::span<const char_buf>(ptr, n));
                    rows = n;
                    break;
                }
                default:
                    throw std::runtime_error("Unknown column type code");
            }
        }

        this->numCols = numCols;
    }

    int GetNumberRows() override { return 1000'000'000; }
    int GetNumberCols() override { return 3; }

    bool IsEmptyCell(int row, int col) override {
        return row >= rows || col >= numCols;
    }

    wxString GetValue(int row, int col) override {
        return std::visit([&](auto&& span) -> wxString {
            using T = std::decay_t<decltype(span)>;
            if (row < 0 || row >= static_cast<int>(span.size()))
                return wxString("N/A");
            if constexpr (std::is_same_v<T, std::span<const int32_t>>)
                return wxString::Format("%d", span[row]);
            else if constexpr (std::is_same_v<T, std::span<const double>>)
                return wxString::Format("%.6f", span[row]);
            else if constexpr (std::is_same_v<T, std::span<const char_buf>>) {
                return wxString::FromUTF8(span[row].data());
            }
            else
                return wxString("?");
        }, cols[col]);
    }

    void SetValue(int, int, const wxString&) override { }

    wxString GetColLabelValue(int col) override {
        return wxString(std::string(1, 'A' + col));
    }

    wxString GetRowLabelValue(int row) override {
        return wxString::Format("%d", row);
    }

private:
    std::vector<mio::mmap_source> colMmappers;
    std::vector<Column> cols;
    int rows = 0;
    int numCols = 0;
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
        
        MmappedTable* table = new MmappedTable(filePath.ToStdString());
        std::cout << "Setting table";

        grid->SetTable(table, true, wxGrid::wxGridSelectCells);
        std::cout << "Rows in grid: " << grid->GetNumberRows() << "\n"; 
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
