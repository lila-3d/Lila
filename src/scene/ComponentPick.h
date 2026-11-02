#pragma once

#include "core/Export.h"

#include "scene/Components.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene::componentPick
{
/** World to widget pixels the way the viewport maps them: clip space scaled to the widget, no y flip. */
struct LILA_SCENE_API Screen
{
    glm::dmat4 viewProj{1.0};
    double width = 1.0;
    double height = 1.0;

    /** nullopt behind the camera. */
    std::optional<glm::dvec2> project(const glm::dvec3& world) const;
    /** The part of segment a-b in front of the near plane, in pixels; nullopt when none of it is. */
    std::optional<std::pair<glm::dvec2, glm::dvec2>> projectSegment(const glm::dvec3& a, const glm::dvec3& b) const;
    /** The part of the polygon in front of the near plane, in pixels; empty when none of it is. */
    std::vector<glm::dvec2> projectPolygon(const std::vector<glm::dvec3>& corners) const;
};

/** How close, in pixels, the cursor must be to a vertex or edge for a click to take it. */
LILA_SCENE_API double radius(ComponentMode mode);

struct Candidate
{
    double px = 0.0;
    uint32_t id = 0;
    auto operator<=>(const Candidate&) const = default;
};

/**
 * Vertices (by their point) or edges (by the projected segment) of the mesh closer than maxPx to the cursor, nearest
 * first. Face mode has none: faces are picked by raycast.
 */
LILA_SCENE_API std::vector<Candidate> nearby(const geo::Mesh& mesh, const glm::dmat4& world, ComponentMode mode, const Screen& screen,
                              glm::dvec2 cursor, double maxPx);

/** The world point that must be unoccluded for a candidate to count: the vertex, or the edge's midpoint. */
LILA_SCENE_API glm::dvec3 anchor(const geo::Mesh& mesh, const glm::dmat4& world, ComponentMode mode, uint32_t id);

/** What a click under the cursor would select, kept while hovering. Object mode leaves id unused. */
struct Hover
{
    ComponentMode mode = ComponentMode::Object;
    NodeId node = kNoNode;
    uint32_t id = 0;

    bool empty() const { return node == kNoNode; }
    bool operator==(const Hover&) const = default;
};

/**
 * Point ids of the triangles covering face f, 3 per triangle, split exactly as the shaded mesh splits it. Overlays
 * that fill a face use these so concave and non-planar n-gons cover the same area the surface does.
 */
LILA_SCENE_API std::vector<uint32_t> fillTriangles(const geo::Mesh& mesh, uint32_t f);

/** Topology that hover feedback calls out. */
enum class Topology : uint8_t
{
    Plain,
    Triangle, // a 3-sided face
    Ngon,     // a face of 5 or more sides
    Pole,     // a vertex with 5 or more edges
};

LILA_SCENE_API Topology faceTopology(const geo::Mesh& mesh, uint32_t f);
LILA_SCENE_API Topology pointTopology(const geo::Mesh& mesh, uint32_t p);

/** Pref keys ("general") for a kind's hover callout: whether it is on and its sRGB "#rrggbb" color. */
LILA_SCENE_API std::string topologyEnabledKey(Topology t);
LILA_SCENE_API std::string topologyColorKey(Topology t);
LILA_SCENE_API const char* topologyDefaultColor(Topology t);
} // namespace lila::scene::componentPick

namespace lila::scene
{
class Node;
}

namespace lila::scene::selectionMask
{
/** Maya's object selection masks: which kinds of object clicks and marquees may pick. Each persists as a "general" pref, on by default. */
enum class Kind : uint8_t
{
    Curve,
    Mesh,
    Joint,
    Locator,
    Other,
    Light,
};

inline constexpr Kind kAll[] = {Kind::Curve, Kind::Mesh, Kind::Joint, Kind::Light, Kind::Locator, Kind::Other};

/** "mesh", "joint", ...: the leaf of the pref key and of the "select.mask.<name>" action. */
LILA_SCENE_API const char* name(Kind k);
LILA_SCENE_API std::string prefKey(Kind k);
LILA_SCENE_API bool enabled(Kind k);
LILA_SCENE_API void setEnabled(Kind k, bool on);

/** Maya's Show menu filters: whether a kind draws in the viewport at all. Hidden kinds can't be picked either. */
LILA_SCENE_API std::string showKey(Kind k);
LILA_SCENE_API bool shown(Kind k);
LILA_SCENE_API void setShown(Kind k, bool on);
/** Whether the kind node falls under is shown. */
LILA_SCENE_API bool shown(const Node& node);

/** The mask an object falls under, by its node type. */
LILA_SCENE_API Kind kindOf(const Node& node);

LILA_SCENE_API bool pickable(const Node& node);
} // namespace lila::scene::selectionMask
