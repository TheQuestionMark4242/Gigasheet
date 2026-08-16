#include "app.hpp"

#ifdef __WXMSW__
#include <windows.h>
#endif

namespace {

#ifdef __WXMSW__
// Opt into DPI awareness so Windows renders the app natively on scaled displays
// instead of bitmap-stretching it (which blurs/pixelates text). Must run before
// any window is created. Prefers Per-Monitor-V2, falls back to system-DPI aware.
void EnableDpiAwareness() {
    if (HMODULE user32 = ::LoadLibraryW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto setCtx = reinterpret_cast<SetCtxFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        const bool ok = setCtx && setCtx(reinterpret_cast<HANDLE>(-4));
        if (!ok) {
            ::SetProcessDPIAware(); // fallback for older Windows
        }
        ::FreeLibrary(user32);
    }
}
#endif

} // namespace

bool SpreadsheetApp::OnInit() {
#ifdef __WXMSW__
    EnableDpiAwareness();
#endif
    // No argument: open an empty window (the user imports a CSV via "Open").
    // An argument is treated as a prepared dataset directory to load.
    wxString dir = (argc > 1) ? wxString(argv[1]) : wxString();
    SpreadsheetFrame* f = new SpreadsheetFrame(dir);
    f->Show(true);
    return true;
}
