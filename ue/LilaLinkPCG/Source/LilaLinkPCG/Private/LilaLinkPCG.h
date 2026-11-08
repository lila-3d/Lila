#pragma once

#include "CoreMinimal.h"

/**
 * General PCG data from Lila (seam points and curves, cliff regions, anything with attributes), registered with LilaLink
 * under the feature "pcgData". Positions are Unreal space, cm.
 *
 * setPcgData {asset, label, sets: [{kind, pin, tag, attributes, points | path, closed}]} writes a PCG data asset at asset
 * (a package path), replacing its data. Each set becomes one tagged entry on its pin:
 * - kind "points": points [{location, rotation [x, y, z, w], scale, boundsMin, boundsMax, density, attributes}]; number,
 *   bool, string and [x, y, z] attributes become per-point attributes (double, bool, string, vector).
 * - kind "spline": path [[x, y, z]...] as a linear spline, closed or open.
 * - kind "polygon": path as a polygon on the XY plane at the first point's height.
 * The set's own attributes go on the data domain. Replies {asset, sets, points}.
 */
namespace LilaLinkPCG
{
inline const FString Feature = TEXT("pcgData");
void RegisterMethods();
} // namespace LilaLinkPCG
