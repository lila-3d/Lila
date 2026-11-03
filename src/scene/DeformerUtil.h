#pragma once

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Plug.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/** Attribute builders, data packing and command lookups shared by the deformer, morph and driver files. */
namespace lila::scene::deformutil
{
// ------------------------------------------------------------------------------------------------ data packing

/** Reads the numbers under key into out, empty when the key is missing or not an array. */
template <class V> void readArray(const json& j, const char* key, std::vector<V>& out)
{
    out.clear();
    if (!j.contains(key) || !j[key].is_array())
        return;
    out.reserve(j[key].size());
    for (const json& x : j[key])
        out.push_back(V(x.get<double>()));
}

inline std::vector<double> flatten(std::span<const glm::dvec3> v)
{
    std::vector<double> out;
    out.reserve(v.size() * 3);
    for (const glm::dvec3& p : v)
        out.insert(out.end(), {p.x, p.y, p.z});
    return out;
}

inline std::vector<glm::dvec3> unflatten(const std::vector<double>& f)
{
    std::vector<glm::dvec3> out(f.size() / 3);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = {f[i * 3], f[i * 3 + 1], f[i * 3 + 2]};
    return out;
}

// ------------------------------------------------------------------------------------------------ attr builders

inline AttrDef keyable(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {},
                       std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

inline AttrDef stored(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {},
                      std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable, .doc = std::move(doc),
            .minValue = lo, .maxValue = hi};
}

inline AttrDef hiddenStored(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrHidden,
            .doc = std::move(doc)};
}

/** An input that only a connection sets. */
inline AttrDef connected(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

inline AttrDef enumAttr(std::string name, int64_t def, std::vector<std::string> names, std::string doc, bool key = false)
{
    AttrDef a = key ? keyable(std::move(name), PlugType::Enum, def, std::move(doc)) : stored(std::move(name), PlugType::Enum, def, std::move(doc));
    a.enumNames = std::move(names);
    return a;
}

inline AttrDef geomMatrixAttr()
{
    return hiddenStored("geomMatrix", PlugType::Matrix, glm::dmat4(1.0), "The mesh's world matrix when the deformer was made.");
}

// ------------------------------------------------------------------------------------------------ math

template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

/** The inverse of m; a runtime_error naming what when it has zero scale. */
inline glm::dmat4 safeInverse(const glm::dmat4& m, const char* what)
{
    if (std::abs(glm::determinant(m)) < 1e-18)
        throw std::runtime_error(std::string(what) + " has zero scale");
    return glm::inverse(m);
}

inline glm::dvec3 xform(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

// ------------------------------------------------------------------------------------------------ command helpers

/** A mesh by name; lattices don't count. */
inline Node& meshNamed(const std::string& name, const char* cmd)
{
    Node& n = rigutil::nodeNamed(name, cmd);
    if (!n.type().isA("mesh") || n.type().isA("lattice"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return n;
}

/** The named meshes; a CommandError when there are none. */
inline std::vector<NodeId> meshesOf(const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : names)
        out.push_back(meshNamed(name, cmd).id());
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give or select at least one mesh");
    return out;
}

inline DeformerNode& deformerNamed(const std::string& name, const char* cmd)
{
    Node& n = rigutil::nodeNamed(name, cmd);
    if (!deformers::isDeformer(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a deformer");
    return static_cast<DeformerNode&>(n);
}

/** World bounds of the meshes' drawn points, the origin alone when they have none. */
inline geo::Bounds worldBounds(Graph& graph, const std::vector<NodeId>& meshes)
{
    graph.evaluate();
    geo::Bounds b;
    for (NodeId id : meshes)
    {
        Node* n = graph.node(id);
        MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        glm::dmat4 w = rigutil::worldOf(*n);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                b.extend(xform(w, m->position(p)));
    }
    if (!b.valid())
        b.extend(glm::dvec3(0.0));
    return b;
}

inline void setPlug(Graph& graph, NodeId node, const std::string& attr, PlugValue v)
{
    int a = graph.node(node)->type().attrIndex(attr);
    if (a < 0)
        throw CommandError("no attribute " + attr + " on " + graph.node(node)->name());
    rigutil::guarded([&] {
        graph.setValue({node, a, -1}, std::move(v));
        return 0;
    });
}

/** Connects src.srcAttr to dst.dstAttr, replacing what fed dst. */
inline void connectPlug(Graph& graph, NodeId src, const std::string& srcAttr, NodeId dst, const std::string& dstAttr)
{
    rigutil::guarded([&] {
        graph.connect({src, graph.node(src)->type().attrIndex(srcAttr), -1}, {dst, graph.node(dst)->type().attrIndex(dstAttr), -1}, true);
        return 0;
    });
}

/** Applies {param: value} from a command's params flag, as addModifier does. */
inline void applyParams(Graph& graph, Node& n, const json& params)
{
    if (!params.is_object())
        return;
    for (auto& [k, val] : params.items())
        rigutil::guarded([&] {
            PlugRef p = graph.plug(n.name() + "." + k);
            AttrDef def = graph.attr(p);
            if (p.component >= 0)
                def.type = PlugType::Float;
            graph.setValue(p, fromJson(def, val));
            return 0;
        });
}

/** "x", "y" or "z" as 0, 1 or 2. */
inline int axisIndex(const std::string& a, const char* cmd)
{
    if (a == "x")
        return 0;
    if (a == "y")
        return 1;
    if (a == "z")
        return 2;
    throw CommandError(std::string(cmd) + ": axis must be x, y or z");
}

/** A skin influence as line segments from its joint to each child joint, or one point segment for a leaf. */
struct SkinBone
{
    uint32_t index;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segments;
};

/** The bones of the influences that have a joint. */
inline std::vector<SkinBone> skinBones(Graph& g, const std::vector<skin::Influence>& infs)
{
    std::vector<SkinBone> bones;
    for (const skin::Influence& inf : infs)
    {
        if (inf.joint == kNoNode)
            continue;
        SkinBone b{inf.index, {}};
        glm::dvec3 a = rigutil::positionOf(*g.node(inf.joint));
        for (NodeId c : g.children(inf.joint))
            if (rig::isJoint(*g.node(c)))
                b.segments.push_back({a, rigutil::positionOf(*g.node(c))});
        if (b.segments.empty())
            b.segments.push_back({a, a});
        bones.push_back(std::move(b));
    }
    return bones;
}

/** Sets the node's visibility unless something drives it. */
inline void setVisible(Graph& graph, NodeId id, bool on)
{
    PlugRef v{id, Transform::kVisibility, -1};
    if (!graph.source(v) && std::get<bool>(graph.storedValue(v)) != on)
        graph.setValue(v, on);
}
} // namespace lila::scene::deformutil
