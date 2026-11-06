#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Skin.h"
#include "scene/Take.h"
#include "scene/Transform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace lila;
using namespace lila::scene;

json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json v3(double x, double y, double z) { return json::array({x, y, z}); }
Graph& graph() { return Graph::current(); }

Node& node(const std::string& name)
{
    Node* n = graph().find(name);
    REQUIRE_MESSAGE(n, name);
    return *n;
}

glm::dmat4 world(const std::string& name) { return node(name).as<glm::dmat4>(Transform::kWorldMatrix); }

fs::path tempDir(const std::string& sub)
{
    fs::path dir = fs::temp_directory_path() / "lila_collada_tests" / sub;
    fs::create_directories(dir);
    return dir;
}

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeAll(const fs::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

const std::vector<std::string> kJoints = {"bone_a", "bone_b", "bone_c"};
constexpr int kFrames = 24;

/** A 100 cm cylinder bound to a three-joint chain with jointOrients, animated over one second at 24 fps. */
void buildSkinned()
{
    run("newScene");
    run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 12}, {"caps", false}});
    run("setAttr", {{"args", {"Tube.translate", v3(3, 0, 0)}}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(3, -50, 0)}, {"name", "bone_a"}, {"orientation", v3(0, 0, 90)}});
    run("joint", {{"position", v3(3, 0, 0)}, {"name", "bone_b"}, {"orientation", v3(10, 0, -20)}});
    run("joint", {{"position", v3(3, 50, 0)}, {"name", "bone_c"}, {"orientation", v3(0, 15, 0)}});
    run("setAttr", {{"args", {"bone_b.rotateOrder", 1}}});
    run("skinCluster", {{"args", {"bone_a", "Tube"}}});
    Graph& g = graph();
    auto key = [&](const char* joint, int attr, int axis, double frame, double value) {
        anim::setKey(g, {node(joint).id(), attr, axis}, frame / 24.0, value);
    };
    key("bone_a", Transform::kTranslate, 0, 0, node("bone_a").as<glm::dvec3>(Transform::kTranslate).x);
    key("bone_a", Transform::kTranslate, 0, 24, 40);
    key("bone_b", Transform::kRotate, 2, 0, 0);
    key("bone_b", Transform::kRotate, 2, 10, 70);
    key("bone_b", Transform::kRotate, 2, 24, -35);
    key("bone_b", Transform::kRotate, 0, 0, 0);
    key("bone_b", Transform::kRotate, 0, 24, 25);
    key("bone_c", Transform::kRotate, 1, 0, 0);
    key("bone_c", Transform::kRotate, 1, 24, 120);
}

struct Frame
{
    std::map<std::string, glm::dmat4> joints;
    std::vector<glm::dvec3> points;
};

std::vector<Frame> sample(const std::string& mesh)
{
    Graph& g = graph();
    std::vector<Frame> out;
    for (int f = 0; f <= kFrames; ++f)
    {
        g.setTime(f / 24.0);
        g.evaluate();
        Frame fr;
        for (const std::string& j : kJoints)
            fr.joints[j] = world(j);
        MeshPtr m = node(mesh).as<MeshPtr>(MeshNode::kOutMesh);
        REQUIRE(m);
        const glm::dmat4 meshWorld = world(mesh);
        for (const glm::dvec3& p : m->positions())
            fr.points.push_back(glm::dvec3(meshWorld * glm::dvec4(p, 1.0)));
        out.push_back(std::move(fr));
    }
    g.setTime(0.0);
    g.evaluate();
    return out;
}

void compare(const std::vector<Frame>& a, const std::vector<Frame>& b, double eps)
{
    REQUIRE(a.size() == b.size());
    double worstJoint = 0.0, worstPoint = 0.0;
    for (size_t f = 0; f < a.size(); ++f)
    {
        for (const auto& [name, m] : a[f].joints)
            for (int c = 0; c < 4; ++c)
                worstJoint = std::max(worstJoint, glm::length(m[c] - b[f].joints.at(name)[c]));
        REQUIRE(a[f].points.size() == b[f].points.size());
        for (const glm::dvec3& p : a[f].points)
        {
            double best = 1e30;
            for (const glm::dvec3& q : b[f].points)
                best = std::min(best, glm::length(p - q));
            worstPoint = std::max(worstPoint, best);
        }
    }
    MESSAGE("worst joint matrix column error " << worstJoint << ", worst skinned point error " << worstPoint << " cm");
    CHECK(worstJoint < eps);
    CHECK(worstPoint < eps);
}

