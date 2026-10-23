// Throwaway visual check: builds a scatter group, opens the node editor offscreen and grabs it to a PNG.
#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "io/SceneFile.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QTimer>

#include <cstdlib>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;

int main(int argc, char** argv)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_gn_demo";
    std::filesystem::create_directories(dir);
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    gn::registerTypes();
    scene::registerCommands();
    io::registerCommands();
    gn::registerCommands();

    QApplication app(argc, argv);
    ui::applyTheme(app);

    auto run = [](const char* n, json c) { return ops::execute(n, c); };
    auto a = [](std::initializer_list<json> l) { return json{{"args", json(l)}}; };
    std::string mesh = run("polyPlane", {{"name", "Ground"}})[0];
    run("gnCreateGroup", a({"Scatter"}));
    auto add = [&](const char* t, double x, double y) { return run("gnAddNode", {{"args", {"Scatter", t}}, {"position", {x, y}}}).get<std::string>(); };
    run("gnMoveNodes", {{"args", {"Scatter"}}, {"positions", {{"GroupInput", {-520, 0}}, {"GroupOutput", {520, 0}}}}});
    std::string dist = add("DistributePointsOnFaces", -300, 0), cube = add("MeshCube", -300, 200), iop = add("InstanceOnPoints", 0, 0),
                rnd = add("RandomValue", -300, 360), real = add("RealizeInstances", 260, 0), join = add("JoinGeometry", 400, 120);
    run("gnSetValue", a({"Scatter", rnd + ".dataType", "vector"}));
    run("gnSetValue", a({"Scatter", cube + ".size", json::array({5, 5, 5})}));
    run("gnConnect", a({"Scatter", "GroupInput.geometry", dist + ".mesh"}));
    run("gnConnect", a({"Scatter", dist + ".points", iop + ".points"}));
    run("gnConnect", a({"Scatter", cube + ".mesh", iop + ".instance"}));
    run("gnConnect", a({"Scatter", dist + ".rotation", iop + ".rotation"}));
    run("gnConnect", a({"Scatter", rnd + ".value", iop + ".scale"}));
    run("gnConnect", a({"Scatter", iop + ".instances", real + ".geometry"}));
    run("gnConnect", a({"Scatter", real + ".geometry", join + ".geometry"}));
    run("gnConnect", a({"Scatter", "GroupInput.geometry", join + ".geometry"}));
    run("gnConnect", a({"Scatter", join + ".geometry", "GroupOutput.geometry"}));
    run("gnExposeInput", a({"Scatter", dist + ".density", "density"}));
    run("gnApply", a({mesh, "Scatter"}));

    ui::NodeEditor editor;
    editor.resize(1400, 640);
    editor.setGroup("Scatter");
    editor.show();
    std::string out = argc > 1 ? argv[1] : (dir / "node_editor.png").string();
    QTimer::singleShot(300, [&] {
        editor.frameAll();
        QTimer::singleShot(200, [&] {
            editor.grab().save(QString::fromStdString(out));
            app.quit();
        });
    });
    return app.exec();
}
