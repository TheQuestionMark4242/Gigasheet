#include "mmapped_table.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <stdexcept>

#include <wx/msgdlg.h>

#include "../parser/expr_eval.hpp"
#include "../parser/parser_driver.hpp"

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

    numBaseCols = numCols;
    statsIndex = ColumnStatsIndex::Load(
        fs::path(dirPath), numCols, static_cast<std::uint64_t>(rows));
}

StatsResult MmappedTable::ComputeColumnStats(int col, int rowBegin, int rowEnd) const {
    if (col < 0 || col >= static_cast<int>(columns.size()) || rows == 0) {
        return {};
    }
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, rows - 1);

    const std::vector<ChunkRecord>* records =
        HasChunkStats(col) ? &statsIndex.Records(col) : nullptr;
    return ComputeStats(columns[col], records, rowBegin, rowEnd);
}

void MmappedTable::AddDerivedColumn(const wxString& expr, wxGrid* gridPtr) {
    // parse expression (strip leading '=' if present)
    std::string s = expr.ToStdString();
    if (!s.empty() && s[0] == '=') s = s.substr(1);

    exprparse::ParseResult parsed = exprparse::Parse(s);
    if (!parsed.root) {
        wxMessageBox("Parse error in expression: " + expr + "\n" + parsed.error,
                     "Parse Error", wxICON_ERROR);
        return;
    }

    // Column names (and A..Z aliases) visible to the expression. Numeric
    // columns evaluate to doubles, CHARBUF columns to strings.
    exprparse::TypedColumnMap columnMap;
    for (size_t i = 0; i < columns.size(); ++i) {
        Column& c = columns[i];
        const std::string alias = (i < 26) ? std::string(1, static_cast<char>('A' + static_cast<char>(i))) : std::string{};

        auto registerColumn = [&](const std::string& key, exprparse::TypedFn fn) {
            if (!key.empty()) {
                columnMap[key] = std::move(fn);
            }
        };

        if (c.type == ColumnType::INT32) {
            auto f = std::get<FnInt>(c.fn); // safe
            exprparse::FnNum fn = [f](int row) -> double { return static_cast<double>(f(row)); };
            registerColumn(c.label, fn);
            registerColumn(alias, fn);
        } else if (c.type == ColumnType::DOUBLE) {
            auto f = std::get<FnDbl>(c.fn);
            exprparse::FnNum fn = [f](int row) -> double { return f(row); };
            registerColumn(c.label, fn);
            registerColumn(alias, fn);
        } else { // CHARBUF
            auto f = std::get<FnChar>(c.fn);
            exprparse::FnStr fn = [f](int row) -> std::string {
                char_buf b = f(row);
                b[63] = '\0';
                return std::string(b.data());
            };
            registerColumn(c.label, fn);
            registerColumn(alias, fn);
        }
    }

    exprparse::TypedCompileResult compiled = exprparse::CompileTyped(*parsed.root, columnMap);
    if (!compiled.fn) {
        wxMessageBox("Error in expression: " + expr + "\n" + compiled.error,
                     "Expression Error", wxICON_ERROR);
        return;
    }

    Column dc;
    dc.label = "D" + std::to_string(derivedColumns.size());
    if (auto* fnum = std::get_if<exprparse::FnNum>(&*compiled.fn)) {
        dc.type = ColumnType::DOUBLE;
        dc.fn = FnDbl(*fnum);
    } else {
        // String-valued expression -> CHARBUF column, truncated to 63 chars.
        exprparse::FnStr fstr = std::get<exprparse::FnStr>(*compiled.fn);
        dc.type = ColumnType::CHARBUF;
        dc.fn = FnChar([fstr](int row) -> char_buf {
            char_buf b{};
            const std::string s = fstr(row);
            const size_t n = std::min(s.size(), b.size() - 1);
            std::copy_n(s.data(), n, b.data());
            return b;
        });
    }

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
