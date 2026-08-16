#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "chunk_stats.hpp"

// Writes <col>.stats.bin into outputDir for each numeric column.
// accumulators must be ordered by column index; CHARBUF columns are skipped
// (no stats file is written for them).
void WriteStatsFiles(
    const std::filesystem::path& outputDir,
    std::vector<ChunkAccumulator>& accumulators,
    std::uint64_t numRows);
