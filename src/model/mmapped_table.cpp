#include "mmapped_table.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <stdexcept>

#include <wx/msgdlg.h>

#include "../parser/ast.hpp"
#include "../parser/parser.cpp"

namespace fs = std::filesystem;

MmappedTable::MmappedTable(const std::string& dirPath) {
    // read metadata:
    //   int32 numCols
    //   repeated numCols times:
    //     uint8  type
    //     uint16 labelLen
    //     char   label[labelLen]
    auto metaPath = fs::path(dirPath) / "metadata.bin";
    std::ifstream meta(metaPath, std::ios::binary);
    if (!meta) throw std::runtime_error("Failed to open metadata: " + metaPath.string());

    int32_t numCols;
    meta.read(reinterpret_cast<char*>(&numCols), sizeof(numCols));
    if (!meta) throw std::runtime_error("Failed to read number of columns");

    std::vector<ColumnType> types(numCols);
    std::vector<std::string> labels(numCols);
    for (int i = 0; i < numCols; ++i) {
        std::uint8_t typeByte = 0;
        meta.read(reinterpret_cast<char*>(&typeByte), sizeof(typeByte));
        if (!meta) throw std::runtime_error("Failed to read column type");
        types[i] = static_cast<ColumnType>(typeByte);

        std::uint16_t labelLen = 0;
        meta.read(reinterpret_cast<char*>(&labelLen), sizeof(labelLen));
        if (!meta) throw std::runtime_error("Failed to read column label length");

        labels[i].resize(labelLen);
        if (labelLen > 0) {
            meta.read(labels[i].data(), labelLen);
            if (!meta) throw std::runtime_error("Failed to read column label");
        }
        if (labels[i].empty()) {
            labels[i] = "Column " + std::to_string(i + 1);
        }
    }

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
            col.label = labels[i];
            col.fn = FnInt([ptr](int row) -> int32_t { return ptr[row]; });
            columns.push_back(std::move(col));
        }
        else if (types[i] == ColumnType::DOUBLE) {
            const double* ptr = reinterpret_cast<const double*>(base);
            size_t n = bytes / sizeof(double);
            rows = std::max(rows, static_cast<int>(n));
            Column col;
            col.type = ColumnType::DOUBLE;
            col.label = labels[i];
            col.fn = FnDbl([ptr](int row) -> double { return ptr[row]; });
            columns.push_back(std::move(col));
        }
        else { // CHARBUF
            const char_buf* ptr = reinterpret_cast<const char_buf*>(base);
            size_t n = bytes / sizeof(char_buf);
            rows = std::max(rows, static_cast<int>(n));
            Column col;
            col.type = ColumnType::CHARBUF;
            col.label = labels[i];
            col.fn = FnChar([ptr](int row) -> char_buf { return ptr[row]; });
            columns.push_back(std::move(col));
        }
    }
}

void MmappedTable::AddDerivedColumn(const wxString& expr, wxGrid* gridPtr) {
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
        Column& c = columns[i];
        const std::string alias = (i < 26) ? std::string(1, static_cast<char>('A' + static_cast<char>(i))) : std::string{};

        auto registerColumn = [&](const std::string& key, auto fn) {
            if (!key.empty()) {
                column_map[key] = std::move(fn);
            }
        };

        // Each mapped function must produce a double for evaluator
        if (c.type == ColumnType::INT32) {
            auto f = std::get<FnInt>(c.fn); // safe
            registerColumn(c.label, [f](int row) -> double { return static_cast<double>(f(row)); });
            registerColumn(alias, [f](int row) -> double { return static_cast<double>(f(row)); });
        } else if (c.type == ColumnType::DOUBLE) {
            auto f = std::get<FnDbl>(c.fn);
            registerColumn(c.label, [f](int row) -> double { return f(row); });
            registerColumn(alias, [f](int row) -> double { return f(row); });
        } else { // char_buf -> we return 0.0 (string ops not supported yet)
            registerColumn(c.label, [](int){ return 0.0; });
            registerColumn(alias, [](int){ return 0.0; });
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

int MmappedTable::GetNumberRows() {
    return rows;
}

int MmappedTable::GetNumberCols() {
    return static_cast<int>(columns.size());
}

bool MmappedTable::IsEmptyCell(int row, int col) {
    return row < 0 || row >= rows || col < 0 || col >= static_cast<int>(columns.size());
}

wxString MmappedTable::GetValue(int row, int col) {
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

void MmappedTable::SetValue(int, int, const wxString&) {
    /* readonly for now */
}

wxString MmappedTable::GetColLabelValue(int col) {
    if (col < 0 || col >= static_cast<int>(columns.size())) return "";
    return columns[col].label;
}

wxString MmappedTable::GetRowLabelValue(int row) {
    return wxString::Format("%d", row);
}
