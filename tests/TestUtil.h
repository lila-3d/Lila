#pragma once

#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace testutil
{
using json = nlohmann::json;

inline json run(const std::string& name, const json& call = json::object()) { return lila::ops::execute(name, call); }

/** Runs a command as the UI would, with its option box defaults. */
inline json runUi(const char* name, json call = json::object()) { return lila::ops::execute(name, call, lila::CallSource::UI); }

inline lila::scene::Graph& g() { return lila::scene::Graph::current(); }

inline lila::scene::NodeId id(const std::string& name) { return g().find(name)->id(); }

inline lila::scene::NodeId idOf(const std::string& name) { return lila::scene::Graph::current().find(name)->id(); }

/** The node name a create command returned, whether alone or first in a list. */
inline std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }

inline double get(const std::string& plug) { return run("getAttr", {{"args", {plug}}}).get<double>(); }

inline size_t count(const char* type) { return run("ls", {{"type", type}}).size(); }

inline int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }

inline std::string kind(const std::string& m) { return run("instanceInfo", {{"args", {m}}})["kind"].get<std::string>(); }

inline size_t selectedCount(const std::string& m, const char* kind)
{
    json sel = run("componentSelection");
    return sel.contains(m) && sel[m].contains(kind) ? sel[m][kind].size() : 0;
}

inline glm::dvec3 vec(const json& j) { return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()}; }

/** A copy of the evaluated output mesh of a mesh node. */
inline lila::geo::Mesh meshOf(const std::string& name)
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    g.evaluate();
    lila::scene::Node* n = g.find(name);
    REQUIRE(n);
    lila::scene::MeshPtr m = n->as<lila::scene::MeshPtr>(lila::scene::MeshNode::kOutMesh);
    REQUIRE(m);
    return *m;
}

inline lila::scene::MeshPtr outMesh(const std::string& name)
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    g.evaluate();
    lila::scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<lila::scene::MeshPtr>(lila::scene::MeshNode::kOutMesh);
}

inline glm::dmat4 worldOf(const std::string& name)
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    g.evaluate();
    lila::scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<glm::dmat4>(lila::scene::Transform::kWorldMatrix);
}

inline glm::dvec3 worldPos(const std::string& name) { return glm::dvec3(worldOf(name)[3]); }

/** A mesh node's valid points in world space. */
inline std::vector<glm::dvec3> worldPoints(const std::string& name)
{
    const lila::geo::Mesh m = meshOf(name);
    const glm::dmat4 w = worldOf(name);
    std::vector<glm::dvec3> out;
    for (lila::geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            out.push_back(glm::dvec3(w * glm::dvec4(m.position(p), 1.0)));
    return out;
}

/** The valid point at (x, z), or kInvalid. */
inline lila::geo::Index pointAt(const lila::geo::Mesh& m, double x, double z)
{
    for (lila::geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && std::abs(m.position(p).x - x) < 1e-6 && std::abs(m.position(p).z - z) < 1e-6)
            return p;
    return lila::geo::kInvalid;
}

/** The valid face centred on (x, z), or kInvalid. */
inline lila::geo::Index faceAt(const lila::geo::Mesh& m, double x, double z)
{
    for (lila::geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && std::abs(m.faceCentroid(f).x - x) < 1e-6 && std::abs(m.faceCentroid(f).z - z) < 1e-6)
            return f;
    return lila::geo::kInvalid;
}

inline bool has(const std::vector<lila::geo::Index>& v, lila::geo::Index i) { return std::find(v.begin(), v.end(), i) != v.end(); }

inline bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

inline bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::distance(a, b) < eps; }

inline bool nearTight(const glm::dvec3& a, const glm::dvec3& b) { return near(a, b, 1e-9); }

inline void checkNear(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6)
{
    for (int i = 0; i < 3; ++i)
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(eps).scale(1.0));
}

/** A file path under the temp folder's lila_tests/<folder>, which is created. */
inline std::string tempPath(const std::string& folder, const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests" / folder;
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}

inline std::string slurp(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace testutil
