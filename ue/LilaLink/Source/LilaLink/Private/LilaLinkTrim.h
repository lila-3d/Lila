#pragma once

#include "LilaLinkServer.h"

class UMaterial;

namespace LilaLink
{
/**
 * Materials for Lila's trim sheets: one master, M_LilaTrim in the masters folder, built the first time it is asked for and
 * then left to artists (regenerate rebuilds its graph in place, keeping references), and an instance per sheet.
 *
 * The master reads the sheet's baked maps (normal, AO, curvature, cavity, height) through uv0 and turns them into paint:
 * worn edges from curvature, dirt in cavities, a tiling detail layer whose scale follows the sheet's world size, a second
 * material blended in by vertex colour red, and optional parallax from the height map. Detail, VertexBlend and Parallax are
 * static switches, so an instance that leaves one off compiles none of it.
 *
 * trimMaterials {folder, regenerate?, slot, meshes: [static mesh paths], instances: [{name, textures: {param: texture asset
 *   path}, scalars: {param: value}, vectors: {param: [r, g, b]}, switches: {param: bool}}]}
 * makes or updates each instance in folder under the master and sets the first on every slot named slot of the meshes.
 * Returns {master, masterVersion, built, instances, assigned: [{mesh, slot, material}], created, warnings}.
 */
void AddTrimMethods(TMap<FString, FLilaLinkMethod>& Methods);

/** Builds M_LilaTrim when missing, or rebuilds its graph when bRegenerate. Null with OutWarnings on failure. */
UMaterial* TrimMaster(bool bRegenerate, bool& bOutBuilt, int32& OutVersion, TArray<FString>& OutCreated, TArray<FString>& OutWarnings);
} // namespace LilaLink
