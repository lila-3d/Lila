#include "BladeExtrude.h"

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/Panels.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

void ensureLoaded()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("TerrainTools"))
        REQUIRE(em.load(LILA_TEST_TERRAIN_TOOLS_PACKAGE) == "TerrainTools");
}

void fresh()
{
    ensureLoaded();
    run("newScene");
    run("selectMode", {{"args", {"object"}}});
}

json range(int first, int last)
{
    json out = json::array();
    for (int i = first; i <= last; ++i)
        out.push_back(i);
    return out;
}

/** A plane with a row of towers grown off it, the shape the tools are meant for. */
std::string shrub(int faces = 8)
{
    const std::string plane = run("polyPlane", {{"width", 10.0}, {"depth", 10.0}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("bladeExtrude", {{"args", {plane}}, {"faces", range(0, faces - 1)}, {"thickness", 3.0}, {"offset", 0.2}, {"divisions", 2}});
    run("select", {{"clear", true}});
    return plane;
}

/** The caps keep their ids and the walls are appended after the plane's 16 faces. */
json towers(const std::string& mesh, int faces = 8)
{
    const int total = int(run("meshData", {{"args", {mesh}}})["faces"].size());
    json out = range(0, faces - 1);
    for (int f = 16; f < total; ++f)
        out.push_back(f);
    return out;
}

json info(const std::string& mesh) { return run("bladeExtrudeInfo", {{"args", {mesh}}}); }

std::vector<double> values(const std::string& mesh, const char* field)
{
    std::vector<double> out;
    for (const json& b : info(mesh))
        out.push_back(b[field].get<double>());
    return out;
}

/** The highest point of each blade along Y, sorted. */
std::vector<double> tips(const std::string& mesh)
{
    const json pts = run("meshData", {{"args", {mesh}}, {"worldSpace", true}})["points"];
    std::vector<double> out;
    for (const json& b : info(mesh))
    {
        double top = -1e300;
        for (const json& p : b["points"])
            top = std::max(top, pts[p.get<size_t>()][1].get<double>());
        out.push_back(std::round(top * 1e4) / 1e4);
    }
    std::sort(out.begin(), out.end());
    return out;
}

double biggestMove(const json& before, const json& after)
{
    double d = 0.0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        for (int k = 0; k < 3; ++k)
            d = std::max(d, std::abs(before[i][k].get<double>() - after[i][k].get<double>()));
    return d;
}

json points(const std::string& mesh) { return run("meshData", {{"args", {mesh}}, {"worldSpace", true}})["points"]; }

json fields(double low, double high) { return {{"thickness", {low, high}}}; }

bool throws(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const CommandError&)
    {
        return true;
    }
    return false;
}

size_t distinct(const std::vector<double>& v)
{
    std::set<long long> s;
    for (double x : v)
        s.insert(std::llround(x * 1e4));
    return s.size();
}
} // namespace

TEST_CASE("terrain tools registers its commands, panel, actions and menu")
{
    ensureLoaded();
    for (const char* c : {"bladeExtrude", "randomizeBladeExtrude", "setBladeExtrude", "resetBladeExtrude", "bladeExtrudeInfo",
                          "bladeExtrudeRanges", "selectBladeFaces", "addBladeDeformer", "randomizeBladeDeformers", "bladeDeformers",
                          "deleteBladeDeformers"})
        CHECK_MESSAGE(ops::find(c) != nullptr, c);
    CHECK(ui::panels::registry().find("terrainTools"));
    CHECK(ui::actions::find("terrainTools.randomizeExtrude"));
    CHECK(ui::actions::find("window.terrainTools"));
    CHECK_FALSE(ui::menus::itemsIn("Mesh/Terrain Tools").empty());
}

