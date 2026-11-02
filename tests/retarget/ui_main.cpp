#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "retarget/SceneRetarget.h"
#include "scene/Node.h"
#include "scene/Rig.h"
#include "scene/SceneCommands.h"
#include "ui/RetargetUi.h"

#include <QApplication>

#include <cstdlib>
#include <filesystem>

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_retarget_ui_tests";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    _putenv_s("QT_QPA_PLATFORM", "offscreen");
#else
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
#endif
    QApplication app(argc, argv);
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerRigTypes();
    lila::scene::registerCommands();
    lila::scene::registerRigCommands();
    lila::io::registerCommands();
    lila::retarget::registerCommands();
    lila::ui::registerRetargetUi();

    doctest::Context ctx(argc, argv);
    return ctx.run();
}
