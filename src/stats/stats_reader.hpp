#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "chunk_stats.hpp"

// Loads the per-column <col>.stats.bin files of a table directory.
// Columns whose stats file is missing or invalid simply have no stats
// (older datasets predate stats files entirely).
class ColumnStatsIndex {
public:
    ColumnStatsIndex() = default;

    // numCols/expectedRows come from metadata.bin / the mapped column files;
    // a stats file whose row count disagrees is treated as stale and ignored.
    static ColumnStatsIndex Load(
        const std::filesystem::path& dir,
        int numCols,
        std::uint64_t expectedRows);

    bool HasStats(int col) const {
        return col >= 0 && col < static_cast<int>(perColumn_.size()) &&
               !perColumn_[col].empty();
    }

    // Valid only when HasStats(col).
    const std::vector<ChunkRecord>& Records(int col) const {
        return perColumn_[col];
    }

private:
    std::vector<std::vector<ChunkRecord>> perColumn_;
};
