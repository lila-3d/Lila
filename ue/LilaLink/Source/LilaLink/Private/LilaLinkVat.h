#pragma once

#include "LilaLinkServer.h"

class UStaticMesh;
class UTexture2D;

namespace LilaLink
{
/**
 * importVertexAnimation {contentPath, fbx, positions, normals?, meta | metaFile, materialParent?, options {materialFolder,
 * rebuildMaterial, lodGroup, save}}: Lila's Vertex Animation Texture bake as a static mesh (no recomputed normals, the
 * addressing UV channel kept at full precision, no Nanite, bounds grown to the whole animation), its position and normal
 * textures (nearest, no mips, no sRGB, never streamed; EXR half float, 16-bit PNG RGBA32F, 8-bit RGBA8) and a material instance of a generated VAT material (or
 * materialParent) assigned to every slot. The instance sets PositionTexture, NormalTexture, HDR, BoundsMin, BoundsMax,
 * FrameCount, RowsPerFrame, TextureHeight, FrameRate and Absolute, each only when the parent has it. Reimporting
 * updates all of them in place.
 */
void AddVatMethods(TMap<FString, FLilaLinkMethod>& Methods);

/**
 * The importVertexAnimation setup on a mesh already in the project, from a bake's metadata .json: the position and
 * normal textures it names (or <name>_pos / <name>_nrm beside it) imported next to the mesh, the mesh's VAT build
 * settings and bounds, and the material instance on every slot. The mesh itself is not reimported, so it must be the
 * bake's rest mesh. Adds what it made or changed to OutAssets; false when anything failed.
 */
bool SetUpVatOnMesh(UStaticMesh* Mesh, const FString& MetaFile, TArray<FString>& OutAssets, TArray<FString>& OutErrors);

/** The VAT data texture settings on an existing texture, compression chosen from its source format as the importer does. */
void SetUpVatDataTexture(UTexture2D* Texture, bool bNormals);
} // namespace LilaLink
