#pragma once
#include <cstdint>

enum class ColumnType : uint8_t {
    INT32  = 0,
    DOUBLE = 1,
    CHARBUF = 2
};
