#pragma once

#include "CoreMinimal.h"

class UMaterialFunction;

namespace LilaLink
{
/**
 * HLSL statements for a Custom node that leave `float3 heightNormal`: the normal of a height map's surface (DirectX tangent
 * space, as Unreal samples normal maps) with a detail normal laid on it by reoriented normal mapping, the rebuild Lila's
 * Normals From Height draws. Tex names a texture object input (with its Tex##Sampler), Uv a float2, Texel the height's
 * texel size, Slope the height's depth over the world size of the UV square per axis, Detail a unit tangent normal.
 */
FString HeightNormalHlsl(const TCHAR* Tex, const TCHAR* Uv, const TCHAR* Texel, const TCHAR* Slope, const TCHAR* Detail);

/**
 * MF_LilaHeightNormal in the masters folder, built when created, built at an older version or bRebuild: HeightMap,
 * DetailNormal, HeightDepth (cm from 0 to 1), HeightUVWidth and HeightUVHeight (cm of world per UV unit) and UVs in, the
 * rebuilt tangent Normal out. Null when it can't be made.
 */
UMaterialFunction* HeightNormalFunction(bool bRebuild, TArray<FString>& OutCreated);
} // namespace LilaLink
