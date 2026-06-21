#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "csv.hpp"

#include "../model/column_types.hpp"

namespace fs = std::filesystem;

namespace {

constexpr size_t kInferenceRows = 20;

std::string MakeColumnName(const std::vector<std::string>& headerRow, size_t col) {
    if (col < headerRow.size() && !headerRow[col].empty()) {
        return headerRow[col];
    }
    return "Column " + std::to_string(col + 1);
}

std::array<char, 64> ToCharBuf(const std::string& text) {
    std::array<char, 64> buf{};
    const size_t copyCount = std::min<size_t>(text.size(), buf.size() - 1);
    std::copy_n(text.data(), copyCount, buf.data());
    return buf;
}

std::vector<ColumnType> InferTypes(
    csv::CSVReader& reader,
    size_t columnCount,
    size_t maxSampleRows
) {
    std::vector<ColumnType> types(columnCount, ColumnType::CHARBUF);
    std::vector<bool> sawValue(columnCount, false);
    std::vector<bool> allInt(columnCount, true);
    std::vector<bool> allDouble(columnCount, true);

    csv::CSVRow row;
    size_t sampled = 0;
    while (sampled < maxSampleRows && reader.read_row(row)) {
        for (size_t col = 0; col < columnCount; ++col) {
            if (col >= row.size()) {
                continue;
            }

            auto field = row[col];
            sawValue[col] = true;
            int32_t intValue = 0;
            if (!field.try_get(intValue)) {
                allInt[col] = false;
            }
            double doubleValue = 0.0;
            if (!field.try_get(doubleValue)) {
                allDouble[col] = false;
            }
        }
        ++sampled;
    }

    for (size_t col = 0; col < columnCount; ++col) {
        if (!sawValue[col]) {
            types[col] = ColumnType::CHARBUF;
        } else if (allInt[col]) {
            types[col] = ColumnType::INT32;
        } else if (allDouble[col]) {
            types[col] = ColumnType::DOUBLE;
        } else {
            types[col] = ColumnType::CHARBUF;
        }
    }

    return types;
}

void WriteMetadata(std::ofstream& meta, const std::vector<std::string>& headerRow, const std::vector<ColumnType>& types) {
    const int32_t numCols = static_cast<int32_t>(types.size());
    meta.write(reinterpret_cast<const char*>(&numCols), sizeof(numCols));

    for (size_t col = 0; col < types.size(); ++col) {
        const std::uint8_t type = static_cast<std::uint8_t>(types[col]);
        const std::string name = MakeColumnName(headerRow, col);
        const std::uint16_t nameLen = static_cast<std::uint16_t>(
            std::min<size_t>(name.size(), std::numeric_limits<std::uint16_t>::max())
        );

        meta.write(reinterpret_cast<const char*>(&type), sizeof(type));
        meta.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        meta.write(name.data(), nameLen);
    }
}

void WriteDataFiles(
    csv::CSVReader& reader,
    const fs::path& outputDir,
    const std::vector<ColumnType>& types
) {
    std::vector<std::ofstream> outs;
    outs.reserve(types.size());
    for (size_t col = 0; col < types.size(); ++col) {
        outs.emplace_back(outputDir / (std::to_string(col) + ".bin"), std::ios::binary);
        if (!outs.back()) {
            throw std::runtime_error("Failed to create column file for column " + std::to_string(col));
        }
    }

    csv::CSVRow row;
    while (reader.read_row(row)) {
        for (size_t col = 0; col < types.size(); ++col) {
            switch (types[col]) {
                case ColumnType::INT32: {
                    const int32_t value = (col < row.size()) ? row[col].get<int32_t>() : 0;
                    outs[col].write(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
                case ColumnType::DOUBLE: {
                    const double value = (col < row.size()) ? row[col].get<double>() : 0.0;
                    outs[col].write(reinterpret_cast<const char*>(&value), sizeof(value));
                    break;
                }
                case ColumnType::CHARBUF: {
                    const auto buf = (col < row.size()) ? ToCharBuf(row[col].get<std::string>()) : std::array<char, 64>{};
                    outs[col].write(buf.data(), static_cast<std::streamsize>(buf.size()));
                    break;
                }
            }

            if (!outs[col]) {
                throw std::runtime_error("Failed while writing column " + std::to_string(col));
            }
        }
    }
}

csv::CSVFormat MakeFormat() {
    csv::CSVFormat format = csv::CSVFormat::guess_csv();
    format.header_row(0)
          .variable_columns(csv::VariableColumnPolicy::KEEP);
    return format;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: csv_importer <input.csv> <output_dir>\n";
        return 1;
    }

    const fs::path inputPath = argv[1];
    const fs::path outputDir = argv[2];

    try {
        const csv::CSVFormat format = MakeFormat();

        csv::CSVReader headerReader(inputPath.string(), format);
        const std::vector<std::string> headerRow = headerReader.get_col_names();
        if (headerRow.empty()) {
            std::cerr << "CSV has no columns to import.\n";
            return 1;
        }

        auto inferenceReader = csv::CSVReader(inputPath.string(), format);
        const auto inferenceStart = std::chrono::steady_clock::now();
        const std::vector<ColumnType> types = InferTypes(inferenceReader, headerRow.size(), kInferenceRows);
        const auto inferenceEnd = std::chrono::steady_clock::now();

        fs::create_directories(outputDir);

        const auto writeStart = std::chrono::steady_clock::now();
        {
            std::ofstream meta(outputDir / "metadata.bin", std::ios::binary);
            if (!meta) {
                std::cerr << "Failed to create metadata.bin in " << outputDir.string() << "\n";
                return 1;
            }

            WriteMetadata(meta, headerRow, types);
            if (!meta) {
                std::cerr << "Failed to write metadata.bin\n";
                return 1;
            }
        }

        auto writeReader = csv::CSVReader(inputPath.string(), format);
        WriteDataFiles(writeReader, outputDir, types);
        const auto writeEnd = std::chrono::steady_clock::now();

        const auto inferenceMs = std::chrono::duration_cast<std::chrono::milliseconds>(inferenceEnd - inferenceStart).count();
        const auto writeMs = std::chrono::duration_cast<std::chrono::milliseconds>(writeEnd - writeStart).count();

        std::cout << "Imported " << headerRow.size() << " columns into " << outputDir.string() << "\n";
        std::cout << "Type inference: " << inferenceMs << " ms\n";
        std::cout << "File writing: " << writeMs << " ms\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "csv_importer failed: " << ex.what() << "\n";
        return 1;
    }
}
