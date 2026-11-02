#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/ComponentSheet.h"

#include <string>
#include <vector>

using namespace lila;

namespace componentSheetTests
{
json csRun(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** A tube skinned to a three-joint chain: vertices 0 and 1 at 0.7/0.3 on bone_a/bone_b, vertex 2 all bone_c. */
std::string csSkinTube(const char* name)
{
    std::string mesh = csRun("polyCylinder", {{"name", name}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    csRun("skinCluster", {{"args", {"bone_a", mesh}}});
    csRun("skinPercent", {{"args", {mesh}}, {"vertices", {0, 1}}, {"transformValue", {{"bone_a", 0.7}, {"bone_b", 0.3}}}});
    csRun("skinPercent", {{"args", {mesh}}, {"vertices", {2}}, {"transformValue", {{"bone_c", 1.0}}}});
    return mesh;
}

std::string csScene()
{
    csRun("newScene");
    csRun("joint", {{"position", json::array({0, -50, 0})}, {"name", "bone_a"}});
    csRun("joint", {{"position", json::array({0, 0, 0})}, {"name", "bone_b"}});
    csRun("joint", {{"position", json::array({0, 50, 0})}, {"name", "bone_c"}});
    csRun("select", {{"args", json::array()}});
    return csSkinTube("Tube");
}

/** The shown column whose id ends with tail, or -1. */
int csColumnOf(const ui::ComponentSheet& sheet, const std::string& tail)
{
    std::vector<std::string> ids = sheet.columnIds();
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i].ends_with(tail))
            return int(i);
    return -1;
}
} // namespace componentSheetTests
using namespace componentSheetTests;

TEST_CASE("Component Spreadsheet: influences on the selection only, heaviest first; search, pin and hiding zeros")
{
    std::string mesh = csScene();
    csRun("selectComponents", {{"args", {mesh}}, {"vertices", {0, 1}}});
    ui::ComponentSheet sheet;
    sheet.refresh();
    CHECK(sheet.rowCount() == 2);
    CHECK(sheet.rowLabel(1) == "vtx[1]");
    const std::vector<std::string> ids = sheet.columnIds();
    REQUIRE(ids.size() >= 5);
    CHECK(ids[0].ends_with("bone_a"));
    CHECK(ids[1].ends_with("bone_b"));
    CHECK(ids[2] == "position.x");
    CHECK(csColumnOf(sheet, "bone_c") < 0);
    CHECK(sheet.moreCount() == 1);
    CHECK(sheet.value(0, 0) == doctest::Approx(0.7));
    CHECK(sheet.value(1, 1) == doctest::Approx(0.3));

    sheet.setPinned(ids[1], true);
    CHECK(sheet.columnIds()[0] == ids[1]);
    sheet.setPinned(ids[1], false);
    CHECK(sheet.columnIds()[0] == ids[0]);

    sheet.setSearch("bone_b");
    CHECK(sheet.columnIds() == std::vector<std::string>{ids[1]});
    sheet.setSearch("pos");
    CHECK(sheet.columnIds() == std::vector<std::string>{"position.x", "position.y", "position.z"});
    sheet.setSearch("");

    sheet.setShowAllInfluences(true);
    CHECK(csColumnOf(sheet, "bone_c") == 2);
    sheet.setHideZeroColumns(true);
    CHECK(csColumnOf(sheet, "bone_c") < 0);
    sheet.setHideZeroColumns(false);
    sheet.setShowAllInfluences(false);

    csRun("selectComponents", {{"args", {mesh}}, {"vertices", {0, 1, 2}}});
    sheet.refresh();
    CHECK(sheet.rowCount() == 3);
    sheet.setSearch("bone_c");
    REQUIRE(sheet.columnIds().size() == 1);
    sheet.setHideZeroRows(true);
    REQUIRE(sheet.rowCount() == 1);
    CHECK(sheet.rowLabel(0) == "vtx[2]");
}

TEST_CASE("Component Spreadsheet: type-to-set writes the weight and renormalises; held columns refuse")
{
    std::string mesh = csScene();
    csRun("selectComponents", {{"args", {mesh}}, {"vertices", {0, 1}}});
    ui::ComponentSheet sheet;
    sheet.refresh();
    int a = csColumnOf(sheet, "bone_a");
    REQUIRE(a >= 0);
    CHECK(sheet.typeValue({{0, a}}, "0.4"));
    a = csColumnOf(sheet, "bone_a");
    int b = csColumnOf(sheet, "bone_b");
    REQUIRE(b >= 0);
    CHECK(sheet.value(0, a) == doctest::Approx(0.4));
    CHECK(sheet.value(0, b) == doctest::Approx(0.6));
    CHECK(sheet.value(1, a) == doctest::Approx(0.7));
    CHECK_FALSE(sheet.typeValue({{0, a}}, "abc"));

    csRun("undo");
    sheet.refresh();
    CHECK(sheet.value(0, csColumnOf(sheet, "bone_a")) == doctest::Approx(0.7));

    const std::string bId = sheet.columnIds()[size_t(csColumnOf(sheet, "bone_b"))];
    sheet.setHeld(bId, true);
    b = csColumnOf(sheet, "bone_b");
    CHECK_FALSE(sheet.typeValue({{0, b}}, "0.5"));
    CHECK(sheet.value(0, b) == doctest::Approx(0.3));
    sheet.setHeld(bId, false);

    // A pasted block lands from the given cell: one row, two weight columns.
    a = csColumnOf(sheet, "bone_a");
    REQUIRE(a == 0);
    CHECK(sheet.paste(1, 0, "0.25\t0.75"));
    CHECK(sheet.value(1, csColumnOf(sheet, "bone_a")) == doctest::Approx(0.25));
    CHECK(sheet.value(1, csColumnOf(sheet, "bone_b")) == doctest::Approx(0.75));
}

TEST_CASE("Component Spreadsheet: edits go to the lead mesh only, unless Alt for all")
{
    csScene();
    std::string other = csSkinTube("Hose");
    csRun("selectComponents", {{"args", {"Tube"}}, {"vertices", {0}}});
    csRun("selectComponents", {{"args", {other}}, {"vertices", {0}}, {"add", true}});
    ui::ComponentSheet sheet;
    sheet.refresh();
    REQUIRE(sheet.rowCount() == 2);
    CHECK(sheet.rowLabel(0).contains(".vtx[0]"));

    ui::attrView::setMultiEditForced(false);
    int a = csColumnOf(sheet, "bone_a");
    REQUIRE(a >= 0);
    CHECK(sheet.typeValue({{0, a}, {1, a}}, "0.2"));
    a = csColumnOf(sheet, "bone_a");
    int changed = 0;
    for (int r = 0; r < 2; ++r)
        changed += sheet.value(r, a) == doctest::Approx(0.2) ? 1 : 0;
    CHECK(changed == 1);

    ui::attrView::setMultiEditForced(true);
    CHECK(sheet.typeValue({{0, a}, {1, a}}, "0.1"));
    ui::attrView::setMultiEditForced(false);
    a = csColumnOf(sheet, "bone_a");
    CHECK(sheet.value(0, a) == doctest::Approx(0.1));
    CHECK(sheet.value(1, a) == doctest::Approx(0.1));
}
