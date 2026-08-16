#include "mmapped_table.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <fstream>
#include <functional>
#include <ostream>
#include <streambuf>
#include <string>
#include <stdexcept>

#include <wx/msgdlg.h>

#include "../../third-party/csv-parser/csv.hpp"
#include "../parser/expr_eval.hpp"
#include "../parser/parser_driver.hpp"
#include "../util/sha256.hpp"

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

// Evaluate a single predicate over every row, setting the bit for each match.
// Mirrors the full-scan bitvector technique from bench/scan_bench.cpp: one bit
// per row, no early exit, values read through the column fn so unsaved edits
// and derived columns are honoured.
void MmappedTable::EvalFilter(const ColumnFilter& f, std::vector<uint64_t>& out) const {
    std::fill(out.begin(), out.end(), 0);
    if (f.col < 0 || f.col >= static_cast<int>(columns.size()) || rows == 0) return;

    const Column& c = columns[f.col];
    auto setBit = [&out](int i) { out[i >> 6] |= (1ull << (i & 63)); };

    if (f.kind == ColumnFilter::Kind::Range) {
        if (c.type == ColumnType::INT32) {
            const auto& fn = std::get<FnInt>(c.fn);
            for (int i = 0; i < rows; ++i) {
                const double v = static_cast<double>(fn(i));
                if (v >= f.lo && v <= f.hi) setBit(i);
            }
        } else if (c.type == ColumnType::DOUBLE) {
            const auto& fn = std::get<FnDbl>(c.fn);
            for (int i = 0; i < rows; ++i) {
                const double v = fn(i);
                if (v >= f.lo && v <= f.hi) setBit(i);
            }
        }
        // Range on a text column matches nothing.
        return;
    }

    // Equals / Contains: only meaningful for text columns.
    if (c.type != ColumnType::CHARBUF) return;
    const auto& fn = std::get<FnChar>(c.fn);
    const std::string& target = f.text;

    if (f.kind == ColumnFilter::Kind::Equals) {
        for (int i = 0; i < rows; ++i) {
            char_buf b = fn(i);
            b[63] = '\0';
            if (target == b.data()) setBit(i);
        }
    } else { // Contains (substring)
        if (target.empty()) { // empty substring matches every row
            for (int i = 0; i < rows; ++i) setBit(i);
            return;
        }
        for (int i = 0; i < rows; ++i) {
            char_buf b = fn(i);
            b[63] = '\0';
            if (std::strstr(b.data(), target.c_str()) != nullptr) setBit(i);
        }
    }
}

// Intersect all cached bitvectors into the visible filteredRows list, then
// (optionally) notify the grid of the row-count delta.
void MmappedTable::RebuildFilteredView(int oldVisible, wxGrid* gridPtr) {
    if (columnFilters.empty() || rows == 0) {
        filteredRows.clear();
        filterActive = false;
    } else {
        const size_t words = (static_cast<size_t>(rows) + 63) / 64;
        std::vector<uint64_t> acc(words, ~0ull);
        // Mask off the padding bits in the last word so they never survive AND.
        if (const int rem = rows & 63) acc.back() = (1ull << rem) - 1;

        for (const auto& [col, bits] : filterBits) {
            for (size_t w = 0; w < words; ++w) acc[w] &= bits[w];
        }

        filteredRows.clear();
        for (int i = 0; i < rows; ++i) {
            if (acc[i >> 6] & (1ull << (i & 63))) filteredRows.push_back(i);
        }
        filterActive = true;
    }

    // Reconcile the row-count change so wxGrid reallocates row geometry, then
    // repaint.
    if (gridPtr) {
        const int newVisible = VisibleRows();
        if (newVisible < oldVisible) {
            wxGridTableMessage msg(this, wxGRIDTABLE_NOTIFY_ROWS_DELETED,
                                   newVisible, oldVisible - newVisible);
            gridPtr->ProcessTableMessage(msg);
        } else if (newVisible > oldVisible) {
            wxGridTableMessage msg(this, wxGRIDTABLE_NOTIFY_ROWS_APPENDED,
                                   newVisible - oldVisible);
            gridPtr->ProcessTableMessage(msg);
        }
        gridPtr->ForceRefresh();
    }
}

