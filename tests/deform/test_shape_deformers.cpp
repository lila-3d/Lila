#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

Graph& graph() { return Graph::current(); }

MeshPtr outMesh(const std::string& mesh)
{
    Graph& g = graph();
    g.evaluate();
    Node* n = g.find(mesh);
    REQUIRE(n);
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return m;
}

std::vector<glm::dvec3> points(const std::string& mesh)
{
    MeshPtr m = outMesh(mesh);
    return {m->positions().begin(), m->positions().end()};
}

bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) <= eps; }

bool samePoints(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, double eps = 1e-6)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!near(a[i], b[i], eps))
            return false;
    return true;
}

size_t nearestTo(const std::vector<glm::dvec3>& pts, const glm::dvec3& at)
{
    size_t best = 0;
    for (size_t p = 0; p < pts.size(); ++p)
        if (glm::length(pts[p] - at) < glm::length(pts[best] - at))
            best = p;
    return best;
}

std::string error(const std::string& node)
{
    graph().evaluate();
    return modifiers::asModifier(graph().find(node))->error();
}

fs::path tempFile(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_deform_tests";
    fs::create_directories(dir);
    return dir / name;
}

void reload(const std::string& name)
{
    std::string path = tempFile(name).string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
}

double maxEdgeError(const std::string& mesh, const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    MeshPtr m = outMesh(mesh);
    double worst = 0.0;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e))
        {
            geo::Index i = m->edgePoint(e, 0), j = m->edgePoint(e, 1);
            worst = std::max(worst, std::abs(glm::length(a[i] - a[j]) - glm::length(b[i] - b[j])) / glm::length(a[i] - a[j]));
        }
    return worst;
}

json idsWhere(const std::vector<glm::dvec3>& pts, const std::function<bool(const glm::dvec3&)>& pred)
{
    json out = json::array();
    for (size_t p = 0; p < pts.size(); ++p)
        if (pred(pts[p]))
            out.push_back(p);
    return out;
}
} // namespace

TEST_CASE("kelvinlet: identity at rest, the handle grabs, envelope and save keep it")
{
    run("newScene");
    std::string mesh = run("polyPlane", {{"name", "Sheet"}, {"width", 100}, {"depth", 100}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json made = run("kelvinlet", {{"args", {mesh}}, {"position", v3(0, 0, 0)}, {"radius", 10.0}});
    REQUIRE(made.size() == 2);
    std::string handle = made[0], k = made[1];
    CHECK(samePoints(points(mesh), rest, 1e-9));
    run("move", {{"args", {handle}}, {"by", v3(0, 10, 0)}});
    std::vector<glm::dvec3> out = points(mesh);
    size_t centre = nearestTo(rest, glm::dvec3(0.0));
    CHECK(near(out[centre], rest[centre] + glm::dvec3(0, 10, 0), 1e-9));
    size_t corner = nearestTo(rest, glm::dvec3(50, 0, 50));
    CHECK(out[corner].y > 0.0);
    CHECK(out[corner].y < 5.0);
    run("setAttr", {{"args", {k + ".falloff", 2}}});
    double fast = points(mesh)[corner].y;
    CHECK(fast < out[corner].y);
    run("setAttr", {{"args", {k + ".envelope", 0.0}}});
    CHECK(samePoints(points(mesh), rest, 1e-9));
    run("setAttr", {{"args", {k + ".envelope", 1.0}}});
    // Rotating the handle twists the sheet around it.
    run("setAttr", {{"args", {k + ".falloff", 0}}});
    run("move", {{"args", {handle}}, {"by", v3(0, -10, 0)}});
    run("setAttr", {{"args", {handle + ".rotateY", 45.0}}});
    std::vector<glm::dvec3> twisted = points(mesh);
    size_t side = nearestTo(rest, glm::dvec3(10, 0, 0));
    CHECK(twisted[side].z < rest[side].z - 1.0);
    CHECK(glm::length(twisted[side]) == doctest::Approx(glm::length(rest[side])));
    reload("kelvinlet.lila");
    CHECK(samePoints(points(mesh), twisted, 1e-6));
}

TEST_CASE("arap and laplacianDeform: anchors stay, handles drag their regions, rotation keeps lengths")
{
    for (const char* cmd : {"arap", "laplacianDeform"})
    {
        CAPTURE(cmd);
        run("newScene");
        std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 60}, {"subdivisionsHeight", 24}})[0];
        std::vector<glm::dvec3> rest = points(mesh);
        json top = idsWhere(rest, [](const glm::dvec3& p) { return p.y > 29.0; });
        json bottom = idsWhere(rest, [](const glm::dvec3& p) { return p.y < -29.0; });
        json made = run(cmd, {{"args", {mesh}}, {"handles", json::array({top})}, {"anchors", bottom}});
        REQUIRE(made.size() == 2);
        std::string d = made[0], handle = made[1];
        CHECK(error(d).empty());
        CHECK(samePoints(points(mesh), rest, 1e-6));
        run("move", {{"args", {handle}}, {"by", v3(10, 0, 0)}});
        std::vector<glm::dvec3> out = points(mesh);
        for (const json& p : top)
            CHECK(near(out[p.get<size_t>()], rest[p.get<size_t>()] + glm::dvec3(10, 0, 0)));
        for (const json& p : bottom)
            CHECK(near(out[p.get<size_t>()], rest[p.get<size_t>()]));
        size_t mid = nearestTo(rest, glm::dvec3(5, 0, 0));
        CHECK(out[mid].x > rest[mid].x + 2.0);
        CHECK(out[mid].x < rest[mid].x + 8.0);
        run("undo");
        CHECK(samePoints(points(mesh), rest, 1e-6));

        // With every point in a region or anchored to nothing else, one handle rotating carries the whole mesh rigidly.
        run("setDeformAnchors", {{"args", {d}}, {"points", json::array()}});
        run("setAttr", {{"args", {handle + ".rotateZ", 40.0}}});
        std::vector<glm::dvec3> turned = points(mesh);
        if (std::string(cmd) == "arap")
            CHECK(maxEdgeError(mesh, rest, turned) < 1e-5);
        CHECK_FALSE(samePoints(turned, rest, 1e-3));
        reload(std::string(cmd) + ".lila");
        CHECK(samePoints(points(mesh), turned, 1e-6));
        CHECK(run("deformHandles", {{"args", {d}}}).size() == 1);
    }
}

