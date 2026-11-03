#pragma once

#include "bake/BakeNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <variant>
#include <vector>

/** Scene lookups, command arguments and raster math the bake sources share. Internal to src/bake. */
namespace lila::bake::detail
{
/** The last element of a DAG path. */
inline std::string shortName(const std::string& path)
{
    const size_t bar = path.rfind('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

/** The node's world matrix, identity when it has none. */
inline glm::dmat4 worldOf(scene::Graph& g, scene::NodeId id)
{
    const scene::PlugValue v = g.value({id, scene::Transform::kWorldMatrix, -1});
    return std::holds_alternative<glm::dmat4>(v) ? std::get<glm::dmat4>(v) : glm::dmat4(1.0);
}

/** The mesh as exported: export-enabled modifiers applied, else its output mesh. */
inline scene::MeshPtr exportedMesh(scene::Graph& g, scene::NodeId id)
{
    scene::MeshPtr m = scene::modifiers::exportMesh(g, id);
    if (!m)
        m = std::get<scene::MeshPtr>(g.value({id, scene::MeshNode::kOutMesh, -1}));
    return m;
}

/** The node by name or path, null when there is none or the name is malformed. */
inline scene::Node* findNode(scene::Graph& g, const std::string& name)
{
    try
    {
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** The mesh node by name or path, null when there is none. */
inline scene::Node* meshByName(scene::Graph& g, const std::string& name)
{
    scene::Node* n = findNode(g, name);
    return n && n->type().isA("mesh") ? n : nullptr;
}

/** A flag given as one string or a list of them. */
inline std::vector<std::string> listArg(const Args& a, const char* flag)
{
    std::vector<std::string> out;
    if (!a.has(flag))
        return out;
    const json& v = a.flags[flag];
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& x : v)
            if (x.is_string())
                out.push_back(x.get<std::string>());
    return out;
}

/** project=, else the scene's only project; create makes one when there is none. */
inline scene::Node& projectArg(scene::Graph& g, const Args& a, const char* cmd, bool create = false)
{
    if (a.has("project"))
    {
        const std::string name = a.get<std::string>("project");
        scene::Node* n = findNode(g, name);
        if (n && isProject(*n))
            return *n;
        if (!n && create)
            return *createProject(g, name);
        throw CommandError(std::string(cmd) + ": " + name + " is not a bake project");
    }
    const std::vector<scene::NodeId> ps = projects(g);
    if (ps.size() == 1)
        return *g.node(ps.front());
    if (ps.empty())
    {
        if (create)
            return *createProject(g, "");
        throw CommandError(std::string(cmd) + ": the scene has no bake project (createBakeProject)");
    }
    throw CommandError(std::string(cmd) + ": the scene has several bake projects; name one with project=");
}

/** Twice the signed area of a, b, p: positive when p is left of a to b. */
inline double edgeFunction(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }

/** The three corner values v[0..2] mixed by barycentric weights w. */
template <class T> T mix3(const T* v, const glm::dvec3& w) { return v[0] * float(w.x) + v[1] * float(w.y) + v[2] * float(w.z); }
} // namespace lila::bake::detail
