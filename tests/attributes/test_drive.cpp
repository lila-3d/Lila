#include <doctest/doctest.h>

#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/AttributeEditor.h"
#include "ui/ChannelBox.h"
#include "ui/DrivePopover.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>

using namespace lila;
using namespace lila::scene;
namespace av = lila::ui::attrView;

namespace driveTests
{
json dCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
void dSettle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
std::string sourceOf(const std::string& plug)
{
    Graph& g = Graph::current();
    auto src = g.source(g.plug(plug));
    return src ? g.plugName(*src) : std::string();
}
} // namespace driveTests
using namespace driveTests;

TEST_CASE("Drive popover: connect from a searched plug, break, driven key from current values, math node")
{
    dCmd("newScene");
    dCmd("spaceLocator", {{"name", "Ctrl"}});
    dCmd("spaceLocator", {{"name", "Box"}});
    {
        ui::DrivePopover pop("Box.ty");
        std::vector<std::string> c = pop.candidates("Ctrl.rot");
        CHECK(std::find(c.begin(), c.end(), "Ctrl.rotateX") != c.end());
        CHECK(std::find(c.begin(), c.end(), "Ctrl.rotate") == c.end()); // a whole vector doesn't fit a scalar
        for (const std::string& p : pop.candidates(""))
            CHECK(p.rfind("Box.", 0) != 0);
        CHECK(pop.connectFrom("Ctrl.rotateX"));
    }
    CHECK(sourceOf("Box.ty") == "Ctrl.rotateX");
    {
        ui::DrivePopover pop("Box.ty");
        CHECK(pop.findChild<QLabel*>("driveSource")->text().contains("Ctrl.rotateX"));
        CHECK(pop.breakSource());
        CHECK(pop.findChild<QLabel*>("driveSource")->text().contains("Not driven"));
    }
    CHECK(sourceOf("Box.ty").empty());

    {
        ui::DrivePopover pop("Box.tz");
        pop.findChild<QLineEdit*>("driveDriver")->setText("Ctrl.tx");
        pop.keyCurrent();
        dCmd("setAttr", {{"args", {"Ctrl.tx", 10.0}}});
        dCmd("setAttr", {{"args", {"Box.tz", 5.0}}});
        pop.keyCurrent();
        CHECK(pop.findChild<QTableWidget*>("driveKeys")->rowCount() == 2);
        CHECK_FALSE(pop.findChild<QLabel*>("driveCurve")->pixmap().isNull());
        CHECK(pop.applyDrivenKey());
    }
    dCmd("setAttr", {{"args", {"Ctrl.tx", 5.0}}});
    CHECK(dCmd("getAttr", {{"args", {"Box.tz"}}}).get<double>() == doctest::Approx(2.5));

    std::string mathType;
    for (const rigmath::Function& f : rigmath::functions())
        for (const rigmath::Overload& o : f.overloads)
            if (mathType.empty() && !o.outputs.empty() && o.outputs[0].kind == rigmath::Kind::Float && f.name == "clamp")
                mathType = o.nodeType;
    REQUIRE(!mathType.empty());
    {
        ui::DrivePopover pop("Box.sx");
        const std::string node = pop.addMath(mathType);
        CHECK_FALSE(node.empty());
        CHECK(sourceOf("Box.sx").rfind(node + ".", 0) == 0);
    }
}

TEST_CASE("Drive popover: an expression compiles to nodes and the Channel Box marks the channel")
{
    dCmd("newScene");
    dCmd("spaceLocator", {{"name", "Ctrl"}});
    dCmd("spaceLocator", {{"name", "Box"}});
    {
        ui::DrivePopover pop("Box.tx");
        pop.findChild<QLineEdit*>("driveExpression")->setText("= Ctrl.ty * 2 + 1");
        CHECK(pop.applyExpression());
    }
    dCmd("setAttr", {{"args", {"Ctrl.ty", 3.0}}});
    CHECK(dCmd("getAttr", {{"args", {"Box.tx"}}}).get<double>() == doctest::Approx(7.0));
    CHECK((ui::ChannelBox::stateOf(Graph::current().plug("Box.tx")) & ui::ChannelBox::kExpression));
    ui::DrivePopover bad("Box.tz");
    bad.findChild<QLineEdit*>("driveExpression")->setText("Ctrl.ty +");
    CHECK_FALSE(bad.applyExpression());
}

TEST_CASE("Drive popover: time, frame, noise and random compile to nodes and follow the scene time")
{
    dCmd("newScene");
    dCmd("spaceLocator", {{"name", "Ctrl"}});
    dCmd("spaceLocator", {{"name", "Box"}});
    {
        ui::DrivePopover pop("Box.ry");
        CHECK(pop.findChild<QLabel*>("driveExpressionNote")->text().contains("time"));
        pop.findChild<QLineEdit*>("driveExpression")->setText("sin(time) * 10 + noise(frame * 0.1, 2) + random(Ctrl.ty)");
        CHECK(pop.applyExpression());
    }
    CHECK(dCmd("expression", {{"args", {"Box.ry"}}, {"query", true}})["native"] == true);
    CHECK((ui::ChannelBox::stateOf(Graph::current().plug("Box.ry")) & ui::ChannelBox::kExpression));
    const double at0 = dCmd("getAttr", {{"args", {"Box.ry"}}}).get<double>();
    Graph::current().setTime(1.0);
    const double at1 = dCmd("getAttr", {{"args", {"Box.ry"}}}).get<double>();
    CHECK(at1 != at0);
    Graph::current().setTime(0.0);
}

TEST_CASE("Attribute Editor shows what drives a row and what the object drives")
{
    dCmd("newScene");
    dCmd("spaceLocator", {{"name", "Ctrl"}});
    dCmd("spaceLocator", {{"name", "Box"}});
    dCmd("connectAttr", {{"args", {"Ctrl.tx", "Box.ty"}}});
    dCmd("select", {{"args", {"Box"}}});
    ui::AttributeEditor ae;
    ae.resize(420, 900);
    ae.show();
    dSettle();
    QLabel* link = ae.findChild<QLabel*>("aeSourceLink");
    REQUIRE(link);
    CHECK(link->text().contains("Ctrl.translateX"));
    Q_EMIT link->linkActivated("x");
    dSettle();
    CHECK(ae.findChild<QLineEdit*>()->text() == "Ctrl");
    QLabel* drives = ae.findChild<QLabel*>("aeDrivesLink");
    REQUIRE(drives);
    CHECK(drives->text().contains("Box.translateY"));
}

TEST_CASE("Scrub settings: speed, hyperbolic steps and the viewport middle drag")
{
    av::setScrubSpeed(2.0);
    av::setHyperbolicScrub(false);
    CHECK(av::scrubStep(0.1, 4) == doctest::Approx(0.2));
    av::setHyperbolicScrub(true);
    CHECK(av::scrubStep(0.1, 4) == doctest::Approx(0.4));
    av::setScrubSpeed(1.0);
    av::setHyperbolicScrub(false);

    dCmd("newScene");
    dCmd("spaceLocator", {{"name", "S"}});
    dCmd("select", {{"args", {"S"}}});
    ui::ChannelBox box;
    box.show();
    dSettle();
    auto* table = box.findChild<QTableWidget*>();
    table->selectRow(0);
    dSettle();
    av::setViewportScrub("off");
    CHECK_FALSE(ui::channelSlider::begin(table->viewport()->mapToGlobal(QPoint(5, 5))));
    av::setViewportScrub("always");
    CHECK(ui::channelSlider::begin(table->viewport()->mapToGlobal(QPoint(5, 5))));
    ui::channelSlider::end();
}
