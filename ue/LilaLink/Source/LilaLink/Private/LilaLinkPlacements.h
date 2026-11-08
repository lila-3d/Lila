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
 *   hlodLayer (an HLOD layer asset path) puts every actor of the group on that layer.
 * placeLights {lights: [{id, name, kind: point | spot | directional | area | disc, location, forward, up, color, intensity,
 *   units, useTemperature, temperature, range, innerConeAngle, outerConeAngle, width, height, sourceRadius, castShadows,
 *   lightingChannels?}]} spawns or updates a light actor per Lila light, matched by node id (forward is where it shines).
 *   Returns {spawned, updated, warnings}.
 * removePlacements {groups?: [...], ids?: [...]} removes what placeInstances made for those groups or nodes. Returns {removed}.
 * listPlacements {} lists them: [{actor, group, node?, instances?}].
 *
 * A placement's physics {simulate, role, material, density, friction, restitution, mass?, collision: simple |
 *   complexAsSimple} sets its component's physical material (PM_<material>, made beside the mesh), and on an actor its
 *   Simulate Physics (Movable when it simulates) and mass; the first placement's collision sets the mesh's collision
 *   complexity, so UCX_ hulls or its own triangles are what it collides with.
 * makeLevelInstance {key, name, kind: levelInstance | packedLevelActor, level, groups: [...]} moves the actors those
 *   placement groups made into a new level (level, or level_2, ... when taken) and places it as a Level Instance or
 *   Packed Level Actor tagged with the key; one made earlier for the key is removed first. Returns {actor, level, kind,
 *   actors, replaced}.
 */
void AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
