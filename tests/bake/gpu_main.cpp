#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "bake/Bake.h"
#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "render/Baker.h"
#include "render/MeshMaps.h"
#include "render/Renderer.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_bake_gpu_tests";
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
    lila::bake::registerCommands();
    lila::render::registerGpuBakeBackend();

    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    lila::render::shutdownLiveMaps();
    lila::render::Renderer::get().stop();
    return rc;
}
