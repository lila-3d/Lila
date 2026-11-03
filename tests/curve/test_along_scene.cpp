#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
std::string modifierError(const std::string& mod)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    auto* m = scene::modifiers::asModifier(g.find(mod));
    REQUIRE(m);
    return m->error();
}
} // namespace

TEST_CASE("deformAlongCurve: live modifier follows the curve")
{
    run("newScene");
    std::string beam = run("polyCube", {{"width", 100}, {"height", 2}, {"depth", 2}, {"subdivisionsX", 10}})[0];
    std::string path = polyCurve({D3(0), D3(0, 0, 300)}, "Path");
    json mods = run("deformAlongCurve", {{"args", {beam}}, {"curve", path}});
    REQUIRE(mods.size() == 1);
    std::string mod = mods[0];
    CHECK(modifierError(mod).empty());
    geo::Bounds b = meshOf(beam)->bounds();
    CHECK(b.min.z == doctest::Approx(0.0));
    CHECK(b.max.z == doctest::Approx(100.0));
    CHECK(b.size().x == doctest::Approx(2.0));

    // Stretch to fit, then edit the curve: the mesh follows.
    run("setAttr", {{"args", {mod + ".stretch", true}}});
    CHECK(meshOf(beam)->bounds().max.z == doctest::Approx(300.0));
    run("move", {{"args", {path}}, {"by", {0, 40, 0}}});
    CHECK(meshOf(beam)->bounds().min.y == doctest::Approx(39.0));
    run("undo");
    run("setCurveData", {{"args", {path}}, {"splines", json::array({{{"type", "poly"}, {"points", pts({D3(0), D3(0, 0, 150)})}}})}});
    CHECK(meshOf(beam)->bounds().max.z == doctest::Approx(150.0));
    run("undo");
    CHECK(meshOf(beam)->bounds().max.z == doctest::Approx(300.0));

    // Saved and loaded with its connection.
    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    std::string file = (dir / "along.lila").string();
    run("saveScene", {{"args", {file}}});
    run("newScene");
    run("openScene", {{"args", {file}}});
    CHECK(meshOf(beam)->bounds().max.z == doctest::Approx(300.0));

    // Without a curve it reports why and passes the mesh through.
    run("newScene");
    beam = run("polyCube", {{"width", 100}, {"height", 2}, {"depth", 2}})[0];
    std::string bare = run("addModifier", {{"args", {beam}}, {"kind", "curveDeform"}});
    CHECK(!modifierError(bare).empty());
    CHECK(meshOf(beam)->bounds().max.x == doctest::Approx(50.0));
}

TEST_CASE("curveDeform from the modifier menu picks up the selected curve")
{
    run("newScene");
    std::string beam = run("polyCube", {{"width", 50}, {"height", 2}, {"depth", 2}})[0];
    std::string path = polyCurve({D3(0, 10, 0), D3(0, 10, 100)}, "Path");
    run("select", {{"args", {beam, path}}});
    std::string mod = run("addModifier", {{"args", {beam}}, {"kind", "curveDeform"}});
    CHECK(modifierError(mod).empty());
    CHECK(meshOf(beam)->bounds().min.y == doctest::Approx(9.0));
    run("select", {{"args", {beam}}});
    CHECK_THROWS_AS(run("deformAlongCurve", {{"args", {beam}}}), CommandError);
}

TEST_CASE("arrayAlongCurve: merged and instances")
{
    run("newScene");
    std::string post = run("polyCube", {{"width", 4}, {"height", 20}, {"depth", 4}})[0];
    std::string path = polyCurve({D3(0), D3(200, 0, 0)}, "Fence");
    json mods = run("arrayAlongCurve", {{"args", {post}}, {"curve", path}, {"fill", "spacing"}, {"spacing", 50.0}});
    std::string mod = mods[0];
    CHECK(modifierError(mod).empty());
    scene::MeshPtr m = meshOf(post);
    CHECK(m->numFaces() == 6 * 5);
    CHECK(m->bounds().max.x == doctest::Approx(202.0));
    run("setAttr", {{"args", {mod + ".endOffset", 100.0}}});
    CHECK(meshOf(post)->numFaces() == 6 * 3);
    run("undo");
    run("undo");
    CHECK(meshOf(post)->numFaces() == 6);

    // Instances: shared meshes under a group, placed along the curve, keeping the piece's settings.
    run("setAttr", {{"args", {post + ".visibility", false}}});
    json groups = run("arrayAlongCurve", {{"args", {post}}, {"curve", path}, {"count", 3}, {"output", "instances"}});
    REQUIRE(groups.size() == 1);
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* group = g.find(groups[0].get<std::string>());
    REQUIRE(group);
    std::vector<scene::NodeId> kids = g.children(group->id());
    REQUIRE(kids.size() == 3);
    for (size_t k = 0; k < kids.size(); ++k)
    {
        auto src = g.source({kids[k], scene::MeshNode::kInMesh, -1});
        REQUIRE(src);
        CHECK(src->node == g.find(post)->id());
        glm::dmat4 w = g.node(kids[k])->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        CHECK(w[3].x == doctest::Approx(100.0 * double(k)));
        CHECK(scene::instancing::kind(g, kids[k]) == scene::instancing::Kind::Instance);
        CHECK_FALSE(g.node(kids[k])->as<bool>(scene::Transform::kVisibility));
    }
    CHECK(scene::instancing::members(g, g.find(post)->id()).size() == 4);
    CHECK(scene::modifiers::stack(g, g.find(post)->id()).empty());
    CHECK_THROWS_AS(run("arrayAlongCurve", {{"args", {post}}, {"curve", path}, {"deform", true}, {"output", "instances"}}), CommandError);
}

