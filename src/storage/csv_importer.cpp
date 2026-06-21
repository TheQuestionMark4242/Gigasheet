#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "csv.hpp"
#include "csv_importer.hpp"

#include "../model/column_types.hpp"

namespace fs = std::filesystem;

namespace {

constexpr size_t kInferenceRows = 20;
constexpr size_t kFlushRows = 1'000'000;

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

    std::vector<std::vector<int32_t>> intBuffers(types.size());
    std::vector<std::vector<double>> doubleBuffers(types.size());
    std::vector<std::vector<char_buf>> charBuffers(types.size());
    for (size_t col = 0; col < types.size(); ++col) {
        switch (types[col]) {
            case ColumnType::INT32:
                intBuffers[col].reserve(kFlushRows);
                break;
            case ColumnType::DOUBLE:
                doubleBuffers[col].reserve(kFlushRows);
                break;
            case ColumnType::CHARBUF:
                charBuffers[col].reserve(kFlushRows);
                break;
        }
    }

    auto flushBuffers = [&]() {
        for (size_t col = 0; col < types.size(); ++col) {
            switch (types[col]) {
                case ColumnType::INT32: {
                    const auto& buffer = intBuffers[col];
                    if (!buffer.empty()) {
                        outs[col].write(reinterpret_cast<const char*>(buffer.data()),
                                        static_cast<std::streamsize>(buffer.size() * sizeof(int32_t)));
                        if (!outs[col]) {
                            throw std::runtime_error("Failed while writing int column " + std::to_string(col));
                        }
                        intBuffers[col].clear();
                    }
                    break;
                }
                case ColumnType::DOUBLE: {
                    const auto& buffer = doubleBuffers[col];
                    if (!buffer.empty()) {
                        outs[col].write(reinterpret_cast<const char*>(buffer.data()),
                                        static_cast<std::streamsize>(buffer.size() * sizeof(double)));
                        if (!outs[col]) {
                            throw std::runtime_error("Failed while writing double column " + std::to_string(col));
                        }
                        doubleBuffers[col].clear();
                    }
                    break;
                }
                case ColumnType::CHARBUF: {
                    const auto& buffer = charBuffers[col];
                    if (!buffer.empty()) {
                        outs[col].write(reinterpret_cast<const char*>(buffer.data()),
                                        static_cast<std::streamsize>(buffer.size() * sizeof(char_buf)));
                        if (!outs[col]) {
                            throw std::runtime_error("Failed while writing string column " + std::to_string(col));
                        }
                        charBuffers[col].clear();
                    }
                    break;
                }
            }
        }
    };

    csv::CSVRow row;
    size_t bufferedRows = 0;
    while (reader.read_row(row)) {
        for (size_t col = 0; col < types.size(); ++col) {
            if (col >= row.size()) {
                switch (types[col]) {
                    case ColumnType::INT32:
                        intBuffers[col].push_back(0);
                        break;
                    case ColumnType::DOUBLE:
                        doubleBuffers[col].push_back(0.0);
                        break;
                    case ColumnType::CHARBUF:
                        charBuffers[col].push_back(std::array<char, 64>{});
                        break;
                }
                continue;
            }

            auto field = row[col];
            switch (types[col]) {
                case ColumnType::INT32:
                {
                    int32_t value = 0;
                    field.try_get(value);
                    intBuffers[col].push_back(value);
                    break;
                }
                case ColumnType::DOUBLE:
                {
                    double value = 0.0;
                    field.try_get(value);
                    doubleBuffers[col].push_back(value);
                    break;
                }
                case ColumnType::CHARBUF:
                    charBuffers[col].push_back(ToCharBuf(field.get<std::string>()));
                    break;
            }
        }

        ++bufferedRows;
        if (bufferedRows >= kFlushRows) {
            flushBuffers();
            bufferedRows = 0;
        }
    }

    flushBuffers();
}

csv::CSVFormat MakeFormat() {
    csv::CSVFormat format = csv::CSVFormat::guess_csv();
    format.header_row(0)
          .variable_columns(csv::VariableColumnPolicy::KEEP);
    return format;
}

} // namespace

std::filesystem::path ImportCsv(
    const fs::path& inputPath,
    const fs::path& outputDir) 
{
    const csv::CSVFormat format = MakeFormat();

    csv::CSVReader headerReader(inputPath.string(), format);
    const std::vector<std::string> headerRow = headerReader.get_col_names();

    if (headerRow.empty()) {
        throw std::runtime_error("CSV has no columns to import.");
    }

    auto inferenceReader = csv::CSVReader(inputPath.string(), format);
    const std::vector<ColumnType> types =
        InferTypes(inferenceReader, headerRow.size(), kInferenceRows);

    fs::create_directories(outputDir);

    {
        std::ofstream meta(outputDir / "metadata.bin", std::ios::binary);

        if (!meta) {
            throw std::runtime_error(
                "Failed to create metadata.bin in " +
                outputDir.string());
        }

        WriteMetadata(meta, headerRow, types);

        if (!meta) {
            throw std::runtime_error(
                "Failed to write metadata.bin");
        }
    }

    auto writeReader = csv::CSVReader(inputPath.string(), format);
    WriteDataFiles(writeReader, outputDir, types);

    return outputDir;
}