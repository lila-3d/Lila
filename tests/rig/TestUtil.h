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

inline void checkNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps).scale(1.0));
}
} // namespace rigtest