TEST_CASE("blade extrude")
{
    fresh();
    const std::string plane = shrub();
    const json blades = info(plane);
    CHECK(blades.size() == 8);
    CHECK(run("meshData", {{"args", {plane}}})["faces"].size() == 16 + 8 * 8);
    for (const json& b : blades)
        CHECK(b["thickness"].get<double>() == doctest::Approx(3.0));

    const std::string cube = run("polyCube")[0];
    CHECK(info(cube).empty());

    run("select", {{"args", {plane}}});
    const json ranges = run("bladeExtrudeRanges", {{"spread", 0.5}});
    CHECK(ranges["thickness"][0].get<double>() == doctest::Approx(1.5));
    CHECK(ranges["thickness"][1].get<double>() == doctest::Approx(4.5));
    CHECK(ranges.size() == 3);

    const json report = run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 7}});
    CHECK(report["blades"] == 8);
    const std::vector<double> first = values(plane, "thickness");
    CHECK(std::all_of(first.begin(), first.end(), [](double v) { return v >= 1.0 && v <= 5.0; }));
    CHECK(distinct(first) > 4);

    run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 7}});
    CHECK(values(plane, "thickness") == first);
    run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 8}});
    CHECK(values(plane, "thickness") != first);

    run("setBladeExtrude", {{"args", {plane}}, {"blades", {0}}, {"thickness", 2.5}});
    CHECK(info(plane)[0]["thickness"].get<double>() == doctest::Approx(2.5));
    run("resetBladeExtrude", {{"args", {plane}}});
    for (double v : values(plane, "thickness"))
        CHECK(v == doctest::Approx(3.0));
}

TEST_CASE("blade extrude bakes the values into the geometry")
{
    fresh();
    const std::string plane = shrub();
    run("select", {{"args", {plane}}});
    run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 7}});
    const std::vector<double> t = tips(plane);
    CHECK(t.size() == 8);
    CHECK(distinct(t) > 4);
    CHECK(std::all_of(t.begin(), t.end(), [](double v) { return v >= 1.0 - 1e-6 && v <= 5.0 + 1e-6; }));
    for (const json& b : info(plane))
        CHECK(b["base"]["thickness"].get<double>() == doctest::Approx(3.0));

    run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 7}});
    CHECK(tips(plane) == t);
    run("randomizeBladeExtrude", {{"fields", fields(1.0, 5.0)}, {"up", "Y"}, {"seed", 9}});
    const std::vector<double> again = tips(plane);
    CHECK(std::all_of(again.begin(), again.end(), [](double v) { return v >= 1.0 - 1e-6 && v <= 5.0 + 1e-6; }));
    CHECK(again != t);

    run("undo");
    CHECK(tips(plane) == t);

    run("select", {{"clear", true}});
    CHECK(throws([] { run("randomizeBladeExtrude", {{"fields", fields(1.0, 2.0)}}); }));
    run("select", {{"args", {plane}}});
    CHECK(throws([] { run("randomizeBladeExtrude", {{"fields", json::object()}}); }));
    const std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    CHECK(throws([] { run("randomizeBladeExtrude", {{"fields", fields(1.0, 2.0)}}); }));
}

TEST_CASE("selecting what the blades made")
{
    fresh();
    const std::string plane = shrub();
    const json caps = run("bladeExtrudeInfo", {{"args", {plane}}});
    std::set<int> capIds, stemIds;
    for (const json& b : caps)
        for (const json& c : b["caps"])
            capIds.insert(c.get<int>());
    CHECK(capIds == std::set<int>{0, 1, 2, 3, 4, 5, 6, 7});

    run("select", {{"args", {plane}}});
    CHECK(run("selectBladeFaces") == 8);
    CHECK(run("componentSelection")[plane]["faces"].size() == 8);

    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {plane}}});
    CHECK(run("selectBladeFaces", {{"stems", true}}) == 8 + 8 * 8);
    const json selected = run("componentSelection");
    for (const json& f : selected[plane]["faces"])
        stemIds.insert(f.get<int>());
    for (int ground = 8; ground < 16; ++ground)
        CHECK_FALSE(stemIds.count(ground));
    for (int cap = 0; cap < 8; ++cap)
        CHECK(stemIds.count(cap));

    run("selectMode", {{"args", {"object"}}});
    const std::string cube = run("polyCube")[0];
    run("select", {{"args", {cube}}});
    CHECK(throws([] { run("selectBladeFaces"); }));
}

