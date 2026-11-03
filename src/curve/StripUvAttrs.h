#pragma once

#include "core/Export.h"
#include "curve/Sweep.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Node;
struct AttrDef;
}

/**
 * Node attrs for swept UVs, shared by the sweep, pipe and strap nodes: uvLayout ("around" or "along"), texelDensity,
 * the strip rectangle uMin/uMax/vMin/vMax, and with caps the cap rectangle capUMin/capUMax/capVMin/capVMax.
 */
namespace lila::curve
{
LILA_SCENE_API std::vector<scene::AttrDef> stripUvAttrs(const StripUv& defaults, bool caps, const std::string& category = {});
/** prefixUMin, prefixUMax, prefixVMin, prefixVMax: one more rectangle, `what` naming what lands in it. */
LILA_SCENE_API std::vector<scene::AttrDef> uvRectAttrs(const std::string& prefix, const UvRect& def, const std::string& what,
                                                       const std::string& category = {});
/** The node's strip settings; caps stay at the defaults' when the node has no cap attrs. */
LILA_SCENE_API StripUv stripUvOf(const scene::Node& n, const StripUv& defaults);
LILA_SCENE_API UvRect uvRectOf(const scene::Node& n, const std::string& prefix);
/** Strip settings from attr values by name (command flags or presets), missing ones at the defaults'. */
LILA_SCENE_API StripUv stripUvFrom(const nlohmann::json& values, const StripUv& defaults);
/** One of the attrs stripUvAttrs or uvRectAttrs makes. */
LILA_SCENE_API bool isUvAttr(std::string_view name);
} // namespace lila::curve