TEST_CASE("flowAlongSurface: live onto a moved surface")
{
    run("newScene");
    std::string surface = run("polyPlane", {{"width", 200}, {"depth", 200}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("move", {{"args", {surface}}, {"by", {0, 50, 0}}});
    std::string tile = run("polyCube", {{"width", 20}, {"height", 10}, {"depth", 20}})[0];
    json mods = run("flowAlongSurface", {{"args", {tile}}, {"target", surface}});
    std::string mod = mods[0];
    CHECK(modifierError(mod).empty());
    // The tile's footprint covers the whole surface; its height (from y = 0) rides the normal.
    geo::Bounds b = meshOf(tile)->bounds();
    CHECK(b.size().x == doctest::Approx(200.0));
    CHECK(b.min.y == doctest::Approx(45.0));
    CHECK(b.max.y == doctest::Approx(55.0));
    run("move", {{"args", {surface}}, {"by", {0, 10, 0}}});
    CHECK(meshOf(tile)->bounds().min.y == doctest::Approx(55.0));

    // A quad patch picked by faces flows by grid.
    run("newScene");
    surface = run("polyPlane", {{"width", 200}, {"depth", 200}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    tile = run("polyCube", {{"width", 20}, {"height", 10}, {"depth", 20}})[0];
    mods = run("flowAlongSurface", {{"args", {tile}}, {"target", surface}, {"faces", {0, 1, 4, 5}}});
    scene::Node* fm = scene::Graph::current().find(mods[0].get<std::string>());
    CHECK(fm->as<int64_t>(fm->type().attrIndex("parametrization")) == 1);
    CHECK(meshOf(tile)->bounds().size().x == doctest::Approx(100.0));
    CHECK_THROWS_AS(run("flowAlongSurface", {{"args", {tile}}}), CommandError);
}

TEST_CASE("curve deformer commands undo")
{
    run("newScene");
    std::string c = polyCurve({D3(0), D3(25, 0, 0), D3(50, 0, 0), D3(75, 0, 0), D3(100, 0, 0)}, "Line");
    json before = run("curveData", {{"args", {c}}});
    run("bendCurve", {{"args", {c}}, {"angle", 90.0}});
    json bent = run("curveData", {{"args", {c}}});
    CHECK(bent != before);
    run("undo");
    CHECK(run("curveData", {{"args", {c}}}) == before);

    // Selected points only.
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({curve::componentId(0, 4, curve::PointRole::Point)})}});
    run("bendCurve", {{"angle", 90.0}});
    json after = run("curveData", {{"args", {c}}});
    CHECK(after[0]["points"][1] == before[0]["points"][1]);
    CHECK(after[0]["points"][4] != before[0]["points"][4]);
    run("scaleCurvature", {{"factor", 0.0}});
    run("smoothCurve", json::object());
    run("curlCurve", {{"args", {c}}, {"amplitude", 1.0}, {"turns", 2.0}});
    CHECK(run("curveData", {{"args", {c}}})[0]["points"].size() == 5);

    // Even spacing via rebuild keeps the point count.
    std::string u = polyCurve({D3(0), D3(1, 0, 0), D3(2, 0, 0), D3(100, 0, 0)}, "Uneven");
    run("rebuildCurve", {{"args", {u}}, {"keepPointCount", true}});
    json even = run("curveData", {{"args", {u}}});
    REQUIRE(even[0]["points"].size() == 4);
    CHECK(even[0]["points"][1][0].get<double>() == doctest::Approx(100.0 / 3.0));
}