void skinnedRoundTrip(const std::string& units, bool baked)
{
    buildSkinned();
    const std::vector<Frame> before = sample("Tube");
    const fs::path file =
        tempDir("skin") / std::format("skin_{}_{}_{}.dae", units, math::upAxis() == math::UpAxis::Z ? "z" : "y", baked ? "baked" : "elements");
    run("exportCollada", {{"args", {file.string(), "Tube", "bone_a"}}, {"units", units}, {"bakeTransforms", baked}});
    const std::string text = readAll(file);
    CHECK(text.find("<up_axis>Y_UP</up_axis>") != std::string::npos);
    CHECK(text.find("<controller") != std::string::npos);
    CHECK(text.find("type=\"JOINT\"") != std::string::npos);
    CHECK(text.find("<polylist") != std::string::npos);
    CHECK((text.find("<matrix sid=\"transform\">") != std::string::npos) == baked);
    CHECK((text.find("sid=\"jointOrientZ\"") != std::string::npos) == !baked);
    CHECK((text.find("bone_b/rotateZ.ANGLE") != std::string::npos) == !baked);
    CHECK(text.find("<technique profile=\"FCOLLADA\">") != std::string::npos);

    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 0.0}, {"maxTime", 4.0}});
    json r = run("importCollada", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["warnings"].empty());
    CHECK(r["joints"].size() == 3);
    CHECK(r["skinClusters"].size() == 1);
    CHECK(r["fps"].get<double>() == doctest::Approx(24.0));
    CHECK(anim::playback().maxTime == doctest::Approx(1.0));
    compare(before, sample("Tube"), 2e-3);
}

/** Positions of a mesh node's points in world space, sorted. */
std::vector<glm::dvec3> worldPoints(const std::string& name)
{
    graph().evaluate();
    MeshPtr m = node(name).as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    std::vector<glm::dvec3> out;
    const glm::dmat4 w = world(name);
    for (const glm::dvec3& p : m->positions())
        out.push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
    std::sort(out.begin(), out.end(), [](const glm::dvec3& a, const glm::dvec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
    return out;
}
} // namespace

TEST_CASE("skinned, animated cylinder round-trips through COLLADA")
{
    for (bool baked : {false, true})
    {
        skinnedRoundTrip("cm", baked);
        skinnedRoundTrip("m", baked);
        math::setUpAxis(math::UpAxis::Z);
        skinnedRoundTrip("cm", baked);
        math::setUpAxis(math::UpAxis::Y);
    }
}

TEST_CASE("COLLADA animation export is the skeleton and its curves, as Maya's animation exports are")
{
    buildSkinned();
    const fs::path file = tempDir("anim") / "anim.dae";
    run("exportCollada", {{"args", {file.string(), "bone_a"}}, {"kind", "animation"}});
    const std::string text = readAll(file);
    CHECK(text.find("<library_geometries") == std::string::npos);
    CHECK(text.find("<library_controllers") == std::string::npos);
    CHECK(text.find("<unit name=\"centimeter\" meter=\"0.01\"/>") != std::string::npos);
    // Every channel of every joint is a curve, constant ones too, as a keyed Maya rig exports.
    for (const std::string& j : kJoints)
        for (const char* channel : {"/translate\"", "/rotateX.ANGLE", "/rotateY.ANGLE", "/rotateZ.ANGLE", "/scale\""})
        {
            const std::string target = j + channel;
            CHECK_MESSAGE(text.find(target) != std::string::npos, target);
        }
    CHECK(text.find("bone_a/translate\"") != std::string::npos);
    CHECK(text.find("<library_animation_clips") == std::string::npos);

    const std::vector<Frame> before = [] {
        std::vector<Frame> out;
        for (int f = 0; f <= kFrames; ++f)
        {
            graph().setTime(f / 24.0);
            graph().evaluate();
            Frame fr;
            for (const std::string& j : kJoints)
                fr.joints[j] = world(j);
            out.push_back(std::move(fr));
        }
        return out;
    }();
    run("newScene");
    json r = run("importCollada", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["joints"].size() == 3);
    double worst = 0.0;
    for (int f = 0; f <= kFrames; ++f)
    {
        graph().setTime(f / 24.0);
        graph().evaluate();
        for (const std::string& j : kJoints)
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, glm::length(world(j)[c] - before[size_t(f)].joints.at(j)[c]));
    }
    MESSAGE("worst joint matrix column error " << worst);
    CHECK(worst < 2e-3);
    graph().setTime(0.0);
}

