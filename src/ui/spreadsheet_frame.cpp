#include <wx/wx.h>
#include <wx/grid.h>
#include <wx/artprov.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/progdlg.h>

#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/iconbndl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __WXMSW__
#include <dwmapi.h>
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#endif

#include <filesystem>

#include "mio/mmap.hpp"

#include "spreadsheet_frame.hpp"
#include "statistics_dialog.hpp"
#include "../util/sha256.hpp"
#include "formula_autocomplete.hpp"
#include "../storage/csv_importer.hpp"
#include "../storage/memory_usage.hpp"
#include "../model/mmapped_table.hpp"
namespace {

const int kStatisticsToolId = wxID_HIGHEST + 1;

// Active palette. ApplyTheme() pushes these onto the widgets; ApplyPreset()
// swaps in a named preset. Initialised to the default (Light) preset.
wxColour kChromeBg;     // nav bar / formula panel
wxColour kChromeText;   // text on nav bar / formula bar
wxColour kChromeHover;  // button background on hover
wxColour kInputBg;      // formula input field
wxColour kCellBg;       // spreadsheet canvas
wxColour kCellText;     // cell values
wxColour kGridLine;     // gridlines
wxColour kHeaderBg;     // grid row/column headers
wxColour kHeaderText;   // header labels
wxColour kSelBg;        // selection background
wxColour kCellHighlight;// current-cell outline

// A complete named colour scheme.
struct Palette {
    wxString name;
    wxColour navBar, navText, navHover, formulaBar, cellBg, cellText,
             gridLine, headerBg, headerText, selection, cellHighlight;
};

// Built-in appearance presets. The first entry is the shipped default.
const std::vector<Palette>& Presets() {
    static const std::vector<Palette> presets = {
        // Light (default) - VS Code "Light+" inspired.
        { "Light",
          wxColour(0xF3,0xF3,0xF3), wxColour(0x1F,0x1F,0x1F), wxColour(0xE0,0xE0,0xE0),
          wxColour(0xFF,0xFF,0xFF), wxColour(0xFF,0xFF,0xFF), wxColour(0x1F,0x1F,0x1F),
          wxColour(0xE5,0xE5,0xE5), wxColour(0xEC,0xEC,0xEC), wxColour(0x33,0x33,0x33),
          wxColour(0xAD,0xD6,0xFF), wxColour(0x00,0x7A,0xCC) },
        // Dark - VS Code "Dark+" inspired, full-black cells.
        { "Dark",
          wxColour(0x25,0x25,0x26), wxColour(0xED,0xED,0xED), wxColour(0x2A,0x2D,0x2E),
          wxColour(0x3C,0x3C,0x3C), wxColour(0x00,0x00,0x00), wxColour(0xFF,0xFF,0xFF),
          wxColour(0x33,0x33,0x33), wxColour(0x2D,0x2D,0x2D), wxColour(0xED,0xED,0xED),
          wxColour(0x26,0x4F,0x78), wxColour(0x00,0x7A,0xCC) },
        // Sauravized Dark - midnight navy.
        { "Sauravized Dark",
          wxColour(0x0C,0x12,0x25), wxColour(0xD6,0xD9,0xDD), wxColour(0x39,0x3D,0x46),
          wxColour(0x00,0x00,0x00), wxColour(0x08,0x08,0x08), wxColour(0xFD,0xFE,0xFF),
          wxColour(0x2E,0x33,0x3B), wxColour(0x03,0x1D,0x4B), wxColour(0xFD,0xFE,0xFE),
          wxColour(0x2E,0x3B,0x52), wxColour(0x0B,0x61,0xF5) },
    };
    return presets;
}

// Whether a preset's chrome is dark (drives the DWM dark title bar).
bool PaletteIsDark(const Palette& p) {
    return (p.navBar.Red() + p.navBar.Green() + p.navBar.Blue()) < 384; // <50% avg
}

// Copy a preset into the active palette globals.
void SetActivePalette(const Palette& p) {
    kChromeBg = p.navBar;         kChromeText = p.navText;
    kChromeHover = p.navHover;    kInputBg = p.formulaBar;
    kCellBg = p.cellBg;           kCellText = p.cellText;
    kGridLine = p.gridLine;       kHeaderBg = p.headerBg;
    kHeaderText = p.headerText;   kSelBg = p.selection;
    kCellHighlight = p.cellHighlight;
}

#ifdef __WXMSW__
// Ask DWM to paint this window's title bar light or dark (Win10 1809+/Win11).
void SetTitleBarDark(wxWindow* win, bool dark) {
    HWND hwnd = static_cast<HWND>(win->GetHWND());
    BOOL flag = dark ? TRUE : FALSE;
    // Attribute id is 20 on current builds, 19 on early Win10 20H1 and before.
    if (DwmSetWindowAttribute(hwnd, 20, &flag, sizeof(flag)) != S_OK) {
        DwmSetWindowAttribute(hwnd, 19, &flag, sizeof(flag));
    }
    // Nudge a non-client repaint so the change shows immediately.
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
#endif

wxString FindCsvImporterPath() {
    wxFileName exeName(wxStandardPaths::Get().GetExecutablePath());
    const wxString exeDir = exeName.GetPathWithSep();
    const wxString windowsCandidate = exeDir + "csv_importer.exe";
    if (wxFileExists(windowsCandidate)) {
        return windowsCandidate;
    }

    const wxString unixCandidate = exeDir + "csv_importer";
    if (wxFileExists(unixCandidate)) {
        return unixCandidate;
    }

    return {};
}

// Runs `work` on a background thread while a pulsing progress dialog keeps the
// GUI responsive, so heavy loads (large CSV imports, big datasets) show a
// loading screen instead of a frozen window. Any exception thrown by `work` is
// rethrown on the calling (GUI) thread once it finishes.
void RunWithLoadingScreen(wxWindow* parent, const wxString& title,
                          const wxString& message,
                          std::function<void()> work) {
    std::atomic<bool> done{false};
    std::exception_ptr error;

    std::thread worker([&] {
        try {
            work();
        } catch (...) {
            error = std::current_exception();
        }
        done.store(true);
    });

    wxProgressDialog dlg(title, message, /*maximum*/ 100, parent,
                         wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);
    while (!done.load()) {
        dlg.Pulse();
        wxMilliSleep(80);
        wxTheApp->Yield(true); // repaint the dialog, stay responsive
    }
    worker.join();

    if (error) {
        std::rethrow_exception(error);
    }
}

// Last path component of a table directory, used as the window title.
wxString DisplayNameForDir(const wxString& dir) {
    wxFileName fn(dir);
    wxString name = fn.GetFullName();
    if (name.IsEmpty()) {
        const wxArrayString dirs = fn.GetDirs();
        if (!dirs.IsEmpty()) name = dirs.Last();
    }
    return name.IsEmpty() ? dir : name;
}

// Base directory where converted datasets are stored: the user's app-data area
// (e.g. %APPDATA%\Gigasheet\datasets on Windows), created if missing. Keeping
// them here rather than next to the source CSV avoids cluttering the user's
// folders and works when the CSV lives on read-only or removable media.
wxString DatasetsBaseDir() {
    const wxString base =
        wxStandardPaths::Get().GetUserDataDir() +
        wxFileName::GetPathSeparator() + "datasets";
    std::error_code ec;
    std::filesystem::create_directories(base.ToStdString(), ec);
    return base;
}

wxString MakeOutputDirectoryPath(const wxString& csvPath) {
    wxFileName fileName(csvPath);
    wxString basePath = DatasetsBaseDir() + wxFileName::GetPathSeparator() +
                        fileName.GetName() + "_gigasheet";
    wxString candidate = basePath;
    int suffix = 1;
    while (wxDirExists(candidate)) {
        candidate = wxString::Format("%s_%d", basePath, suffix++);
    }
    return candidate;
}

// Human-readable byte count, e.g. "271.9 MB".
wxString HumanSize(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return (u == 0) ? wxString::Format("%llu %s", (unsigned long long)bytes, units[0])
                    : wxString::Format("%.1f %s", v, units[u]);
}

// Total size of a converted dataset directory (its column/*.bin files etc.).
std::uint64_t DirDataSize(const wxString& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir.ToStdString(), ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

// SHA-256 of a file's contents (memory-mapped). Empty string on failure.
std::string Sha256OfFile(const wxString& path) {
    try {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path.ToStdString(), ec);
        if (ec) return {};
        if (sz == 0) return sha256_hex("", 0);
        mio::mmap_source mm(path.ToStdString());
        if (!mm.is_open()) return {};
        return sha256_hex(mm.data(), mm.size());
    } catch (...) {
        return {};
    }
}

// A converted dataset records its source in "source.txt":
//   line 1: original file name (for the title bar)
//   line 2: sha256 (to detect an already-converted source)
//   line 3: absolute path of the original CSV (for writing edits back).
// The dataset now lives under AppData rather than next to the CSV, so the full
// source path must be recorded explicitly - it can't be derived from the dir.
void WriteSourceInfo(const wxString& dir, const wxString& originalName,
                     const std::string& sha, const wxString& originalPath) {
    std::ofstream f((std::filesystem::path(dir.ToStdString()) / "source.txt"));
    if (f) {
        f << originalName.ToStdString() << "\n"
          << sha << "\n"
          << originalPath.ToStdString() << "\n";
    }
}

struct SourceInfo { wxString name; std::string sha; wxString path; bool ok = false; };

SourceInfo ReadSourceInfo(const wxString& dir) {
    SourceInfo info;
    std::ifstream f((std::filesystem::path(dir.ToStdString()) / "source.txt"));
    if (!f) return info;
    std::string name, sha, path;
    if (std::getline(f, name)) {
        info.name = wxString::FromUTF8(name);
        if (std::getline(f, sha)) info.sha = sha;
        if (std::getline(f, path)) info.path = wxString::FromUTF8(path);
        info.ok = true;
    }
    return info;
}

// Absolute path of the original CSV a dataset was imported from, or "" if
// unknown. Prefers the recorded path (line 3); for older datasets that predate
// it, falls back to the dataset's own parent + recorded name.
wxString OriginalCsvPathForDir(const wxString& dir) {
    const SourceInfo info = ReadSourceInfo(dir);
    if (info.ok && !info.path.IsEmpty()) return info.path;
    if (info.ok && !info.name.IsEmpty()) {
        wxFileName fn(dir, "");
        fn.RemoveLastDir(); // dataset dir -> its parent
        return fn.GetPath() + wxFileName::GetPathSeparator() + info.name;
    }
    return {};
}

// Display name for the title bar: prefer the original converted file name
// (from source.txt), falling back to the directory's own name.
wxString OriginalNameForDir(const wxString& dir) {
    const SourceInfo info = ReadSourceInfo(dir);
    if (info.ok && !info.name.IsEmpty()) return info.name;
    return DisplayNameForDir(dir);
}

// Look for an existing converted directory whose recorded source hash matches
// `sha` (i.e. this exact file was already imported). Returns "" if none.
wxString FindConvertedDir(const wxString& csvPath, const std::string& sha) {
    if (sha.empty()) return {};
    wxFileName fn(csvPath);
    const std::string base = (fn.GetName() + "_gigasheet").ToStdString();
    const std::string parent = DatasetsBaseDir().ToStdString();
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(parent, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = it->path().filename().string();
        if (name.rfind(base, 0) != 0) continue; // must start with "<name>_gigasheet"
        const wxString cand(it->path().string());
        if (!wxFileExists(cand + "/metadata.bin")) continue;
        const SourceInfo info = ReadSourceInfo(cand);
        if (info.ok && info.sha == sha) return cand;
    }
    return {};
}

}

SpreadsheetFrame::SpreadsheetFrame(const wxString& dir)
    : wxFrame(nullptr, wxID_ANY, "Gigasheet", wxDefaultPosition, wxSize(1000,700)) {
    // A dataset directory must contain metadata.bin. When it doesn't (no
    // argument given, or a bad path), open to an empty window instead of
    // crashing; the user can then import a CSV with "Open".
    const bool hasDataset =
        !dir.IsEmpty() && wxFileExists(dir + "/metadata.bin");

    sourceName = hasDataset ? OriginalNameForDir(dir) : wxString("Untitled");
    if (hasDataset) {
        datasetDir = dir;
        originalCsvPath = OriginalCsvPathForDir(dir);
    }
    SetBackgroundColour(kChromeBg);
    CreateStatusBar();
    SetStatusText(hasDataset ? "Loading..." : "No dataset open. Use Open to import a CSV.");

    Bind(wxEVT_CLOSE_WINDOW, &SpreadsheetFrame::OnClose, this);

    if (hasDataset) {
        // Load the (possibly large) dataset behind a loading screen. mmap itself
        // is lazy, but reading metadata and the chunk stats index can take a
        // moment on big tables. A load failure falls back to an empty table.
        try {
            RunWithLoadingScreen(
                this, "Loading",
                wxString::Format("Loading %s (%s)", sourceName, HumanSize(DirDataSize(dir))),
                [this, &dir] { table = new MmappedTable(dir.ToStdString()); });
        } catch (const std::exception& ex) {
            wxMessageBox(wxString("Failed to open dataset:\n") + ex.what(),
                         "Open Failed", wxICON_ERROR, this);
            table = nullptr;
        }
    }
    if (!table) {
        table = new MmappedTable(); // empty: window opens, no data shown
    }

    // Dark chrome: the action row + formula bar sit on one dark panel; the grid
    // canvas stays light. Regions read by tone, not by borders.
    topBar = new wxPanel(this);
    topBar->SetBackgroundColour(kChromeBg);
    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    // Flat, dark "buttons" built from static text so we fully control colour and
    // hover (native wxButton ignores background colour on Windows). Each is kept
    // in navButtons so ApplyTheme() can recolour it live.
    auto navButton = [&](const wxString& label, std::function<void()> onClick) {
        auto* b = new wxStaticText(topBar, wxID_ANY, "  " + label + "  ",
            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE);
        b->SetForegroundColour(kChromeText);
        b->SetBackgroundColour(kChromeBg);
        b->SetMinSize(wxSize(-1, FromDIP(24)));
        b->SetCursor(wxCursor(wxCURSOR_HAND));
        b->Bind(wxEVT_ENTER_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(kChromeHover); b->Refresh(); e.Skip(); });
        b->Bind(wxEVT_LEAVE_WINDOW, [b](wxMouseEvent& e) {
            b->SetBackgroundColour(kChromeBg); b->Refresh(); e.Skip(); });
        b->Bind(wxEVT_LEFT_DOWN, [onClick](wxMouseEvent& e) {
            onClick(); e.Skip(); });
        navButtons.push_back(b);
        return b;
    };

