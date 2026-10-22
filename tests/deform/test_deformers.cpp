#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "geo/Mesh.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/matrix_transform.hpp>
#include <ufbx.h>

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }

Graph& graph() { return Graph::current(); }

std::vector<glm::dvec3> points(const std::string& mesh)
{
    Graph& g = graph();
    g.evaluate();
    Node* n = g.find(mesh);
    REQUIRE(n);
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
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

/** Replaces a mesh's base points: point p moves by offset(p). */
void editPoints(const std::string& mesh, const std::function<glm::dvec3(geo::Index, const glm::dvec3&)>& offset)
{
    Graph& g = graph();
    NodeId id = g.find(mesh)->id();
    PlugRef plug = modifiers::editPlug(g, id);
    auto m = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value(plug)));
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        m->setPosition(p, m->position(p) + offset(p, m->position(p)));
    g.setValue(plug, MeshPtr(m));
}

fs::path tempFile(const std::string& name)
{
    fs::path dir = fs::temp_directory_path() / "lila_deform_tests";
    fs::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("blendShape: weights, envelope, weight map, live targets and undo")
{
    run("newScene");
    std::string base = run("polyCube", {{"name", "Base"}})[0];
    std::string smile = run("polyCube", {{"name", "Smile"}})[0];
    std::string frown = run("polyCube", {{"name", "Frown"}})[0];
    editPoints(smile, [](geo::Index p, const glm::dvec3&) { return p == 0 ? glm::dvec3(0, 10, 0) : glm::dvec3(0.0); });
    std::vector<glm::dvec3> rest = points(base);

    std::string bs = run("blendShape", {{"args", {smile, base}}});
    CHECK(run("deformers", {{"args", {base}}}) == json::array({bs}));
    json targets = run("blendTargets", {{"args", {bs}}});
    REQUIRE(targets.size() == 1);
    CHECK(targets[0]["name"] == "Smile");
    std::string smileTarget = targets[0]["node"];
    CHECK(samePoints(points(base), rest));

    run("setAttr", {{"args", {smileTarget + ".weight", 1.0}}});
    CHECK(near(points(base)[0], rest[0] + glm::dvec3(0, 10, 0)));
    CHECK(near(points(base)[1], rest[1]));
    run("setAttr", {{"args", {smileTarget + ".weight", 0.5}}});
    CHECK(near(points(base)[0], rest[0] + glm::dvec3(0, 5, 0)));
    run("setAttr", {{"args", {bs + ".envelope", 0.5}}});
    CHECK(near(points(base)[0], rest[0] + glm::dvec3(0, 2.5, 0)));
    run("setAttr", {{"args", {bs + ".envelope", 1.0}}});
    run("setDeformerWeights", {{"args", {bs}}, {"points", {0}}, {"value", 0.0}});
    CHECK(near(points(base)[0], rest[0]));
    CHECK(run("deformerWeights", {{"args", {bs}}})[0].get<double>() == 0.0);
    run("undo");
    CHECK(near(points(base)[0], rest[0] + glm::dvec3(0, 5, 0)));

    // A live target follows edits to its mesh.
    json added = run("addBlendTarget", {{"args", {base, frown}}, {"live", true}});
    REQUIRE(added.size() == 1);
    std::string frownTarget = added[0];
    run("setAttr", {{"args", {frownTarget + ".weight", 1.0}}});
    CHECK(near(points(base)[3], rest[3]));
    editPoints(frown, [](geo::Index p, const glm::dvec3&) { return p == 3 ? glm::dvec3(-4, 0, 0) : glm::dvec3(0.0); });
    CHECK(near(points(base)[3], rest[3] + glm::dvec3(-4, 0, 0)));

    // Extracting rebuilds the full target shape.
    std::string extracted = run("extractBlendTarget", {{"args", {smileTarget}}});
    CHECK(near(points(extracted)[0], rest[0] + glm::dvec3(0, 10, 0)));

    run("removeBlendTarget", {{"args", {frownTarget}}});
    CHECK(run("blendTargets", {{"args", {base}}}).size() == 1);
    CHECK(near(points(base)[3], rest[3]));
    run("undo");
    CHECK(run("blendTargets", {{"args", {base}}}).size() == 2);
}

TEST_CASE("blendShape sits under the skinCluster, and deformers save and load")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    std::string target = run("polyCylinder", {{"name", "Bulge"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    editPoints(target, [](geo::Index, const glm::dvec3& p) { return glm::dvec3(p.x, 0, p.z) * 0.2; });
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_b"}});
    std::string skin = run("skinCluster", {{"args", {"bone_a", mesh}}});
    std::string bs = run("blendShape", {{"args", {target, mesh}}});
    json stack = run("modifiers", {{"args", {mesh}}});
    REQUIRE(stack.size() == 2);
    CHECK(stack[0] == bs);
    CHECK(stack[1] == skin);
    std::string t = run("blendTargets", {{"args", {bs}}})[0]["node"];
    run("setAttr", {{"args", {t + ".weight", 0.75}}});
    std::vector<glm::dvec3> before = points(mesh);

    std::string handle = run("nonlinear", {{"args", {mesh}}, {"type", "twist"}})[0];
    std::vector<glm::dvec3> twisted = points(mesh);
    CHECK_FALSE(samePoints(twisted, before));

    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = tempFile(std::string("deformers") + ext).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        CHECK(samePoints(points(mesh), twisted, 1e-4));
        CHECK(run("blendTargets", {{"args", {bs}}})[0]["weight"].get<double>() == doctest::Approx(0.75));
        CHECK(run("deformers", {{"args", {mesh}}}).size() == 2);
    }
    (void)handle;
}

TEST_CASE("nonlinear: rest is untouched, the handle drives it, falloff and envelope limit it")
{
    run("newScene");
    std::string mesh = run("polyCube", {{"name", "Pole"}, {"width", 10}, {"height", 100}, {"depth", 10}, {"subdivisionsY", 10}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json made = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 0.0}}}});
    REQUIRE(made.size() == 2);
    std::string handle = made[0], bend = made[1];
    CHECK(samePoints(points(mesh), rest));
    run("setAttr", {{"args", {bend + ".angle", 90.0}}});
    std::vector<glm::dvec3> bent = points(mesh);
    CHECK_FALSE(samePoints(bent, rest));
    // The bottom stays (low bound -1 is the bottom, bend is centred), the top curls toward +x.
    double topX = -1e9;
    for (const auto& p : bent)
        topX = std::max(topX, p.x);
    CHECK(topX > 10.0);

    run("setAttr", {{"args", {bend + ".envelope", 0.0}}});
    CHECK(samePoints(points(mesh), rest));
    run("setAttr", {{"args", {bend + ".envelope", 1.0}}});

    // A falloff radius well inside the mesh leaves the far ends alone.
    run("setAttr", {{"args", {bend + ".falloffRadius", 0.3}}});
    std::vector<glm::dvec3> limited = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        if (std::abs(rest[i].y) > 20.0)
            CHECK(near(limited[i], rest[i]));
    run("setAttr", {{"args", {bend + ".falloffRadius", 0.0}}});

    // Every kind evaluates and changes the mesh.
    for (const char* kind : {"twist", "taper", "squash", "sine", "wave"})
    {
        run("newScene");
        std::string m = run("polyCube", {{"name", "Box"}, {"width", 20}, {"height", 100}, {"depth", 20}, {"subdivisionsX", 6},
                                         {"subdivisionsY", 10}, {"subdivisionsZ", 6}})[0];
        std::vector<glm::dvec3> r = points(m);
        json res = run("nonlinear", {{"args", {m}}, {"type", kind}});
        CAPTURE(kind);
        CHECK_FALSE(samePoints(points(m), r));
        // Rotating the handle changes the result: the deformation lives in handle space.
        std::vector<glm::dvec3> a = points(m);
        run("setAttr", {{"args", {res[0].get<std::string>() + ".rotateZ", 30.0}}});
        CHECK_FALSE(samePoints(points(m), a));
    }
}

