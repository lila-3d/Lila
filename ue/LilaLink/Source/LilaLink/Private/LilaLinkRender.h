#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/**
 * Renderer checks for tests and tools:
 * - rendererInfo: the editor world's feature level and shader platform, desktop forward, mobile shading path, the
 *   active preview platform, and which optional ForwardLevelUp settings this engine has (null when it lacks one).
 * - setPreviewPlatform {mode: "mobile" | "off" | <preview shader platform name>}: the editor's preview rendering level,
 *   with shaders compiled before it returns. "mobile" picks an ES3.1 mobile preview, Android when present.
 * - captureView {location, rotation | lookAt, fov, width, height, path, warmup}: renders the editor world from a
 *   transient scene capture at the current preview level and writes a PNG; returns the mean colour.
 */
void AddRenderMethods(TMap<FString, FLilaLinkMethod>& Methods);
} // namespace LilaLink
