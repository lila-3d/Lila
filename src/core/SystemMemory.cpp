#include "core/SystemMemory.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lila
{
size_t physicalMemoryBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX mem{sizeof(mem)};
    return GlobalMemoryStatusEx(&mem) ? size_t(mem.ullTotalPhys) : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? size_t(pages) * size_t(size) : 0;
#endif
}
} // namespace lila