TEST_CASE("lattice: identity at rest, moving a lattice point or the lattice moves the mesh")
{
    run("newScene");
    std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    for (const char* interp : {"trilinear", "bernstein", "bspline"})
    {
        run("newScene");
        mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
        json made = run("lattice", {{"args", {mesh}}, {"divisions", v3(3, 3, 3)}, {"interpolation", interp}});
        REQUIRE(made.size() == 2);
        std::string lat = made[0];
        CAPTURE(interp);
        CHECK(samePoints(points(mesh), rest, 1e-6));
        // Raise the top layer of lattice points: the top of the ball rises, the bottom stays.
        editPoints(lat, [](geo::Index, const glm::dvec3& p) { return p.y > 0.4 ? glm::dvec3(0, 0.5, 0) : glm::dvec3(0.0); });
        std::vector<glm::dvec3> out = points(mesh);
        size_t top = 0, bottom = 0;
        for (size_t i = 0; i < rest.size(); ++i)
        {
            if (rest[i].y > rest[top].y)
                top = i;
            if (rest[i].y < rest[bottom].y)
                bottom = i;
        }
        CHECK(out[top].y > rest[top].y + 1.0);
        CHECK(std::abs(out[bottom].y - rest[bottom].y) < 1.0);
    }
    // Moving the whole lattice carries the mesh inside it.
    run("newScene");
    mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
    std::string lat = run("lattice", {{"args", {mesh}}})[0];
    run("move", {{"args", {lat}}, {"by", v3(0, 5, 0)}});
    std::vector<glm::dvec3> moved = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(moved[i], rest[i] + glm::dvec3(0, 5, 0), 1e-6));
}

