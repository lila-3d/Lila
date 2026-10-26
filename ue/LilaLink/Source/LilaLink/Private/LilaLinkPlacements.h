#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Level placements of Lila instance groups, keyed by Lila node id and tagged on the actors they make:
 *
 * placeInstances {group, mesh, mode: actors | ism | hism, placements: [{id, name, location, rotation (quat xyzw), scale}]}
 *   makes the open level hold exactly these placements of the static mesh: an AStaticMeshActor per member, or one actor
 *   with an (H)ISM component holding every member as an instance. Members no longer sent go; switching mode swaps one
 *   representation for the other. One undo transaction. Returns {spawned, updated, removed, mode}.
 * removePlacements {groups?: [...], ids?: [...]} removes what placeInstances made for those groups or nodes. Returns {removed}.
 * listPlacements {} lists them: [{actor, group, node?, instances?}].
 */
void AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