TEST_CASE("bias pulls the range up or down the mesh")
{
    fresh();
    const std::string cube = run("polyCube")[0];
    run("bladeExtrude", {{"args", {cube}}, {"faces", range(0, 5)}, {"thickness", 1.0}});
    run("select", {{"args", {cube}}});
    auto byHeight = [&] {
        std::vector<std::pair<double, double>> out;
        for (const json& b : info(cube))
            out.push_back({b["root"][1].get<double>(), b["thickness"].get<double>()});
        std::sort(out.begin(), out.end());
        return out;
    };
    run("randomizeBladeExtrude", {{"fields", fields(0.0, 10.0)}, {"bias", 1.0}, {"up", "Y"}, {"seed", 3}});
    const auto rising = byHeight();
    CHECK(rising.front().second < rising.back().second);
    CHECK(rising.front().second < 0.01);
    CHECK(rising.back().second > 9.99);
    run("randomizeBladeExtrude", {{"fields", fields(0.0, 10.0)}, {"bias", -1.0}, {"up", "Y"}, {"seed", 3}});
    const auto falling = byHeight();
    CHECK(falling.front().second > falling.back().second);
    run("randomizeBladeExtrude", {{"fields", fields(0.0, 10.0)}, {"bias", 0.0}, {"up", "Y"}, {"seed", 3}});
    const auto none = byHeight();
    CHECK(none != rising);
    CHECK(none != falling);
}

TEST_CASE("blades come apart however the faces touch")
{
    fresh();
    const std::string plane = shrub();
    const json blades = info(plane);
    REQUIRE(blades.size() == 8);
    std::set<int> all;
    size_t total = 0;
    for (const json& b : blades)
    {
        CHECK_FALSE(b["points"].empty());
        total += b["points"].size();
        for (const json& p : b["points"])
            all.insert(p.get<int>());
        CHECK(std::abs(b["root"][1].get<double>()) < 1e-6);
    }
    CHECK(total == all.size());

    Graph& g = Graph::current();
    g.evaluate();
    const MeshPtr m = g.find(plane)->as<MeshPtr>(MeshNode::kOutMesh);
    std::vector<geo::Index> picked;
    for (const json& f : towers(plane))
        picked.push_back(f.get<geo::Index>());
    CHECK(terraintools::faceRegions(*m, picked).size() == 1);

    run("selectComponents", {{"args", {plane}}, {"faces", towers(plane)}});
    CHECK(run("addBladeDeformer", {{"kind", "bend"}, {"up", "Y"}, {"seed", 5}}).size() == 8);
    run("undo");
    CHECK(run("addBladeDeformer", {{"kind", "bend"}, {"up", "Y"}, {"seed", 5}, {"perBlade", false}}).size() == 1);
}

