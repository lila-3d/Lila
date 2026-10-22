// Renders the UV editor offscreen to PNGs for a visual check: uv_editor_demo <out-dir>
#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "scene/Components.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Theme.h"
#include "ui/UvEditor.h"
#include "uv/UvCommands.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QPixmap>
#include <QToolButton>

#include <cstdlib>
#include <filesystem>

using namespace lila;

int main(int argc, char** argv)
{
    auto cfg = std::filesystem::temp_directory_path() / "lila_uv_demo";
    std::filesystem::create_directories(cfg);
    _putenv_s("LILA_CONFIG_DIR", cfg.string().c_str());
    QString out = argc > 1 ? QString(argv[1]) : QDir::currentPath();

    mainq::init();
    cmds::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    uv::registerCommands();

    QApplication app(argc, argv);
    ui::applyTheme(app);

    cmds::execute("newScene", json::object());
    std::string sphere = cmds::execute("polySphere", json::object())[0];
    std::string cube = cmds::execute("polyCube", {{"position", {150, 0, 0}}})[0];
    cmds::execute("select", {{"args", {sphere, cube}}});
    cmds::execute("uvAuto", {{"resolution", 1024}, {"padding", 8}});

    ui::UvEditor editor;
    editor.resize(900, 760);
    editor.show();
    app.processEvents();
    editor.grab().save(out + "/uv_editor_checker.png");

    // Face selection on one island plus the move manipulator.
    cmds::execute("selectComponents", {{"args", {sphere}}, {"faces", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}}});
    for (QComboBox* c : editor.findChildren<QComboBox*>())
        if (c->count() == 3)
            c->setCurrentIndex(2);
    for (QToolButton* b : editor.findChildren<QToolButton*>())
        if (b->text() == "Move")
        {
            b->click();
            break;
        }
    app.processEvents();
    editor.grab().save(out + "/uv_editor_distortion.png");
    return 0;
}
