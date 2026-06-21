#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../src/model/column_types.hpp"

namespace fs = std::filesystem;

namespace {

std::string TrimTrailingCR(std::string s) {
    if (!s.empty() && s.back() == '\r') {
        s.pop_back();
    }
    return s;
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;

    for (char ch : line) {
        if (ch == ',') {
            cells.push_back(TrimTrailingCR(cell));
            cell.clear();
        } else {
            cell.push_back(ch);
        }
    }

    cells.push_back(TrimTrailingCR(cell));
    return cells;
}

bool IsInt32(std::string_view text) {
    if (text.empty()) return false;

    try {
        size_t pos = 0;
        long long value = std::stoll(std::string(text), &pos, 10);
        if (pos != text.size()) return false;
        return value >= std::numeric_limits<int32_t>::min() &&
               value <= std::numeric_limits<int32_t>::max();
    } catch (...) {
        return false;
    }
}

bool IsDouble(std::string_view text) {
    if (text.empty()) return false;

    try {
        size_t pos = 0;
        (void)std::stod(std::string(text), &pos);
        return pos == text.size();
    } catch (...) {
        return false;
    }
}

std::array<char, 64> ToCharBuf(const std::string& text) {
    std::array<char, 64> buf{};
    const size_t copyCount = std::min<size_t>(text.size(), buf.size() - 1);
    std::copy_n(text.data(), copyCount, buf.data());
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: csv_importer <input.csv> <output_dir>\n";
        return 1;
    }

    const fs::path inputPath = argv[1];
    const fs::path outputDir = argv[2];

    std::ifstream input(inputPath);
    if (!input) {
        std::cerr << "Failed to open input CSV: " << inputPath.string() << "\n";
        return 1;
    }

    std::vector<std::vector<std::string>> rows;
    std::string line;
    size_t columnCount = 0;

    while (std::getline(input, line)) {
        auto cells = SplitCsvLine(line);
        columnCount = std::max(columnCount, cells.size());
        rows.push_back(std::move(cells));
    }

    if (rows.empty()) {
        std::cerr << "CSV is empty, nothing to import.\n";
        return 1;
    }

    std::vector<ColumnType> types(columnCount, ColumnType::CHARBUF);
    for (size_t col = 0; col < columnCount; ++col) {
        bool allInt = true;
        bool allDouble = true;

        for (const auto& row : rows) {
            const std::string value = col < row.size() ? row[col] : std::string{};
            if (value.empty()) {
                allInt = false;
                allDouble = false;
                break;
            }

            if (!IsInt32(value)) {
                allInt = false;
            }
            if (!IsDouble(value)) {
                allDouble = false;
            }
        }

        if (allInt) {
            types[col] = ColumnType::INT32;
        } else if (allDouble) {
            types[col] = ColumnType::DOUBLE;
        } else {
            types[col] = ColumnType::CHARBUF;
        }
    }

    fs::create_directories(outputDir);

    {
        std::ofstream meta(outputDir / "metadata.bin", std::ios::binary);
        if (!meta) {
            std::cerr << "Failed to create metadata.bin in " << outputDir.string() << "\n";
            return 1;
        }

        const int32_t numCols = static_cast<int32_t>(columnCount);
        meta.write(reinterpret_cast<const char*>(&numCols), sizeof(numCols));
        meta.write(reinterpret_cast<const char*>(types.data()), static_cast<std::streamsize>(types.size() * sizeof(ColumnType)));
        if (!meta) {
            std::cerr << "Failed to write metadata.bin\n";
            return 1;
        }
    }

    for (size_t col = 0; col < columnCount; ++col) {
        std::ofstream out(outputDir / (std::to_string(col) + ".bin"), std::ios::binary);
        if (!out) {
            std::cerr << "Failed to create column file for column " << col << "\n";
            return 1;
        }

        for (const auto& row : rows) {
            const std::string value = col < row.size() ? row[col] : std::string{};

            switch (types[col]) {
                case ColumnType::INT32: {
                    const int32_t parsed = static_cast<int32_t>(std::stoll(value));
                    out.write(reinterpret_cast<const char*>(&parsed), sizeof(parsed));
                    break;
                }
                case ColumnType::DOUBLE: {
                    const double parsed = std::stod(value);
                    out.write(reinterpret_cast<const char*>(&parsed), sizeof(parsed));
                    break;
                }
                case ColumnType::CHARBUF: {
                    const auto buf = ToCharBuf(value);
                    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
                    break;
                }
            }

            if (!out) {
                std::cerr << "Failed while writing column " << col << "\n";
                return 1;
            }
        }
    }

    std::cout << "Imported " << rows.size() << " rows and " << columnCount
              << " columns into " << outputDir.string() << "\n";
    return 0;
}
