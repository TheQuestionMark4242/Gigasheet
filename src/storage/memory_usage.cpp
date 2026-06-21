#include "memory_usage.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
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
#else
    return 0;
#endif
}

double get_memory_usage_MB() {
    return static_cast<double> (get_memory_usage_bytes())/(1024.0*1024.0);
}