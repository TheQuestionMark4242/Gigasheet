#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "../model/column_types.hpp"

// Per-chunk summary statistics for a column, persisted to <col>.stats.bin
// next to the column's <col>.bin data file. CHARBUF columns have no stats
// file (absence of the file means no stats for that column).
//
// <col>.stats.bin layout (all little-endian, natural alignment):
//   Header (24 bytes):
//     char[4]  magic      = "CSTA"
//     uint16   version    = 1
//     uint16   reserved   = 0
//     uint32   chunkSize
//     uint32   reserved2  = 0
//     uint64   numRows
//   Then ceil(numRows/chunkSize) ChunkRecords of 32 bytes each.

inline constexpr std::uint32_t kChunkSize = 65536;
inline constexpr char kStatsMagic[4] = {'C', 'S', 'T', 'A'};
inline constexpr std::uint16_t kStatsVersion = 1;
inline constexpr const char* kStatsFileSuffix = ".stats.bin";

struct ChunkRecord {
    // int64 for INT32 columns (exact sums), double for DOUBLE columns;
    // interpretation is keyed off the column type in metadata.bin.
    union Sum {
        std::int64_t i;
        double d;
    } sum;
    double min;
    double max;
    std::uint64_t count;
};
static_assert(sizeof(ChunkRecord) == 32, "ChunkRecord must be 32 bytes on disk");

// Accumulates one column's values row by row, closing a chunk every
// kChunkSize rows. Chunk boundaries are independent of any write-flush
// cadence used by the caller.
class ChunkAccumulator {
public:
    explicit ChunkAccumulator(ColumnType type) : type_(type) {}

    void Add(std::int32_t value) {
        isum_ += value;
        AddCommon(static_cast<double>(value));
    }

    void Add(double value) {
        dsum_ += value;
        AddCommon(value);
    }

    // Closes any partially filled chunk and returns all records.
    const std::vector<ChunkRecord>& Finish() {
        if (count_ > 0) {
            CloseChunk();
        }
        return records_;
    }

    ColumnType Type() const { return type_; }

private:
    void AddCommon(double value) {
        if (value < min_) min_ = value;
        if (value > max_) max_ = value;
        if (++count_ == kChunkSize) {
            CloseChunk();
        }
    }

    void CloseChunk() {
        ChunkRecord rec;
        if (type_ == ColumnType::INT32) {
            rec.sum.i = isum_;
        } else {
            rec.sum.d = dsum_;
        }
        rec.min = min_;
        rec.max = max_;
        rec.count = count_;
        records_.push_back(rec);

        isum_ = 0;
        dsum_ = 0.0;
        min_ = std::numeric_limits<double>::infinity();
        max_ = -std::numeric_limits<double>::infinity();
        count_ = 0;
    }

    ColumnType type_;
    std::int64_t isum_ = 0;
    double dsum_ = 0.0;
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
    std::uint64_t count_ = 0;
    std::vector<ChunkRecord> records_;
};
