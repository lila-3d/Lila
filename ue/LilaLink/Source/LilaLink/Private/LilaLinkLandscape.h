#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's terrain sends:
 *
 * setLandscape {id, label, heightmap, resolution: [x, y], quadsPerSection, sectionsPerComponent, location, scale, material?,
 * gridSize?} makes the landscape Lila's terrain id sends, or updates it in place. heightmap is a raw 16-bit file in Unreal's
 * layout, resolution vertices a side, read on this machine. Lila's heights go to an edit layer named "Lila", so layers the
 * user adds on top survive later sends. A landscape whose resolution or layout changed is rebuilt. gridSize > 0 sets the
 * World Partition grid size in a partitioned world. Replies {actor, created, resolution, editLayer}.
 *
 * removeLandscape {id} deletes it. Replies {removed}.
 */
void AddLandscapeMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
