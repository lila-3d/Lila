#pragma once

#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace deformtest
{
inline nlohmann::json run(const char* name, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(name, call); }

inline nlohmann::json v3(double x, double y, double z) { return nlohmann::json::array({x, y, z}); }

inline lila::scene::Graph& graph() { return lila::scene::Graph::current(); }

/** The evaluated output mesh of a mesh node. */
inline lila::scene::MeshPtr outMesh(const std::string& mesh)
{
    lila::scene::Graph& g = graph();
    g.evaluate();
    lila::scene::Node* n = g.find(mesh);
    REQUIRE(n);
    lila::scene::MeshPtr m = n->as<lila::scene::MeshPtr>(lila::scene::MeshNode::kOutMesh);
    REQUIRE(m);
    return m;
}

inline std::vector<glm::dvec3> points(const std::string& mesh)
{
    lila::scene::MeshPtr m = outMesh(mesh);
    return {m->positions().begin(), m->positions().end()};
}

inline std::vector<glm::dvec3> pointsOf(const lila::geo::Mesh& m) { return {m.positions().begin(), m.positions().end()}; }

inline bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) <= eps; }

inline bool nearTight(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return near(a, b, eps); }

inline bool samePoints(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, double eps = 1e-6)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!near(a[i], b[i], eps))
            return false;
    return true;
}

inline double maxDistance(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b)
{
    REQUIRE(a.size() == b.size());
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max(d, glm::length(a[i] - b[i]));
    return d;
}

inline std::vector<glm::dvec3> randomPoints(size_t n, double lo, double hi, unsigned seed = 11)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(lo, hi);
    std::vector<glm::dvec3> out(n);
    for (auto& p : out)
        p = {d(rng), d(rng), d(rng)};
    return out;
}

/** Replaces a mesh's base points: point p moves by offset(p). */
inline void editPoints(const std::string& mesh, const std::function<glm::dvec3(lila::geo::Index, const glm::dvec3&)>& offset)
{
    lila::scene::Graph& g = graph();
    lila::scene::PlugRef plug = lila::scene::modifiers::editPlug(g, g.find(mesh)->id());
    auto m = std::make_shared<lila::geo::Mesh>(*std::get<lila::scene::MeshPtr>(g.value(plug)));
    for (lila::geo::Index p = 0; p < m->pointCapacity(); ++p)
        m->setPosition(p, m->position(p) + offset(p, m->position(p)));
    g.setValue(plug, lila::scene::MeshPtr(m));
}

inline std::filesystem::path tempFile(const std::string& name)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_deform_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}
} // namespace deformtest
