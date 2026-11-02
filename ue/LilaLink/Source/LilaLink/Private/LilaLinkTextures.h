#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * importTextures {folder, textures: [{name, map, files: [{file, udim}], srgb, compression, lodGroup, flipGreen,
 * virtualTexture}], materialMode (none | instance), materialParent, materialFolder, materials: [{name, slot, parameters
 * {parameter: texture name}, meshes: [mesh paths]}], options {save, rebuildMaterial}}: Lila's bake textures as Texture2D assets, created or
 * updated in place so materials keep their references. Several files make one UDIM texture (blocks by tile), which is
 * virtual textured. In instance mode each material becomes a material instance of the parent with its texture
 * parameters set (only those the parent has), assigned to the slot of that name on every listed static or skeletal mesh.
 * rebuildMaterial rebuilds a "lila:painterly" or "lila:toon" parent and its function.
 */
void AddTextureMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
