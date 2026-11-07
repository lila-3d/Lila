#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#pragma push_macro("slots")
#undef slots
#include "tex/GraphFiles.h"
#include "tex/Layers.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/NodeEditor.h"
#include "ui/TextureGraphUi.h"
#include "ui/TextureSetPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
void pumpWithDeletes()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

json stack() { return run("texLayerStack", {{"args", {"Rock"}}}); }

std::vector<std::string> rootIds()
{
    std::vector<std::string> ids;
    const json s = stack();
    for (const json& l : s["layers"])
        ids.push_back(l["id"].get<std::string>());
    return ids;
}

const json* layerIn(const json& layers, const std::string& id)
{
    for (const json& l : layers)
    {
        if (l["id"] == id)
            return &l;
        if (const json* c = layerIn(l.value("children", json::array()), id))
            return c;
    }
    return nullptr;
}
} // namespace

TEST_CASE("texture set panel builds a layer stack through tex commands, and undo restores it")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});

    std::vector<std::string> executed;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done && e.ok)
            executed.push_back(std::string(e.name));
    });
    auto ran = [&](const char* name) { return std::find(executed.begin(), executed.end(), name) != executed.end(); };

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    panel->setTextureSet("Rock");
    pumpWithDeletes();
    CHECK(panel->setPicker()->currentText() == "Rock");

    // Two fill layers: the second goes above the selected first, and the rows show the top first.
    const std::string a = panel->addLayer("fill");
    REQUIRE(a == "L1");
    CHECK(ran("texAddLayer"));
    CHECK(panel->selectedLayer() == a);
    const std::string b = panel->addLayer("fill");
    REQUIRE(!b.empty());
    pumpWithDeletes();
    CHECK(rootIds() == std::vector<std::string>{a, b});
    REQUIRE(panel->tree()->topLevelItemCount() == 2);
    CHECK(panel->tree()->topLevelItem(0)->data(0, Qt::UserRole + 1).toString().toStdString() == b);

    // Reorder: drop the top layer below the other.
    executed.clear();
    panel->moveLayer(b, a, ui::TextureSetPanel::Drop::Below);
    pumpWithDeletes();
    CHECK(ran("texMoveLayer"));
    CHECK(rootIds() == std::vector<std::string>{b, a});
    CHECK(panel->tree()->topLevelItem(0)->data(0, Qt::UserRole + 1).toString().toStdString() == a);

    // A channel toggle on a row turns a channel the fill had no value for on.
    CHECK_FALSE(layerIn(stack()["layers"], a)->at("resolved")["emissive"]["enabled"].get<bool>());
    auto* emissive = panel->findChild<QToolButton*>(QString::fromStdString("channel:" + a + ":emissive"));
    REQUIRE(emissive);
    executed.clear();
    emissive->click();
    pumpWithDeletes();
    CHECK(ran("texSetLayer"));
    CHECK(layerIn(stack()["layers"], a)->at("resolved")["emissive"]["enabled"].get<bool>());

    // The row's opacity sets every channel's (the channel picker shows all channels).
    auto* opacity = panel->findChild<QDoubleSpinBox*>(QString::fromStdString("opacity:" + a));
    REQUIRE(opacity);
    executed.clear();
    opacity->setValue(50);
    pumpWithDeletes();
    CHECK(ran("texSetLayer"));
    json resolved = layerIn(stack()["layers"], a)->at("resolved");
    CHECK(resolved["baseColor"]["opacity"].get<double>() == doctest::Approx(0.5));
    CHECK(resolved["roughness"]["opacity"].get<double>() == doctest::Approx(0.5));
    CHECK(resolved["emissive"]["enabled"].get<bool>());

    // Undo puts the opacity back, and the row follows.
    REQUIRE(UndoStack::get().undo());
    pumpWithDeletes();
    CHECK(layerIn(stack()["layers"], a)->at("resolved")["baseColor"]["opacity"].get<double>() == doctest::Approx(1.0));
    opacity = panel->findChild<QDoubleSpinBox*>(QString::fromStdString("opacity:" + a));
    REQUIRE(opacity);
    CHECK(opacity->value() == doctest::Approx(100.0));

    // Hiding a layer, and a folder taking a dropped layer.
    executed.clear();
    auto* eye = panel->findChild<QToolButton*>(QString::fromStdString("eye:" + a));
    REQUIRE(eye);
    eye->click();
    pumpWithDeletes();
    CHECK_FALSE(layerIn(stack()["layers"], a)->at("enabled").get<bool>());
    const std::string folder = panel->addLayer("folder");
    REQUIRE(!folder.empty());
    panel->moveLayer(a, folder, ui::TextureSetPanel::Drop::Into);
    pumpWithDeletes();
    const json afterMove = stack();
    const json* f = layerIn(afterMove["layers"], folder);
    REQUIRE(f);
    REQUIRE(f->at("children").size() == 1);
    CHECK(f->at("children")[0]["id"] == a);

    // A mask on the selected layer, then duplicate keeps it with fresh ids.
    panel->selectLayer(b);
    pumpWithDeletes();
    const std::string mask = panel->addMask("black");
    REQUIRE(!mask.empty());
    CHECK(ran("texAddMask"));
    CHECK(panel->selectedMask() == mask);
    panel->selectLayer(b);
    const size_t before = rootIds().size();
    panel->duplicateSelected();
    pumpWithDeletes();
    std::vector<std::string> ids = rootIds();
    REQUIRE(ids.size() == before + 1);
    const json afterCopy = stack();
    const json* copy = layerIn(afterCopy["layers"], panel->selectedLayer());
    REQUIRE(copy);
    CHECK(copy->at("id") != b);
    REQUIRE(copy->at("masks").size() == 1);
    CHECK(copy->at("masks")[0]["id"] != mask);

    // Delete the copy.
    executed.clear();
    panel->deleteSelected();
    pumpWithDeletes();
    CHECK(ran("texRemoveLayer"));
    CHECK(rootIds().size() == before);

    ops::removeExecuteListener(listener);
}

