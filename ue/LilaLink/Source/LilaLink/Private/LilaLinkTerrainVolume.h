#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * The volume half of Lila's terrain sends (caves, overhangs, arches split off a heightfield):
 *
 * setLandscapeVolume {id, label, resolution: [x, y], visibility?, meshes: [static mesh paths], material?} finishes what
 * setLandscape started for the same id. visibility is an 8-bit file in the landscape's vertex layout (255 is a hole),
 * written to the Lila edit layer's visibility; a generated landscape material gets a Landscape Visibility Mask so the holes
 * render, and a user's material without one is reported. meshes get material, or MI_<Label>_Volume on M_LilaTerrainVolume
 * (built on MF_LilaTerrainVolume, a triplanar ground for any material), with the landscape material's GroundColor and
 * GroundRoughness. Replies {holes, holeMaterial, material, meshes, assets, warnings}.
 */
void AddTerrainVolumeMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
