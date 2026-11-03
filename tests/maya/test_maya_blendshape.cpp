#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "maya/Maya.h"
#include "maya/MayaFile.h"
#include "scene/Deformers.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Skin.h"

#include <ufbx.h>

#include <bit>
#include <filesystem>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace mayatest;
namespace fs = std::filesystem;

namespace
{
maya::ImportResult loadFresh(const std::string& name)
{
    io::refs::clear();
    Graph& g = Graph::current();
    g.clear();
    maya::ImportResult r = maya::importMaya(g, fixture(name));
    if (!r.ok)
        MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

glm::dvec3 worldPoint(const std::string& mesh, geo::Index p)
{
    Graph& g = Graph::current();
    scene::Node* n = node(mesh);
    MeshPtr m = std::get<MeshPtr>(g.value({n->id(), MeshNode::kOutMesh, -1}));
    REQUIRE(m);
    glm::dmat4 w = std::get<glm::dmat4>(g.value({n->id(), Transform::kWorldMatrix, -1}));
    return glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
}

void checkVecTight(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    INFO("got " << a.x << " " << a.y << " " << a.z << ", want " << b.x << " " << b.y << " " << b.z);
    CHECK(glm::length(a - b) < eps);
}

/** The morph's targets by export name. */
std::map<std::string, NodeId> targetsByName(NodeId morph)
{
    Graph& g = Graph::current();
    std::map<std::string, NodeId> out;
    for (NodeId t : morphs::targetNodes(g, morph))
        out[g.node(t)->as<std::string>(morphs::kMorphName)] = t;
    return out;
}

void setWeight(NodeId target, double w) { Graph::current().setValue({target, morphs::kWeight, -1}, w); }

std::string be32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string be64(uint64_t v) { return be32(uint32_t(v >> 32)) + be32(uint32_t(v)); }
std::string f64(double d) { return be64(std::bit_cast<uint64_t>(d)); }

/** A FOR8 leaf chunk, padded to 8. */
std::string leaf(const char* tag, const std::string& data)
{
    std::string s = std::string(tag, 4) + be32(0) + be64(data.size()) + data;
    s.resize(16 + (data.size() + 7) / 8 * 8, '\0');
    return s;
}

std::string group(const char* tag, const char* form, const std::string& kids)
{
    std::string s = std::string(tag, 4) + be32(0) + be64(4 + kids.size()) + std::string(form, 4) + kids;
    s.resize(16 + (4 + kids.size() + 3) / 4 * 4, '\0');
    return s;
}
} // namespace

TEST_CASE("maya binary: blendShape point arrays and attribute aliases decode as in ASCII")
{
    std::string ipt = std::string("it[0].itg[0].iti[6000].ipt") + '\0' + char(0x20);
    for (double d : {0.0, 1.0, 0.0, 1.0, 2.0, 0.0, -1.0, 1.0})
        ipt += f64(d);
    std::string aal = std::string("aal") + '\0' + char(0x20) + be32(4);
    for (const char* s : {"smile", "weight[0]", "frown", "weight[1]"})
        aal += std::string(s) + '\0';
    std::string node = leaf("CREA", std::string(1, '\x80') + "faceBlend" + '\0') + leaf("PNT#", ipt) + leaf("ATAL", aal);
    std::string file = group("FOR8", "Maya", group("FOR8", "FBLS", node));

    maya::File f = maya::readBinary(std::span(reinterpret_cast<const uint8_t*>(file.data()), file.size()));
    REQUIRE(f.nodes.size() == 1);
    CHECK(f.nodes[0].type == "blendShape");
    REQUIRE(f.nodes[0].attrs.size() == 2);
    const maya::AttrValue& points = f.nodes[0].attrs[0];
    CHECK(points.type == "pointArray");
    CHECK(points.nums == std::vector<double>{0, 1, 0, 1, 2, 0, -1, 1});
    const maya::AttrValue& alias = f.nodes[0].attrs[1];
    CHECK(alias.type == "attributeAlias");
    CHECK(alias.strs == std::vector<std::string>{"smile", "weight[0]", "frown", "weight[1]"});
}

TEST_CASE("maya blendShape: inline targets, in-betweens, target and base weights, keyed and driven weights")
{
    maya::ImportResult r = loadFresh("blendshape.ma");
    Graph& g = Graph::current();
    REQUIRE(r.morphs.size() == 1);
    const NodeId morph = r.morphs[0];
    CHECK(g.node(morph)->name() == "faceBlend");
    CHECK(morphs::find(g, node("strip")->id()) == morph);
    std::map<std::string, NodeId> t = targetsByName(morph);
    REQUIRE(t.size() == 5);
    for (const char* name : {"smile", "frown", "raise", "tilt", "scripted"})
        CHECK(t.contains(name));
    CHECK(contains(r.skipped, "an expression driving faceBlend.w[4]"));
    CHECK_FALSE(contains(r.warnings, "blendShape"));
    CHECK_FALSE(contains(r.skipped, "attributeAliasList"));

    // Frame 0: smile at its in-between, raise from ctrl.ty = 1 through the driven key, tilt wired to ctrl.rz = 0.25.
    g.setTime(0.0);
    checkVecTight(worldPoint("strip", 4), {1, 2, 0});
    checkVecTight(worldPoint("strip", 5), {1, 2, 0});
    checkVecTight(worldPoint("strip", 1), {1, 1, 0});
    checkVecTight(worldPoint("strip", 3), {1, 1, 0.25});
    checkVecTight(worldPoint("strip", 0), {0, 0, 0});

    // Frame 24: frown keyed to 1, scaled by the base weight at vertex 0 and the target weight at vertex 2.
    g.setTime(1.0);
    checkVecTight(worldPoint("strip", 0), {0, 0, 0.5});
    checkVecTight(worldPoint("strip", 2), {0, 1, 1});

    // Halfway between the in-between and the full shape.
    setWeight(t["smile"], 0.75);
    checkVecTight(worldPoint("strip", 4), {0.5, 2.5, 0});
    checkVecTight(worldPoint("strip", 5), {1, 2.5, 0});
    setWeight(t["smile"], 1.0);
    checkVecTight(worldPoint("strip", 4), {0, 3, 0});

    // The driven key follows its control.
    g.setValue({node("ctrl")->id(), Transform::kTranslate, 1}, 2.0);
    checkVecTight(worldPoint("strip", 1), {1, 2, 0});
    CHECK(g.source({t["raise"], morphs::kWeight, -1}).has_value());
    CHECK(drivers::isChannel(*g.node(g.source({t["raise"], morphs::kWeight, -1})->node)));

    // vtx[*] covers every point.
    setWeight(t["scripted"], 1.0);
    checkVecTight(worldPoint("strip", 5), {1, 3, 3});
    checkVecTight(worldPoint("strip", 0), {0, 0, 0.5 + 1.5});
}

TEST_CASE("maya blendShape: live target meshes stay live, others bake, world origin uses both placements")
{
    maya::ImportResult r = loadFresh("blendshape_live.ma");
    Graph& g = Graph::current();
    REQUIRE(r.morphs.size() == 2);
    std::map<std::string, NodeId> t = targetsByName(morphs::find(g, node("strip")->id()));
    REQUIRE(t.size() == 2);
    auto liveSrc = g.source({t["stretch"], morphs::kTargetMesh, -1});
    REQUIRE(liveSrc.has_value());
    CHECK(liveSrc->node == node("stretch")->id());
    CHECK_FALSE(g.source({t["push"], morphs::kTargetMesh, -1}).has_value());

    checkVecTight(worldPoint("strip", 5), {1, 3, 0});
    checkVecTight(worldPoint("strip", 0), {0, 0, -1});
    setWeight(t["stretch"], 0.5);
    checkVecTight(worldPoint("strip", 5), {2, 2, 0});

    // Moving a point of the live target moves the blend.
    setWeight(t["stretch"], 1.0);
    scene::Node* stretch = node("stretch");
    auto edited = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value({stretch->id(), MeshNode::kOutMesh, -1})));
    edited->setPosition(5, {1, 4, 0});
    g.setValue({stretch->id(), MeshNode::kInMesh, -1}, MeshPtr(edited));
    checkVecTight(worldPoint("strip", 5), {1, 4, 0});

    // World origin: the target sits 5 along X of the base, so half the weight moves the base 2.5.
    checkVecTight(worldPoint("strip2", 0), {2.5, 0, 10});
    checkVecTight(worldPoint("strip2", 5), {3.5, 2, 10});
}

