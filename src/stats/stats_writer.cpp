#include "stats_writer.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

void WriteStatsFiles(
    const std::filesystem::path& outputDir,
    std::vector<ChunkAccumulator>& accumulators,
    std::uint64_t numRows)
{
    for (size_t col = 0; col < accumulators.size(); ++col) {
        auto& acc = accumulators[col];
        if (acc.Type() == ColumnType::CHARBUF) {
            continue;
        }

        const auto path = outputDir / (std::to_string(col) + kStatsFileSuffix);
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Failed to create " + path.string());
        }

        const std::uint16_t version = kStatsVersion;
        const std::uint16_t reserved = 0;
        const std::uint32_t chunkSize = kChunkSize;
        const std::uint32_t reserved2 = 0;

        out.write(kStatsMagic, sizeof(kStatsMagic));
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(&reserved), sizeof(reserved));
        out.write(reinterpret_cast<const char*>(&chunkSize), sizeof(chunkSize));
        out.write(reinterpret_cast<const char*>(&reserved2), sizeof(reserved2));
        out.write(reinterpret_cast<const char*>(&numRows), sizeof(numRows));

        const auto& records = acc.Finish();
        out.write(reinterpret_cast<const char*>(records.data()),
                  static_cast<std::streamsize>(records.size() * sizeof(ChunkRecord)));

        if (!out) {
            throw std::runtime_error("Failed while writing " + path.string());
        }
    }
}
