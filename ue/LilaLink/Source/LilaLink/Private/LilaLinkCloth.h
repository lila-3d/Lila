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
} // namespace LilaLink
