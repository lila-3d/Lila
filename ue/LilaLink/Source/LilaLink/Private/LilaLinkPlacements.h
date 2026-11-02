#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Level placements of Lila instance groups, keyed by Lila node id and tagged on the actors they make:
 *
 * placeInstances {group, mesh, mode: actors | ism | hism, placements: [{id, name, location, rotation (quat xyzw), scale, customData?, components?}]}
 *   makes the open level hold exactly these placements of the static mesh: an AStaticMeshActor per member, or one actor
 *   with an (H)ISM component holding every member as an instance. Members no longer sent go; switching mode swaps one
 *   representation for the other. A placement's components (Lila attribute components) go onto its actor; an (H)ISM
 *   holder takes the first placement's. One undo transaction. Returns {spawned, updated, removed, mode, warnings, assets}.
 *   A placement's lightingChannels [bool, bool, bool] set its component's Lighting Channels 0, 1 and 2 (an (H)ISM's
 *   from the first placement, as instances share them).
 * placeLights {lights: [{id, name, kind: point | spot | directional | area | disc, location, forward, up, color, intensity,
 *   units, useTemperature, temperature, range, innerConeAngle, outerConeAngle, width, height, sourceRadius, castShadows,
 *   lightingChannels?}]} spawns or updates a light actor per Lila light, matched by node id (forward is where it shines).
 *   Returns {spawned, updated, warnings}.
 * removePlacements {groups?: [...], ids?: [...]} removes what placeInstances made for those groups or nodes. Returns {removed}.
 * listPlacements {} lists them: [{actor, group, node?, instances?}].
 */
void AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
