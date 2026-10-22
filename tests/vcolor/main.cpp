#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "fbx/Fbx.h"
#include "io/SceneFile.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "vcolor/VertexColors.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_vcolor_tests";
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
    lila::fbx::registerCommands();
    lila::vcolor::registerCommands();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
