#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * importImposter {contentPath, metaFile, options {materialFolder, rebuildMaterial, save}}: a bake from Lila's Imposter
 * node as a card static mesh (no Nanite, bounds that hold the card turned any way), its base colour and normal-depth
 * atlases, and a material instance of the generated M_LilaImposter on every slot. M_LilaImposter calls MF_LilaImposter,
 * which any material can call: it turns the card to the camera and blends the three nearest baked views, without dither
 * or TAA (outputs WorldPositionOffset, BaseColor, Opacity, world-space Normal). The instance sets Frames, Hemi, Centre,
 * Radius, BaseColor and Normal, parameters the function brings with it. Reimporting updates all of them in place.
 */
void AddImposterMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
