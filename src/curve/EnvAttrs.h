#pragma once

#include "curve/CurveNodes.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

/** Attr and flag helpers the contour and cable nodes share. Internal to lila_curve_scene. */
namespace lila::curve::env
{
using scene::AttrDef;
using scene::PlugType;

inline constexpr uint32_t kIn = scene::AttrInput | scene::AttrStorable;
inline constexpr uint32_t kKey = scene::AttrInput | scene::AttrStorable | scene::AttrKeyable;

inline AttrDef fAttr(std::string name, double def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

inline AttrDef iAttr(std::string name, int64_t def, std::string doc, double lo = 0.0, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}

inline AttrDef bAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
}

inline AttrDef eAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kIn, .enumNames = std::move(names), .doc = std::move(doc)};
}

inline AttrDef vAttr(std::string name, const glm::dvec3& def, std::string doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
        a.childNames[size_t(i)] = name + char('X' + i);
    return a;
}

inline AttrDef meshIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Mesh, .defaultValue = scene::MeshPtr(), .flags = scene::AttrInput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef matrixIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = scene::AttrInput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef dataIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = scene::DataPtr(), .flags = scene::AttrInput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef dataOut(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Data, .defaultValue = scene::DataPtr(), .flags = scene::AttrOutput | scene::AttrHidden, .doc = std::move(doc)};
}

inline AttrDef stringOut(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = scene::AttrOutput, .doc = std::move(doc)};
}

inline int idx(const scene::Node& n, const char* a) { return n.type().attrIndex(a); }
inline double f(const scene::Node& n, const char* a) { return n.as<double>(idx(n, a)); }
inline int64_t i(const scene::Node& n, const char* a) { return n.as<int64_t>(idx(n, a)); }
inline bool b(const scene::Node& n, const char* a) { return n.as<bool>(idx(n, a)); }
inline glm::dvec3 v(const scene::Node& n, const char* a) { return n.as<glm::dvec3>(idx(n, a)); }

/** Option flags for every editable attr of a node type (enums by name), with no defaults so only given ones apply. */
inline std::vector<FlagDef> attrFlags(const std::string& type)
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : scene::types::find(type)->ownAttrs)
    {
        if (!(d.flags & scene::AttrInput) || (d.flags & scene::AttrHidden))
            continue;
        FlagDef fd{d.name, "", ArgType::Float, nullptr, d.doc};
        fd.min = d.minValue;
        fd.max = d.maxValue;
        switch (d.type)
        {
        case PlugType::Bool: fd.type = ArgType::Bool; break;
        case PlugType::Int: fd.type = ArgType::Int; break;
        case PlugType::Vec3: fd.type = ArgType::Vec3; break;
        case PlugType::String: fd.type = ArgType::String; break;
        case PlugType::Enum:
            fd.type = ArgType::String;
            fd.choices = d.enumNames;
            break;
        default: break;
        }
        out.push_back(std::move(fd));
    }
    return out;
}

/** Sets the node's editable attrs from the flags given (enum values by name). */
inline void applyFlags(scene::Graph& g, scene::Node& n, const Args& a, const char* cmd)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & scene::AttrInput) || (d.flags & scene::AttrHidden) || !a.has(d.name))
            continue;
        const int at = n.type().attrIndex(d.name);
        const json& val = a.flags[d.name];
        if (d.type == PlugType::Enum && val.is_string())
        {
            auto it = std::ranges::find(d.enumNames, val.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(std::string(cmd) + ": " + d.name + ": unknown value '" + val.get<std::string>() + "'");
            g.setValue({n.id(), at, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            g.setValue({n.id(), at, -1}, scene::fromJson(d, val));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + d.name + ": " + e.what());
        }
    }
}

/** Bakes what a history node makes into its destination and drops the node. */
inline void bake(scene::Graph& g, scene::Node& op, const scene::PlugRef& dst)
{
    g.evaluate();
    scene::PlugValue val = g.value(dst);
    g.disconnect(dst);
    g.setValue(dst, val);
    g.deleteNode(op.id());
}
} // namespace lila::curve::env
