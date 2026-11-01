#include "core/Profiler.h"

#include "core/Profile.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

#include <vector>

namespace lila::profiler
{
namespace
{
// Tracy listens on the first free port of these; 0.14 has no call that says which.
constexpr uint16_t kFirstPort = 8086;
constexpr uint16_t kPortCount = 20;
} // namespace

bool available()
{
#ifdef LILA_TRACY
    return true;
#else
    return false;
#endif
}

uint16_t port()
{
#if defined(LILA_TRACY) && defined(_WIN32)
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return 0;
    std::vector<char> buffer(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return 0;
    const DWORD pid = GetCurrentProcessId();
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
    {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        const uint16_t p = ntohs(uint16_t(row.dwLocalPort));
        if (row.dwOwningPid == pid && row.dwLocalAddr == htonl(INADDR_LOOPBACK) && p >= kFirstPort && p < kFirstPort + kPortCount)
            return p;
    }
    return 0;
#elif defined(LILA_TRACY)
    return kFirstPort;
#else
    return 0;
#endif
}

bool connected()
{
#ifdef LILA_TRACY
    return TracyIsConnected;
#else
    return false;
#endif
}

bool elevated()
{
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
#else
    return false;
#endif
}

bool beginSystemTracing()
{
#ifdef LILA_TRACY
    return elevated() && tracy::BeginSamplingProfiling();
#else
    return false;
#endif
}

void endSystemTracing()
{
#ifdef LILA_TRACY
    tracy::EndSamplingProfiling();
#endif
}

void setAppInfo(std::string_view text)
{
#ifdef LILA_TRACY
    TracyAppInfo(text.data(), text.size());
#else
    (void)text;
#endif
}
} // namespace lila::profiler
