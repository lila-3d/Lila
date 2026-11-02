#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Theme.h"

#include <QApplication>

#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    // A test fails to the console, never to a dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    // Keep tests out of the real user prefs and stubs.
    auto dir = std::filesystem::temp_directory_path() / "lila_attribute_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    _putenv_s("QT_QPA_PLATFORM", "offscreen");
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    _putenv_s("LILA_NO_STUB_WRITE", "1");
#else
    setenv("QT_QPA_PLATFORM", "offscreen", 1);
    setenv("LILA_CONFIG_DIR", dir.string().c_str(), 1);
    setenv("LILA_NO_STUB_WRITE", "1", 1);
#endif
    QApplication app(argc, argv);
    lila::ui::applyTheme(app);
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    lila::io::registerCommands();
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