TEST_CASE("COLLADA keeps quads, UVs, hard edges and material slots")
{
    run("newScene");
    run("polyCube", {{"name", "Box"}, {"width", 20}, {"height", 10}, {"depth", 30}});
    run("setAttr", {{"args", {"Box.translate", v3(5, 6, -7)}}});
    run("setAttr", {{"args", {"Box.rotate", v3(10, 20, 30)}}});
    Node* mat = materials::create(graph(), "Red");
    graph().setValue({mat->id(), material::kBaseColor, -1}, glm::dvec3(0.8, 0.1, 0.1));
    run("assignMaterial", {{"args", {"Red", "Box"}}, {"faces", json::array({0, 1})}});
    const std::vector<glm::dvec3> before = worldPoints("Box");
    const fs::path file = tempDir("box") / "box.dae";
    run("exportCollada", {{"args", {file.string(), "Box"}}, {"kind", "static"}});

    run("newScene");
    json r = run("importCollada", {{"args", {file.string()}}, {"report", true}});
    REQUIRE(r["roots"].size() == 1);
    const std::string name = r["roots"][0].get<std::string>();
    const std::vector<glm::dvec3> after = worldPoints(name);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
        CHECK(glm::length(after[i] - before[i]) < 1e-4);
    MeshPtr m = node(name).as<MeshPtr>(MeshNode::kOutMesh);
    CHECK(m->numFaces() == 6);
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        if (m->faceValid(f))
            CHECK(m->faceSize(f) == 4);
    CHECK(m->cornerAttrs().get<glm::vec2>(geo::attr::uvName(0)).size() == m->cornerCapacity());
    std::span<const bool> hard = m->edgeAttrs().get<bool>(geo::attr::kHard);
    REQUIRE(hard.size() == m->edgeCapacity());
    CHECK(std::count(hard.begin(), hard.end(), true) == 12);
    std::map<std::string, uint32_t> usage;
    for (const auto& [slot, faces] : materials::slotUsage(*m))
        usage[slot] = faces;
    CHECK(usage["Red"] == 2);
    Node* red = materials::find(graph(), "Red");
    REQUIRE(red);
    CHECK(red->as<glm::dvec3>(material::kBaseColor).x == doctest::Approx(0.8));
}