TEST_CASE("texture set panel applies smart materials from its shelf and shows the graph-mode banner")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    panel->setTextureSet("Rock");
    pumpWithDeletes();

    REQUIRE(panel->smartShelf()->count() > 0);
    std::string material;
    for (int i = 0; i < panel->smartShelf()->count(); ++i)
        if (panel->smartShelf()->item(i)->data(Qt::UserRole + 1).toString() == "material")
            material = panel->smartShelf()->item(i)->data(Qt::UserRole).toString().toStdString();
    REQUIRE(!material.empty());

    std::vector<std::string> executed;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done && e.ok)
            executed.push_back(std::string(e.name));
    });
    panel->applySmartMaterial(material);
    pumpWithDeletes();
    CHECK(std::find(executed.begin(), executed.end(), "texApplySmartMaterial") != executed.end());
    REQUIRE(rootIds().size() == 1);
    CHECK(panel->selectedLayer() == rootIds().front());
    CHECK(panel->tree()->topLevelItemCount() == 1);
    CHECK(panel->properties() != nullptr);
    CHECK(panel->banner()->isHidden());

    // A node added by hand that the stack can't show puts the set in graph mode; Rebuild Layers drops it.
    run("texAddNode", {{"args", {"Rock", "levels"}}});
    pumpWithDeletes();
    CHECK(stack()["mode"] == "graph");
    CHECK_FALSE(panel->banner()->isHidden());
    auto* rebuild = panel->banner()->findChild<QPushButton*>("rebuildLayers");
    REQUIRE(rebuild);
    rebuild->click();
    pumpWithDeletes();
    CHECK(stack()["mode"] == "stack");
    CHECK(panel->banner()->isHidden());

    ops::removeExecuteListener(listener);
}
