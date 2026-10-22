// lilawer.dll: loaded by WerFault.exe for crashes that end Lila without running its own handlers (fail-fast, heap
// corruption). Lila registers it with WerRegisterRuntimeExceptionModule and a pointer to a WerBlock.

#include "core/CrashWatch.h"

#include <werapi.h>

using namespace lila::crash;

extern "C" __declspec(dllexport) HRESULT WINAPI OutOfProcessExceptionEventCallback(PVOID context,
                                                                                    const PWER_RUNTIME_EXCEPTION_INFORMATION info,
                                                                                    BOOL* ownershipClaimed, PWSTR, PDWORD, PDWORD)
{
    // Not claimed: Windows still files its own report and event log entry, which the watchdog reads too.
    *ownershipClaimed = FALSE;
    watch::WerBlock block;
    SIZE_T read = 0;
    if (!ReadProcessMemory(info->hProcess, context, &block, sizeof(block), &read) || read != sizeof(block) ||
        block.magic != watch::kWerMagic || block.size != sizeof(block) || block.handled)
        return S_OK;
    block.sessionsDir[std::size(block.sessionsDir) - 1] = 0;
    watch::writeWerCapture(info->hProcess, GetProcessId(info->hProcess), info->hThread, info->exceptionRecord, info->context,
                           fs::path(block.sessionsDir), block.fullDump != 0);
    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT WINAPI OutOfProcessExceptionEventSignatureCallback(PVOID, const PWER_RUNTIME_EXCEPTION_INFORMATION,
                                                                                             DWORD, PWSTR, PDWORD, PWSTR, PDWORD)
{
    return E_FAIL;
}

extern "C" __declspec(dllexport) HRESULT WINAPI OutOfProcessExceptionEventDebuggerLaunchCallback(PVOID,
                                                                                                  const PWER_RUNTIME_EXCEPTION_INFORMATION,
                                                                                                  BOOL* isCustomDebugger, PWSTR, PDWORD, BOOL*)
{
    *isCustomDebugger = FALSE;
    return S_OK;
}
