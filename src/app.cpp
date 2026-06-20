#include <wx/wx.h>
#include <wx/cmdline.h>
#include <wx/grid.h>
#include <wx/artprov.h>

#include <numeric>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <variant>
#include <functional>
#include <array>
#include <cstdint>
#include <map>

#include "mio/mmap.hpp"

#include "parser/ast.hpp"
#include "parser/parser.cpp"


using char_buf = std::array<char, 64>;

// ----------------- Unified column function variant -----------------
using FnInt   = std::function<int32_t(int)>;
using FnDbl   = std::function<double(int)>;
using FnChar  = std::function<char_buf(int)>;

using ColumnFnVariant = std::variant<FnInt, FnDbl, FnChar>;

struct Column {
    ColumnType type;
    ColumnFnVariant fn;   // callable: row -> value
    std::string label;    // "A", "B", ... or "D0"
};

// ----------------- MmappedTable implementation -----------------
class MmappedTable : public wxGridTableBase {
public:
    MmappedTable(const std::string& dirPath) {
        // read metadata (int32 numcols, then uint8_t codes)
        auto metaPath = fs::path(dirPath) / "metadata.bin";
        std::ifstream meta(metaPath, std::ios::binary);
        if (!meta) throw std::runtime_error("Failed to open metadata: " + metaPath.string());

        int32_t numCols;
        meta.read(reinterpret_cast<char*>(&numCols), sizeof(numCols));
        if (!meta) throw std::runtime_error("Failed to read number of columns");

        std::vector<ColumnType> types(numCols);
        meta.read(reinterpret_cast<char*>(types.data()), numCols * sizeof(ColumnType));
        if (!meta) throw std::runtime_error("Failed to read column type list");

        // load columns: mmap each file, create callable that returns element by index
        for (int i = 0; i < numCols; ++i) {
            fs::path p = fs::path(dirPath) / (std::to_string(i) + ".bin");
            mio::mmap_source mm(p.string());
            if (!mm.is_open()) throw std::runtime_error("Failed to mmap: " + p.string());

            // keep mmap alive
            mmaps.emplace_back(std::move(mm));
            const auto& mmapRef = mmaps.back();
            const char* base = mmapRef.data();
            size_t bytes = mmapRef.size();

            if (types[i] == ColumnType::INT32) {
                const int32_t* ptr = reinterpret_cast<const int32_t*>(base);
                size_t n = bytes / sizeof(int32_t);
                rows = std::max(rows, static_cast<int>(n));
                Column col;
                col.type = ColumnType::INT32;
                col.label = std::string(1, 'A' + static_cast<char>(columns.size()));
                col.fn = FnInt([ptr](int row) -> int32_t { return ptr[row]; });
                columns.push_back(std::move(col));
            }
            else if (types[i] == ColumnType::DOUBLE) {
                const double* ptr = reinterpret_cast<const double*>(base);
                size_t n = bytes / sizeof(double);
                rows = std::max(rows, static_cast<int>(n));
                Column col;
                col.type = ColumnType::DOUBLE;
                col.label = std::string(1, 'A' + static_cast<char>(columns.size()));
                col.fn = FnDbl([ptr](int row) -> double { return ptr[row]; });
                columns.push_back(std::move(col));
            }
            else { // CHARBUF
                const char_buf* ptr = reinterpret_cast<const char_buf*>(base);
                size_t n = bytes / sizeof(char_buf);
                rows = std::max(rows, static_cast<int>(n));
                Column col;
                col.type = ColumnType::CHARBUF;
                col.label = std::string(1, 'A' + static_cast<char>(columns.size()));
                col.fn = FnChar([ptr](int row) -> char_buf { return ptr[row]; });
                columns.push_back(std::move(col));
            }
        }
    }

