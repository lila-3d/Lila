#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <cmath>
#include <filesystem>
#include <random>

using namespace lila;
using geo::Index;
using json = nlohmann::json;
using namespace testutil;

namespace
{
struct Case
{
    int count;
    bool mirror;
};
const Case kCases[] = {{3, false}, {6, false}, {8, false}, {6, true}};

std::string name(const Case& c)
{
    return (c.mirror ? "D" : "C") + std::to_string(c.count);
}

/** A 24-sided open tube: every tested count divides its columns. */
std::string tube()
{
    return run("polyCylinder", {{"subdivisionsAxis", 24}, {"subdivisionsHeight", 3}, {"caps", false}})[0];
}

void setRadial(const std::string& mesh, const Case& c)
{
    run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", c.count}, {"mirror", c.mirror}, {"radialAxis", "y"}});
}
} // namespace

TEST_CASE("radial symmetry: random edits stay exactly symmetric")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        run("newScene");
        const std::string mesh = tube();
        setRadial(mesh, c);
        std::mt19937 rng(77);
        auto pick = [&](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
        std::uniform_real_distribution<double> d(-1.0, 1.0);
        for (int i = 0; i < 120; ++i)
        {
            json info = run("meshInfo", {{"args", {mesh}}});
            const int points = info["points"].get<int>(), faceCount = info["faces"].get<int>();
            const int op = pick(12);
            if (op == 0 && faceCount < 600)
            {
                run("selectComponents", {{"args", {mesh}}, {"faces", {pick(faceCount)}}});
                run("polyExtrude", {{"thickness", 2.0 + d(rng)}});
            }
            else if (op == 1 && faceCount < 600)
            {
                run("selectComponents", {{"args", {mesh}}, {"faces", {pick(faceCount)}}});
                run("polyInset", {{"thickness", 0.5}});
            }
            else
            {
                json verts = json::array();
                for (int k = 0, n = 1 + pick(3); k < n; ++k)
                    verts.push_back(pick(points));
                run("selectComponents", {{"args", {mesh}}, {"vertices", verts}});
                if (op % 3 == 0)
                    run("moveComponents", {{"by", {d(rng) * 3.0, d(rng) * 3.0, d(rng) * 3.0}}});
                else if (op % 3 == 1)
                    run("rotateComponents", {{"by", {d(rng) * 20.0, d(rng) * 20.0, d(rng) * 20.0}}});
                else
                    run("scaleComponents", {{"by", {1.0 + d(rng) * 0.1, 1.0 + d(rng) * 0.1, 1.0 + d(rng) * 0.1}}});
            }
        }
        json check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
        CHECK(check["axis"] == "radial");
        CHECK(check["count"].get<int>() == c.count);
        CHECK(check["maxAsymmetry"].get<double>() < 1e-9);
        CHECK(check["unmatched"].get<int>() == 0);
        CHECK(check["withoutStoredPartner"].get<int>() == 0);
    }
}

TEST_CASE("radial symmetry: a component move is turned (and mirrored) into every wedge")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        run("newScene");
        const std::string mesh = tube();
        setRadial(mesh, c);
        const geo::Mesh before = meshOf(mesh);
        // A point off every mirror plane: the middle of a column pair isn't a vertex, so take one and move it sideways.
        run("selectComponents", {{"args", {mesh}}, {"vertices", {30}}});
        run("moveComponents", {{"by", {1.0, 2.0, 0.5}}});
        geo::Mesh after = meshOf(mesh);
        const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(*scene::Graph::current().find(mesh));
        REQUIRE(spec);
        REQUIRE(spec->radial);
        geo::SymmetryMap map = geo::SymmetryMap::persistent(after, *spec->radial, 1e-4);
        const glm::dvec3 delta = after.position(30) - before.position(30);
        CHECK(glm::length(delta) > 0.5);
        for (int g = 1; g < map.order(); ++g)
        {
            const Index q = map.image(g, 30);
            REQUIRE(q != geo::kInvalid);
            if (q == 30)
                continue;
            CHECK(glm::length(after.position(q) - map.transform(g, after.position(30))) < 1e-9);
        }
        Index moved = 0;
        for (Index p = 0; p < after.pointCapacity(); ++p)
            moved += glm::length(after.position(p) - before.position(p)) > 1e-9;
        CHECK(moved == Index(map.expandPoints(std::vector<Index>{30}).size()));
    }
}

