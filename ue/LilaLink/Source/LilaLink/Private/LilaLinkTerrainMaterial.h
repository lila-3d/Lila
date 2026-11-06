#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * setTerrainMaterial {id, label, layers: [{name, color: [r, g, b] linear, roughness, tileSize (cm)}], rvt, assign} gives
 * the landscape setLandscape made for Lila's terrain id its material layers: each layer's info weight-blended, and with
 * assign a generated M_<label>_Terrain built on MF_LilaTerrainLayer (one call per layer, adding weight times the layer's
 * tinted texture and roughness, so weights summing to 1 mix exactly) set as its material. rvt also writes a runtime
 * virtual texture RVT_<label> (base colour and roughness) from that material, adds it to the landscape and fits a
 * volume to the landscape, so meshes can blend into the ground. Replies {material, function, rvt, volume, layers,
 * assets, warnings}.
 */
void AddTerrainMaterialMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
