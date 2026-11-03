#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Graph.h"
#include "scene/LodGroup.h"
#include "scene/MeshNodes.h"

#include <ufbx.h>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;

namespace
{
Node* addCube(Graph& g, const std::string& name, double size, NodeId parent = kNoNode)
{
    Node* n = g.createNode(*types::find("mesh"), name, parent, false);
    g.setValue({n->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(geo::makeCube({.width = size, .height = size, .depth = size}))));
    return n;
}

/** Rock with three cube levels, offset so the group sits away from the origin. */
NodeId buildGroup(Graph& g, const std::string& sizes)
{
    Node* group = g.createNode(*types::find("lodGroup"), "Rock", kNoNode, false);
    g.setValue({group->id(), Transform::kTranslate, -1}, glm::dvec3(300, 0, 0));
    for (int i = 0; i < 3; ++i)
        addCube(g, "Rock_LOD" + std::to_string(i), 100.0 - 20.0 * i, group->id());
    g.setValue({group->id(), LodGroup::kScreenSizes, -1}, sizes);
    g.evaluate();
    return group->id();
}

std::string exported(Graph& g, NodeId root, const std::string& file, fbx::ExportKind kind = fbx::ExportKind::Static)
{
    fbx::ExportOptions o;
    o.kind = kind;
    o.atOrigin = true;
    fs::path path = tempFile(file);
    std::string err;
    REQUIRE(fbx::writeFbx(fbx::gatherExport(g, {root}, o), path, o, &err));
    return path.string();
}
} // namespace

TEST_CASE("LOD screen sizes follow Unreal's rules")
{
    CHECK(lod::autoScreenSize(0, 2.0) == doctest::Approx(1.0));
    CHECK(lod::autoScreenSize(1, 1.0) == doctest::Approx(0.75));
    CHECK(lod::autoScreenSize(2, 0.75) == doctest::Approx(0.5625));
    CHECK(lod::autoScreenSize(2, 0.3) == doctest::Approx(0.29));

    CHECK(lod::parseSizes("1 0.5, 0.25") == std::vector<double>{1.0, 0.5, 0.25});
    CHECK(lod::parseSizes("[1, x, 0.2]") == std::vector<double>{1.0, 0.2});
    CHECK(lod::formatSizes({1.0, 0.4}) == "1 0.4");

    const std::vector<double> sizes = {1.0, 0.5, 0.25};
    CHECK(lod::levelForScreenSize(sizes, 2.0) == 0);
    CHECK(lod::levelForScreenSize(sizes, 0.6) == 0);
    CHECK(lod::levelForScreenSize(sizes, 0.4) == 1);
    CHECK(lod::levelForScreenSize(sizes, 0.1) == 2);

    lod::View v;
    v.proj = glm::dmat4(1.0);
    v.proj[0][0] = v.proj[1][1] = 2.0;
    v.eye = glm::dvec3(0, 0, 100);
    // 0.5 * 2 * radius 10 over distance 100, doubled.
    CHECK(lod::boundsScreenSize(glm::dvec3(0.0), 10.0, v) == doctest::Approx(0.2));
}

TEST_CASE("views show one LOD level by screen size, a fixed level, or all")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    const NodeId group = buildGroup(g, "1 0.5 0.25");
    const std::vector<NodeId> lv = lod::levels(g, group);
    REQUIRE(lv.size() == 3);
    CHECK(lod::screenSizes(*g.node(group), 4) == std::vector<double>{1.0, 0.5, 0.25, lod::autoScreenSize(3, 0.25)});

    lod::View v;
    v.proj = glm::dmat4(1.0);
    v.proj[0][0] = v.proj[1][1] = 2.0;
    // LOD0 is a 100 cm cube, so its bounding sphere radius is about 86.6 cm.
    v.eye = glm::dvec3(300, 0, 150);
    CHECK(lod::shownLevel(g, group, v) == 0);
    v.eye = glm::dvec3(300, 0, 500);
    CHECK(lod::shownLevel(g, group, v) == 1);
    v.eye = glm::dvec3(300, 0, 5000);
    CHECK(lod::shownLevel(g, group, v) == 2);
    std::unordered_set<NodeId> hidden = lod::hidden(g, v);
    CHECK(hidden.contains(lv[0]));
    CHECK(hidden.contains(lv[1]));
    CHECK_FALSE(hidden.contains(lv[2]));

    v.mode = lod::Mode::Level;
    v.level = 1;
    CHECK(lod::shownLevel(g, group, v) == 1);
    v.level = 7;
    CHECK(lod::shownLevel(g, group, v) == 2);
    v.mode = lod::Mode::All;
    CHECK(lod::shownLevel(g, group, v) == -1);
    CHECK(lod::hidden(g, v).empty());

    // The group's own setting wins over the view's.
    g.setValue({group, LodGroup::kLodDisplay, -1}, int64_t(lod::Display::Level));
    g.setValue({group, LodGroup::kDisplayLevel, -1}, int64_t(1));
    CHECK(lod::shownLevel(g, group, v) == 1);
}