void MmappedTable::SetColumnFilter(const ColumnFilter& f, wxGrid* gridPtr) {
    if (f.col < 0 || f.col >= static_cast<int>(columns.size())) return;
    const int oldVisible = VisibleRows();

    const size_t words = (static_cast<size_t>(rows) + 63) / 64;
    std::vector<uint64_t> bits(words);
    EvalFilter(f, bits);

    columnFilters[f.col] = f;
    filterBits[f.col] = std::move(bits);

    RebuildFilteredView(oldVisible, gridPtr);
}

void MmappedTable::ClearColumnFilter(int col, wxGrid* gridPtr) {
    if (columnFilters.erase(col) == 0) return;
    filterBits.erase(col);
    const int oldVisible = VisibleRows();
    RebuildFilteredView(oldVisible, gridPtr);
}

void MmappedTable::ClearFilters(wxGrid* gridPtr) {
    if (columnFilters.empty()) return;
    const int oldVisible = VisibleRows();
    columnFilters.clear();
    filterBits.clear();
    RebuildFilteredView(oldVisible, gridPtr);
}

void MmappedTable::RecomputeFiltersAfterEdit(int col, wxGrid* gridPtr) {
    if (columnFilters.empty()) return;

    const int oldVisible = VisibleRows();
    const size_t words = (static_cast<size_t>(rows) + 63) / 64;

    // Re-evaluate the edited column's own predicate (if any). Derived columns
    // may read the edited base column, so any predicate on a derived column is
    // re-evaluated too; base-column predicates on other columns are unaffected
    // by an edit and keep their cached bitvector.
    for (auto& [fcol, f] : columnFilters) {
        const bool isEditedCol = (fcol == col);
        const bool isDerived = (fcol >= numBaseCols);
        if (isEditedCol || isDerived) {
            std::vector<uint64_t>& bits = filterBits[fcol];
            if (bits.size() != words) bits.assign(words, 0);
            EvalFilter(f, bits);
        }
    }

    RebuildFilteredView(oldVisible, gridPtr);
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

namespace {
// A streambuf that forwards everything written to `dest` while folding the same
// bytes into a running SHA-256, so we can fingerprint the output during the one
// write pass instead of re-reading the finished file.
class Sha256TeeBuf : public std::streambuf {
public:
    explicit Sha256TeeBuf(std::streambuf* dest) : dest_(dest) {}
    std::string hex() { return ctx_.hex(); }

protected:
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return ch;
        const char c = static_cast<char>(ch);
        ctx_.update(reinterpret_cast<const unsigned char*>(&c), 1);
        return dest_->sputc(c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        ctx_.update(reinterpret_cast<const unsigned char*>(s),
                    static_cast<std::size_t>(n));
        return dest_->sputn(s, n);
    }

private:
    std::streambuf* dest_;
    sha256_detail::Ctx ctx_;
};
} // namespace

bool MmappedTable::ExportBaseColumnsToCsv(const std::string& path,
                                          std::string& errorOut,
                                          std::string* newShaOut) {
    errorOut.clear();
    if (numBaseCols == 0) return true; // nothing to write

    const fs::path target(path);
    const fs::path tmp = target.parent_path() /
        (target.filename().string() + ".gigasheet.tmp");

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            errorOut = "Failed to open for writing: " + tmp.string();
            return false;
        }

        // Tee the CSV writer's output through a SHA-256 accumulator.
        Sha256TeeBuf tee(file.rdbuf());
        std::ostream out(&tee);

        // Vince's CSV writer handles RFC-4180 quoting. Crucially we disable
        // auto-flush: by default it flushes the stream after *every* row, which
        // makes writing a large file pathologically slow. With it off, output
        // is batched (64 KB) and flushed in bulk.
        auto writer = csv::make_csv_writer(out);
        writer.set_auto_flush(false);

        // Reused per-row buffer of field strings (fed to the writer as a range).
        std::vector<std::string> record(static_cast<std::size_t>(numBaseCols));

        // Header row: base column labels.
        for (int col = 0; col < numBaseCols; ++col) record[col] = columns[col].label;
        writer << record;

        char numbuf[32];
        // One line per underlying row (full table, ignoring any active filter).
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < numBaseCols; ++col) {
                const Column& c = columns[col];
                if (c.type == ColumnType::INT32) {
                    std::snprintf(numbuf, sizeof(numbuf), "%d",
                                  std::get<FnInt>(c.fn)(row));
                    record[col] = numbuf;
                } else if (c.type == ColumnType::DOUBLE) {
                    // %.15g round-trips a double without gratuitous trailing
                    // zeros (whole values print as e.g. "3", not "3.000000").
                    std::snprintf(numbuf, sizeof(numbuf), "%.15g",
                                  std::get<FnDbl>(c.fn)(row));
                    record[col] = numbuf;
                } else { // CHARBUF
                    char_buf b = std::get<FnChar>(c.fn)(row);
                    b[63] = '\0';
                    record[col].assign(b.data());
                }
            }
            writer << record;
        }

        writer.flush();
        if (!file) {
            errorOut = "Write failed: " + tmp.string();
            std::error_code ec; fs::remove(tmp, ec);
            return false;
        }
        if (newShaOut) *newShaOut = tee.hex();
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        // Cross-device or locked target: fall back to copy+remove.
        fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
        std::error_code ec2; fs::remove(tmp, ec2);
        if (ec) {
            errorOut = "Failed to replace " + target.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

StatsResult MmappedTable::ComputeColumnStats(int col, int rowBegin, int rowEnd) const {
    if (col < 0 || col >= static_cast<int>(columns.size()) || rows == 0) {
        return {};
    }

    // With a filter active, statistics cover only the visible (matching) rows.
    // The incoming [rowBegin, rowEnd] is in view space; scan those filtered
    // rows directly (the chunk index doesn't apply to a sparse row set).
    if (filterActive) {
        rowBegin = std::max(rowBegin, 0);
        rowEnd = std::min(rowEnd, static_cast<int>(filteredRows.size()) - 1);
        if (rowEnd < rowBegin) return {};
        std::vector<int> sub(filteredRows.begin() + rowBegin,
                             filteredRows.begin() + rowEnd + 1);
        return ComputeStatsRows(columns[col], sub);
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
    // View-space clamp: the visible row count is the filtered count when a
    // filter is active, otherwise the full table.
    const int maxRow = VisibleRows() - 1;
    rowBegin = std::max(rowBegin, 0);
    rowEnd = std::min(rowEnd, maxRow);
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
            // Evaluate at the first visible (underlying) row.
            r.value = (*fn)(UnderlyingRow(rowBegin));
            r.usedIndex = aggregateIndexUsed;
            return r;
        }
        if (filterActive) {
            std::vector<int> sub(filteredRows.begin() + rowBegin,
                                 filteredRows.begin() + rowEnd + 1);
            return ScanFnRows(*fn, sub);
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
    return VisibleRows();
}

int MmappedTable::GetNumberCols() {
    return static_cast<int>(columns.size());
}

bool MmappedTable::IsEmptyCell(int row, int col) {
    return row < 0 || row >= VisibleRows() || col < 0 ||
           col >= static_cast<int>(columns.size());
}

wxString MmappedTable::GetValue(int row, int col) {
    if (IsEmptyCell(row, col)) return "N/A";
    row = UnderlyingRow(row);
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
    if (row < 0 || row >= VisibleRows() || col < 0 ||
        col >= static_cast<int>(columns.size())) {
        return;
    }
    row = UnderlyingRow(row);
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
    wxString label = columns[col].label;
    if (filteringEnabled) {
        // A filled marker for a column with an active predicate, an outline one
        // for a column you can click to add a filter.
        label += HasColumnFilter(col)
                     ? wxString::FromUTF8(" \xE2\x96\xBC")   // " ▼"
                     : wxString::FromUTF8(" \xE2\x96\xBD");  // " ▽"
    }
    return label;
}

wxString MmappedTable::GetRowLabelValue(int row) {
    // Show the underlying row number so filtered rows keep their real identity.
    const int underlying = UnderlyingRow(row);
    return wxString::Format("%d", (underlying < 0 ? row : underlying) + 1);
}
