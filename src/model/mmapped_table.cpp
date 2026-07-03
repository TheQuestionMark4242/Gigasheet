#include "mmapped_table.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
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

    // load columns: mmap each file, create callable that returns element by
    // index (checking the unsaved-edit overrides first)
    overrides.resize(numCols);
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
            col.fn = FnInt([this, i, ptr](int row) -> int32_t {
                const auto& ov = overrides[i];
                if (!ov.empty()) {
                    if (auto it = ov.find(row); it != ov.end()) {
                        return std::get<int32_t>(it->second);
                    }
                }
                return ptr[row];
            });
            columns.push_back(std::move(col));
        }
        else if (types[i] == ColumnType::DOUBLE) {
            const double* ptr = reinterpret_cast<const double*>(base);
            size_t n = bytes / sizeof(double);
            rows = std::max(rows, static_cast<int>(n));
            Column col;
            col.type = ColumnType::DOUBLE;
            col.label = labels[i];
            col.fn = FnDbl([this, i, ptr](int row) -> double {
                const auto& ov = overrides[i];
                if (!ov.empty()) {
                    if (auto it = ov.find(row); it != ov.end()) {
                        return std::get<double>(it->second);
                    }
                }
                return ptr[row];
            });
            columns.push_back(std::move(col));
        }
        else { // CHARBUF
            const char_buf* ptr = reinterpret_cast<const char_buf*>(base);
            size_t n = bytes / sizeof(char_buf);
            rows = std::max(rows, static_cast<int>(n));
            Column col;
            col.type = ColumnType::CHARBUF;
            col.label = labels[i];
            col.fn = FnChar([this, i, ptr](int row) -> char_buf {
                const auto& ov = overrides[i];
                if (!ov.empty()) {
                    if (auto it = ov.find(row); it != ov.end()) {
                        return std::get<char_buf>(it->second);
                    }
                }
                return ptr[row];
            });
            columns.push_back(std::move(col));
        }
    }

    numBaseCols = numCols;
    this->dirPath = dirPath;
    statsIndex = ColumnStatsIndex::Load(
        fs::path(dirPath), numCols, static_cast<std::uint64_t>(rows));
}

bool MmappedTable::SaveOverrides(std::string& errorOut) {
    errorOut.clear();
    // Header size of <col>.stats.bin; records follow (see chunk_stats.hpp).
    constexpr std::streamoff kStatsHeaderSize = 24;

    for (int col = 0; col < numBaseCols; ++col) {
        auto& ov = overrides[col];
        if (ov.empty()) continue;

        const Column& c = columns[col];
        const std::unordered_set<std::int64_t> dirty = DirtyChunks(col);

        // 1. Patch the column data file in place. The read-only mapping stays
        //    live: mio opens with FILE_SHARE_WRITE and mapped views are
        //    coherent with writes through a second handle on local files.
        const fs::path dataPath = fs::path(dirPath) / (std::to_string(col) + ".bin");
        std::fstream f(dataPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) {
            errorOut = "Failed to open for writing: " + dataPath.string();
            return false; // overrides kept, nothing lost
        }
        const std::streamoff stride =
            c.type == ColumnType::INT32 ? sizeof(std::int32_t) :
            c.type == ColumnType::DOUBLE ? sizeof(double) : sizeof(char_buf);
        for (const auto& [row, value] : ov) {
            f.seekp(static_cast<std::streamoff>(row) * stride);
            std::visit([&f](const auto& v) {
                f.write(reinterpret_cast<const char*>(&v), sizeof(v));
            }, value);
        }
        f.flush();
        if (!f) {
            errorOut = "Write failed: " + dataPath.string();
            return false; // overrides kept; a partial patch is re-written next Save
        }
        ov.clear(); // column fns now read the patched bytes from the mmap

        // 2. Refresh the stats records of the edited chunks (in memory and
        //    on disk) by rescanning those chunks through the column fn.
        if (!HasChunkStats(col)) continue;

        auto& records = statsIndex.MutableRecords(col);
        const fs::path statsPath =
            fs::path(dirPath) / (std::to_string(col) + kStatsFileSuffix);
        std::fstream sf(statsPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!sf) {
            // Data is saved but stats can't be refreshed: drop this column's
            // stats (full scans stay correct) and report the problem.
            records.clear();
            errorOut = "Cell edits saved, but failed to update " + statsPath.string()
                + "; the stats index for this column is disabled.";
            return false;
        }

        for (const std::int64_t chunk : dirty) {
            if (chunk < 0 || chunk >= static_cast<std::int64_t>(records.size())) continue;

            ChunkAccumulator acc(c.type);
            const int begin = static_cast<int>(chunk * kChunkSize);
            const int end = begin + static_cast<int>(records[chunk].count);
            if (c.type == ColumnType::INT32) {
                const auto& fn = std::get<FnInt>(c.fn);
                for (int r = begin; r < end; ++r) acc.Add(fn(r));
            } else {
                const auto& fn = std::get<FnDbl>(c.fn);
                for (int r = begin; r < end; ++r) acc.Add(fn(r));
            }
            const ChunkRecord rec = acc.Finish().front();

            records[chunk] = rec;
            sf.seekp(kStatsHeaderSize + chunk * static_cast<std::streamoff>(sizeof(ChunkRecord)));
            sf.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
        }
        sf.flush();
        if (!sf) {
            records.clear();
            errorOut = "Cell edits saved, but failed to update " + statsPath.string()
                + "; the stats index for this column is disabled.";
            return false;
        }
    }
    return true;
}

