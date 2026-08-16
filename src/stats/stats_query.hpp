#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "../model/column_types.hpp"
#include "chunk_stats.hpp"

struct StatsResult {
    // For INT32 columns the sum is kept exactly in isum (intSum == true);
    // sum always carries the double view for AVG etc.
    double sum = 0.0;
    std::int64_t isum = 0;
    bool intSum = false;
    double min = 0.0;
    double max = 0.0;
    std::uint64_t count = 0;
    bool usedIndex = false;   // true if precomputed chunk records were used
    bool valid = false;       // false for empty ranges / CHARBUF columns

    // Excel-style aggregate expressions (=SUM(A1:A100)/COUNT(B:B)+1) yield a
    // single number instead of per-row SUM/AVG/MIN/MAX/COUNT.
    bool scalar = false;
    double value = 0.0;
};

// Computes SUM/MIN/MAX/COUNT over rows [rowBegin, rowEnd] (inclusive) of a
// numeric column. If records is non-null, fully covered chunks are folded
// from the precomputed stats and only the partial boundary chunks are
// scanned through the column function; otherwise the whole range is scanned.
// Chunks listed in dirtyChunks (unsaved cell edits) are scanned instead of
// folded, since their records reflect the on-disk data.
// CHARBUF columns get count only (valid == false, count set).
StatsResult ComputeStats(
    const Column& column,
    const std::vector<ChunkRecord>* records,
    int rowBegin,
    int rowEnd,
    const std::unordered_set<std::int64_t>* dirtyChunks = nullptr);

// Computes SUM/MIN/MAX/COUNT over rows [rowBegin, rowEnd] (inclusive) of an
// arbitrary numeric row function (e.g. a compiled formula). Always a full
// scan; the sum is accumulated in double (intSum == false).
StatsResult ScanFn(
    const std::function<double(int)>& fn,
    int rowBegin,
    int rowEnd);

// Like ComputeStats but over an explicit list of (underlying) row indices —
// used when a row filter is active. Always a direct scan (no chunk index,
// since the rows are sparse). CHARBUF columns get count only.
StatsResult ComputeStatsRows(
    const Column& column,
    const std::vector<int>& rowsList);

// Like ScanFn but over an explicit list of (underlying) row indices.
StatsResult ScanFnRows(
    const std::function<double(int)>& fn,
    const std::vector<int>& rowsList);
