#pragma once

#include "CoreMinimal.h"

/**
 * Methods behind terrainSendToUnreal(kind='meshTerrain'), registered with LilaLink under the feature "meshTerrain"
 * (Unreal space, cm):
 *
 * setMeshTerrain {id, label, heightmap, meshResolution: [x, y], sections: [x, y], meshSize: [x, y, z], location, rotation,
 * material?, saveAndUnload?} imports heightmap (a 16-bit greyscale PNG) with Mesh Partition's FHeightmapImporter: a Mesh
 * Partition actor whose base modifiers (one mesh provider per section) hold Lila's terrain. The sections are then
 * placed: the importer centres the mesh on the origin from Z 0, location and rotation move it to the terrain's place.
 * Sending the same id again with the same sections swaps the new meshes into the existing base modifiers, so the user's
 * own modifiers on that Mesh Partition stay; other sections replace the base modifiers and move the user's modifiers
 * to the new actor. Replies {actor, created, sections, warnings}.
 *
 * removeMeshTerrain {id} deletes the Mesh Partition actor and its base modifiers. Replies {removed}.
 */
namespace LilaLinkMeshTerrain
{
inline const FString Feature = TEXT("meshTerrain");
void RegisterMethods();
} // namespace LilaLinkMeshTerrain
