#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/Events.h"
#include "core/MainQueue.h"
#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "render/Renderer.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

/** Whether loading the extension also loaded Pixar's libraries, which it must leave for the first USD command. */
bool gUsdLoadedWithExtension = false;

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_usd_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", (dir / "config").string().c_str());
#else
    setenv("LILA_CONFIG_DIR", (dir / "config").string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::curve::registerTypes();
    lila::curve::registerCommands();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::extensions::registerCommands();
    lila::extensions::ExtensionManager::get().load(LILA_TEST_OPENUSD_PACKAGE);
    // Scene events with no usdStage in the scene leave OpenUSD unloaded too.
    lila::ops::execute("newScene", {});
    lila::events::send({.type = lila::events::Event::SceneOpened});
    lila::events::send({.type = lila::events::Event::SceneBeforeSave});
    lila::events::send({.type = lila::events::Event::AppIdle});
#ifdef _WIN32
    gUsdLoadedWithExtension = GetModuleHandleW(L"usd_usd.dll") || GetModuleHandleW(L"OpenUSDCore.dll");
#endif

    doctest::Context ctx(argc, argv);
    const int rc = ctx.run();
    // The Hydra cases may start Lila's renderer to composite.
    lila::render::Renderer::get().stop();
    return rc;
}
