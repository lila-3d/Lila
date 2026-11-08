#pragma once

#include "SettlementCore.h"

#include <lila/GeometryNodes.h>

#include <cstdint>
#include <functional>
#include <string>

namespace lila::ext
{
class Context;
}
namespace lila
{
struct Args;
}
namespace lila::scene
{
class Graph;
}

namespace settlement
{
/**
 * The site in a geometry: the largest closed curve or loose-edge loop is the outline, open ones are fixed streets, loose
 * points are the centre (the first) and attractors. planeY is the average height of what was given, for a flat ground.
 */
Site siteFrom(const lila::gn::Geometry& g, double* planeY = nullptr);

/** A mesh in the geometry as ground, else a plane at planeY. */
Ground groundFrom(const lila::gn::Geometry& g, double planeY);
/** A mesh in the geometry as a walkable surface, else none. */
WalkSurface walkFrom(const lila::gn::Geometry& g, const WalkOptions& o);
/** The edits input's JSON; an empty object for empty text. Throws std::runtime_error when it is not a JSON object. */
json parseEdits(const std::string& text);

/** Options from named values (a node's inputs, or a command's flags). Throws std::runtime_error for a bad profile, pattern or overrides. */
Options optionsFrom(const std::function<double(const char*)>& number, const std::function<int64_t(const char*)>& integer,
                    const std::function<std::string(const char*)>& text, const std::function<bool(const char*)>& flag);

struct Parts
{
    lila::gn::GeometryPtr streets, lots, buildings, water;
};

/** Streets (ribbons and centrelines), lots (outlines) and buildings (envelope volumes and footprint curves). */
Parts toGeometry(const Layout& l, const Ground& ground);

/** The layout a settlement modifier shows, rebuilt from its inputs, ground, walkable mesh and edits. */
struct Rebuilt
{
    Layout layout;
    Ground ground;
    WalkSurface walk;
    Options options;
    json values;
    std::string mesh, modifier;
};

/** Throws CommandError naming cmd when name is not a settlement or its layout fails. */
Rebuilt rebuildSettlement(lila::scene::Graph& g, const std::string& name, const char* cmd);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
/** settlementGrade and settlementSendToUnreal. */
void registerUnrealCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
/** settlementExport (files: JSON, GeoJSON, USD, DXF, Unity, Godot). */
void registerExportCommands(lila::ext::Context& ctx);
/** Appends the placements flag's entries (a list or its JSON text) to the layout's placements; CommandError naming cmd when malformed. */
void addPlacements(Layout& l, const lila::Args& a, const char* cmd);
/** settlementRoadMesh, settlementFurniture, settlementNames, settlementLanes and settlementTransit. */
void registerStreetscapeCommands(lila::ext::Context& ctx);
} // namespace settlement
