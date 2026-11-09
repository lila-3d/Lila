#include <doctest/doctest.h>

#include "curve/CurveNodes.h"
#include "curve/TestUtil.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lila;
using json = nlohmann::json;

namespace
{
/** A head with a groom of ten drawn locks and its strands. */
json groomedHead()
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    const std::string head = curvetest::run("polySphere", {{"name", "Head"}, {"radius", 10.0}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    const json made = ops::execute("createGroom", {{"args", {head}}, {"strands", true}});
    const std::string groom = made["groom"];
    for (int k = 0; k < 10; ++k)
    {
        const double a = (-80.0 + k * 17.0) * 3.14159265358979 / 180.0;
        json pts = json::array();
        for (int i = 0; i < 8; ++i)
        {
            const double pol = 0.15 + 1.3 * i / 7.0, r = 10.3 + 0.4 * i / 7.0;
            pts.push_back({r * std::sin(pol) * std::sin(a), r * std::cos(pol), r * std::sin(pol) * std::cos(a)});
        }
        ops::execute("groomDraw", {{"args", {groom}}, {"points", pts}});
    }
    return made;
}

std::filesystem::path scratch(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "lila_groom_io";
    std::filesystem::create_directories(dir);
    return dir / name;
}

std::string readText(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
} // namespace

TEST_CASE("exportGroom: Unreal Alembic over frames, USD curves, and a .groom file that importGroom reads back")
{
    const json made = groomedHead();
    const std::string groom = made["groom"];
    scene::Graph& g = scene::Graph::current();
    const double before = g.time();

    const auto abc = scratch("hair.abc");
    const json a = ops::execute("exportGroom", {{"args", {groom}}, {"path", abc.string()}, {"start", 1.0}, {"end", 3.0}});
    CHECK(a["frames"] == 3);
    CHECK(a["strands"].get<size_t>() > 1000);
    REQUIRE(std::filesystem::exists(abc));
    const std::string abcBytes = readText(abc);
    CHECK(abcBytes.find("groom_width") != std::string::npos);
    CHECK(abcBytes.find("groom_guide") != std::string::npos);
    CHECK(g.time() == before);

    const auto usd = scratch("hair.usda");
    const json u = ops::execute("exportGroom", {{"args", {groom}}, {"path", usd.string()}});
    CHECK(u["frames"] == 1);
    CHECK(readText(usd).find("BasisCurves") != std::string::npos);

    const auto file = scratch("hair.groom");
    ops::execute("exportGroom", {{"args", {groom}}, {"path", file.string()}});
    REQUIRE(std::filesystem::exists(file));
    scene::Node* gr = g.find(groom);
    const std::string hierarchy = std::get<std::string>(gr->value(gr->type().attrIndex("hierarchy")));
    const int64_t count = std::get<int64_t>(gr->value(gr->type().attrIndex("strandCount")));

    const json in = ops::execute("importGroom", {{"args", {file.string()}}, {"name", "Again"}});
    scene::Node* again = g.find(in["groom"].get<std::string>());
    REQUIRE(again);
    CHECK(g.find(in["scalp"].get<std::string>()) != nullptr);
    CHECK(std::get<std::string>(again->value(again->type().attrIndex("hierarchy"))) == hierarchy);
    g.evaluate();
    CHECK(std::get<int64_t>(again->value(again->type().attrIndex("strandCount"))) == count);

    CHECK_THROWS(ops::execute("exportGroom", {{"args", {groom}}, {"path", scratch("hair.obj").string()}}));
}