TEST_CASE("arap: handles from the component selection, added later, and rebinding")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 5}, {"height", 60}, {"subdivisionsHeight", 12}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json top = idsWhere(rest, [](const glm::dvec3& p) { return p.y > 29.0; });
    std::string d = run("arap", {{"args", {mesh}}, {"anchors", idsWhere(rest, [](const glm::dvec3& p) { return p.y < -29.0; })}})[0];
    run("select", {{"args", {mesh}}});
    run("selectComponents", {{"args", {mesh}}, {"vertices", top}});
    std::string h = run("addDeformHandle", {{"args", {mesh}}});
    run("selectMode", {{"args", {"object"}}});
    CHECK(run("deformHandles", {{"args", {d}}}).size() == 1);
    run("move", {{"args", {h}}, {"by", v3(0, 5, 0)}});
    std::vector<glm::dvec3> out = points(mesh);
    CHECK(near(out[top[0].get<size_t>()], rest[top[0].get<size_t>()] + glm::dvec3(0, 5, 0)));
    // Rebinding makes the handle's current pose its rest, so the mesh goes back to its input.
    run("rebindDeformer", {{"args", {d}}});
    CHECK(samePoints(points(mesh), rest, 1e-6));
    run("move", {{"args", {h}}, {"by", v3(0, -5, 0)}});
    CHECK(near(points(mesh)[top[0].get<size_t>()], rest[top[0].get<size_t>()] - glm::dvec3(0, 5, 0)));
}

