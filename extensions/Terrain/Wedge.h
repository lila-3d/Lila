#pragma once

#include "Terrain.h"

namespace terrain
{
/** terrainMap (a shaded top-down PNG of a terrain) and terrainWedge (spec variants in parallel lilabatch processes, as a contact sheet). */
void registerWedge(lila::ext::Context& ctx);
} // namespace terrain
