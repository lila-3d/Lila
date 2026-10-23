#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "io/SceneFile.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/SceneCommands.h"
#include "ui/Modes.h"
#include "ui/Tools.h"

#include <QApplication>

#include <cstdlib>
#include <memory>
#include <filesystem>
#include <random>

int main(int argc, char** argv)
{
    // Keep tests out of the real user prefs.
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("lila_puppeteer_tests_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
#endif
    // The viewport test needs widgets: an offscreen QApplication when the plugin is deployed next to the exe.
    std::unique_ptr<QApplication> app;
    {
        const std::filesystem::path exe = std::filesystem::absolute(argv[0]).parent_path();
        std::error_code ec;
        if (std::filesystem::exists(exe / "platforms" / "qoffscreen.dll", ec) || std::filesystem::exists(exe / "platforms" / "libqoffscreen.so", ec))
        {
#ifdef _WIN32
            _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
            setenv("QT_QPA_PLATFORM", "offscreen", 1);
#endif
            app = std::make_unique<QApplication>(argc, argv);
        }
    }
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerRigTypes();
    lila::scene::registerIkTypes();
    lila::scene::registerConstraintTypes();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    lila::extensions::registerCommands();
    lila::ui::modes::registerBuiltins();
    lila::ui::tools::registerActions();

    doctest::Context ctx(argc, argv);
    int result = ctx.run();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return result;
}