TEST_CASE("bbw: point handles deform smoothly and bbwSkinWeights sets skin weights")
{
    run("newScene");
    std::string mesh = run("polyPlane", {{"name", "Sheet"}, {"width", 100}, {"depth", 100}, {"subdivisionsX", 12}, {"subdivisionsZ", 12}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json made = run("bbw", {{"args", {mesh}}, {"positions", json::array({v3(-50, 0, -50), v3(50, 0, 50), v3(50, 0, -50)})}});
    REQUIRE(made.size() == 4);
    std::string d = made[0], a = made[1], b = made[2];
    CHECK(error(d).empty());
    CHECK(samePoints(points(mesh), rest, 1e-6));
    run("move", {{"args", {a}}, {"by", v3(0, 20, 0)}});
    std::vector<glm::dvec3> out = points(mesh);
    CHECK(near(out[nearestTo(rest, {-50, 0, -50})], glm::dvec3(-50, 20, -50), 1e-4));
    CHECK(near(out[nearestTo(rest, {50, 0, 50})], glm::dvec3(50, 0, 50), 1e-4));
    double mid = out[nearestTo(rest, {0, 0, -50})].y;
    CHECK(mid > 1.0);
    CHECK(mid < 19.0);
    reload("bbw.lila");
    CHECK(samePoints(points(mesh), out, 1e-4));
    std::string added = run("addDeformHandle", {{"args", {d}}, {"position", v3(-50, 0, 50)}});
    CHECK(run("deformHandles", {{"args", {d}}}).size() == 4);
    CHECK(near(points(mesh)[nearestTo(rest, {-50, 0, 50})], glm::dvec3(-50, 0, 50), 1e-4));

    run("newScene");
    std::string tube = run("polyCylinder", {{"name", "Tube"}, {"radius", 5}, {"height", 100}, {"subdivisionsHeight", 20}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    std::string skin = run("skinCluster", {{"args", {"bone_a", tube}}});
    run("bbwSkinWeights", {{"args", {skin}}});
    std::vector<glm::dvec3> tp = points(tube);
    auto heaviest = [&](const glm::dvec3& at) {
        json q = run("skinPercent", {{"args", {skin}}, {"vertices", json::array({nearestTo(tp, at)})}});
        REQUIRE(q.size() == 1);
        std::string best;
        double bestW = -1.0, sum = 0.0;
        for (auto& [joint, w] : q[0]["weights"].items())
        {
            sum += w.get<double>();
            if (w.get<double>() > bestW)
            {
                bestW = w.get<double>();
                best = joint;
            }
        }
        CHECK(sum == doctest::Approx(1.0).epsilon(1e-4));
        return best;
    };
    CHECK(heaviest({5, -25, 0}).ends_with("bone_a"));
    CHECK(heaviest({5, 25, 0}).ends_with("bone_b"));
}

TEST_CASE("cage coordinates: Green and Somigliana are identities at rest and exact for rigid cage motion")
{
    for (const char* mode : {"green", "somigliana", "meanValue", "harmonic"})
    {
        CAPTURE(mode);
        run("newScene");
        std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}, {"subdivisionsAxis", 12}, {"subdivisionsHeight", 8}})[0];
        std::string cage = run("polyCube", {{"name", "Proxy"}, {"width", 30}, {"height", 30}, {"depth", 30}})[0];
        std::vector<glm::dvec3> rest = points(mesh);
        std::string d = run("cage", {{"args", {mesh, cage}}, {"coordinates", mode}})[0];
        CHECK(error(d).empty());
        CHECK(samePoints(points(mesh), rest, 1e-6));
        run("move", {{"args", {cage}}, {"by", v3(3, 0, -2)}});
        run("setAttr", {{"args", {cage + ".rotateY", 30.0}}});
        glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(3, 0, -2)) * glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0));
        std::vector<glm::dvec3> out = points(mesh);
        // Mean value and harmonic coordinates reproduce rotations only approximately; harmonic is limited by its grid.
        double tol = std::string(mode) == "meanValue" ? 1e-3 : std::string(mode) == "harmonic" ? 0.6 : 1e-4;
        for (size_t i = 0; i < rest.size(); ++i)
            CHECK(near(out[i], glm::dvec3(m * glm::dvec4(rest[i], 1.0)), tol));
        run("setAttr", {{"args", {cage + ".rotateY", 0.0}}});
        run("move", {{"args", {cage}}, {"by", v3(-3, 0, 2)}});
        // Squashing the cage: Somigliana's bulge pushes the sides out further.
        run("setAttr", {{"args", {cage + ".scaleY", 0.6}}});
        std::vector<glm::dvec3> squashed = points(mesh);
        size_t side = nearestTo(rest, {10, 0, 0});
        CHECK(std::abs(squashed[side].y - rest[side].y) < 0.5);
        CHECK(squashed[nearestTo(rest, {0, 10, 0})].y < 8.0);
        if (std::string(mode) == "somigliana")
        {
            run("setAttr", {{"args", {d + ".bulge", 0.0}}});
            double flat = points(mesh)[side].x;
            run("setAttr", {{"args", {d + ".bulge", 1.0}}});
            double bulged = points(mesh)[side].x;
            CHECK(bulged > flat);
        }
        reload(std::string("cage_") + mode + ".lila");
        CHECK(error(d).empty());
    }
    // Switching modes live on a coordinates binding.
    run("newScene");
    std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}, {"subdivisionsAxis", 12}, {"subdivisionsHeight", 8}})[0];
    std::string cage = run("polyCube", {{"name", "Proxy"}, {"width", 30}, {"height", 30}, {"depth", 30}})[0];
    std::string d = run("cage", {{"args", {mesh, cage}}, {"coordinates", "green"}})[0];
    run("setAttr", {{"args", {cage + ".scaleX", 1.5}}});
    std::vector<glm::dvec3> green = points(mesh);
    run("setAttr", {{"args", {d + ".coordinates", 0}}});
    CHECK(error(d).empty());
    CHECK_FALSE(samePoints(points(mesh), green, 1e-3));
}

