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

// A single column predicate. Numeric columns (INT32/DOUBLE) use a closed range
// [lo, hi]; text columns (CHARBUF) match on equality or substring against
// `text`. Multiple filters are combined with AND (see ApplyFilters).
struct ColumnFilter {
    enum class Kind { Range, Equals, Contains };

    int col = -1;
    Kind kind = Kind::Range;

    // Range predicate (numeric columns).
    double lo = 0.0;
    double hi = 0.0;

    // Equals / Contains predicate (text columns).
    std::string text;
};

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

    // -------- Filtering (bitvector full-scan predicate evaluation) --------
    // Replace the active filter set and rebuild the filtered-row view by
    // scanning the mmapped columns into per-predicate bitvectors and AND-ing
    // them. gridPtr (if given) is notified so the view refreshes. Passing an
    // empty list clears filtering (the full table is shown again).
    void SetFilters(const std::vector<ColumnFilter>& filters, wxGrid* gridPtr);
    void ClearFilters(wxGrid* gridPtr);

    bool IsFiltered() const { return filterActive; }
    const std::vector<ColumnFilter>& ActiveFilters() const { return activeFilters; }

    // Rows currently visible: the filtered count when a filter is active,
    // otherwise the full table. TotalRows() is always the underlying count.
    int VisibleRows() const { return filterActive ? static_cast<int>(filteredRows.size()) : rows; }
    int TotalRows() const { return rows; }

    // Translate a grid (view) row into an underlying table row. When no filter
    // is active this is the identity.
    int UnderlyingRow(int viewRow) const {
        if (!filterActive) return viewRow;
        if (viewRow < 0 || viewRow >= static_cast<int>(filteredRows.size())) return -1;
        return filteredRows[viewRow];
    }

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

    // -------- Filtered view state --------
    // Evaluate one predicate over every row into `out` (one bit per row).
    void EvalFilter(const ColumnFilter& f, std::vector<uint64_t>& out) const;

    std::vector<ColumnFilter> activeFilters;   // AND-combined predicates
    std::vector<int> filteredRows;             // view row -> underlying row
    bool filterActive = false;
};