StatsResult MmappedTable::ComputeColumnStats(int col, int rowBegin, int rowEnd) const {
    if (col < 0 || col >= static_cast<int>(columns.size()) || rows == 0) {
        return {};
    }
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, rows - 1);

    const std::vector<ChunkRecord>* records =
        HasChunkStats(col) ? &statsIndex.Records(col) : nullptr;
    const std::unordered_set<std::int64_t> dirty = DirtyChunks(col);
    return ComputeStats(columns[col], records, rowBegin, rowEnd,
                        dirty.empty() ? nullptr : &dirty);
}

std::unordered_set<std::int64_t> MmappedTable::DirtyChunks(int col) const {
    std::unordered_set<std::int64_t> dirty;
    if (col >= 0 && col < static_cast<int>(overrides.size())) {
        for (const auto& [row, value] : overrides[col]) {
            dirty.insert(row / static_cast<std::int64_t>(kChunkSize));
        }
    }
    return dirty;
}

bool MmappedTable::HasUnsavedChanges() const {
    for (const auto& m : overrides) {
        if (!m.empty()) return true;
    }
    return false;
}

std::size_t MmappedTable::UnsavedCellCount() const {
    std::size_t n = 0;
    for (const auto& m : overrides) {
        n += m.size();
    }
    return n;
}

exprparse::TypedColumnMap MmappedTable::BuildColumnMap() const {
    // Column names (and A..Z aliases) visible to expressions. Numeric
    // columns evaluate to doubles, CHARBUF columns to strings.
    exprparse::TypedColumnMap columnMap;
    for (size_t i = 0; i < columns.size(); ++i) {
        const Column& c = columns[i];
        // Excel-style letter aliases, case-insensitive (A and a).
        const std::string alias = (i < 26) ? std::string(1, static_cast<char>('A' + static_cast<char>(i))) : std::string{};
        const std::string aliasLower = (i < 26) ? std::string(1, static_cast<char>('a' + static_cast<char>(i))) : std::string{};

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
            registerColumn(aliasLower, fn);
        } else if (c.type == ColumnType::DOUBLE) {
            auto f = std::get<FnDbl>(c.fn);
            exprparse::FnNum fn = [f](int row) -> double { return f(row); };
            registerColumn(c.label, fn);
            registerColumn(alias, fn);
            registerColumn(aliasLower, fn);
        } else { // CHARBUF
            auto f = std::get<FnChar>(c.fn);
            exprparse::FnStr fn = [f](int row) -> std::string {
                char_buf b = f(row);
                b[63] = '\0';
                return std::string(b.data());
            };
            registerColumn(c.label, fn);
            registerColumn(alias, fn);
            registerColumn(aliasLower, fn);
        }
    }
    return columnMap;
}

int MmappedTable::FindColumn(const std::string& name) const {
    int found = -1;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].label == name) {
            found = static_cast<int>(i); // last label match wins, like the map
        }
    }
    if (found >= 0) {
        return found;
    }
    if (name.size() == 1 &&
        ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) {
        const int idx = (name[0] >= 'a') ? name[0] - 'a' : name[0] - 'A';
        if (idx < static_cast<int>(columns.size())) {
            return idx;
        }
    }
    return -1;
}

exprparse::Aggregator MmappedTable::MakeAggregator(bool* usedIndexOut) const {
    return [this, usedIndexOut](const exprparse::AggregateRequest& req,
                                double& out, std::string& error) -> bool {
        const int col = FindColumn(req.column);
        if (col < 0) {
            error = "Unknown column: " + req.column;
            return false;
        }
        if (rows == 0) {
            error = "The table is empty.";
            return false;
        }

        const int first = static_cast<int>(std::max<std::int64_t>(req.first, 0));
        const int last = req.last < 0
            ? rows - 1
            : static_cast<int>(std::min<std::int64_t>(req.last, rows - 1));
        if (first > last) {
            error = "Range is outside the table (" + std::to_string(rows) + " rows).";
            return false;
        }

        const StatsResult r = ComputeColumnStats(col, first, last);
        if (usedIndexOut && r.usedIndex) *usedIndexOut = true;

        if (req.fn == "count") {
            out = static_cast<double>(r.count);
            return true;
        }
        if (!r.valid) {
            error = req.column + " is a text column; only COUNT() applies.";
            return false;
        }
        if (req.fn == "sum") out = r.sum;
        else if (req.fn == "average") out = r.count > 0 ? r.sum / static_cast<double>(r.count) : 0.0;
        else if (req.fn == "min") out = r.min;
        else if (req.fn == "max") out = r.max;
        else {
            error = "Unknown aggregate: " + req.fn;
            return false;
        }
        return true;
    };
}