    wxBoxSizer* actions = new wxBoxSizer(wxHORIZONTAL);
    actions->Add(navButton("Open",       [this]{ wxCommandEvent e; OnOpenCsv(e); }),    0, wxRIGHT, 4);
    actions->Add(navButton("Save",       [this]{ wxCommandEvent e; OnSave(e); }),       0, wxRIGHT, 4);
    actions->Add(navButton("Add Column", [this]{ wxCommandEvent e; OnAddColumn(e); }),  0, wxRIGHT, 4);
    actions->Add(navButton("Statistics", [this]{ wxCommandEvent e; OnStatistics(e); }), 0, wxRIGHT, 4);
    filterButton = navButton("Enable Filtering", [this]{ OnToggleFiltering(); });
    actions->Add(filterButton, 0, wxRIGHT, 4);
    actions->Add(navButton("Appearance", [this]{ OnAppearance(); }), 0, wxRIGHT, 4);
    topSizer->Add(actions, 0, wxLEFT | wxTOP, 8);

    // formula bar: cell address + editable value field
    cellRefLabel = new wxStaticText(topBar, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxSize(90, -1),
        wxALIGN_CENTRE_VERTICAL | wxST_NO_AUTORESIZE);
    cellRefLabel->SetForegroundColour(kChromeText);
    cellRefLabel->SetBackgroundColour(kChromeBg);
    formulaBar = new wxTextCtrl(topBar, wxID_ANY, wxEmptyString,
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    formulaBar->SetBackgroundColour(kInputBg);
    formulaBar->SetForegroundColour(kChromeText);
    wxBoxSizer* formulaSizer = new wxBoxSizer(wxHORIZONTAL);
    formulaSizer->Add(cellRefLabel, 0, wxALIGN_CENTRE_VERTICAL | wxRIGHT, 8);
    formulaSizer->Add(formulaBar, 1, wxALIGN_CENTRE_VERTICAL);
    topSizer->Add(formulaSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 10);
    topBar->SetSizer(topSizer);

    // grid
    grid = new wxGrid(this, wxID_ANY);
    grid->SetTable(table, true, wxGrid::wxGridSelectCells);
    grid->EnableEditing(true);
    StyleGrid();

    grid->Bind(wxEVT_GRID_SELECT_CELL, &SpreadsheetFrame::OnGridSelectCell, this);
    grid->Bind(wxEVT_GRID_LABEL_LEFT_CLICK, &SpreadsheetFrame::OnColumnLabelClick, this);
    // Inline cell edits: re-evaluate active filters against the new value so
    // the row drops out / stays as appropriate, then repaint.
    grid->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& e) {
        RecomputeFiltersAfterEdit(e.GetCol());
        e.Skip();
    });
    formulaBar->Bind(wxEVT_TEXT_ENTER, &SpreadsheetFrame::OnFormulaEnter, this);

    UpdateStatus();
    if (table->GetNumberRows() > 0 && table->GetNumberCols() > 0) {
        ShowCellInFormulaBar(0, 0);
    }
    memoryTimer.SetOwner(this);

    Bind(
        wxEVT_TIMER,
        &SpreadsheetFrame::OnTimer,
        this);

    memoryTimer.Start(1000); // every second

    wxBoxSizer* s = new wxBoxSizer(wxVERTICAL);
    s->Add(topBar, 0, wxEXPAND);
    s->Add(grid, 1, wxEXPAND);
    SetSizer(s);

    // Appearance: pick a built-in preset. The last choice is remembered in a
    // small file next to the exe; default to the first preset (Light).
    wxFileName exeName(wxStandardPaths::Get().GetExecutablePath());
    prefPath = exeName.GetPathWithSep() + "appearance.txt";
    int startIndex = 0;
    {
        std::ifstream pf(prefPath.ToStdString());
        std::string saved;
        if (pf && std::getline(pf, saved)) {
            const auto& presets = Presets();
            for (size_t i = 0; i < presets.size(); ++i) {
                if (presets[i].name == wxString(saved)) { startIndex = (int)i; break; }
            }
        }
    }
    ApplyPreset(startIndex);

    // Size columns to their header + first rows so content isn't clipped.
    AutoSizeColumns(10);

    // Window / taskbar icon: appicon.png (GitHub avatar) shipped next to the exe.
    if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG)) {
        wxImage::AddHandler(new wxPNGHandler());
    }
    const wxString iconPath = exeName.GetPathWithSep() + "appicon.png";
    if (wxFileExists(iconPath)) {
        wxImage img(iconPath, wxBITMAP_TYPE_PNG);
        if (img.IsOk()) {
            wxIconBundle icons;
            for (int px : {16, 24, 32, 48, 64, 256}) {
                wxBitmap bmp(img.Scale(px, px, wxIMAGE_QUALITY_HIGH));
                wxIcon ic;
                ic.CopyFromBitmap(bmp);
                icons.AddIcon(ic);
            }
            SetIcons(icons);
        }
    }
}

