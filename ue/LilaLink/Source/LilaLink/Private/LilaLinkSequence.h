#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Methods behind Lila's meshSequence mappings: deleteAssets {paths} (forced, no dialogs: frames past the end of a
 * shorter range) and setNiagaraMeshes {system, meshes, emitter?, renderer?, save?} (a mesh renderer's Meshes array set
 * to the frames in order, for a mesh index flipbook).
 */
void AddSequenceMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
