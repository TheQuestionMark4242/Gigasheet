#include "memory_usage.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
#elif defined(__linux__)
    #include <cstdio>
    #include <unistd.h>
#endif

std::size_t get_memory_usage_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;

    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            &pmc,
            sizeof(pmc)))
    {
        return pmc.WorkingSetSize;
    }

    return 0;
#elif defined(__linux__)
    // /proc/self/statm: size resident shared text lib data dt (in pages)
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }

    unsigned long size = 0, resident = 0;
    const int matched = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);

    if (matched != 2) {
        return 0;
    }

    return static_cast<std::size_t>(resident) *
           static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}

double get_memory_usage_MB() {
    return static_cast<double> (get_memory_usage_bytes())/(1024.0*1024.0);
}