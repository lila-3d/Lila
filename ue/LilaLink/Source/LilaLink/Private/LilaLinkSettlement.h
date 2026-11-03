#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's settlementSendToUnreal (positions are Unreal space, cm):
 *
 * setSettlementData {id, asset, label, lots: [{id, block, outline, use, area, frontages, corner, frontClass, seed}],
 * envelopes: [{lot, footprint, sides, center, front, base, ground, top, maxHeight, height, floors, floorHeight, use, roof,
 * era, style, seed}], roads: [{id, class, profile, width, lanes, surface, path, points}]} writes a PCG data asset at asset
 * (a package path), replacing its data: pin Lots (a polygon per lot), Envelopes (a polygon per footprint), EnvelopePoints
 * (a point per envelope) and Roads (a spline per road), each with its fields as attributes (the polygons' and splines' on
 * the data domain, the points' per point). Replies {asset, lots, envelopes, roads}.
 *
 * setZoneGraph {id, label, profiles: {name: {lanes: [{width, direction: forward | backward | none, tags: [...]}]}},
 * roads: [{points, profile, tags}], junctions: [{center, points: [{position, forward, profile}], tags}]} makes the lane
 * profiles (named Lila_<name>) and tags in the project's ZoneGraph settings, then an actor holding a spline zone shape
 * per road and a polygon zone shape per junction, and asks ZoneGraph to rebuild. Replies {actor, shapes, profiles}.
 *
 * removeSettlement {id} deletes the ZoneGraph actor made for id. Replies {removed}.
 */
void AddSettlementMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
