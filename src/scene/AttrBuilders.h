#pragma once

#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/** Short AttrDef builders the node type files share. */
namespace lila::scene::attrdefs
{
inline constexpr uint32_t kStoredFlags = AttrInput | AttrStorable;

/** A keyable float. */
inline AttrDef floatAttr(std::string name, double def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kStoredFlags | AttrKeyable, .doc = std::move(doc),
            .minValue = lo, .maxValue = hi};
}

/** A keyable float with a short name. */
inline AttrDef floatAttr(std::string name, std::string shortName, double def, std::string doc, std::optional<double> lo = {},
                         std::optional<double> hi = {})
{
    return {.name = std::move(name), .shortName = std::move(shortName), .type = PlugType::Float, .defaultValue = def,
            .flags = kStoredFlags | AttrKeyable, .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

inline AttrDef boolAttr(std::string name, bool def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kStoredFlags, .doc = std::move(doc)};
}

inline AttrDef enumAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kStoredFlags, .enumNames = std::move(names),
            .doc = std::move(doc)};
}

/** An enum that starts at its first name. */
inline AttrDef enumAttr(std::string name, std::vector<std::string> names, std::string doc)
{
    return enumAttr(std::move(name), std::move(names), 0, std::move(doc));
}

/** A vector with X, Y and Z children; shortName may be empty. */
inline AttrDef vec3Attr(std::string name, std::string shortName, glm::dvec3 def, uint32_t flags, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = flags, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + char('X' + i);
        a.childShort[i] = shortName.empty() ? "" : shortName + char('x' + i);
    }
    return a;
}

inline AttrDef vec3Attr(std::string name, glm::dvec3 def, uint32_t flags, std::string doc)
{
    return vec3Attr(std::move(name), {}, def, flags, std::move(doc));
}

inline AttrDef matrixAttr(std::string name, uint32_t flags, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = flags, .doc = std::move(doc)};
}

/** A keyable modifier parameter. */
inline AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = kStoredFlags | AttrKeyable, .doc = std::move(doc),
            .minValue = lo, .maxValue = hi};
}

/** A keyable modifier parameter with enum names. */
inline AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::vector<std::string> options)
{
    AttrDef a = param(std::move(name), type, std::move(def), std::move(doc));
    a.enumNames = std::move(options);
    return a;
}

/** def, shown under category in the Attribute Editor. */
inline AttrDef inCategory(AttrDef def, std::string category)
{
    def.category = std::move(category);
    return def;
}
} // namespace lila::scene::attrdefs
