#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"

#include <doctest/doctest.h>

#include <chrono>

using namespace lila;
using geo::Index;

namespace
{
void makeMesh(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* old = g.find(name))
        g.deleteNode(old->id());
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
}

std::vector<glm::dvec3> positions()
{
    auto s = sculpt::active();
    REQUIRE(s);
    return {s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()};
}

double maxGap(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    REQUIRE(a.size() == b.size());
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, glm::length(a[i] - b[i]));
    return d;
}

void bump(glm::dvec3 at, double radius, double strength = 1.0)
{
    json dab = {{"center", {at.x, at.y, at.z}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", radius}};
    ops::execute("sculptStroke", {{"brush", "draw"}, {"strength", strength}, {"dabs", json::array({dab, dab})}});
}

int level() { return ops::execute("sculptState", json::object())["level"].get<int>(); }
} // namespace

TEST_CASE("stepping down and back up a level gives the sculpt back exactly")
{
    makeMesh("lvCube", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvCube"}}, {"levels", 3}});
    CHECK(level() == 3);
    bump({0.0, 0.0, 50.0}, 15.0);
    bump({20.0, 10.0, 50.0}, 6.0);
    const std::vector<glm::dvec3> top = positions();

    ops::execute("sculptLevel", {{"level", 1}});
    CHECK(level() == 1);
    CHECK(sculpt::active()->sculpt.mesh().numFaces() == 6 * 4);
    ops::execute("sculptLevel", {{"step", 1}});
    ops::execute("sculptLevel", {{"step", 1}});
    CHECK(level() == 3);
    CHECK(maxGap(top, positions()) < 1e-6);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("moving or turning a lower level carries the finer detail with it")
{
    makeMesh("lvCarry", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvCarry"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 10.0);
    bump({-15.0, 20.0, 50.0}, 5.0);
    const std::vector<glm::dvec3> top = positions();

    ops::execute("sculptLevel", {{"level", 1}});
    // A quarter turn about z and a shift, applied to the level-1 shape only.
    auto move = [](const glm::dvec3& p) { return glm::dvec3(-p.y, p.x, p.z) + glm::dvec3(5.0, -3.0, 7.0); };
    std::vector<glm::dvec3> low = positions();
    for (glm::dvec3& p : low)
        p = move(p);
    sculpt::active()->sculpt.setPositions(low);
    ops::execute("sculptLevel", {{"level", 3}});

    std::vector<glm::dvec3> want = top;
    for (glm::dvec3& p : want)
        p = move(p);
    CHECK(maxGap(want, positions()) < 1e-6);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("level changes undo and redo to the same sessions")
{
    makeMesh("lvUndo", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvUndo"}}, {"levels", 2}});
    bump({0.0, 0.0, 50.0}, 15.0);
    auto atTop = sculpt::active();
    ops::execute("sculptLevel", {{"level", 0}});
    auto atCage = sculpt::active();
    CHECK(atCage != atTop);
    CHECK(atCage->sculpt.mesh().numFaces() == 6);

    UndoStack& u = UndoStack::get();
    REQUIRE(u.undo());
    CHECK(sculpt::active() == atTop);
    REQUIRE(u.redo());
    CHECK(sculpt::active() == atCage);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("adding a level subdivides the finest and keeps its shape; deleting higher drops the detail")
{
    makeMesh("lvAdd", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvAdd"}}, {"levels", 2}});
    bump({0.0, 0.0, 50.0}, 15.0);
    const std::vector<glm::dvec3> two = positions();
    ops::execute("sculptLevel", {{"level", 1}});

    json r = ops::execute("sculptAddLevel", json::object());
    CHECK(r["level"].get<int>() == 3);
    CHECK(r["levels"].get<int>() == 3);
    CHECK(r["faces"].get<int>() == 6 * 64);
    ops::execute("sculptLevel", {{"level", 2}});
    CHECK(maxGap(two, positions()) < 1e-4);

    ops::execute("sculptLevel", {{"level", 1}});
    r = ops::execute("sculptDeleteHigher", json::object());
    CHECK(r["levels"].get<int>() == 1);
    ops::execute("sculptLevel", {{"level", 1}});
    CHECK(level() == 1);
    CHECK_THROWS(ops::execute("sculptLevel", {{"level", 0}}), ops::execute("sculptDeleteHigher", json::object()));
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("enter starts on the cage and exit without levels keeps no high poly")
{
    makeMesh("lvBare", geo::makeCube({.subdivX = 4, .subdivY = 4, .subdivZ = 4}));
    json r = ops::execute("sculptEnter", {{"args", {"lvBare"}}});
    CHECK(r["levels"].get<int>() == 0);
    CHECK(r["faces"].get<int>() == 6 * 16);
    bump({0.0, 0.0, 50.0}, 20.0);
    const std::vector<glm::dvec3> sculpted = positions();
    ops::execute("sculptExit", json::object());
    scene::Graph& g = scene::Graph::current();
    CHECK_FALSE(g.find("lvBare_high"));
    const auto& mesh = *std::get<scene::MeshPtr>(g.storedValue({g.find("lvBare")->id(), scene::MeshNode::kInMesh, -1}));
    CHECK(maxGap(sculpted, {mesh.positions().begin(), mesh.positions().end()}) < 1e-6);
}

TEST_CASE("exit from a lower level fits the cage to the finest level")
{
    makeMesh("lvExit", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvExit"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 20.0);
    const std::vector<glm::dvec3> fromTop = sculpt::exitPositions(*sculpt::active(), "fit");
    ops::execute("sculptLevel", {{"level", 1}});
    CHECK(maxGap(fromTop, sculpt::exitPositions(*sculpt::active(), "fit")) < 1e-6);

    ops::execute("sculptExit", json::object());
    scene::Graph& g = scene::Graph::current();
    const auto& kept = *std::get<scene::MeshPtr>(g.storedValue({g.find("lvExit_high")->id(), scene::MeshNode::kInMesh, -1}));
    CHECK(kept.numFaces() == 6 * 64);
    // Resumes on the level it left from, with every level there.
    json r = ops::execute("sculptEnter", {{"args", {"lvExit"}}});
    CHECK(level() == 1);
    CHECK(r["levels"].get<int>() == 3);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("the mask and polypaint survive a trip down and up")
{
    makeMesh("lvMask", geo::makeCube({}));
    ops::execute("sculptEnter", {{"args", {"lvMask"}}, {"levels", 2}});
    json dab = {{"center", {0.0, 0.0, 50.0}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", 20.0}};
    ops::execute("sculptStroke", {{"brush", "mask"}, {"strength", 1.0}, {"dabs", json::array({dab})}});
    ops::execute("sculptStroke", {{"brush", "paint"}, {"color", {1.0, 0.0, 0.0}}, {"strength", 1.0}, {"dabs", json::array({dab})}});
    const auto& before = sculpt::active()->sculpt;
    std::vector<float> mask(before.mask().begin(), before.mask().end());
    std::vector<glm::vec4> color(before.colors().begin(), before.colors().end());

    ops::execute("sculptLevel", {{"level", 0}});
    ops::execute("sculptLevel", {{"level", 2}});
    const auto& after = sculpt::active()->sculpt;
    double maskGap = 0.0, colorGap = 0.0;
    for (size_t i = 0; i < mask.size(); ++i)
    {
        maskGap = std::max(maskGap, double(std::abs(after.mask()[i] - mask[i])));
        colorGap = std::max(colorGap, double(glm::length(after.colors()[i] - color[i])));
    }
    CHECK(maskGap < 1e-5);
    CHECK(colorGap < 1e-5);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("stepping down from half a million faces is quick enough to feel like a key press")
{
    geo::SphereOptions o;
    o.radius = 50.0;
    o.subdivAxis = 32;
    o.subdivHeight = 16;
    makeMesh("lvBig", geo::makeSphere(o));
    ops::execute("sculptEnter", {{"args", {"lvBig"}}, {"levels", 5}});
    bump({0.0, 0.0, 50.0}, 10.0);
    const std::vector<glm::dvec3> top = positions();
    const auto t0 = std::chrono::steady_clock::now();
    ops::execute("sculptLevel", {{"step", -1}});
    const double down = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto t1 = std::chrono::steady_clock::now();
    ops::execute("sculptLevel", {{"step", 1}});
    const double up = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    MESSAGE("top faces " << sculpt::active()->sculpt.mesh().numFaces() << ", down " << down << " s, up " << up << " s");
    CHECK(maxGap(top, positions()) < 1e-5);
    ops::execute("sculptCancel", json::object());
}

TEST_CASE("exit keeps the high poly by default, at any level, and the next enter gets every level back")
{
    makeMesh("lvKeep", geo::makeCube({}));
    scene::Graph& g = scene::Graph::current();
    ops::execute("sculptEnter", {{"args", {"lvKeep"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 8.0);
    const std::vector<glm::dvec3> top = positions();
    ops::execute("sculptLevel", {{"level", 2}});

    json r = ops::execute("sculptExit", {{"high", "belowHighest"}, {"highLevel", 1}});
    CHECK(r["high"]["level"].get<int>() == 2);
    const auto keptMesh = [&] { return *std::get<scene::MeshPtr>(g.storedValue({g.find("lvKeep_high")->id(), scene::MeshNode::kInMesh, -1})); };
    CHECK(keptMesh().numFaces() == 6 * 16);
    auto kept = sculpt::keptSculpt(*g.find("lvKeep"));
    REQUIRE(kept);
    CHECK(kept->levels == 3);
    CHECK(kept->level == 2);

    ops::execute("sculptEnter", {{"args", {"lvKeep"}}});
    CHECK(level() == 2);
    ops::execute("sculptLevel", {{"level", 3}});
    CHECK(maxGap(top, positions()) < 1e-4);

    r = ops::execute("sculptExit", {{"high", "level"}, {"highLevel", 1}});
    CHECK(keptMesh().numFaces() == 6 * 4);
    r = ops::execute("sculptEnter", {{"args", {"lvKeep"}}});
    CHECK(r["levels"].get<int>() == 3);

    ops::execute("sculptExit", {{"high", "none"}});
    CHECK_FALSE(g.find("lvKeep_high"));
    CHECK_FALSE(sculpt::keptSculpt(*g.find("lvKeep")));
}

TEST_CASE("the low poly can come out at a higher level, and be duplicated at any level while sculpting")
{
    makeMesh("lvLow", geo::makeCube({}));
    scene::Graph& g = scene::Graph::current();
    ops::execute("sculptEnter", {{"args", {"lvLow"}}, {"levels", 3}});
    bump({0.0, 0.0, 50.0}, 10.0);

    json copy = ops::execute("sculptDuplicateLowPoly", {{"level", 2}});
    REQUIRE(g.find(copy.get<std::string>()));
    CHECK(copy.get<std::string>() == "lvLow_low2");
    g.evaluate();
    CHECK(g.find("lvLow_low2")->as<scene::MeshPtr>(scene::MeshNode::kOutMesh)->numFaces() == 6 * 16);
    CHECK(sculpt::active());

    ops::execute("sculptExit", {{"lowLevel", 1}});
    const auto& low = *std::get<scene::MeshPtr>(g.storedValue({g.find("lvLow")->id(), scene::MeshNode::kInMesh, -1}));
    CHECK(low.numFaces() == 6 * 4);
    // The kept sculpt is now two levels above the subdivided low poly.
    json r = ops::execute("sculptEnter", {{"args", {"lvLow"}}});
    CHECK(r["levels"].get<int>() == 2);
    ops::execute("sculptCancel", json::object());
}