// Flat, line-light grid styling: near-white gridlines, airy rows, and quiet
// left-aligned headers instead of the default heavy 3D look.
void SpreadsheetFrame::StyleGrid() {
    // Black canvas, light values, softened gridlines.
    grid->SetDefaultCellBackgroundColour(kCellBg);
    grid->SetDefaultCellTextColour(kCellText);
    grid->SetGridLineColour(kGridLine);
    grid->GetGridWindow()->SetBackgroundColour(kCellBg); // area past the data

    grid->SetDefaultRowSize(grid->FromDIP(24), true);
    grid->SetColLabelSize(grid->FromDIP(28));
    grid->SetRowLabelSize(grid->FromDIP(52));

    // Dark, flat headers (row numbers + column labels) with muted, low-contrast
    // text so the header/gridline seams read soft rather than stark white.
    grid->SetLabelBackgroundColour(kHeaderBg);
    grid->SetLabelTextColour(kHeaderText);
    wxFont labelFont = grid->GetLabelFont();
    labelFont.SetWeight(wxFONTWEIGHT_NORMAL);
    grid->SetLabelFont(labelFont);
    grid->SetColLabelAlignment(wxALIGN_CENTRE, wxALIGN_CENTRE);

    // Soft selection + a thin current-cell outline (not the fat default box).
    grid->SetSelectionBackground(kSelBg);
    grid->SetSelectionForeground(kCellText);
    grid->SetCellHighlightColour(kCellHighlight);
    grid->SetCellHighlightPenWidth(1);

    grid->DisableDragRowSize();
}

