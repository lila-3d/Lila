#pragma once

#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

namespace rigtest
{
using namespace lila;
using json = nlohmann::json;

inline constexpr double kFps = 30.0;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline scene::Graph& graph() { return scene::Graph::current(); }

inline scene::Node& node(const std::string& name)
{
    scene::Node* n = graph().find(name);
    REQUIRE(n);
    return *n;
}

inline glm::dmat4 world(const std::string& name)
{
    graph().evaluate();
    return node(name).as<glm::dmat4>(scene::Transform::kWorldMatrix);
}

inline glm::dvec3 pos(const std::string& name) { return glm::dvec3(world(name)[3]); }

inline scene::MeshPtr outMesh(const std::string& mesh)
{
    graph().evaluate();
    return node(mesh).as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

inline json v3(double x, double y, double z) { return json::array({x, y, z}); }

inline scene::NodeId id(const std::string& name) { return node(name).id(); }

inline void key(const std::string& plug, double t, double v) { run("setKeyframe", {{"args", {plug}}, {"time", t}, {"value", v}}); }

inline std::vector<glm::dvec3> points(const std::string& mesh)
{
    scene::MeshPtr m = outMesh(mesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

inline std::vector<std::string> selectedNames()
{
    std::vector<std::string> out;
    for (scene::NodeId id : graph().selection())
        out.push_back(graph().node(id)->name());
    return out;
}

/** A 100 cm cylinder along Y (y in [-50, 50]) with a three-joint chain up its middle, bound with default weights. */
inline std::string skinnedCylinder(int subdivisionsHeight = 10)
{
    run("newScene");
    std::string mesh =
        run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", subdivisionsHeight}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}

inline void checkNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1.0));
}
} // namespace rigtest