TEST_CASE("deformers per blade")
{
    fresh();
    const std::string plane = shrub();
    run("selectComponents", {{"args", {plane}}, {"faces", towers(plane)}});
    const json before = points(plane);
    const json made = run("addBladeDeformer", {{"kind", "bend"}, {"up", "Y"}, {"seed", 5}, {"spin", true}, {"low", 0.0}, {"high", 0.0}});
    REQUIRE(made.size() == 8);
    Graph& g = Graph::current();
    for (const json& d : made)
        CHECK(g.find(d.get<std::string>())->type().name == "bendDeformer");
    Node* d0 = g.find(made[0].get<std::string>());
    CHECK(std::get<double>(g.value(g.plug(d0->path() + ".lowBound"))) == 0.0);
    CHECK(std::get<double>(g.value(g.plug(d0->path() + ".highBound"))) == 2.0);
    const std::string handle = g.node(g.source(g.plug(d0->path() + ".handleMatrix"))->node)->path();
    const json t = run("xform", {{"args", {handle}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    CHECK(std::abs(t[1].get<double>()) < 1e-3);

    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {plane}}});
    CHECK(run("randomizeBladeDeformers", {{"kind", "bend"}, {"low", 20.0}, {"high", 80.0}, {"up", "Y"}, {"seed", 4}}) == 8);
    std::vector<double> curves;
    for (const json& d : made)
        curves.push_back(std::get<double>(g.value(g.plug(d.get<std::string>() + ".angle"))) / 2.0);
    CHECK(std::all_of(curves.begin(), curves.end(), [](double c) { return c >= 20.0 && c <= 80.0; }));
    CHECK(distinct(curves) > 4);
    CHECK(biggestMove(before, points(plane)) > 1e-4);

    CHECK(run("bladeDeformers", {{"kind", "bend"}}).size() == 8);
    CHECK(run("bladeDeformers", {{"kind", "flare"}}).empty());
    run("select", {{"args", {handle}}});
    CHECK(run("bladeDeformers", {{"kind", "bend"}}) == json::array({made[0]}));

    struct Kind
    {
        const char* kind;
        double high;
    };
    for (const Kind spec : {Kind{"bend", 70.0}, {"flare", 0.2}, {"twist", 60.0}, {"sine", 0.3}, {"squash", 0.4}, {"wave", 0.3}})
    {
        CAPTURE(spec.kind);
        fresh();
        const std::string p = shrub(4);
        run("selectComponents", {{"args", {p}}, {"faces", towers(p, 4)}});
        const json b = points(p);
        const json one = run("addBladeDeformer", {{"kind", spec.kind}, {"perBlade", false}, {"up", "Y"}, {"seed", 2},
                                                  {"low", spec.high}, {"high", spec.high}});
        CHECK(one.size() == 1);
        CHECK(biggestMove(b, points(p)) > 1e-4);
        run("selectMode", {{"args", {"object"}}});
        run("select", {{"args", {p}}});
        CHECK(run("deleteBladeDeformers", {{"kind", spec.kind}}) == 1);
        CHECK(run("bladeDeformers", {{"kind", spec.kind}}).empty());
        CHECK(biggestMove(b, points(p)) < 1e-9);
    }
}

TEST_CASE("deformers take whole objects and converted components")
{
    fresh();
    const std::string plane = shrub(4);
    run("select", {{"args", {plane}}});
    CHECK(run("addBladeDeformer", {{"kind", "flare"}, {"perBlade", false}, {"up", "Y"}, {"seed", 1}}).size() == 1);
    run("selectComponents", {{"args", {plane}}, {"vertices", {0}}});
    CHECK(run("addBladeDeformer", {{"kind", "twist"}, {"perBlade", false}, {"up", "Y"}, {"seed", 1}}).size() == 1);
}

TEST_CASE("faces kept together grow one blade per connected region")
{
    fresh();
    const std::string a = run("polyCube")[0];
    const std::string b = run("polyCube")[0];
    run("move", {{"args", {b}}, {"by", {1000.0, 0.0, 0.0}}});
    const std::string merged = run("polyCombine", {{"args", {a, b}}});
    run("bladeExtrude", {{"args", {merged}}, {"faces", range(0, 11)}, {"thickness", 1.0}, {"keepFacesTogether", true}});
    CHECK(info(merged).size() == 2);
}

TEST_CASE("the blade maths is deterministic for a seed")
{
    terraintools::Rng a(42), b(42);
    for (int i = 0; i < 16; ++i)
        CHECK(a.uniform() == b.uniform());
    const std::vector<double> f = terraintools::fractions(std::vector<double>{2.0, 4.0, 3.0});
    CHECK(f == std::vector<double>{0.0, 1.0, 0.5});
    CHECK(terraintools::fractions(std::vector<double>{1.0, 1.0}) == std::vector<double>{0.5, 0.5});
    terraintools::Rng r(1);
    CHECK(terraintools::biased(r, 0.0, 10.0, 1.0, 1.0) == 10.0);
}
