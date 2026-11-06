#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/ChannelBox.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QToolButton>

using namespace lila;
using namespace lila::scene;
namespace av = lila::ui::attrView;
using CB = lila::ui::ChannelBox;

namespace channelBoxTests
{
json cbCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
void cbSettle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}
QStringList names(QTableWidget* t)
{
    QStringList out;
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0))
            out << t->item(r, 0)->text();
    return out;
}
int rowOf(QTableWidget* t, const QString& name)
{
    for (int r = 0; r < t->rowCount(); ++r)
        if (t->item(r, 0) && t->item(r, 0)->text() == name)
            return r;
    return -1;
}
void typeValue(QTableWidget* t, const QString& name, const QString& text)
{
    const int r = rowOf(t, name);
    REQUIRE(r >= 0);
    t->clearSelection();
    t->item(r, 1)->setText(text);
    cbSettle();
}
} // namespace channelBoxTests
using namespace channelBoxTests;

TEST_CASE("Channel Box: filter text and chips, state stripes, edits all selected, lead-only pref with Alt for all")
{
    cbCmd("newScene");
    cbCmd("spaceLocator", {{"name", "A"}});
    cbCmd("spaceLocator", {{"name", "B"}});
    cbCmd("spaceLocator", {{"name", "Src"}});
    cbCmd("setKeyframe", {{"args", {"B.tx"}}, {"time", 0.0}, {"value", 1.0}});
    cbCmd("connectAttr", {{"args", {"Src.tx", "B.ty"}}});
    cbCmd("setAttr", {{"args", {"B.tz"}}, {"lock", true}});
    cbCmd("addAttr", {{"args", {"B"}}, {"longName", "power"}, {"keyable", true}});
    cbCmd("select", {{"args", {"A", "B"}}});
    av::setNameMode(av::NameMode::Nice);
    av::setLead(kNoNode);
    av::setHintEnabled(true);
    av::setChannelBoxEditsAll(true);
    ui::ChannelBox box;
    box.resize(300, 700);
    box.show();
    cbSettle();
    auto* table = box.findChild<QTableWidget*>();
    REQUIRE(table);
    CHECK(rowOf(table, "Translate X") >= 0);
    CHECK(rowOf(table, "Power") >= 0);

    // Name mode cycles nice, long, short and back to nice.
    auto* mode = box.findChild<QToolButton*>("cbNameMode");
    for (const char* expect : {"translateX", "tx", "Translate X"})
    {
        mode->click();
        cbSettle();
        CHECK(rowOf(table, expect) >= 0);
    }
    CHECK(av::nameMode() == av::NameMode::Nice);

    // States: keyed, connected, locked.
    const NodeId b = Graph::current().find("B")->id();
    NodeId source = kNoNode;
    CHECK((CB::stateOf(Graph::current().plug("B.tx")) & CB::kKeyed));
    CHECK((CB::stateOf(Graph::current().plug("B.ty"), &source) & CB::kConnected));
    CHECK(source == Graph::current().find("Src")->id());
    CHECK((CB::stateOf(Graph::current().plug("B.tz")) & CB::kLocked));
    CHECK_FALSE(table->item(rowOf(table, "Translate X"), 0)->icon().isNull());
    CHECK(table->item(rowOf(table, "Translate Y"), 0)->toolTip().startsWith("Connected from Src"));
    const QString txTip = table->item(rowOf(table, "Translate X"), 0)->toolTip();
    CHECK(txTip.indexOf("Middle-click to reset") > txTip.indexOf("font-style:italic"));
    CHECK(txTip.indexOf("font-style:italic") > txTip.indexOf("tx"));
    CHECK(table->item(rowOf(table, "Rotate X"), 0)->icon().isNull());

    // Filter text, then chips: once for only, twice for all but.
    box.findChild<QLineEdit*>("cbFilter")->setText("rot");
    cbSettle();
    CHECK(names(table) == QStringList({"Rotate X", "Rotate Y", "Rotate Z"}));
    box.findChild<QLineEdit*>("cbFilter")->clear();
    auto* keyed = box.findChild<QToolButton*>("cbChip_Keyed");
    CHECK(keyed->text().isEmpty());
    CHECK_FALSE(keyed->icon().isNull());
    CHECK_FALSE(keyed->isChecked());
    // Chips wrap instead of setting the panel's width.
    QWidget* chipRow = keyed->parentWidget();
    CHECK(chipRow->minimumSizeHint().width() <= keyed->sizeHint().width() + 4);
    CHECK(chipRow->layout()->heightForWidth(keyed->sizeHint().width() * 3) > chipRow->layout()->heightForWidth(1000));
    keyed->click();
    cbSettle();
    CHECK(keyed->isChecked());
    CHECK(keyed->toolTip().contains("Showing only keyed"));
    CHECK(names(table) == QStringList({"Translate X"}));
    keyed->click();
    cbSettle();
    CHECK(keyed->isChecked());
    CHECK(keyed->toolTip().contains("Hiding keyed"));
    auto* trs = box.findChild<QToolButton*>("cbChip_TRS");
    trs->click();
    trs->click();
    CHECK_FALSE(trs->icon().isNull());
    trs->click();
    CHECK(rowOf(table, "Translate X") < 0);
    CHECK(rowOf(table, "Translate Y") >= 0);
    keyed->click();
    box.findChild<QToolButton*>("cbChip_User")->click();
    cbSettle();
    CHECK(names(table) == QStringList({"Power"}));
    box.findChild<QToolButton*>("cbChip_User")->click();
    box.findChild<QToolButton*>("cbChip_User")->click();
    cbSettle();
    CHECK(rowOf(table, "Translate X") >= 0);

    // Every selected holder by default, as in Maya.
    typeValue(table, "Rotate Y", "20");
    CHECK(cbCmd("getAttr", {{"args", {"A.ry"}}}).get<double>() == doctest::Approx(20.0));
    CHECK(cbCmd("getAttr", {{"args", {"B.ry"}}}).get<double>() == doctest::Approx(20.0));
    CHECK(box.findChild<QWidget*>("cbHint")->isHidden());

    // Lead only with the pref off, with the hint; Alt edits every holder.
    av::setChannelBoxEditsAll(false);
    typeValue(table, "Rotate X", "30");
    CHECK(cbCmd("getAttr", {{"args", {"B.rx"}}}).get<double>() == doctest::Approx(30.0));
    CHECK(cbCmd("getAttr", {{"args", {"A.rx"}}}).get<double>() == doctest::Approx(0.0));
    CHECK_FALSE(box.findChild<QWidget*>("cbHint")->isHidden());
    CHECK(box.findChild<QLabel*>("cbHintText")->text() == "Changed B only. Hold Alt to change all 2.");
    av::setMultiEditForced(true);
    typeValue(table, "Rotate X", "45");
    av::setMultiEditForced(false);
    CHECK(cbCmd("getAttr", {{"args", {"A.rx"}}}).get<double>() == doctest::Approx(45.0));
    CHECK(cbCmd("getAttr", {{"args", {"B.rx"}}}).get<double>() == doctest::Approx(45.0));
    av::setChannelBoxEditsAll(true);

    // The object strip: selection order, click for the lead.
    auto* strip = box.findChild<QListWidget*>("cbObjects");
    REQUIRE(strip->count() == 2);
    CHECK(strip->item(0)->text() == "A");
    CHECK(strip->item(1)->font().bold());
    Q_EMIT strip->itemClicked(strip->item(0));
    cbSettle();
    CHECK(av::lead() == Graph::current().find("A")->id());
    CHECK(box.findChild<QLineEdit*>()->text() == "A");
    av::setLead(b);

    // The shared name mode relabels.
    av::setNameMode(av::NameMode::Long);
    cbSettle();
    CHECK(rowOf(table, "translateX") >= 0);
    av::setNameMode(av::NameMode::Nice);
    cbSettle();
    av::setLead(kNoNode);
}

