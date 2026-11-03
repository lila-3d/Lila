#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * importGeometryCollection {file, contentPath, cache?, place?, save?, materials?, options?}: Lila's baked rigid-body
 * destruction (a .lilagc.json manifest and its .bin) as a Chaos Geometry Collection at contentPath. The collection has a
 * root cluster with one rigid child per piece (bone named after the piece, at its rest transform), convex collision and
 * the manifest's materials in order, matched to Unreal materials by slot name as other imports do (interior faces carry
 * the collection's Internal flag). Reimporting replaces the asset's content in place.
 *
 * With cache (default true) and frames in the bake, CC_<name> beside it holds one Chaos cache named after the bake, laid
 * out as the geometry collection cache adapter records: the root cluster keyed until the first piece's releaseFrame, a
 * GC_Enable event per piece there that breaks the cluster, then each piece's transform per frame, and the bake's breaks
 * as GC_Breaking events. With place, a Geometry Collection actor and a Chaos Cache Manager playing the cache on it are
 * added to the editor's current level at the origin.
 *
 * Returns {assets, created, warnings, errors, materialSlots: [{asset, slot, material, how}], cache?, actors?}.
 */
void AddGeometryCollectionMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