// Push the (possibly just-reloaded) palette globals onto every widget.
void SpreadsheetFrame::ApplyTheme() {
    SetBackgroundColour(kChromeBg);

    topBar->SetBackgroundColour(kChromeBg);
    for (wxStaticText* b : navButtons) {
        b->SetForegroundColour(kChromeText);
        b->SetBackgroundColour(kChromeBg);
        b->Refresh();
    }
    cellRefLabel->SetForegroundColour(kChromeText);
    cellRefLabel->SetBackgroundColour(kChromeBg);
    formulaBar->SetBackgroundColour(kInputBg);
    formulaBar->SetForegroundColour(kChromeText);

    StyleGrid();

    topBar->Refresh();
    formulaBar->Refresh();
    grid->ForceRefresh();
}

void SpreadsheetFrame::ApplyPreset(int index) {
    const auto& presets = Presets();
    if (index < 0 || index >= (int)presets.size()) index = 0;
    const Palette& p = presets[index];

    SetActivePalette(p);
    currentThemeName = p.name;
    ApplyTheme();

#ifdef __WXMSW__
    SetTitleBarDark(this, PaletteIsDark(p));
#endif

    // Remember the choice for next launch.
    std::ofstream pf(prefPath.ToStdString(), std::ios::trunc);
    if (pf) pf << p.name.ToStdString() << "\n";

    SetStatusText("Appearance: " + p.name);
}

