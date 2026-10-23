#include "app/Host.h"
#include "core/CrashHandler.h"
#include "core/Paths.h"

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
        return;
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

    host::startCore();
    loadUiCommands();
    host::openStartupWorkspace(opts);
    const bool pythonOk = host::startPython(opts);
    const int rc = host::runBatch(opts, pythonOk);
    host::shutdown();
    return rc;
}