TEST_CASE("symmetrizeRadial rebuilds one wedge round the axis and turns radial symmetry on")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        run("newScene");
        const std::string sphere = run("polySphere", {{"subdivisionsAxis", 24}, {"subdivisionsHeight", 8}})[0];
        // Spoil the mesh with symmetry off, then rebuild it from wedge 0.
        for (int v = 0; v < 40; v += 7)
        {
            run("selectComponents", {{"args", {sphere}}, {"vertices", {v}}});
            run("moveComponents", {{"by", {0.0, 1.5, 0.0}}});
        }
        run("select", {{"args", {sphere}}});
        run("selectMode", {{"args", {"object"}}});
        json out = run("symmetrizeRadial", {{"args", {sphere}}, {"count", c.count}, {"mirror", c.mirror}, {"radialAxis", "y"}, {"wedge", 0}});
        CHECK(out[sphere].get<int>() > 0);
        scene::Node* n = scene::Graph::current().find(sphere);
        CHECK(n->as<int64_t>(scene::MeshNode::kSymmetry) == scene::kSymmetryRadial);
        CHECK(n->as<int64_t>(scene::MeshNode::kRadialCount) == c.count);
        const geo::Mesh m = meshOf(sphere);
        CHECK(m.validate(geo::kValidateDefault).empty());
        json check = run("symmetryCheck", {{"args", {sphere}}})[sphere];
        CHECK(check["maxAsymmetry"].get<double>() < 1e-9);
        CHECK(check["unmatched"].get<int>() == 0);
        CHECK(check["stored"].get<bool>());
    }
}

TEST_CASE("radial symmetry mismatch: unmatched points and the count that fits")
{
    run("newScene");
    const std::string mesh = tube();
    run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", 5}});
    json check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
    CHECK(check["unmatched"].get<int>() > 0);
    CHECK(check["unmatchedPoints"].size() == check["unmatched"].get<size_t>());
    CHECK(check["bestCount"].get<int>() == 24);
    run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", 8}});
    check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
    CHECK(check["unmatched"].get<int>() == 0);
    CHECK(check["unmatchedPoints"].empty());
}

TEST_CASE("radial orbits survive save, load and drift with symmetry off")
{
    run("newScene");
    const std::string mesh = tube();
    setRadial(mesh, {6, true});
    run("selectComponents", {{"args", {mesh}}, {"vertices", {3}}});
    run("moveComponents", {{"by", {0.0, 1.0, 0.0}}}); // stores the orbits
    const std::string layer = geo::radialLayerName(*scene::symmetrySpec(*scene::Graph::current().find(mesh))->radial);
    CHECK(meshOf(mesh).pointAttrs().has(layer));

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "radial_orbits.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(meshOf(mesh).pointAttrs().has(layer));

    run("setSymmetry", {{"args", {mesh}}, {"axis", "off"}});
    for (int v = 0; v < 10; ++v)
    {
        run("selectComponents", {{"args", {mesh}}, {"vertices", {v * 5}}});
        run("moveComponents", {{"by", {2.0, -1.0, 3.0}}});
    }
    setRadial(mesh, {6, true});
    json check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
    CHECK(check["withoutStoredPartner"].get<int>() == 0);
    CHECK(check["maxAsymmetry"].get<double>() > 1.0);
    run("symmetrize", {{"args", {mesh}}, {"wedge", 0}});
    check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
    CHECK(check["maxAsymmetry"].get<double>() < 1e-9);
}

TEST_CASE("setSymmetryPlane with radial symmetry puts the axis through the selection's centre")
{
    run("newScene");
    const std::string mesh = tube();
    json all = json::array();
    for (int v = 0; v < 96; ++v)
        all.push_back(v);
    run("selectComponents", {{"args", {mesh}}, {"vertices", all}});
    run("moveComponents", {{"by", {10.0, 0.0, 0.0}}});
    run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", 24}});
    CHECK(run("symmetryCheck", {{"args", {mesh}}})[mesh]["unmatched"].get<int>() > 0);
    // The first 24 points are the bottom ring.
    json ring = json::array();
    for (int v = 0; v < 24; ++v)
        ring.push_back(v);
    run("selectComponents", {{"args", {mesh}}, {"vertices", ring}});
    json out = run("setSymmetryPlane");
    CHECK(out[mesh][0] == "radial");
    scene::Node* n = scene::Graph::current().find(mesh);
    CHECK(n->as<int64_t>(scene::MeshNode::kSymmetryPlane) == 1);
    const glm::dvec3 centre = n->as<glm::dvec3>(scene::MeshNode::kRadialCenter);
    CHECK(centre.x == doctest::Approx(10.0));
    CHECK(std::abs(centre.z) < 1e-9);
    const json check = run("symmetryCheck", {{"args", {mesh}}})[mesh];
    CHECK(check["unmatched"].get<int>() == 0);
    CHECK(check["maxAsymmetry"].get<double>() < 1e-6);
}

TEST_CASE("toggleSymmetry turns radial symmetry on and off")
{
    run("newScene");
    const std::string mesh = tube();
    CHECK(run("toggleSymmetry", {{"args", {mesh}}, {"axis", "radial"}}) == "radial");
    CHECK(scene::Graph::current().find(mesh)->as<int64_t>(scene::MeshNode::kSymmetry) == scene::kSymmetryRadial);
    CHECK(run("toggleSymmetry", {{"args", {mesh}}}) == "off");
    CHECK(run("toggleSymmetry", {{"args", {mesh}}}) == "radial");
}
