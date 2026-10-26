#pragma once

#include "MasonryCore.h"

#include <lila/GeometryNodes.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace masonry
{
/** Footprints in a geometry: its curves (evaluated), then its mesh's loose edges. */
std::vector<Footprint> footprintsOf(const lila::gn::Geometry& g);

/** Build options from named values (the node's inputs, or a command's flags). Throws gn::ExecError for an unknown bond. */
BuildOptions optionsFrom(const std::function<double(const char*)>& number, const std::function<int64_t(const char*)>& integer,
                         const std::string& bond);

/** The per-stone instance attributes the Masonry Wall node writes. */
void writeStoneAttrs(geo::AttributeSet& attrs, const Built& b);

bool registerNodes(lila::ext::Context& ctx);
void registerCommands(lila::ext::Context& ctx);
void registerUi(lila::ext::Context& ctx);
} // namespace masonry
