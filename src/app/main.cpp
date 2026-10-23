#include "app/Host.h"
#include "core/CrashHandler.h"
#include "ui/AppMain.h"

#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace lila;

int main(int argc, char** argv)
{
    crash::install();
    host::Options opts = host::parse(argc, argv);
#if defined(_WIN32) && defined(LILA_GUI_SUBSYSTEM)
    // The GUI build has no console of its own; batch output goes to the shell that launched it, if any.
    if (opts.batch && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif

    host::startCore();
    ui::registerBatchCommands();
    const std::filesystem::path startupScene = host::openStartupWorkspace(opts);
    const bool pythonOk = host::startPython(opts);
    const int rc = opts.batch ? host::runBatch(opts, pythonOk) : ui::runApp(argc, argv, opts, startupScene);
    host::shutdown();
    return rc;
}
