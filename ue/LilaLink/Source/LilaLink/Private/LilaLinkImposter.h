#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * importImposter {contentPath, metaFile, options {materialFolder, rebuildMaterial, save}}: a bake from Lila's Imposter
 * node as a card static mesh (no Nanite, bounds that hold the card turned any way), its base colour and normal-depth
 * atlases, and a material instance of the generated M_LilaImposter on every slot. The material turns the card to the
 * camera and blends the three nearest baked views, without dither or TAA; the instance sets Frames, Hemi, Centre, Radius,
 * BaseColor and Normal. Reimporting updates all of them in place.
 */
void AddImposterMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
