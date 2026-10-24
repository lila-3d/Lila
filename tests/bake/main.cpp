#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "bake/Bake.h"
#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "vcs/Process.h"
#include "vcs/SourceControl.h"

#include <cstdlib>
#include <filesystem>

namespace
{
/** Nothing is under source control: bakes into temp folders never spawn git or p4, which would hold the folders open. */
class NoTools final : public lila::vcs::ProcessRunner
{
public:
    lila::vcs::ProcessResult run(const lila::vcs::ProcessSpec&) override { return {}; }
};
} // namespace

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs (and their bake targets).
    auto dir = std::filesystem::temp_directory_path() / "lila_bake_tests";
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
    lila::vcs::SourceControl::get().setRunner(std::make_shared<NoTools>());

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