TEST_CASE("wrap and cage follow their driver")
{
    for (const char* cmd : {"wrap", "cage"})
    {
        CAPTURE(cmd);
        run("newScene");
        std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
        std::string driver = run("polyCube", {{"name", "Proxy"}, {"width", 30}, {"height", 30}, {"depth", 30}})[0];
        std::vector<glm::dvec3> rest = points(mesh);
        json made = run(cmd, {{"args", {mesh, driver}}});
        REQUIRE(made.size() == 1);
        CHECK(samePoints(points(mesh), rest, 1e-6));
        run("move", {{"args", {driver}}, {"by", v3(3, 0, -2)}});
        std::vector<glm::dvec3> moved = points(mesh);
        for (size_t i = 0; i < rest.size(); ++i)
            CHECK(near(moved[i], rest[i] + glm::dvec3(3, 0, -2), 1e-6));
        // Pulling one side of the driver pulls that side of the mesh most.
        run("move", {{"args", {driver}}, {"by", v3(-3, 0, 2)}});
        editPoints(driver, [](geo::Index, const glm::dvec3& p) { return p.x > 0 ? glm::dvec3(6, 0, 0) : glm::dvec3(0.0); });
        std::vector<glm::dvec3> pulled = points(mesh);
        size_t right = 0, left = 0;
        for (size_t i = 0; i < rest.size(); ++i)
        {
            if (rest[i].x > rest[right].x)
                right = i;
            if (rest[i].x < rest[left].x)
                left = i;
        }
        CHECK(pulled[right].x - rest[right].x > pulled[left].x - rest[left].x + 1.0);
        for (const char* ext : {".lila", ".lilaa"})
        {
            std::string path = tempFile(std::string(cmd) + ext).string();
            run("saveScene", {{"args", {path}}});
            run("newScene");
            run("openScene", {{"args", {path}}});
            CHECK(samePoints(points(mesh), pulled, 1e-4));
        }
        // Rebinding makes the current state the rest.
        run("rebindDeformer", {{"args", {made[0]}}});
        CHECK(samePoints(points(mesh), rest, 1e-6));
    }
}

TEST_CASE("deltaMush keeps rest detail and smooths a bend")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Arm"}, {"radius", 5}, {"height", 100}, {"subdivisionsHeight", 20}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json made = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 120.0}, {"lowBound", 0.0}, {"highBound", 0.2}}}});
    std::vector<glm::dvec3> bent = points(mesh);
    std::string mush = run("deltaMush", {{"args", {mesh}}})[0];
    // Bound above the bend, the rest shape is the bent one, so it changes nothing.
    CHECK(samePoints(points(mesh), bent, 1e-6));
    // Bound at rest, it corrects the bend: the output differs from the raw bend but stays finite.
    run("setAttr", {{"args", {made[1].get<std::string>() + ".angle", 0.0}}});
    run("rebindDeformer", {{"args", {mush}}});
    CHECK(samePoints(points(mesh), rest, 1e-6));
    run("setAttr", {{"args", {made[1].get<std::string>() + ".angle", 120.0}}});
    std::vector<glm::dvec3> mushed = points(mesh);
    CHECK_FALSE(samePoints(mushed, bent, 1e-3));
    for (const auto& p : mushed)
        CHECK(std::isfinite(p.x + p.y + p.z));
    std::string path = tempFile("mush.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(samePoints(points(mesh), mushed, 1e-6));
}

