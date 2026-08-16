#pragma once

#include <atomic>
#include <filesystem>

// Thrown by ImportCsv when it observes the cancel flag set. Callers running a
// speculative import (see the concurrent open path) catch this to abort quietly.
struct ImportCancelled {};

// Import a CSV into the columnar on-disk format under outputDir.
// If `cancel` is non-null and becomes true mid-import, the import aborts by
// throwing ImportCancelled (the partially written outputDir is the caller's to
// clean up).
std::filesystem::path ImportCsv(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputDir,
    const std::atomic<bool>* cancel = nullptr);
