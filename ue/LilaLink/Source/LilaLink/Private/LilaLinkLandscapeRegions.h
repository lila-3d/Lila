#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * setLandscapeRegions: setLandscape for landscapes too big to read in one piece. Takes setLandscape's parameters with
 * regions [{rect: [x0, y0, x1, y1], heightmap, layers: [{name, weightmap}]}] in place of heightmap: each region's
 * 16-bit heights (and 8-bit weights) cover vertices x0..x1, y0..y1 inclusive. The landscape is made flat (or found),
 * split into World Partition proxies when gridSize > 0, then written region by region on the "Lila" edit layer, one
 * region's files in memory at a time. Replies {actor, created, resolution, regions, editLayer, layers, material,
 * assets, warnings}.
 */
void AddLandscapeRegionMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