TEST_CASE("Channel Box settings: scrub speed presets set the speed and show which one is current")
{
    av::setScrubSpeed(1.0);
    ui::ChannelBox box;
    box.show();
    cbSettle();
    box.findChild<QToolButton*>("cbSettings")->click();
    cbSettle();
    auto* fast = box.findChild<QToolButton*>("cbScrubPreset_Fast");
    auto* normal = box.findChild<QToolButton*>("cbScrubPreset_Normal");
    REQUIRE(fast);
    REQUIRE(normal);
    CHECK(normal->isChecked());
    fast->click();
    CHECK(av::scrubSpeed() == doctest::Approx(4.0));
    CHECK(fast->isChecked());
    CHECK_FALSE(normal->isChecked());
    box.findChild<QDoubleSpinBox*>("cbScrubSpeed")->setValue(2.5);
    CHECK_FALSE(fast->isChecked());
    av::setScrubSpeed(1.0);
}

TEST_CASE("Channel Box states: mute, breakdown, driven key; highlighted channels for the Graph Editor")
{
    cbCmd("newScene");
    cbCmd("spaceLocator", {{"name", "M"}});
    cbCmd("spaceLocator", {{"name", "D"}});
    cbCmd("setKeyframe", {{"args", {"M.tx"}}, {"time", 0.0}, {"value", 1.0}, {"breakdown", true}});
    CHECK((CB::stateOf(Graph::current().plug("M.tx")) & CB::kBreakdown));
    cbCmd("mute", {{"args", {"M.tx"}}});
    CHECK((CB::stateOf(Graph::current().plug("M.tx")) & CB::kMuted));
    cbCmd("driveChannel", {{"args", {"M.ty"}}, {"driver", "D.tx"}, {"keys", json::array({json::array({0.0, 0.0}), json::array({1.0, 2.0})})}});
    CHECK((CB::stateOf(Graph::current().plug("M.ty")) & CB::kDrivenKey));

    cbCmd("select", {{"args", {"M"}}});
    ui::ChannelBox box;
    box.show();
    cbSettle();
    auto* table = box.findChild<QTableWidget*>();
    table->selectRow(rowOf(table, "Translate X"));
    cbSettle();
    REQUIRE(av::highlightedChannels().size() == 1);
    CHECK(av::highlightedChannels()[0] == Graph::current().plug("M.tx"));
}
