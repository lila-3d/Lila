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
#include <map>
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

inline bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

/** True when any string in list contains part. */
inline bool listed(const json& list, const std::string& part)
{
    for (const json& s : list)
        if (s.get<std::string>().find(part) != std::string::npos)
            return true;
    return false;
}

/** Every attribute of the named material as JSON, by attribute name. */
inline std::map<std::string, json> materialValues(const std::string& name)
{
    std::map<std::string, json> out;
    scene::Node* n = scene::materials::find(graph(), name);
    REQUIRE_MESSAGE(n, name);
    for (size_t a = 0; a < n->type().attrs.size(); ++a)
        out[n->type().attrs[a].name] = scene::toJson(n->value(int(a)));
    return out;
}

/** Checks every value in a against b: file paths by identity, floats to 1e-5, the rest exactly. */
inline void compareMaterial(const std::map<std::string, json>& a, const std::map<std::string, json>& b)
{
    for (const auto& [key, value] : a)
    {
        INFO(key);
        const json& other = b.at(key);
        if (value.is_string() && !value.get<std::string>().empty())
        {
            REQUIRE(other.is_string());
            std::error_code ec;
            CHECK(fs::equivalent(fs::path(value.get<std::string>()), fs::path(other.get<std::string>()), ec));
        }
        else if (value.is_number_float())
            CHECK(other.get<double>() == doctest::Approx(value.get<double>()).epsilon(1e-5));
        else if (value.is_array())
            for (size_t i = 0; i < value.size(); ++i)
                CHECK(other[i].get<double>() == doctest::Approx(value[i].get<double>()).epsilon(1e-5));
        else
            CHECK(value == other);
    }
}

inline void compareMaterial(const std::string& name, const std::map<std::string, json>& a)
{
    INFO(name);
    compareMaterial(a, materialValues(name));
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