TEST_CASE("simple set: shear, inflate, cast, smooth, warp, noise")
{
    run("newScene");
    std::string box = run("polyCube", {{"name", "Box"}, {"width", 20}, {"height", 20}, {"depth", 20}, {"subdivisionsX", 4}, {"subdivisionsY", 4},
                                       {"subdivisionsZ", 4}})[0];
    std::vector<glm::dvec3> rest = points(box);
    json sh = run("shear", {{"args", {box}}, {"params", {{"shearX", 0.0}}}});
    CHECK(samePoints(points(box), rest, 1e-9));
    run("setAttr", {{"args", {sh[1].get<std::string>() + ".shearX", 1.0}}});
    std::vector<glm::dvec3> sheared = points(box);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(sheared[i], rest[i] + glm::dvec3(rest[i].y, 0, 0), 1e-6));
    run("removeModifier", {{"args", {sh[1]}}});
    CHECK(samePoints(points(box), rest, 1e-9));

    std::string inf = run("inflate", {{"args", {box}}, {"amount", 2.0}})[0];
    std::vector<glm::dvec3> puffed = points(box);
    size_t faceCentre = nearestTo(rest, {0, 10, 0});
    CHECK(near(puffed[faceCentre], rest[faceCentre] + glm::dvec3(0, 2, 0), 1e-6));
    run("removeModifier", {{"args", {inf}}});

    json c = run("cast", {{"args", {box}}, {"radius", 15.0}});
    for (const glm::dvec3& p : points(box))
        CHECK(glm::length(p) == doctest::Approx(15.0));
    run("setAttr", {{"args", {c[1].get<std::string>() + ".factor", 0.0}}});
    CHECK(samePoints(points(box), rest, 1e-9));
    run("removeModifier", {{"args", {c[1]}}});

    std::string sm = run("smoothDeformer", {{"args", {box}}, {"mode", "laplacian"}})[0];
    CHECK_FALSE(samePoints(points(box), rest, 1e-3));
    run("setAttr", {{"args", {sm + ".mode", 2}}});
    CHECK(samePoints(points(box), rest, 1e-6));
    run("removeModifier", {{"args", {sm}}});

    json w = run("warp", {{"args", {box}}, {"position", v3(10, 10, 10)}, {"radius", 8.0}});
    REQUIRE(w.size() == 3);
    CHECK(samePoints(points(box), rest, 1e-9));
    run("move", {{"args", {w[1]}}, {"by", v3(5, 0, 0)}});
    std::vector<glm::dvec3> warped = points(box);
    size_t corner = nearestTo(rest, {10, 10, 10});
    CHECK(near(warped[corner], rest[corner] + glm::dvec3(5, 0, 0), 1e-6));
    size_t far = nearestTo(rest, {-10, -10, -10});
    CHECK(near(warped[far], rest[far], 1e-9));
    run("removeModifier", {{"args", {w[2]}}});

    std::string nz = run("noiseDeform", {{"args", {box}}, {"amplitude", 1.0}, {"frequency", 0.3}, {"seed", 4}})[0];
    std::vector<glm::dvec3> noisy = points(box);
    CHECK_FALSE(samePoints(noisy, rest, 1e-3));
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(glm::length(noisy[i] - rest[i]) <= 1.5);
    run("setAttr", {{"args", {nz + ".seed", 5}}});
    CHECK_FALSE(samePoints(points(box), noisy, 1e-3));
    run("setAttr", {{"args", {nz + ".seed", 4}}});
    CHECK(samePoints(points(box), noisy, 0.0));
}

TEST_CASE("tension is zero at rest and reads stretch through the stack")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 60}, {"subdivisionsHeight", 12}})[0];
    json sq = run("nonlinear", {{"args", {mesh}}, {"type", "squash"}, {"params", {{"factor", 0.0}}}});
    std::string t = run("tension", {{"args", {mesh}}})[0];
    MeshPtr m = outMesh(mesh);
    auto values = m->pointAttrs().get<float>("tension");
    REQUIRE(values.size() == m->pointCapacity());
    for (float v : values)
        CHECK(v == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(geo::colors::has(*m, "tension"));
    run("setAttr", {{"args", {sq[1].get<std::string>() + ".factor", 0.5}}});
    m = outMesh(mesh);
    values = m->pointAttrs().get<float>("tension");
    double sum = 0.0;
    for (float v : values)
        sum += v;
    CHECK(sum > 0.0);
    reload("tension.lila");
    CHECK(outMesh(mesh)->pointAttrs().get<float>("tension").size() == m->pointCapacity());
}
