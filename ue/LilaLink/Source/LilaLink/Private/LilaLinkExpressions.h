#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * expressionPostProcess {skeletalMesh, rig, animBlueprint?}: runs a Lila expression Control Rig from the mesh's
 * post-process Anim Blueprint and makes it the mesh's default animating rig. Lila's unrealExpressionRig calls it.
 */
void AddExpressionMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
