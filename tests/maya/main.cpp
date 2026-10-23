#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "maya/Maya.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

namespace
{
// Runs before doctest's main, on the main thread.
struct Init
{
    Init()
    {
        // Keep tests out of the real user prefs.
        auto dir = std::filesystem::temp_directory_path() / "lila_maya_tests";
        std::filesystem::create_directories(dir);
#ifdef _WIN32
        _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
        setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
        lila::mainq::init();
        lila::ops::registerBuiltins();
        lila::scene::types::registerBuiltins();
        lila::scene::registerCommands();
        lila::io::registerCommands();
        lila::maya::registerCommands();
    }
} gInit;
} // namespace