void SpreadsheetFrame::OnAppearance() {
    const auto& presets = Presets();
    wxMenu menu;
    const int base = wxID_HIGHEST + 100;
    for (size_t i = 0; i < presets.size(); ++i) {
        wxMenuItem* item = menu.AppendRadioItem(base + (int)i, presets[i].name);
        if (presets[i].name == currentThemeName) item->Check(true);
    }
    menu.Bind(wxEVT_MENU, [this, base](wxCommandEvent& e) {
        ApplyPreset(e.GetId() - base);
    });
    PopupMenu(&menu);
}

void SpreadsheetFrame::AutoSizeColumns(int sampleRows) {
    if (!grid || !table) return;
    const int cols = table->GetNumberCols();
    const int rows = std::min(sampleRows, table->GetNumberRows());
    const int padding = grid->FromDIP(18); // left+right cell padding

    wxClientDC dc(grid);
    const wxFont cellFont  = grid->GetDefaultCellFont();
    const wxFont labelFont = grid->GetLabelFont();

    for (int c = 0; c < cols; ++c) {
        dc.SetFont(labelFont);
        int width = dc.GetTextExtent(table->GetColLabelValue(c)).GetWidth();

        dc.SetFont(cellFont);
        for (int r = 0; r < rows; ++r) {
            const int w = dc.GetTextExtent(table->GetValue(r, c)).GetWidth();
            if (w > width) width = w;
        }
        grid->SetColSize(c, width + padding);
    }
}

void SpreadsheetFrame::ShowCellInFormulaBar(int row, int col) {
    if (!table || row < 0 || col < 0) return;
    cellRefLabel->SetLabel(
        wxString::Format("%s:%d", table->ColumnLabel(col), row + 1));
    formulaBar->ChangeValue(table->GetValue(row, col));
}

void SpreadsheetFrame::OnGridSelectCell(wxGridEvent& event) {
    ShowCellInFormulaBar(event.GetRow(), event.GetCol());
    event.Skip();
}

void SpreadsheetFrame::OnFormulaEnter(wxCommandEvent&) {
    const int row = grid->GetGridCursorRow();
    const int col = grid->GetGridCursorCol();
    if (row >= 0 && col >= 0) {
        grid->SetCellValue(row, col, formulaBar->GetValue());
        grid->ForceRefresh();
        RecomputeFiltersAfterEdit(col);
        UpdateStatus();
    }
    grid->SetFocus();
}

void SpreadsheetFrame::OnTimer(wxTimerEvent&) {
    UpdateStatus();
}

void SpreadsheetFrame::UpdateStatus() {
    wxString rowsText;
    if (table->IsFiltered()) {
        rowsText = wxString::Format("Showing %d of %d rows",
                                    table->VisibleRows(), table->TotalRows());
    } else {
        rowsText = wxString::Format("Rows: %d", table->TotalRows());
    }
    wxString text = wxString::Format(
        "%s | Columns: %d | RAM: %.1f MB",
        rowsText,
        table->GetNumberCols(),
        get_memory_usage_MB());

    const size_t unsaved = table->UnsavedCellCount();
    if (unsaved > 0) {
        text += wxString::Format(" | Unsaved edits: %zu", unsaved);
    }
    SetStatusText(text);
    // Build the title without a narrow multibyte literal (the em dash was being
    // mangled by the local charset); decode the separator as explicit UTF-8.
    wxString title = sourceName;
    if (unsaved > 0) title += " *";
    title += wxString::FromUTF8(" \xE2\x80\x94 Gigasheet"); // " — Gigasheet"
    SetTitle(title);
}

