#pragma once

#include <filesystem>

std::filesystem::path ImportCsv(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDir);