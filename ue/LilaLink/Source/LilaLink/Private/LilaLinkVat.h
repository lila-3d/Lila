#pragma once

#include "LilaLinkServer.h"

class UStaticMesh;
class UTexture2D;

namespace LilaLink
{
/**
 * importVertexAnimation {contentPath, fbx, positions, normals?, rotations?, meta | metaFile, materialParent?, options
 * {materialFolder, rebuildMaterial, lodGroup, save}}: Lila's Vertex Animation Texture bake as a static mesh (no recomputed
 * normals, UVs at full precision, no Nanite, bounds grown to the whole animation), its data textures (nearest, no mips,
 * no sRGB, never streamed; half float EXR as half, float EXR and 16-bit PNG as RGBA32F, 8-bit PNG as RGBA8) and a
 * material instance of a generated VAT material (or materialParent) assigned to every slot. The instance sets
 * PositionTexture, NormalTexture, RotationTexture, HDR, BoundsMin, BoundsMax, FrameCount, RowsPerFrame, TextureHeight,
 * FrameRate and Absolute, each only when the parent has it. Reimporting updates all of them in place.
 *
 * A rigid bake (meta mode "rigid") takes its rotation texture from "rotations" or from the metadata's files.rotations
 * beside the position texture, and its parent is M_LilaVatRigid_UV<n>, built on the material function
 * MF_LilaVatRigid_UV<n> that any other material can call to play the same bake (outputs WorldPositionOffset, Normal,
 * Tangent, Rotation), with an Interpolate static switch for blending between frames.
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
