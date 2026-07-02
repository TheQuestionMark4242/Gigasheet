#pragma once

#include <cstdint>
#include <functional>
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
};

// Computes SUM/MIN/MAX/COUNT over rows [rowBegin, rowEnd] (inclusive) of a
// numeric column. If records is non-null, fully covered chunks are folded
// from the precomputed stats and only the partial boundary chunks are
// scanned through the column function; otherwise the whole range is scanned.
// CHARBUF columns get count only (valid == false, count set).
StatsResult ComputeStats(
    const Column& column,
    const std::vector<ChunkRecord>* records,
    int rowBegin,
    int rowEnd);

// Computes SUM/MIN/MAX/COUNT over rows [rowBegin, rowEnd] (inclusive) of an
// arbitrary numeric row function (e.g. a compiled formula). Always a full
// scan; the sum is accumulated in double (intSum == false).
StatsResult ScanFn(
    const std::function<double(int)>& fn,
    int rowBegin,
    int rowEnd);
