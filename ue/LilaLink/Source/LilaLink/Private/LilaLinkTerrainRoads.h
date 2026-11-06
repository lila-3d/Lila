#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain road sends:
 *
 * setTerrainRoads {id, landscape, location, roads: [{id, label, kind, halfWidth, sideFalloff, endFalloff, raiseTerrain, lowerTerrain,
 * layer, points, mesh?, spans}]} lays the roads of Lila's terrain id as landscape splines on its landscape (the one setLandscape
 * tagged for id, else the landscape labelled landscape): a control point per point (position relative to location, Unreal
 * space, with the tangents, up and roll of setSpline's points), half-width and falloffs from the road, segments painting
 * layer and raising or lowering the landscape as asked (never inside bridge or tunnel spans), carrying mesh {asset,
 * forwardAxis, scaleToWidth} as their spline mesh. In a partitioned world the roads go on one landscape spline actor tagged
 * for id. Sending again replaces what the last send made. Replies {actor, roads, controlPoints, segments, warnings}.
 *
 * removeTerrainRoads {id} deletes them. Replies {removed}.
 */
void AddTerrainRoadMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