void SpreadsheetFrame::OnOpenCsv(wxCommandEvent&) {
    wxFileDialog dlg(
        this,
        "Open CSV",
        wxEmptyString,
        wxEmptyString,
        "CSV files (*.csv)|*.csv|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) {
        return;
    }

    const wxString csvPath = dlg.GetPath();
    const wxString csvName = wxFileName(csvPath).GetFullName();
    std::uint64_t csvBytes = 0;
    { std::error_code ec; csvBytes = std::filesystem::file_size(csvPath.ToStdString(), ec); }

    try {
        // Start importing speculatively *while* we fingerprint the file, so a
        // first-time open overlaps the import with the hash instead of running
        // them back to back. If the hash then proves the file was already
        // converted, we cancel the import and delete its partial output.
        const wxString importDir = MakeOutputDirectoryPath(csvPath);
        std::atomic<bool> cancelImport{false};
        std::exception_ptr importError;
        std::thread importThread([&] {
            try {
                ImportCsv(csvPath.ToStdString(), importDir.ToStdString(),
                          &cancelImport);
            } catch (const ImportCancelled&) {
                // Expected: the file turned out to be already converted.
            } catch (...) {
                importError = std::current_exception();
            }
        });
        // Ensure the worker is always stopped and joined, even if something
        // below throws, so the std::thread never destructs while joinable.
        struct Joiner {
            std::thread& t; std::atomic<bool>& cancel;
            ~Joiner() { if (t.joinable()) { cancel.store(true); t.join(); } }
        } joiner{importThread, cancelImport};

        std::string sha;
        RunWithLoadingScreen(
            this, "Opening",
            wxString::Format("Checking %s (%s)", csvName, HumanSize(csvBytes)),
            [&] { sha = Sha256OfFile(csvPath); });

        const wxString existing = FindConvertedDir(csvPath, sha);

        wxString outputDir;
        if (!existing.IsEmpty()) {
            // Already converted: stop the speculative import and discard it.
            cancelImport.store(true);
            RunWithLoadingScreen(
                this, "Opening",
                wxString::Format("Opening %s (%s)", csvName, HumanSize(csvBytes)),
                [&] {
                    importThread.join();
                    std::error_code ec;
                    std::filesystem::remove_all(importDir.ToStdString(), ec);
                });
            outputDir = existing;
            SetStatusText("Already converted - opening " + outputDir);
        } else {
            // Genuinely new file: wait for the (already running) import.
            RunWithLoadingScreen(
                this, "Importing CSV",
                wxString::Format("Importing %s (%s)", csvName, HumanSize(csvBytes)),
                [&] { importThread.join(); });
            if (importError) {
                std::error_code ec;
                std::filesystem::remove_all(importDir.ToStdString(), ec);
                std::rethrow_exception(importError);
            }
            WriteSourceInfo(importDir, csvName, sha, csvPath);
            outputDir = importDir;
        }

        auto* frame = new SpreadsheetFrame(outputDir);
        frame->Show(true);
        frame->Raise();

        // If this window had no file open (an empty launcher window), replace
        // it with the newly loaded one instead of leaving an empty window
        // behind. A window that already has a file loaded is kept, so opening
        // another file gives you a second window.
        if (datasetDir.IsEmpty()) {
            Close();
        }
    }
    catch (const std::exception& ex) {
        wxMessageBox(
            ex.what(),
            "CSV Import Failed",
            wxICON_ERROR | wxOK,
            this);
    }
}

void SpreadsheetFrame::OnSave(wxCommandEvent&) {
    DoSave();
}

bool SpreadsheetFrame::DoSave() {
    if (!table->HasUnsavedChanges()) {
        SetStatusText("No unsaved changes.");
        return true;
    }
    // Commit any in-progress cell editor so its value is saved too.
    grid->SaveEditControlValue();

    const size_t count = table->UnsavedCellCount();
    wxBusyCursor busy;
    std::string error;
    if (!table->SaveOverrides(error)) {
        wxMessageBox(error, "Save Failed", wxICON_ERROR, this);
        UpdateStatus();
        return false;
    }

    // Mirror the edits back to the original CSV so the source file stays in
    // sync with our internal format. This rewrites the whole file, so it runs on
    // a background thread behind a loading screen (keeping the UI responsive on
    // large tables). Failure here isn't fatal - the edits are already committed
    // to the dataset - but we tell the user. The new file's hash is computed in
    // the same write pass, so we don't re-read the file to refresh source.txt.
    bool csvWritten = false;
    if (!originalCsvPath.IsEmpty()) {
        std::string csvError;
        std::string newSha;
        RunWithLoadingScreen(
            this, "Saving",
            wxString::Format("Writing %s", wxFileName(originalCsvPath).GetFullName()),
            [&] {
                csvWritten = table->ExportBaseColumnsToCsv(
                    originalCsvPath.ToStdString(), csvError, &newSha);
            });
        if (!csvWritten) {
            wxMessageBox(
                "Cell edits were saved to the dataset, but writing them back to "
                "the original CSV failed:\n" + csvError,
                "CSV Write-Back Failed", wxICON_WARNING, this);
        } else if (!datasetDir.IsEmpty() && !newSha.empty()) {
            // The CSV's contents (and hash) just changed. Refresh the recorded
            // hash so reopening the edited file reuses this dataset instead of
            // re-importing it.
            WriteSourceInfo(datasetDir, sourceName, newSha, originalCsvPath);
        }
    }

    UpdateStatus();
    if (csvWritten) {
        SetStatusText(wxString::Format(
            "Saved %zu cell edit(s) (dataset + %s).", count,
            wxFileName(originalCsvPath).GetFullName()));
    } else {
        SetStatusText(wxString::Format("Saved %zu cell edit(s).", count));
    }
    return true;
}

