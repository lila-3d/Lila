#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "core/MainQueue.h"
#include "fbx/Fbx.h"
#include "gltf/Gltf.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_gltf_tests";
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
    lila::fbx::registerCommands();
    lila::gltf::registerCommands();
    lila::io::translators::registerBuiltins();
    lila::io::translators::registerCommands();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