TEST_CASE("softMod moves points near the handle and leaves far ones")
{
    run("newScene");
    std::string mesh = run("polyPlane", {{"name", "Sheet"}, {"width", 100}, {"depth", 100}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    std::vector<glm::dvec3> rest = points(mesh);
    json made = run("softMod", {{"args", {mesh}}, {"position", v3(0, 0, 0)}, {"radius", 30.0}});
    REQUIRE(made.size() == 2);
    CHECK(samePoints(points(mesh), rest));
    run("move", {{"args", {made[0]}}, {"by", v3(0, 10, 0)}});
    std::vector<glm::dvec3> out = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
    {
        double d = glm::length(rest[i]);
        if (d < 1e-6)
            CHECK(out[i].y == doctest::Approx(10.0));
        else if (d > 30.0)
            CHECK(near(out[i], rest[i]));
        else
            CHECK(out[i].y <= 10.0 + 1e-9);
    }
    // Radius 0 is a cluster: everything moves.
    run("setAttr", {{"args", {made[1].get<std::string>() + ".falloffRadius", 0.0}}});
    out = points(mesh);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(out[i], rest[i] + glm::dvec3(0, 10, 0)));
}

TEST_CASE("morph targets export to FBX with weight curves and import back")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    std::string target = run("polyCylinder", {{"name", "Bulge"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}})[0];
    editPoints(target, [](geo::Index, const glm::dvec3& p) { return glm::dvec3(p.x, 0, p.z) * 0.5; });
    std::vector<glm::dvec3> rest = points(mesh), full = points(target);
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_b"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    std::string bs = run("blendShape", {{"args", {target, mesh}}});
    std::string t = run("blendTargets", {{"args", {bs}}})[0]["node"];
    run("setKeyframe", {{"args", {t + ".weight"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {t + ".weight"}}, {"time", 1.0}, {"value", 1.0}});
    run("currentTime", {{"args", {0.5}}});

    std::string path = tempFile("morphs.fbx").string();
    run("exportFbx", {{"args", {path, mesh, "bone_a"}}, {"kind", "skeletal"}, {"start", 0}, {"end", 30}, {"profile", "generic"},
                      {"triangulate", false}, {"tangents", false}});

    ufbx_load_opts opts{};
    ufbx_error err{};
    ufbx_scene* fbx = ufbx_load_file(path.c_str(), &opts, &err);
    REQUIRE(fbx);
    REQUIRE(fbx->blend_deformers.count == 1);
    const ufbx_blend_deformer* bd = fbx->blend_deformers.data[0];
    REQUIRE(bd->channels.count == 1);
    const ufbx_blend_channel* ch = bd->channels.data[0];
    CHECK(std::string(ch->name.data, ch->name.length) == "Bulge");
    REQUIRE(ch->target_shape);
    CHECK(ch->target_shape->num_offsets > 0);
    double maxOffset = 0.0;
    for (size_t k = 0; k < ch->target_shape->num_offsets; ++k)
    {
        ufbx_vec3 o = ch->target_shape->position_offsets.data[k];
        maxOffset = std::max(maxOffset, std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z));
    }
    CHECK(maxOffset == doctest::Approx(5.0).epsilon(1e-3));
    // The mesh goes out neutral (weight 0), whatever the current weight is.
    const ufbx_mesh* um = nullptr;
    for (size_t i = 0; i < fbx->meshes.count; ++i)
        um = fbx->meshes.data[i];
    REQUIRE(um);
    double maxRadius = 0.0;
    for (size_t v = 0; v < um->vertices.count; ++v)
        maxRadius = std::max(maxRadius, std::hypot(um->vertices.data[v].x, um->vertices.data[v].z));
    CHECK(maxRadius == doctest::Approx(10.0).epsilon(1e-3));
    REQUIRE(fbx->anim_layers.count >= 1);
    CHECK(ufbx_find_anim_prop(fbx->anim_layers.data[0], &ch->element, "DeformPercent") != nullptr);
    ufbx_free_scene(fbx);

    // Importing brings the morph back as a blendShape target.
    run("newScene");
    run("importFbx", {{"args", {path}}});
    json imported = run("blendTargets", {{"args", {"Tube"}}});
    REQUIRE(imported.size() == 1);
    CHECK(imported[0]["name"] == "Bulge");
    std::string node = imported[0]["node"];
    run("setAttr", {{"args", {node + ".weight", 1.0}}});
    std::string ibs = run("deformers", {{"args", {"Tube"}}})[0];
    graph().evaluate();
    std::vector<deformers::MorphTarget> morphs = deformers::morphTargets(graph(), graph().find(ibs)->id());
    REQUIRE(morphs.size() == 1);
    double maxDelta = 0.0;
    for (const glm::dvec3& d : morphs[0].deltas)
        maxDelta = std::max(maxDelta, glm::length(d));
    CHECK(maxDelta == doctest::Approx(5.0).epsilon(1e-3));
    (void)rest;
    (void)full;
}