    // Add a derived column by expression (e.g. "=A + B*2"). gridPtr is used to notify view.
    void AddDerivedColumn(const wxString& expr, wxGrid* gridPtr) {
        // parse expression (strip leading '=' if present)
        std::string s = expr.ToStdString();
        if (!s.empty() && s[0] == '=') s = s.substr(1);

        using iterator_type = std::string::const_iterator;
        client::ast::program program;
        iterator_type iter = s.begin(), end = s.end();
        boost::spirit::x3::ascii::space_type space;
        bool ok = phrase_parse(iter, end, client::calculator, space, program);
        if (!ok || iter != end) {
            wxMessageBox("Parse error in expression: " + expr, "Parse Error", wxICON_ERROR);
            return;
        }

        // Build temporary column_map used by parser evaluator.
        column_map.clear();
        for (size_t i = 0; i < columns.size(); ++i) {
            char name = static_cast<char>('A' + i);
            Column& c = columns[i];

            // Each mapped function must produce a double for evaluator
            if (c.type == ColumnType::INT32) {
                auto f = std::get<FnInt>(c.fn); // safe
                column_map[name] = [f](int row) -> double { return static_cast<double>(f(row)); };
            } else if (c.type == ColumnType::DOUBLE) {
                auto f = std::get<FnDbl>(c.fn);
                column_map[name] = [f](int row) -> double { return f(row); };
            } else { // char_buf -> we return 0.0 (string ops not supported yet)
                column_map[name] = [](int){ return 0.0; };
            }
        }

        // Evaluate AST -> std::function<double(int)>
        client::ast::eval evaluator;
        std::function<double(int)> derived = evaluator(program);

        // push derived column as a DOUBLE-returning callable
        Column dc;
        dc.type = ColumnType::DOUBLE;
        dc.label = "D" + std::to_string(derivedColumns.size());
        dc.fn = FnDbl([derived](int row) -> double { return derived(row); });

        derivedColumns.push_back(dc);

        // notify grid view: append 1 column
        if (gridPtr) {
            // append to columns vector as well so indexing is consistent (derivedColumns holds only derived cols)
            columns.push_back(dc);

            wxGridTableMessage msg(this, wxGRIDTABLE_NOTIFY_COLS_APPENDED, 1);
            gridPtr->ProcessTableMessage(msg);
            gridPtr->ForceRefresh();
        }
    }

    // -------- wxGridTableBase overrides ----------
    int GetNumberRows() override { return rows; }
    int GetNumberCols() override { return static_cast<int>(columns.size()); }

    bool IsEmptyCell(int row, int col) override {
        return row < 0 || row >= rows || col < 0 || col >= static_cast<int>(columns.size());
    }

    wxString GetValue(int row, int col) override {
        if (IsEmptyCell(row, col)) return "N/A";
        Column& c = columns[col];

        return std::visit([row](auto&& f) -> wxString {
            using F = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<F, FnInt>) {
                return wxString::Format("%d", f(row));
            } else if constexpr (std::is_same_v<F, FnDbl>) {
                return wxString::Format("%.6f", f(row));
            } else if constexpr (std::is_same_v<F, FnChar>) {
                char_buf b = f(row);
                // ensure null termination
                b[63] = '\0';
                return wxString::FromUTF8(b.data());
            } else return wxString("?");
        }, c.fn);
    }

    void SetValue(int, int, const wxString&) override { /* readonly for now */ }

    wxString GetColLabelValue(int col) override {
        if (col < 0 || col >= static_cast<int>(columns.size())) return "";
        return columns[col].label;
    }

    wxString GetRowLabelValue(int row) override {
        return wxString::Format("%d", row);
    }

private:
    std::vector<mio::mmap_source> mmaps;   // keep mmaps alive
    std::vector<Column> columns;           // base + derived (in the same vector)
    std::vector<Column> derivedColumns;    // keep derived for metadata if needed
    int rows = 0;
};

// ----------------- UI glue: SpreadsheetFrame -----------------
class SpreadsheetFrame : public wxFrame {
public:
    SpreadsheetFrame(const wxString& dir)
        : wxFrame(nullptr, wxID_ANY, "Spreadsheet", wxDefaultPosition, wxSize(1000,700))
    {
        // toolbar
        wxToolBar* toolbar = CreateToolBar();
        toolbar->AddTool(wxID_ADD, "Add Column", wxArtProvider::GetBitmap(wxART_PLUS));
        toolbar->Realize();

        Bind(wxEVT_TOOL, &SpreadsheetFrame::OnAddColumn, this, wxID_ADD);

        // grid
        grid = new wxGrid(this, wxID_ANY);
        table = new MmappedTable(dir.ToStdString());
        grid->SetTable(table, true, wxGrid::wxGridSelectCells);

        wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
        s->Add(grid, 1, wxEXPAND);
        SetSizer(s);
    }

private:
    void OnAddColumn(wxCommandEvent&) {
        wxTextEntryDialog dlg(this, "Enter expression like =A + B*2", "Add Derived Column");
        if (dlg.ShowModal() == wxID_OK) {
            wxString expr = dlg.GetValue();
            table->AddDerivedColumn(expr, grid);
        }
    }

    wxGrid* grid = nullptr;
    MmappedTable* table = nullptr;
};

// ----------------- App -----------------
class SpreadsheetApp : public wxApp {
public:
    bool OnInit() override {
        // parse arg: directory
        wxString dir = ".";
        if (argc > 1) dir = wxString(argv[1]);
        SpreadsheetFrame* f = new SpreadsheetFrame(dir);
        f->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(SpreadsheetApp);
