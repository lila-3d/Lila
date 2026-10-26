#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_viewport_renderer_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    _putenv_s("LILA_NO_STUB_WRITE", "1");
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
    setenv("LILA_NO_STUB_WRITE", "1", 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::extensions::registerCommands();

    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    lila::render::Renderer::get().stop();
    return rc;
}
