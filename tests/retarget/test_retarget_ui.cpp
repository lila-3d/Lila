#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Take.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Panels.h"
#include "ui/RetargetUi.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>

#include <memory>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** Two small bipeds: Hips (source, keyed forward) and pelvis (target). */
void makeScene()
{
    run("newScene");
    for (int k = 0; k < 2; ++k)
    {
        const bool t = k == 1;
        const double x = t ? 150.0 : 0.0;
        auto j = [&](const std::string& n, double px, double py, double pz, const std::string& p) {
            run("joint", {{"position", {px + x, py, pz}}, {"name", n}, {"parent", p}, {"skipSelect", true}});
        };
        const std::string hips = t ? "pelvis" : "Hips";
        j(hips, 0, 100, 0, "");
        j(t ? "spine_01" : "Spine", 0, 130, 0, hips);
        for (int s = 0; s < 2; ++s)
        {
            const double sx = s == 0 ? 1.0 : -1.0;
            const std::string side = t ? (s == 0 ? "_l" : "_r") : "";
            const std::string pre = t ? "" : (s == 0 ? "Left" : "Right");
            j(pre + (t ? "thigh" : "UpLeg") + side, 10 * sx, 95, 0, hips);
            j(pre + (t ? "calf" : "Leg") + side, 10 * sx, 52, 2, pre + (t ? "thigh" : "UpLeg") + side);
            j(pre + (t ? "foot" : "Foot") + side, 10 * sx, 8, 0, pre + (t ? "calf" : "Leg") + side);
            j(pre + (t ? "ball" : "ToeBase") + side, 10 * sx, 2, 12, pre + (t ? "foot" : "Foot") + side);
        }
    }
    scene::Graph& g = scene::Graph::current();
    const scene::NodeId hips = g.find("Hips")->id();
    for (int k = 0; k < 3; ++k)
        scene::anim::setKey(g, {hips, scene::Transform::kTranslate, k}, 0.0, k == 1 ? 100.0 : 0.0);
    scene::anim::setKey(g, {hips, scene::Transform::kTranslate, 2}, 1.0, 50.0);
}

QDialog* retargetDialog()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<QDialog*>(w); d && d->isVisible() && d->windowTitle() == "Retarget Animation")
            return d;
    return nullptr;
}

QPushButton* button(QWidget* in, const char* text)
{
    for (QPushButton* b : in->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

bool hasTake(const char* name)
{
    for (scene::NodeId t : scene::anim::takes(scene::Graph::current()))
        if (scene::Graph::current().node(t)->name() == name)
            return true;
    return false;
}
} // namespace

TEST_CASE("the Retarget Animation dialog previews, keeps and takes back")
{
    makeScene();
    const ui::Action* a = ui::actions::find("anim.retarget");
    REQUIRE(a);
    a->run();
    QDialog* d = retargetDialog();
    REQUIRE(d);
    const QList<QComboBox*> combos = d->findChildren<QComboBox*>();
    REQUIRE(combos.size() >= 2);
    combos[0]->setCurrentText("Hips");
    combos[1]->setCurrentText("pelvis");
    button(d, "Preview")->click();
    bool keyed = false;
    for (QLabel* l : d->findChildren<QLabel*>())
        keyed = keyed || l->text().startsWith("Keyed 31 frames");
    CHECK(keyed);
    CHECK(hasTake("Hips_retarget"));
    // Close takes the preview back.
    button(d, "Close")->click();
    QApplication::processEvents();
    CHECK_FALSE(hasTake("Hips_retarget"));

    a->run();
    d = retargetDialog();
    REQUIRE(d);
    d->findChildren<QComboBox*>()[0]->setCurrentText("Hips");
    d->findChildren<QComboBox*>()[1]->setCurrentText("pelvis");
    button(d, "Retarget")->click();
    QApplication::processEvents();
    CHECK(hasTake("Hips_retarget"));
    CHECK(scene::Graph::current().find("Hips_to_pelvis"));
}

TEST_CASE("the Retargeter panel lists chains and edits settings")
{
    makeScene();
    run("retargeter", {{"source", "Hips"}, {"target", "pelvis"}});
    const ui::panels::Panel* p = ui::panels::registry().find(ui::kRetargeterDock);
    REQUIRE(p);
    std::unique_ptr<QWidget> panel(p->create(nullptr));
    panel->show();
    QApplication::processEvents();
    QTabWidget* tabs = panel->findChild<QTabWidget*>();
    REQUIRE(tabs);
    CHECK(tabs->count() == 4);
    QTreeWidget* chains = tabs->widget(0)->findChild<QTreeWidget*>();
    REQUIRE(chains);
    CHECK(chains->topLevelItemCount() >= 4);
    QTreeWidgetItem* leg = nullptr;
    for (int i = 0; i < chains->topLevelItemCount(); ++i)
        if (chains->topLevelItem(i)->text(0) == "LeftLeg")
            leg = chains->topLevelItem(i);
    REQUIRE(leg);
    auto* ik = qobject_cast<QCheckBox*>(chains->itemWidget(leg, 3));
    REQUIRE(ik);
    CHECK(ik->isChecked());
    ik->setChecked(false);
    const json s = run("editRetargeter", {{"args", {"Hips_to_pelvis"}}, {"query", true}});
    for (const json& c : s["chains"])
        if (c["target"] == "LeftLeg")
            CHECK(c["ik"]["enabled"] == false);
}
