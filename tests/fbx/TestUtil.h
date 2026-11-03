#pragma once

#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

namespace fbxtest
{
using namespace lila;
using json = nlohmann::json;

inline json run(std::string_view name, const json& call = json::object()) { return ops::execute(name, call); }
inline json v3(double x, double y, double z) { return json::array({x, y, z}); }

inline std::filesystem::path tempFile(const std::string& name)
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_fbx_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

/** A file read back with ufbx; scene stays null when it fails to load. */
struct UfbxScene
{
    ufbx_scene* scene = nullptr;
    explicit UfbxScene(const std::filesystem::path& p)
    {
        ufbx_load_opts opts{};
        ufbx_error err{};
        std::string s = p.string();
        scene = ufbx_load_file(s.c_str(), &opts, &err);
        if (!scene)
            MESSAGE(std::string(err.description.data, err.description.length));
    }
    ~UfbxScene() { ufbx_free_scene(scene); }
};

inline glm::dvec3 vec(const ufbx_vec3& v) { return {v.x, v.y, v.z}; }
inline bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-6) { return glm::length(a - b) < eps; }

inline const ufbx_node* nodeNamed(const ufbx_scene& s, const char* name)
{
    for (size_t i = 0; i < s.nodes.count; ++i)
        if (std::string(s.nodes.data[i]->name.data) == name)
            return s.nodes.data[i];
    return nullptr;
}

inline double maxDiff(const glm::dmat4& a, const glm::dmat4& b)
{
    double d = 0.0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            d = std::max(d, std::abs(a[c][r] - b[c][r]));
    return d;
}

inline glm::dmat4 worldOf(const char* name) { return scene::Graph::current().find(name)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }
} // namespace fbxtest
