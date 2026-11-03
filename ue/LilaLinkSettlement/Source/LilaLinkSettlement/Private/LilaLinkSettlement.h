#pragma once

#include "CoreMinimal.h"

/**
 * Methods behind Lila's settlementSendToUnreal and road profile commands, registered with LilaLink under the feature
 * "settlement" (positions are Unreal space, cm):
 *
 * setSettlementData {id, asset, label, lots, envelopes, roads, intersections, parking} writes a PCG data asset at asset (a
 * package path), replacing its data: pin Lots (a polygon per lot), Envelopes (a polygon per footprint), EnvelopePoints (a
 * point per envelope), Roads (a spline per road), Intersections (a point per junction: Control, Approaches, Phases,
 * Radius) and ParkingSpots (a point per space, bounds the space), each with its fields as attributes. Replies {asset,
 * lots, envelopes, roads, intersections, parking}.
 *
 * setZoneGraph {id, label, profiles: {name: {lanes: [{width, direction, tags}]}}, roads: [{points, profile, tags}],
 * junctions: [{center, points: [{position, forward, profile}], tags, control, phases, crosswalks}], parking} makes the
 * lane profiles (Lila_<name>) and tags in the ZoneGraph settings, then an actor holding a spline zone shape per road and a
 * polygon zone shape per junction, and asks ZoneGraph to rebuild. When the project has MassTraffic, the tags its settings
 * name for vehicle, pedestrian, intersection, crosswalk and parking lanes go on the matching shapes too. Replies {actor,
 * shapes, profiles, massTraffic, warnings}.
 *
 * removeSettlement {id} deletes the ZoneGraph actor made for id. Replies {removed}.
 *
 * setRoadGrammar {asset, label, modules: [{symbol, size, scalable}], grammar} writes a road cross-section as a PCG data
 * asset: pin ModulesInfo (Symbol, Size, Scalable, DebugColor per module, as Subdivide Segment reads them) and pin Grammar
 * (a Grammar string attribute). Replies {asset, modules}.
 *
 * readRoadGrammar {asset} reads a road definition's slots: from such a data asset, or from any asset holding arrays of
 * structs with a name (Symbol, Name, Slot, Id) and a size (Size, Width), and a Grammar string if it has one. Replies
 * {asset, class, modules: [{symbol, size}], grammar}.
 */
namespace LilaLinkSettlement
{
inline const FString Feature = TEXT("settlement");
void RegisterMethods();
} // namespace LilaLinkSettlement
