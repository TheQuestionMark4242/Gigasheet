#include "stats_reader.hpp"

#include <cstring>
#include <fstream>
#include <string>

namespace {

std::vector<ChunkRecord> LoadOneColumn(
    const std::filesystem::path& path,
    std::uint64_t expectedRows)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }

    char magic[4];
    std::uint16_t version = 0, reserved = 0;
    std::uint32_t chunkSize = 0, reserved2 = 0;
    std::uint64_t numRows = 0;

    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
    in.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));
    in.read(reinterpret_cast<char*>(&reserved2), sizeof(reserved2));
    in.read(reinterpret_cast<char*>(&numRows), sizeof(numRows));

    if (!in ||
        std::memcmp(magic, kStatsMagic, sizeof(magic)) != 0 ||
        version != kStatsVersion ||
        chunkSize != kChunkSize ||
        numRows != expectedRows ||
        numRows == 0)
    {
        return {};
    }

    const std::uint64_t numChunks = (numRows + chunkSize - 1) / chunkSize;
    std::vector<ChunkRecord> records(numChunks);
    in.read(reinterpret_cast<char*>(records.data()),
            static_cast<std::streamsize>(numChunks * sizeof(ChunkRecord)));
    if (!in || static_cast<std::uint64_t>(in.gcount()) != numChunks * sizeof(ChunkRecord)) {
        return {};
    }

    return records;
}

} // namespace

ColumnStatsIndex ColumnStatsIndex::Load(
    const std::filesystem::path& dir,
    int numCols,
    std::uint64_t expectedRows)
{
    ColumnStatsIndex index;
    index.perColumn_.resize(numCols);
    for (int col = 0; col < numCols; ++col) {
        const auto path = dir / (std::to_string(col) + kStatsFileSuffix);
        index.perColumn_[col] = LoadOneColumn(path, expectedRows);
    }
    return index;
}
