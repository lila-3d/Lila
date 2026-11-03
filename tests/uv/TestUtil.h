#pragma once

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Uv.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uvtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

inline scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

inline std::vector<glm::vec2> uvsOf(const std::string& name)
{
    auto uv = meshOf(name)->cornerAttrs().get<glm::vec2>("uv0");
    return {uv.begin(), uv.end()};
}

inline double seconds(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

inline void clearSeams(geo::Mesh& m)
{
    std::span<bool> s = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    std::fill(s.begin(), s.end(), false);
}

inline size_t overlaps(const geo::Mesh& m, std::string_view set = "uv0")
{
    std::vector<uint8_t> flags;
    return uv::overlappingFaces(m, set, flags);
}

inline double segDist(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b)
{
    glm::dvec2 d = b - a;
    double t = glm::dot(d, d) > 0.0 ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
    return glm::length(p - (a + t * d));
}
} // namespace uvtest
