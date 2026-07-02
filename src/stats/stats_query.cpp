#include "stats_query.hpp"

#include <algorithm>
#include <limits>
#include <variant>

namespace {

// Scans rows [a, b] inclusive through the column function.
void ScanRange(const Column& column, int a, int b, StatsResult& r) {
    if (column.type == ColumnType::INT32) {
        const auto& f = std::get<FnInt>(column.fn);
        for (int i = a; i <= b; ++i) {
            const std::int32_t v = f(i);
            r.isum += v;
            const double d = static_cast<double>(v);
            if (d < r.min) r.min = d;
            if (d > r.max) r.max = d;
        }
    } else {
        const auto& f = std::get<FnDbl>(column.fn);
        for (int i = a; i <= b; ++i) {
            const double d = f(i);
            r.sum += d;
            if (d < r.min) r.min = d;
            if (d > r.max) r.max = d;
        }
    }
}

} // namespace

StatsResult ComputeStats(
    const Column& column,
    const std::vector<ChunkRecord>* records,
    int rowBegin,
    int rowEnd)
{
    StatsResult r;
    if (rowBegin < 0 || rowEnd < rowBegin) {
        return r;
    }
    r.count = static_cast<std::uint64_t>(rowEnd) - rowBegin + 1;

    if (column.type == ColumnType::CHARBUF) {
        return r; // count only
    }

    r.intSum = column.type == ColumnType::INT32;
    r.min = std::numeric_limits<double>::infinity();
    r.max = -std::numeric_limits<double>::infinity();

    if (records && !records->empty()) {
        const std::int64_t cs = kChunkSize;
        const std::int64_t cBegin = rowBegin / cs;
        const std::int64_t cEnd = rowEnd / cs;

        for (std::int64_t c = cBegin;
             c <= cEnd && c < static_cast<std::int64_t>(records->size());
             ++c)
        {
            const ChunkRecord& rec = (*records)[c];
            const std::int64_t chunkStart = c * cs;
            const std::int64_t chunkEnd = chunkStart + static_cast<std::int64_t>(rec.count) - 1;

            if (rowBegin <= chunkStart && chunkEnd <= rowEnd) {
                if (r.intSum) {
                    r.isum += rec.sum.i;
                } else {
                    r.sum += rec.sum.d;
                }
                if (rec.min < r.min) r.min = rec.min;
                if (rec.max > r.max) r.max = rec.max;
                r.usedIndex = true;
            } else {
                const int a = static_cast<int>(std::max<std::int64_t>(rowBegin, chunkStart));
                const int b = static_cast<int>(std::min<std::int64_t>(rowEnd, chunkEnd));
                if (a <= b) {
                    ScanRange(column, a, b, r);
                }
            }
        }
    } else {
        ScanRange(column, rowBegin, rowEnd, r);
    }

    if (r.intSum) {
        r.sum = static_cast<double>(r.isum);
    }
    r.valid = true;
    return r;
}
