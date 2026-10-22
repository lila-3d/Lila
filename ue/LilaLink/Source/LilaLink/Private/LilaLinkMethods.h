#pragma once

#include "LilaLinkServer.h"

namespace LilaLink
{
/** Every JSON-RPC method the server answers, by name. */
TMap<FString, FLilaLinkMethod> MakeMethods();

/**
 * importAsset: Interchange when it can read the file, the legacy FBX factory otherwise. Interchange runs
 * asynchronously; the call gets progress notifications until the reply.
 */
void ImportAsset(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call);

/** Saves the packages of these object or package paths. Returns false if any failed. */
bool SaveAssetPaths(const TArray<FString>& Paths, TArray<FString>& OutErrors);
} // namespace LilaLink