StatsResult MmappedTable::ComputeFormulaStats(
    const std::string& expr, int rowBegin, int rowEnd,
    std::string& errorOut) const
{
    errorOut.clear();
    std::string s = expr;
    if (!s.empty() && s[0] == '=') s = s.substr(1);

    exprparse::ParseResult parsed = exprparse::Parse(s);
    if (!parsed.root) {
        errorOut = parsed.error;
        return {};
    }

    if (rows == 0) {
        return {};
    }
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, rows - 1);
    if (rowEnd < rowBegin) {
        return {};
    }

    // Bare column reference: use the chunk index via the column path.
    if (const auto* ref = std::get_if<exprparse::ColumnRef>(&parsed.root->value)) {
        const int col = FindColumn(ref->name);
        if (col >= 0) {
            return ComputeColumnStats(col, rowBegin, rowEnd);
        }
        // Unknown name falls through to CompileTyped for its error message.
    }

    bool aggregateUsed = false;
    bool aggregateIndexUsed = false;
    const exprparse::Aggregator inner = MakeAggregator(&aggregateIndexUsed);
    const exprparse::Aggregator aggregator =
        [&](const exprparse::AggregateRequest& req, double& out, std::string& err) {
            aggregateUsed = true;
            return inner(req, out, err);
        };

    exprparse::TypedCompileResult compiled =
        exprparse::CompileTyped(*parsed.root, BuildColumnMap(), aggregator);
    if (!compiled.fn) {
        errorOut = compiled.error;
        return {};
    }

    if (const auto* fn = std::get_if<exprparse::FnNum>(&*compiled.fn)) {
        // Excel-style aggregate expression: the ranges are in the formula,
        // so the result is a single number (aggregates fold to constants;
        // any per-row terms are evaluated at the first row).
        if (aggregateUsed) {
            StatsResult r;
            r.scalar = true;
            r.valid = true;
            r.value = (*fn)(rowBegin);
            r.usedIndex = aggregateIndexUsed;
            return r;
        }
        return ScanFn(*fn, rowBegin, rowEnd);
    }

    // String-valued formula: count only (valid stays false).
    StatsResult r;
    r.count = static_cast<std::uint64_t>(rowEnd) - rowBegin + 1;
    return r;
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

    // Aggregates (=A - AVERAGE(A:A)) fold to constants at creation time.
    exprparse::TypedCompileResult compiled = exprparse::CompileTyped(
        *parsed.root, BuildColumnMap(), MakeAggregator(nullptr));
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

void MmappedTable::SetValue(int row, int col, const wxString& value) {
    if (row < 0 || row >= rows || col < 0 || col >= static_cast<int>(columns.size())) {
        return;
    }
    if (col >= numBaseCols) {
        wxMessageBox("Derived columns are computed from their formula; edit the input columns instead.",
                     "Read-only column", wxICON_WARNING);
        return;
    }

    const Column& c = columns[col];
    const char* base = mmaps[col].data();
    CellOverride ov;
    bool sameAsFile = false;

    if (c.type == ColumnType::INT32) {
        long v = 0;
        if (!value.ToLong(&v) ||
            v < std::numeric_limits<std::int32_t>::min() ||
            v > std::numeric_limits<std::int32_t>::max())
        {
            wxMessageBox("\"" + value + "\" is not a valid 32-bit integer.",
                         "Invalid value", wxICON_ERROR);
            return;
        }
        const auto iv = static_cast<std::int32_t>(v);
        sameAsFile = reinterpret_cast<const std::int32_t*>(base)[row] == iv;
        ov = iv;
    } else if (c.type == ColumnType::DOUBLE) {
        double d = 0.0;
        if (!value.ToDouble(&d)) {
            wxMessageBox("\"" + value + "\" is not a valid number.",
                         "Invalid value", wxICON_ERROR);
            return;
        }
        const double fileVal = reinterpret_cast<const double*>(base)[row];
        sameAsFile = std::memcmp(&fileVal, &d, sizeof(double)) == 0;
        ov = d;
    } else { // CHARBUF, truncated to 63 bytes + NUL
        char_buf b{};
        const std::string s = value.utf8_string();
        const size_t n = std::min(s.size(), b.size() - 1);
        std::copy_n(s.data(), n, b.data());
        const char_buf& fileVal = reinterpret_cast<const char_buf*>(base)[row];
        sameAsFile = std::memcmp(fileVal.data(), b.data(), b.size()) == 0;
        ov = b;
    }

    // Keep the map sparse: typing the on-disk value back reverts the cell.
    if (sameAsFile) {
        overrides[col].erase(row);
    } else {
        overrides[col][row] = ov;
    }
}

wxString MmappedTable::GetColLabelValue(int col) {
    if (col < 0 || col >= static_cast<int>(columns.size())) return "";
    return columns[col].label;
}

wxString MmappedTable::GetRowLabelValue(int row) {
    return wxString::Format("%d", row + 1); // 1-based, like Excel (A1 = first row)
}
