#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "tex/GraphFiles.h"
#include "tex/Library.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/MaterialBrowser.h"
#include "ui/ViewportHooks.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <filesystem>
#include <fstream>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
namespace fs = std::filesystem;
namespace lib = tex::library;

void registerBrowserUi()
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    if (!scene::types::find(tex::kTextureSetType))
        tex::registerTypes();
    if (!ops::find("texCreateSet"))
        tex::registerCommands();
    if (!ops::find("texExportGraph"))
        tex::registerFileCommands();
    if (!ops::find("materialLibrary"))
        lib::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerMaterialBrowserUi();
}

void writeMaterial(const std::string& id, json meta, json values)
{
    tex::Graph g = tex::Graph::makeMaterial("M");
    g.nodes.front().values = std::move(values);
    json doc = {{"format", "lilamat"}, {"version", 1}, {"name", meta.value("name", "M")}, {"meta", meta}, {"graph", g.toJson()}};
    doc["presets"] = {{"Rough", {{"Output.roughness", 0.95}}}};
    fs::path f = lib::tierDir("user") / (id + ".lilamat");
    fs::create_directories(f.parent_path());
    std::ofstream(f, std::ios::binary) << doc.dump(2);
}

QStringList userIds(const ui::MaterialBrowser& b)
{
    QStringList out;
    for (const QString& id : b.shownIds())
        if (id.startsWith("BrowserTest/"))
            out << id;
    return out;
}
} // namespace

TEST_CASE("material browser searches, filters by category, picks presets and applies by drop and double-click")
{
    registerBrowserUi();
    run("newScene");
    fs::remove_all(lib::tierDir("user") / "BrowserTest");
    writeMaterial("BrowserTest/brick", {{"name", "Red Brick"}, {"category", "Stone/Brick"}, {"tags", {"wall"}}, {"scaleCm", 100}},
                  {{"baseColor", {0.3, 0.08, 0.05, 1.0}}, {"roughness", 0.8}});
    writeMaterial("BrowserTest/steel", {{"name", "Bare Steel"}, {"category", "Metal/Steel"}, {"physical", "metal"}, {"scaleCm", 50}},
                  {{"baseColor", {0.56, 0.57, 0.58, 1.0}}, {"metallic", 1.0}, {"roughness", 0.3}});

    ui::MaterialBrowser b;
    b.resize(700, 500);
    b.show();
    pump();
    CHECK(userIds(b) == QStringList{"BrowserTest/steel", "BrowserTest/brick"});

    b.searchField()->setText("brick");
    pump();
    CHECK(userIds(b) == QStringList{"BrowserTest/brick"});
    b.searchField()->clear();

    // The tree has Metal and its Steel branch; choosing Metal shows only metals.
    QTreeWidgetItem* metal = nullptr;
    for (QTreeWidgetItemIterator it(b.filterTree()); *it; ++it)
        if ((*it)->data(0, Qt::UserRole + 1).toString() == "cat:Metal")
            metal = *it;
    REQUIRE(metal);
    bool steelBranch = false;
    for (int i = 0; i < metal->childCount(); ++i)
        steelBranch |= metal->child(i)->data(0, Qt::UserRole + 1).toString() == "cat:Metal/Steel";
    CHECK(steelBranch);
    b.filterTree()->setCurrentItem(metal);
    pump();
    CHECK(userIds(b) == QStringList{"BrowserTest/steel"});
    b.filterTree()->setCurrentItem(b.filterTree()->topLevelItem(0));
    pump();

    // Selecting a material lists its presets; choosing one is what drops and Apply use.
    b.select("BrowserTest/brick");
    pump();
    REQUIRE(b.presetBox()->count() == 2);
    b.presetBox()->setCurrentIndex(1);
    CHECK(b.preset() == "Rough");
    CHECK(ui::chosenPreset("BrowserTest/brick") == "Rough");

    // Drop onto a mesh in a viewport: the hook applies the material to the mesh's slot.
    std::string plane = run("polyPlane", {{"width", 200.0}, {"depth", 200.0}})[0];
    scene::Node* n = scene::Graph::current().find(plane);
    REQUIRE(n);
    QMimeData mime;
    mime.setData(ui::kMaterialMime, "BrowserTest/brick");
    CHECK(ui::viewportHooks::handleDrop(mime, {n->id(), 0}, false));
    CHECK(ui::viewportHooks::handleDrop(mime, {n->id(), 0}, true));
    std::vector<std::string> sets = tex::setNames(scene::Graph::current());
    REQUIRE(sets.size() == 1);
    tex::Graph g = tex::readGraph(*tex::findSet(scene::Graph::current(), sets[0]));
    REQUIRE(g.node(lib::kTilingNode));
    CHECK(g.node(lib::kTilingNode)->values["tilesX"].get<double>() == doctest::Approx(2.0).epsilon(1e-3));
    REQUIRE(g.node("LibraryChannels"));
    CHECK(g.node("LibraryChannels")->values["roughness"] == 0.95);
    QMimeData other;
    other.setText("not a material");
    CHECK_FALSE(ui::viewportHooks::handleDrop(other, {n->id(), 0}, false));

    // Double-click equivalent on the selection replaces the set's graph with the steel, at its own scale.
    run("select", {{"args", {plane}}});
    b.select("BrowserTest/steel");
    pump();
    b.applySelected();
    g = tex::readGraph(*tex::findSet(scene::Graph::current(), sets[0]));
    CHECK(g.node(lib::kTilingNode)->values["tilesX"].get<double>() == doctest::Approx(4.0).epsilon(1e-3));

    // The grid's drag payload is the material id.
    QListWidget* grid = b.grid();
    QListWidgetItem* item = grid->currentItem();
    REQUIRE(item);
    std::unique_ptr<QMimeData> drag(grid->model()->mimeData({grid->currentIndex()}));
    REQUIRE(drag);
    CHECK(drag->data(ui::kMaterialMime) == "BrowserTest/steel");

    fs::remove_all(lib::tierDir("user") / "BrowserTest");
}
