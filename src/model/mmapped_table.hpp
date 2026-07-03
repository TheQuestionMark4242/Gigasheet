#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <wx/grid.h>

#include "mio/mmap.hpp"

#include "column_types.hpp"
#include "../parser/expr_eval.hpp"
#include "../stats/stats_query.hpp"
#include "../stats/stats_reader.hpp"

class MmappedTable : public wxGridTableBase {
public:
    /** 
     * Create a wxWidget data source from a memory mmapped file
     */
    MmappedTable(const std::string&);


    // Add a derived column by expression (e.g. "=A + B*2"). gridPtr is used to notify view.
    void AddDerivedColumn(const wxString&, wxGrid*);

    // SUM/MIN/MAX/COUNT over rows [rowBegin, rowEnd] of a column, using the
    // precomputed chunk index for base numeric columns when available and
    // falling back to a full scan (derived columns, pre-stats datasets).
    StatsResult ComputeColumnStats(int col, int rowBegin, int rowEnd) const;

    // Statistics over an arbitrary formula (same syntax as derived columns).
    // A bare column reference delegates to ComputeColumnStats (chunk index);
    // anything else compiles the expression and scans the range. Parse and
    // compile failures set errorOut and return an invalid result.
    StatsResult ComputeFormulaStats(
        const std::string& expr, int rowBegin, int rowEnd,
        std::string& errorOut) const;

    const Column& GetColumn(int col) const { return columns[col]; }

    bool HasChunkStats(int col) const {
        return col < numBaseCols && statsIndex.HasStats(col);
    }

    // Unsaved sparse cell edits (kept in memory until Save).
    bool HasUnsavedChanges() const;
    std::size_t UnsavedCellCount() const;

    // Writes all unsaved edits into the column files in place, refreshes
    // the stats records of the affected chunks (on disk and in memory) and
    // clears the override map. Returns false with errorOut set on failure;
    // unwritten edits are kept.
    bool SaveOverrides(std::string& errorOut);

    // -------- wxGridTableBase overrides ----------
    int GetNumberRows() override;
    int GetNumberCols() override;
    bool IsEmptyCell(int row, int col) override;
    wxString GetValue(int row, int col) override;
    void SetValue(int, int, const wxString&) override;
    wxString GetColLabelValue(int col) override;
    wxString GetRowLabelValue(int row) override;

private:
    // Column names (labels and A..Z aliases) visible to expressions.
    exprparse::TypedColumnMap BuildColumnMap() const;

    // Resolves a column label or A..Z alias to an index; -1 if unknown.
    // Matches BuildColumnMap precedence (a later registration wins).
    int FindColumn(const std::string& name) const;

    // Resolves SUM/AVERAGE/MIN/MAX/COUNT over a cell range for the
    // expression compiler. Sets *usedIndexOut when chunk records were used.
    exprparse::Aggregator MakeAggregator(bool* usedIndexOut) const;

    // Chunk ids of the given base column that contain unsaved edits (their
    // precomputed stats records reflect the on-disk data, not the edits).
    std::unordered_set<std::int64_t> DirtyChunks(int col) const;

    using CellOverride = std::variant<std::int32_t, double, char_buf>;

    std::vector<mio::mmap_source> mmaps;   // keep mmaps alive
    std::vector<Column> columns;           // base + derived (in the same vector)
    // Per base column: row -> unsaved edited value, overriding the mmap.
    // Column fns consult this map, so the grid, derived columns and
    // statistics all see unsaved edits.
    std::vector<std::unordered_map<int, CellOverride>> overrides;
    std::vector<Column> derivedColumns;    // keep derived for metadata if needed
    ColumnStatsIndex statsIndex;           // per-chunk stats for base columns
    std::string dirPath;                   // table directory (for Save)
    int numBaseCols = 0;                   // columns backed by files (not derived)
    int rows = 0;
};
