#pragma once

#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "math/MathCore.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace lila::usdtest
{
using json = nlohmann::json;
namespace fs = std::filesystem;

inline json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

inline scene::Graph& graph() { return scene::Graph::current(); }

inline void fresh()
{
    auto& em = extensions::ExtensionManager::get();
    if (!em.isLoaded("OpenUSD"))
        REQUIRE(em.load(LILA_TEST_OPENUSD_PACKAGE) == "OpenUSD");
    run("newScene");
}

inline scene::Node& node(const std::string& name)
{
    scene::Node* n = graph().find(name);
    REQUIRE_MESSAGE(n, name);
    return *n;
}

inline scene::Node* addMesh(const std::string& name, geo::Mesh m, scene::NodeId parent = scene::kNoNode)
{
    scene::Node* n = graph().createNode(*scene::types::find("mesh"), name, parent, false);
    graph().setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n;
}

inline glm::dmat4 world(const std::string& name)
{
    graph().evaluate();
    return node(name).as<glm::dmat4>(scene::Transform::kWorldMatrix);
}

inline scene::MeshPtr outMesh(const std::string& name)
{
    graph().evaluate();
    return node(name).as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

inline fs::path tempDir()
{
    fs::path dir = fs::temp_directory_path() / "lila_usd_tests";
    fs::create_directories(dir);
    return dir;
}

inline fs::path fixture(const char* name) { return fs::path(LILA_TEST_USD_FIXTURES) / name; }

inline std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool near(const glm::dvec3& a, const glm::dvec3& b, double eps) { return glm::length(a - b) <= eps; }

inline bool near(const glm::dmat4& a, const glm::dmat4& b, double eps)
{
    for (int c = 0; c < 4; ++c)
        if (glm::length(a[c] - b[c]) > eps)
            return false;
    return true;
}

/** The scene up axis for one test, put back after. */
struct UpAxisScope
{
    math::UpAxis was = math::upAxis();
    explicit UpAxisScope(math::UpAxis up) { math::setUpAxis(up); }
    ~UpAxisScope() { math::setUpAxis(was); }
};
} // namespace lila::usdtest