TEST_CASE("maya blendShape: ahead of a skinCluster, posed like Maya, and exported to FBX as morph targets")
{
    maya::ImportResult r = loadFresh("blendshape_skin.ma");
    Graph& g = Graph::current();
    REQUIRE(r.skins.size() == 1);
    REQUIRE(r.morphs.size() == 1);
    const NodeId mesh = node("strip")->id();
    std::vector<NodeId> stack = modifiers::stack(g, mesh);
    auto at = [&](NodeId id) { return std::find(stack.begin(), stack.end(), id) - stack.begin(); };
    CHECK(at(r.morphs[0]) < at(r.skins[0]));
    CHECK_FALSE(contains(r.warnings, "after its skinCluster"));

    g.setTime(0.0);
    checkVecTight(worldPoint("strip", 5), {2, 2, 0});
    g.setTime(1.0); // frame 30 at ntsc: the elbow is at 90
    checkVecTight(worldPoint("strip", 5), {-1, 3, 0});
    checkVecTight(worldPoint("strip", 3), {0.25, 1.75, 0});

    fs::path path = fs::temp_directory_path() / "lila_maya_tests" / "blendshape_skin.fbx";
    fs::create_directories(path.parent_path());
    fbx::ExportOptions opts;
    opts.kind = fbx::ExportKind::Skeletal;
    opts.profile = fbx::AxisProfile::Generic;
    opts.start = 0.0;
    opts.end = 1.0;
    std::string error;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {mesh, node("root")->id()}, opts), path, opts, &error));
    ufbx_load_opts lo{};
    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_file(path.string().c_str(), &lo, &err);
    REQUIRE(scene);
    REQUIRE(scene->blend_deformers.count == 1);
    const ufbx_blend_deformer* bd = scene->blend_deformers.data[0];
    REQUIRE(bd->channels.count == 1);
    CHECK(std::string(bd->channels.data[0]->name.data) == "bulge");
    const ufbx_blend_shape* shape = bd->channels.data[0]->target_shape;
    REQUIRE(shape);
    REQUIRE(shape->num_offsets == 1);
    CHECK(glm::length(glm::dvec3(shape->position_offsets.data[0].x, shape->position_offsets.data[0].y, shape->position_offsets.data[0].z)) ==
          doctest::Approx(1.0));
    ufbx_free_scene(scene);
}

TEST_CASE("maya blendShape: the inline targets of a converted .lila still blend")
{
    fs::path out = fs::temp_directory_path() / "lila_maya_tests" / "blend_convert";
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out);
    io::refs::clear();
    Graph::current().clear();
    io::markClean();
    json res = ops::execute("mayaConvert", {{"source", fixture("blendshape.ma").generic_string()}, {"destination", (out / "blend.lila").generic_string()}});
    REQUIRE(res.size() == 1);
    CHECK_FALSE(res[0].contains("error"));
    io::load(out / "blend.lila");
    Graph& g = Graph::current();
    NodeId morph = morphs::find(g, node("strip")->id());
    REQUIRE(morph != kNoNode);
    std::map<std::string, NodeId> t = targetsByName(morph);
    REQUIRE(t.size() == 5);
    g.setTime(1.0);
    checkVecTight(worldPoint("strip", 0), {0, 0, 0.5});
    checkVecTight(worldPoint("strip", 2), {0, 1, 1});
    checkVecTight(worldPoint("strip", 4), {1, 2, 0});
    io::refs::clear();
    Graph::current().clear();
    io::setCurrentPath({});
    io::markClean();
}
