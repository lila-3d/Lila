#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain water sends. Water is placement data on plain actors, never Water plugin bodies:
 *
 * setWater {id, label, splines, locators, folder?} makes or updates the water of Lila's terrain id. Each spline {id, kind
 * (river, lakeShore, seaShore), feature, closed, points: [{position, width?, depth?, waterLevel, bedHeight?, flowSpeed?,
 * discharge?, flowDirection?, distance?}], mouth?, downstream?} becomes an actor with a USplineComponent and a
 * ULilaWaterData holding the per-point data; each locator {id, kind (lakeLevel, seaLevel, spring, waterfallTop,
 * waterfallBottom), position, level?, ...} an actor at its position with a ULilaWaterData. Positions are Unreal space,
 * cm. Actors are matched by id under the terrain, updated in place, and the ones no longer sent are deleted. Replies
 * {actors, created, updated, removed}.
 *
 * removeWater {id} deletes the water sent for terrain id. Replies {removed}.
 */
void AddTerrainWaterMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
