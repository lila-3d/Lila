#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * applyClothing {mesh, name, lod?, section?, maxDistance [[x, y, z, cm], ...], config {density, edgeStiffness,
 * bendStiffness, damping, friction, collisionThickness}, save?}: Chaos Cloth on a skeletal mesh section, made from the
 * section itself (Lila's Tailor sends the garment's game mesh). Clothing of the same name is replaced. Each cloth vertex
 * takes the max distance of the nearest sample. Replies {asset, vertices}.
 */
void AddClothMethods(TMap<FString, FLilaLinkMethod>& Methods);

/**
 * createClothAsset {contentPath, vertices2D [[u, v], ...] (pattern, cm), triangles2D, vertices3D [[x, y, z], ...] (the drape,
 * cm), triangles3D (the same triangles over the welded drape), pattern [panel per triangle], maxDistance [cm per 3D vertex],
 * skeletalMesh?, material?, config {density, edgeStiffness, bendStiffness, damping, friction, collisionThickness}, save?}:
 * a Chaos Cloth Asset built from the garment's own sewing pattern, the panels as sim patterns and the seams where their
 * points weld in 3D, bound to the root bone, with a MaxDistance weight map and property. Replies {asset, simVertices, patterns}.
 */
void AddClothAssetMethods(TMap<FString, FLilaLinkMethod>& Methods);

/** wrinkleFunction: MF_LilaTensionWrinkles (and optionally M_LilaTensionWrinkles on it) from two wrinkle normal maps. */
void AddWrinkleMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
