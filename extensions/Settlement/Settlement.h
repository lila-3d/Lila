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

namespace settlement
{
/**
 * The site in a geometry: the largest closed curve or loose-edge loop is the outline, open ones are fixed streets, loose
 * points are the centre (the first) and attractors. planeY is the average height of what was given, for a flat ground.
 */
Site siteFrom(const lila::gn::Geometry& g, double* planeY = nullptr);

/** A mesh in the geometry as ground, else a plane at planeY. */
Ground groundFrom(const lila::gn::Geometry& g, double planeY);

/** Options from named values (a node's inputs, or a command's flags). Throws std::runtime_error for a bad profile, pattern or overrides. */
Options optionsFrom(const std::function<double(const char*)>& number, const std::function<int64_t(const char*)>& integer,
                    const std::function<std::string(const char*)>& text);

struct Parts
{
    lila::gn::GeometryPtr streets, lots, buildings, water;
};

/** Streets (ribbons and centrelines), lots (outlines) and buildings (envelope volumes and footprint curves). */
Parts toGeometry(const Layout& l, const Ground& ground);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace settlement