TEST_CASE("lodGroup command groups levels in order, names them and generates reduced ones")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    Node* a = addCube(g, "Crate", 100.0);
    g.setValue({a->id(), Transform::kTranslate, -1}, glm::dvec3(50, 0, 0));
    addCube(g, "CrateLow", 100.0);
    addCube(g, "CrateLowest", 100.0);
    g.evaluate();

    std::string path = ops::execute("lodGroup", {{"args", {"Crate", "CrateLow", "CrateLowest"}}, {"screenSizes", {1.0, 0.3, 0.1}}}).get<std::string>();
    Node* group = g.find(path);
    REQUIRE(group);
    CHECK(group->name() == "Crate");
    CHECK(lod::isGroup(*group));
    CHECK(group->as<glm::dvec3>(Transform::kTranslate).x == doctest::Approx(50.0));
    const std::vector<NodeId> lv = lod::levels(g, group->id());
    REQUIRE(lv.size() == 3);
    for (size_t i = 0; i < lv.size(); ++i)
        CHECK(g.node(lv[i])->name() == "Crate_LOD" + std::to_string(i));
    CHECK(group->as<std::string>(LodGroup::kScreenSizes) == "1 0.3 0.1");
    g.evaluate();
    CHECK(g.node(lv[0])->as<glm::dmat4>(Transform::kWorldMatrix)[3].x == doctest::Approx(50.0));
    CHECK_THROWS_AS(ops::execute("lodGroup", {{"args", {path}}, {"screenSizes", {1.0, 3.0}}}), CommandError);

    g.clear();
    Node* boulder = g.createNode(*types::find("mesh"), "Boulder", kNoNode, false);
    g.setValue({boulder->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(geo::makeIcoSphere())));
    path = ops::execute("lodGroup", {{"args", {"Boulder"}}, {"generate", 2}, {"ratio", 0.5}}).get<std::string>();
    group = g.find(path);
    REQUIRE(group);
    const std::vector<NodeId> made = lod::levels(g, group->id());
    REQUIRE(made.size() == 3);
    g.evaluate();
    auto triangles = [&](NodeId id) {
        const geo::Mesh& m = *g.node(id)->as<MeshPtr>(MeshNode::kOutMesh);
        return m.numCorners() - 2 * m.numFaces();
    };
    CHECK(g.node(made[2])->name() == "Boulder_LOD2");
    CHECK(triangles(made[1]) < triangles(made[0]));
    CHECK(triangles(made[2]) < triangles(made[1]));
}

TEST_CASE("LOD groups export as FbxLODGroup and import back")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    const NodeId group = buildGroup(g, "1 0.4 0.15");
    const std::string path = exported(g, group, "lod.fbx");

    {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        CHECK(ss.str().find("FbxLODGroup") != std::string::npos);
    }
    UfbxScene s(path);
    REQUIRE(s.scene);
    REQUIRE(s.scene->lod_groups.count == 1);
    const ufbx_lod_group& lod = *s.scene->lod_groups.data[0];
    CHECK(lod.relative_distances);
    REQUIRE(lod.lod_levels.count == 3);
    CHECK(lod.lod_levels.data[1].distance == doctest::Approx(40.0));
    CHECK(lod.lod_levels.data[2].distance == doctest::Approx(15.0));
    REQUIRE(lod.instances.count == 1);
    const ufbx_node& node = *lod.instances.data[0];
    CHECK(node.attrib_type == UFBX_ELEMENT_LOD_GROUP);
    CHECK(std::string(node.name.data) == "Rock");
    REQUIRE(node.children.count == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(std::string(node.children.data[i]->name.data) == "Rock_LOD" + std::to_string(i));
        CHECK(node.children.data[i]->mesh);
    }

    g.clear();
    fbx::ImportResult r = fbx::importFbx(g, path);
    REQUIRE(r.ok);
    CHECK(r.skipped.empty());
    Node* back = g.find("Rock");
    REQUIRE(back);
    CHECK(lod::isGroup(*back));
    CHECK(lod::parseSizes(back->as<std::string>(LodGroup::kScreenSizes)) == std::vector<double>{1.0, 0.4, 0.15});
    const std::vector<NodeId> lv = lod::levels(g, back->id());
    REQUIRE(lv.size() == 3);
    CHECK(g.node(lv[2])->name() == "Rock_LOD2");
}

TEST_CASE("LOD groups without screen sizes leave them to the importer, in skeletal exports too")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    const NodeId group = buildGroup(g, "");
    for (fbx::ExportKind kind : {fbx::ExportKind::Static, fbx::ExportKind::Skeletal})
    {
        const std::string path = exported(g, group, kind == fbx::ExportKind::Static ? "lod_auto.fbx" : "lod_skeletal.fbx", kind);
        UfbxScene s(path);
        REQUIRE(s.scene);
        REQUIRE(s.scene->lod_groups.count == 1);
        CHECK(s.scene->lod_groups.data[0]->instances.data[0]->children.count == 3);
        CHECK_FALSE(ufbx_find_prop(&s.scene->lod_groups.data[0]->props, "Thresholds|Level0"));
    }

    const std::string path = exported(g, group, "lod_auto.fbx");
    g.clear();
    REQUIRE(fbx::importFbx(g, path).ok);
    Node* back = g.find("Rock");
    REQUIRE(back);
    CHECK(lod::isGroup(*back));
    CHECK(back->as<std::string>(LodGroup::kScreenSizes).empty());
}

TEST_CASE("Send to Unreal takes screen sizes from the first group that sets them")
{
    math::setUpAxis(math::UpAxis::Y);
    Graph& g = Graph::current();
    g.clear();
    const NodeId group = buildGroup(g, "1 0.4");
    Node* top = g.createNode(*types::find("transform"), "Props", kNoNode, false);
    g.setParent(group, top->id());
    const std::vector<double> sizes = lod::exportScreenSizes(g, {top->id()});
    REQUIRE(sizes.size() == 3);
    CHECK(sizes[1] == doctest::Approx(0.4));
    CHECK(sizes[2] == doctest::Approx(lod::autoScreenSize(2, 0.4)));
    g.setValue({group, LodGroup::kScreenSizes, -1}, std::string());
    CHECK(lod::exportScreenSizes(g, {top->id()}).empty());
}