void SpreadsheetFrame::OnClose(wxCloseEvent& event) {
    if (table && table->HasUnsavedChanges() && event.CanVeto()) {
        const int rc = wxMessageBox(
            "There are unsaved cell edits. Save them before closing?",
            "Unsaved Changes",
            wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (rc == wxCANCEL || (rc == wxYES && !DoSave())) {
            event.Veto();
            return;
        }
    }
    event.Skip();
}

void SpreadsheetFrame::OnStatistics(wxCommandEvent&) {
    if (table->GetNumberCols() == 0) {
        SetStatusText("Open a dataset first (Open) before computing statistics.");
        return;
    }
    StatisticsDialog dlg(this, table, grid->GetGridCursorCol());
    dlg.ShowModal();
}

void SpreadsheetFrame::OnToggleFiltering() {
    if (table->GetNumberCols() == 0 && !table->FilteringEnabled()) {
        SetStatusText("Open a dataset first (Open) before filtering.");
        return;
    }
    const bool enable = !table->FilteringEnabled();
    table->SetFilteringEnabled(enable);

    if (!enable) {
        // Leaving filter mode: drop all predicates and show the full table.
        wxBusyCursor busy;
        table->ClearFilters(grid);
    }

    if (filterButton) {
        filterButton->SetLabel(
            enable ? "  Disable Filtering  " : "  Enable Filtering  ");
        topBar->Layout();
    }

    RefreshFilterMarkers();
    UpdateStatus();
    grid->ForceRefresh();

    if (enable) {
        SetStatusText("Filtering on: click a column header to filter it.");
    }
}

void SpreadsheetFrame::RefreshFilterMarkers() {
    // Column labels are pulled from the table (which appends the ▼/▽ marker) on
    // each paint, so repainting the header window is enough to show/hide them.
    if (!grid) return;
    grid->GetGridColLabelWindow()->Refresh();
}

void SpreadsheetFrame::OnColumnLabelClick(wxGridEvent& event) {
    if (!table->FilteringEnabled()) { event.Skip(); return; }
    const int col = event.GetCol();
    if (col < 0) { event.Skip(); return; } // row-label corner / row labels
    ShowColumnFilterPopup(col);
    // handled: don't let the grid start a column selection/sort
}

void SpreadsheetFrame::RecomputeFiltersAfterEdit(int col) {
    if (!table->IsFiltered()) return;
    wxBusyCursor busy;
    table->RecomputeFiltersAfterEdit(col, grid);
    UpdateStatus();
}

// A small dropdown-style dialog to set one column's predicate. Numeric columns
// get a Min/Max range; text columns get a contains/equals match. The dialog is
// positioned just under the clicked column header so it reads like a dropdown.
void SpreadsheetFrame::ShowColumnFilterPopup(int col) {
    const bool isText = table->GetColumn(col).type == ColumnType::CHARBUF;
    const ColumnFilter* existing = table->ColumnFilterFor(col);

    wxDialog dlg(this, wxID_ANY,
                 "Filter: " + table->GetColumn(col).label,
                 wxDefaultPosition, wxDefaultSize,
                 wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER);

    auto* form = new wxFlexGridSizer(2, wxSize(8, 6));
    form->AddGrowableCol(1);

    wxTextCtrl* loCtrl = nullptr;
    wxTextCtrl* hiCtrl = nullptr;
    wxTextCtrl* textCtrl = nullptr;
    wxChoice*   matchChoice = nullptr;

    if (isText) {
        wxArrayString kinds;
        kinds.Add("contains");
        kinds.Add("equals");
        matchChoice = new wxChoice(&dlg, wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, kinds);
        matchChoice->SetSelection(
            existing && existing->kind == ColumnFilter::Kind::Equals ? 1 : 0);
        textCtrl = new wxTextCtrl(&dlg, wxID_ANY,
            existing ? wxString::FromUTF8(existing->text) : wxString(),
            wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);

        form->Add(new wxStaticText(&dlg, wxID_ANY, "Match:"), 0, wxALIGN_CENTER_VERTICAL);
        form->Add(matchChoice, 1, wxEXPAND);
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Value:"), 0, wxALIGN_CENTER_VERTICAL);
        form->Add(textCtrl, 1, wxEXPAND);
    } else {
        auto fmt = [](double v) -> wxString {
            if (v == -std::numeric_limits<double>::infinity() ||
                v ==  std::numeric_limits<double>::infinity()) return wxString();
            return wxString::Format("%g", v);
        };
        loCtrl = new wxTextCtrl(&dlg, wxID_ANY,
            existing ? fmt(existing->lo) : wxString(),
            wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        hiCtrl = new wxTextCtrl(&dlg, wxID_ANY,
            existing ? fmt(existing->hi) : wxString(),
            wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);

        form->Add(new wxStaticText(&dlg, wxID_ANY, "Min:"), 0, wxALIGN_CENTER_VERTICAL);
        form->Add(loCtrl, 1, wxEXPAND);
        form->Add(new wxStaticText(&dlg, wxID_ANY, "Max:"), 0, wxALIGN_CENTER_VERTICAL);
        form->Add(hiCtrl, 1, wxEXPAND);
    }

    auto* applyBtn  = new wxButton(&dlg, wxID_OK, "Apply");
    auto* removeBtn = new wxButton(&dlg, wxID_ANY, "Remove");
    auto* clearBtn  = new wxButton(&dlg, wxID_ANY, "Clear All");
    auto* cancelBtn = new wxButton(&dlg, wxID_CANCEL, "Cancel");
    removeBtn->Enable(existing != nullptr);
    clearBtn->Enable(table->ActiveFilterCount() > 0);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->Add(applyBtn, 0, wxRIGHT, 6);
    btns->Add(removeBtn, 0, wxRIGHT, 6);
    btns->Add(clearBtn, 0, wxRIGHT, 6);
    btns->AddStretchSpacer();
    btns->Add(cancelBtn, 0);

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(new wxStaticText(&dlg, wxID_ANY,
        isText ? "Show rows where this column matches:"
               : "Show rows where this column is in range\n(leave a box empty for no bound):"),
        0, wxALL, 10);
    top->Add(form, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
    top->Add(btns, 0, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(top);
    dlg.SetMinSize(wxSize(320, dlg.GetMinSize().y));

    // Position the dialog just below the clicked column header, like a dropdown.
    {
        int colLeft = 0; // logical x of the column within the grid content
        for (int c = 0; c < col; ++c) colLeft += grid->GetColSize(c);
        const int scrolledX = grid->CalcScrolledPosition(
            wxPoint(colLeft, 0)).x;
        const wxPoint anchor = grid->GetGridColLabelWindow()->ClientToScreen(
            wxPoint(scrolledX, grid->GetColLabelSize()));
        dlg.Move(anchor);
    }

    removeBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_REMOVE); });
    clearBtn->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { dlg.EndModal(wxID_CLEAR); });
    if (textCtrl) textCtrl->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent&) { dlg.EndModal(wxID_OK); });
    if (loCtrl)   loCtrl->Bind(wxEVT_TEXT_ENTER,   [&](wxCommandEvent&) { dlg.EndModal(wxID_OK); });
    if (hiCtrl)   hiCtrl->Bind(wxEVT_TEXT_ENTER,   [&](wxCommandEvent&) { dlg.EndModal(wxID_OK); });

    const int rc = dlg.ShowModal();

    if (rc == wxID_CLEAR) {
        wxBusyCursor busy;
        table->ClearFilters(grid);
    } else if (rc == wxID_REMOVE) {
        wxBusyCursor busy;
        table->ClearColumnFilter(col, grid);
    } else if (rc == wxID_OK) {
        ColumnFilter f;
        f.col = col;
        if (isText) {
            f.kind = matchChoice->GetSelection() == 1
                         ? ColumnFilter::Kind::Equals
                         : ColumnFilter::Kind::Contains;
            f.text = textCtrl->GetValue().utf8_string();
        } else {
            const wxString loS = loCtrl->GetValue().Strip(wxString::both);
            const wxString hiS = hiCtrl->GetValue().Strip(wxString::both);
            double lo = -std::numeric_limits<double>::infinity();
            double hi =  std::numeric_limits<double>::infinity();
            if (!loS.IsEmpty() && !loS.ToDouble(&lo)) {
                wxMessageBox("Min is not a valid number.", "Invalid filter",
                             wxICON_ERROR, this);
                return;
            }
            if (!hiS.IsEmpty() && !hiS.ToDouble(&hi)) {
                wxMessageBox("Max is not a valid number.", "Invalid filter",
                             wxICON_ERROR, this);
                return;
            }
            f.kind = ColumnFilter::Kind::Range;
            f.lo = lo;
            f.hi = hi;
        }
        wxBusyCursor busy;
        table->SetColumnFilter(f, grid);
    } else {
        return; // cancelled
    }

    RefreshFilterMarkers();
    UpdateStatus();
}

