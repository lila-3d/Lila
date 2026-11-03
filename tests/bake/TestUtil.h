#pragma once

#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace baketest
{
using namespace lila;
using json = nlohmann::json;
/** An empty folder of that name under the system temp folder. */
inline std::filesystem::path tempDir(const std::string& name)
{
    std::filesystem::path d = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

inline scene::Graph& graph() { return scene::Graph::current(); }

inline void newScene()
{
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
}

inline std::string addMesh(const std::string& name, geo::Mesh m, const glm::dvec3& translate = glm::dvec3(0.0))
{
    scene::Graph& g = graph();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    if (translate != glm::dvec3(0.0))
        g.setValue({n->id(), scene::Transform::kTranslate, -1}, translate);
    return n->path();
}

/** A size cm plane in XZ facing +Y; uv0 spans [uvMin, uvMax] (u along +x, v along -z). */
inline geo::Mesh plane(int subdiv = 1, glm::vec2 uvMin = {0.0f, 0.0f}, glm::vec2 uvMax = {1.0f, 1.0f}, double size = 100.0)
{
    geo::PlaneOptions o;
    o.width = o.depth = size;
    o.subdivX = o.subdivZ = subdiv;
    geo::Mesh m = geo::makePlane(o);
    for (glm::vec2& uv : m.cornerAttrs().get<glm::vec2>("uv0"))
        uv = uvMin + uv * (uvMax - uvMin);
    return m;
}

/** A dense plane with y = f(x, z). */
template <class F> geo::Mesh displaced(int subdiv, F f)
{
    geo::Mesh m = plane(subdiv);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q = m.position(p);
        q.y = f(q.x, q.z);
        m.setPosition(p, q);
    }
    return m;
}

inline geo::Mesh lifted(geo::Mesh m, double y)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        m.setPosition(p, m.position(p) + glm::dvec3(0.0, y, 0.0));
    return m;
}

inline scene::MeshPtr baseMesh(const std::string& name)
{
    scene::Graph& g = graph();
    return std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, g.find(name)->id())));
}

/** A project writing res x res EXR (or the format given) textures into dir, one sample per texel unless ss says otherwise. */
inline std::string project(const std::filesystem::path& dir, int res, int ss = 1, const std::string& format = "exr", const std::string& name = "{group}_{map}")
{
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p},          {"folder", dir.generic_string()}, {"resolution", res},
                                   {"supersampling", ss},  {"format", format},                {"name", name},
                                   {"padding", 4}});
    return p;
}

inline image::Image readImage(const json& out)
{
    std::string err;
    image::Image img = image::read(std::filesystem::path(out["file"].get<std::string>()), &err);
    INFO(err);
    REQUIRE(img.width > 0);
    return img;
}

inline glm::vec4 texel(const image::Image& img, int x, int y)
{
    const float* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
    return {p[0], p[1], p[2], p[3]};
}

inline const json& outputFor(const json& result, const std::string& map)
{
    for (const json& o : result["outputs"])
        if (o["map"] == map)
            return o;
    FAIL("no output for " << map);
    static json none;
    return none;
}

inline float angleDeg(glm::vec3 a, glm::vec3 b) { return glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f))); }
} // namespace baketest
