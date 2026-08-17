#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// Thrown by ImportCsv when it observes the cancel flag set. Callers running a
// speculative import (see the concurrent open path) catch this to abort quietly.
struct ImportCancelled {};

// Header labels + the first few data rows of a CSV, values as raw strings, for
// painting an instant preview before the full typed import finishes.
struct CsvPreview {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

// Read the header row and up to `maxRows` data rows from a CSV. Cheap: it stops
// after `maxRows`, so it's suitable to call synchronously on the UI thread.
CsvPreview PreviewCsv(const std::filesystem::path& inputPath, std::size_t maxRows);

// Import a CSV into the columnar on-disk format under outputDir.
// If `cancel` is non-null and becomes true mid-import, the import aborts by
// throwing ImportCancelled (the partially written outputDir is the caller's to
// clean up).
std::filesystem::path ImportCsv(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDir,
    const std::atomic<bool>* cancel = nullptr);
