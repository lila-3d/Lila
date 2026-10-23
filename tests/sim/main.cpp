#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "scene/Anim.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>
#include <random>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("lila_sim_tests_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::cmds::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::extensions::registerCommands();

    doctest::Context ctx(argc, argv);
    int result = ctx.run();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return result;
}
