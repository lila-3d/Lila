#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/AgentCapture.h"

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    auto dir = std::filesystem::temp_directory_path() / "lila_agentic_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    lila::mainq::init();
    lila::PrefStore::get().start();
    lila::cmds::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::mcp::registerBuiltins();
    lila::ui::capture::registerCommands();
    lila::ui::capture::registerMcpTools();

    doctest::Context ctx(argc, argv);
    int rc = ctx.run();
    lila::mcp::Server::get().stop();
    lila::render::Renderer::get().stop();
    lila::PrefStore::get().stop();
    return rc;
}
