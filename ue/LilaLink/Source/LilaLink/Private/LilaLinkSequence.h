#pragma once

#include "LilaLinkServer.h"

class UNiagaraSystem;
class UStaticMesh;

namespace LilaLink
{
/**
 * Methods behind Lila's meshSequence mappings: deleteAssets {paths} (forced, no dialogs: frames past the end of a
 * shorter range) and setNiagaraMeshes {system, meshes, emitter?, renderer?, save?} (a mesh renderer's Meshes array set
 * to the frames in order, for a mesh index flipbook).
 */
void AddSequenceMethods(TMap<FString, FLilaLinkMethod>& Methods);

/**
 * Sets the Meshes array of each matching mesh renderer to Meshes in order, every entry copying entry 0's pivot, scale
 * and rotation. An empty EmitterName and a RendererIndex of INDEX_NONE match them all. Returns "Emitter/Renderer" for
 * each one set; empty when the system has no match. Undoable inside a transaction.
 */
TArray<FString> FillNiagaraMeshRenderers(UNiagaraSystem* System, const TArray<UStaticMesh*>& Meshes, const FString& EmitterName, int32 RendererIndex);
} // namespace LilaLink