void SpreadsheetFrame::OnAddColumn(wxCommandEvent&) {
    if (table->GetNumberCols() == 0) {
        SetStatusText("Open a dataset first (Open) before adding a derived column.");
        return;
    }
    wxDialog dlg(this, wxID_ANY, "Add Derived Column",
        wxDefaultPosition, wxDefaultSize,
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto* formulaCtrl = new wxTextCtrl(&dlg, wxID_ANY, "=",
        wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    auto* hint = new wxStaticText(&dlg, wxID_ANY,
        "e.g. =A + B*2 or =CONCATENATE(Name, ' - ', A)");

    wxArrayString labels;
    for (int i = 0; i < table->GetNumberCols(); ++i) {
        labels.Add(table->ColumnLabel(i));
    }
    FormulaAutocomplete autocomplete(
        formulaCtrl, formulahint::BuildFormulaCandidates(labels));
    // Enter confirms the dialog once the suggestion popup is closed.
    formulaCtrl->Bind(wxEVT_TEXT_ENTER,
        [&dlg](wxCommandEvent&) { dlg.EndModal(wxID_OK); });

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(new wxStaticText(&dlg, wxID_ANY, "Expression:"), 0,
        wxLEFT | wxRIGHT | wxTOP, 10);
    top->Add(formulaCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
    top->Add(hint, 0, wxEXPAND | wxALL, 10);
    top->Add(dlg.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0,
        wxEXPAND | wxALL, 6);
    dlg.SetSizerAndFit(top);
    dlg.SetMinSize(wxSize(480, dlg.GetMinSize().y));
    dlg.SetSize(wxSize(480, -1));
    formulaCtrl->SetFocus();
    formulaCtrl->SetInsertionPointEnd();

    if (dlg.ShowModal() == wxID_OK) {
        wxString expr = formulaCtrl->GetValue();
        table->AddDerivedColumn(expr, grid);
    }
}
