#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * createMLDeformer {path, skeletalMesh, animSequence, geometryCache, save?}: an ML Deformer asset (the project's
 * highest priority model, Neural Morph by default) on the skeletal mesh, with the animation and its geometry cache as a
 * training input. Reruns update the asset in place and replace a training input with the same animation. Training stays
 * in the ML Deformer editor. Needs the ML Deformer Framework plugin and a model plugin enabled in the project.
 */
void AddMLDeformerMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
