#pragma once
#include <cstdint>

enum class ColumnType : uint8_t {
    INT32  = 0,
    DOUBLE = 1,
    CHARBUF = 2
};

using char_buf = std::array<char, 64>;
namespace fs = std::filesystem;
// ----------------- Unified column function variant -----------------
using FnInt   = std::function<int32_t(int)>;
using FnDbl   = std::function<double(int)>;
using FnChar  = std::function<char_buf(int)>;

using ColumnFnVariant = std::variant<FnInt, FnDbl, FnChar>;

struct Column {
    ColumnType type;
    ColumnFnVariant fn;   // callable: row -> value
    std::string label;    // "A", "B", ... or "D0"
};