TEST_CASE("COLLADA import reads Z up, metres, transform stacks and element animation")
{
    // What Blender or 3ds Max write: Z up in metres, translate/rotate/scale elements, rotate angles animated by member.
    const std::string dae = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- a comment before the root -->
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><unit name="meter" meter="1"/><up_axis>Z_UP</up_axis></asset>
  <library_geometries>
    <geometry id="tri-mesh" name="Tri &amp; Co">
      <mesh>
        <source id="tri-pos"><float_array id="tri-pos-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
          <technique_common><accessor source="#tri-pos-array" count="3" stride="3">
            <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/></accessor></technique_common></source>
        <vertices id="tri-vtx"><input semantic="POSITION" source="#tri-pos"/></vertices>
        <triangles count="1"><input semantic="VERTEX" source="#tri-vtx" offset="0"/><p>0 1 2</p></triangles>
      </mesh>
    </geometry>
  </library_geometries>
  <library_animations>
    <animation id="spin">
      <source id="spin-in"><float_array id="spin-in-array" count="2">0 2</float_array>
        <technique_common><accessor source="#spin-in-array" count="2"><param name="TIME" type="float"/></accessor></technique_common></source>
      <source id="spin-out"><float_array id="spin-out-array" count="2">0 90</float_array>
        <technique_common><accessor source="#spin-out-array" count="2"><param name="ANGLE" type="float"/></accessor></technique_common></source>
      <source id="spin-interp"><Name_array id="spin-interp-array" count="2">LINEAR LINEAR</Name_array>
        <technique_common><accessor source="#spin-interp-array" count="2"><param name="INTERPOLATION" type="name"/></accessor></technique_common></source>
      <sampler id="spin-sampler"><input semantic="INPUT" source="#spin-in"/><input semantic="OUTPUT" source="#spin-out"/>
        <input semantic="INTERPOLATION" source="#spin-interp"/></sampler>
      <channel source="#spin-sampler" target="Tri/rotZ.ANGLE"/>
    </animation>
  </library_animations>
  <library_visual_scenes>
    <visual_scene id="Scene">
      <node id="Tri" name="Tri" type="NODE">
        <translate sid="location">1 2 3</translate>
        <rotate sid="rotZ">0 0 1 0</rotate>
        <scale sid="scale">2 2 2</scale>
        <instance_geometry url="#tri-mesh"/>
      </node>
    </visual_scene>
  </library_visual_scenes>
  <scene><instance_visual_scene url="#Scene"/></scene>
</COLLADA>)";
    const fs::path file = tempDir("stack") / "stack.dae";
    writeAll(file, dae);
    run("newScene");
    json r = run("importCollada", {{"args", {file.string()}}, {"report", true}, {"fps", 24.0}});
    CHECK(r["upAxis"] == "Z_UP");
    CHECK(r["unitMeters"].get<double>() == doctest::Approx(1.0));
    CHECK(r["curves"].get<int>() > 0);
    CHECK(anim::playback().maxTime == doctest::Approx(48.0 / 24.0));

    // The file's (x, y, z) Z-up metres land on Lila's Y-up centimetres as (x, z, -y).
    auto fileToLila = [](glm::dvec3 p) { return glm::dvec3(p.x, p.z, -p.y) * 100.0; };
    graph().setTime(0.0);
    std::vector<glm::dvec3> pts = worldPoints("Tri");
    REQUIRE(pts.size() == 3);
    const std::vector<glm::dvec3> expected0 = {fileToLila({1, 2, 3}), fileToLila({3, 2, 3}), fileToLila({1, 4, 3})};
    for (const glm::dvec3& e : expected0)
        CHECK(std::any_of(pts.begin(), pts.end(), [&](const glm::dvec3& p) { return glm::length(p - e) < 1e-3; }));
    // Half way the rotation about the file's Z is 45 degrees.
    graph().setTime(1.0);
    pts = worldPoints("Tri");
    const double s = std::sqrt(0.5) * 2.0;
    const std::vector<glm::dvec3> expected1 = {fileToLila({1, 2, 3}), fileToLila({1 + s, 2 + s, 3}), fileToLila({1 - s, 2 + s, 3})};
    for (const glm::dvec3& e : expected1)
        CHECK(std::any_of(pts.begin(), pts.end(), [&](const glm::dvec3& p) { return glm::length(p - e) < 1e-3; }));
    graph().setTime(0.0);
}

TEST_CASE("COLLADA import refuses files that are not COLLADA")
{
    const fs::path dir = tempDir("bad");
    writeAll(dir / "broken.dae", "<COLLADA><asset></COLLADA>");
    writeAll(dir / "other.dae", "<?xml version=\"1.0\"?><svg/>");
    run("newScene");
    CHECK_THROWS_AS(run("importCollada", {{"args", {(dir / "broken.dae").string()}}}), CommandError);
    CHECK_THROWS_AS(run("importCollada", {{"args", {(dir / "other.dae").string()}}}), CommandError);
}

TEST_CASE("COLLADA takes export as clips and import as takes")
{
    buildSkinned();
    Graph& g = graph();
    anim::TakeSettings s;
    s.fps = 24.0;
    s.start = 0.0;
    s.end = 1.0;
    const NodeId walk = anim::createTake(g, "Walk", s);
    s.end = 0.5;
    const NodeId wave = anim::createTake(g, "Wave", s);
    anim::setActiveTake(g, walk);
    REQUIRE(walk != kNoNode);
    REQUIRE(wave != kNoNode);
    const fs::path file = tempDir("takes") / "takes.dae";
    run("exportCollada", {{"args", {file.string(), "Tube", "bone_a"}}, {"takes", "all"}});
    const std::string text = readAll(file);
    CHECK(text.find("name=\"Walk\"") != std::string::npos);
    CHECK(text.find("name=\"Wave\"") != std::string::npos);

    run("newScene");
    json r = run("importCollada", {{"args", {file.string()}}, {"report", true}});
    CHECK(r["clips"].size() == 2);
    CHECK(anim::takes(graph()).size() == 2);
}
