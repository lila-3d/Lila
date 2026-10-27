#include "app/Host.h"
#include "core/CrashHandler.h"
#include "core/Paths.h"
#include "core/Version.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace lila;

namespace
{
/**
 * The capture commands render through the UI library, which lilabatch does not link: load it when it sits next to
 * the exe, and run without those commands when it does not.
 */
void loadUiCommands()
{
#ifdef _WIN32
    const std::filesystem::path dll = paths::executableDir() / "LilaUI.dll";
    HMODULE ui = LoadLibraryW(dll.c_str());
    if (!ui)
    {
        std::fprintf(stderr, "lilabatch: %s did not load (error %lu): captureView, captureTurntable and the other UI commands are "
                             "unavailable\n", dll.string().c_str(), GetLastError());
        return;
    }
    using RegisterFn = void (*)();
    if (auto fn = reinterpret_cast<RegisterFn>(GetProcAddress(ui, "lilaUiRegisterBatchCommands")))
        fn();
#endif
}
} // namespace

int main(int argc, char** argv)
{
    crash::install();
    host::Options opts = host::parse(argc, argv);
    opts.batch = true;
    if (opts.printVersion)
    {
        std::printf("Lila %s\n", version::full().c_str());
        return 0;
    }

    host::startCore();
    loadUiCommands();
    host::openStartupWorkspace(opts);
    const bool pythonOk = host::startPython(opts);
    const int rc = host::runBatch(opts, pythonOk);
    host::shutdown();
    return rc;
}
