# Gigasheet

A desktop spreadsheet for datasets far larger than memory. Gigasheet opens
multi-million-row tables instantly and keeps browsing, filtering, and computing
statistics responsive, by treating the data as an immutable columnar store that
is memory-mapped rather than loaded.

Built in C++20 with [wxWidgets](https://www.wxwidgets.org/) for the UI.

## Download & run (Windows)

Grab the latest `gigasheet-windows-*.zip` from the
[Releases](https://github.com/TheQuestionMark4242/Gigasheet/releases) page,
unzip it anywhere, and double-click `gigasheet.exe`. Everything it needs (the
wxWidgets/MinGW runtime DLLs) is bundled — no install required.

## Features

- **Open huge datasets instantly** — CSVs are imported once into a columnar
  on-disk format, then memory-mapped; open time is independent of row count.
- **CSV import** with a live loading screen; already-imported files are detected
  (by content hash) and skipped instead of re-converted.
- **Column filtering** — an "Enable Filtering" mode adds a dropdown to each
  column header: numeric range (`min`–`max`) or text `contains`/`equals`.
  Filters across columns combine with AND, and the view shows *X of Y rows*.
- **Derived columns** from Excel-style formulas (`=A + B*2`,
  `=CONCATENATE(Name, ' - ', A)`).
- **Statistics** over any column or formula — SUM / AVERAGE / MIN / MAX / COUNT,
  including aggregate ranges like `=SUM(A1:A100)/COUNT(B:B)`, with formula
  autocomplete.
- **In-place cell editing** with unsaved-edit tracking and save-back to disk.
- **Themeable UI** (light/dark presets), DPI-aware, native dark title bar.

## The challenge: doing this at scale

A naïve spreadsheet loads every cell into RAM. At the scale we target
(~4M rows × many columns) that is gigabytes of allocation, a slow load, and a
GC/heap-fragmentation problem — before the user has done anything. The hard
parts:

- **Loading** must not scale with row count. Reading and parsing every value up
  front is a non-starter.
- **Scanning** (filter a column, compute a SUM) touches every row. Done poorly
  it thrashes cache and stalls the UI thread.
- **Editing** must appear instant on an otherwise immutable, memory-mapped file,
  and must not force a full rewrite.
- **Statistics** on demand can't afford a full scan for every query on every
  column.

## How it works (algorithms & techniques)

**Columnar, memory-mapped storage.** On import, each column is written to its
own flat binary file (`<i>.bin`): `int32[]`, `double[]`, or fixed 64-byte
strings (`char[64][]`). A small `metadata.bin` records column types and labels.
At open time we `mmap` each column (via [mio](https://github.com/vimpunk/mio))
instead of reading it — the OS pages data in on demand, so open time is O(1) in
rows and the working set is only what's on screen. Columnar layout also means a
scan of one column reads contiguous bytes (cache- and prefetch-friendly).

**Bitvector predicate evaluation for filtering.** Each active filter is
evaluated as a single linear pass over its column, producing a bitvector
(`std::vector<uint64_t>`, one bit per row). Combining filters is then just
bitwise `AND` across bitvectors — cheap and branch-free. The set bits are
materialized into a `matching row → underlying row` index, and the grid renders
through that mapping, so filtering is a view layer over the immutable data.
Each column's bitvector is **cached**, so editing a cell only re-scans the one
affected column and re-runs the AND, rather than recomputing everything.

**Chunk-summary index for statistics.** Alongside each numeric column we persist
a `.stats.bin` file holding per-chunk summaries (sum/min/max/count for every
65,536-row chunk). A SUM/MIN/MAX/COUNT over a range folds the fully-covered
chunks straight from these precomputed records and only scans the partial chunks
at the two ends — turning a full-column scan into a handful of memory reads.
Filtered stats fall back to scanning just the matching rows.

**Copy-on-write edits.** Cell edits are kept in a sparse in-memory override map
(`row → value`) layered on top of the read-only mapping, so an edit is O(1) and
the file is untouched until *Save*. Saving patches only the edited bytes in
place and refreshes just the affected chunk summaries. Derived columns and
statistics read through the override, so everything stays consistent with
unsaved edits.

**Formula engine.** A Flex/Bison grammar parses Excel-style expressions into an
AST that is compiled once into a typed row function (numeric or string). Derived
columns and statistics reuse the same compiled function; bare column references
and aggregate ranges are recognized specially so they hit the chunk index.

## Building from source

Requires a C++20 compiler, CMake ≥ 3.20, Flex, Bison, Boost, and wxWidgets 3.2.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

CI builds on both Linux (wxGTK) and Windows (MSYS2/MinGW); Windows-only code is
`#ifdef __WXMSW__`-guarded and Windows-only libraries are guarded in CMake.
