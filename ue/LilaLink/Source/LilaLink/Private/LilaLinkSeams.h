#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * seamMaterials (MF_LilaContactMask, MF_LilaSeamStrip, MF_LilaSkirtBlend with their generated materials, built when created,
 * older or regenerate) and seamDecals (decal actors for contact strips, the desktop and clustered-decal opt-in).
 */
void AddSeamMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
