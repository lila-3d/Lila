#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Methods other plugins add to LilaLink's server, so features that need plugins LilaLink does not depend on (PCG,
 * ZoneGraph, MassTraffic) live in their own optional plugin. Register in StartupModule with a feature name (it shows in
 * hello's features, which Lila checks before calling), unregister the feature in ShutdownModule. Methods run on the game
 * thread and answer before returning.
 */
namespace LilaLink
{
using FExternalMethod = TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonObject>& Params, FString& OutError)>;

/** Replaces any method of the same name. */
LILALINK_API void RegisterMethod(const FString& Name, FExternalMethod Method, const FString& Feature);
/** Removes every method registered under the feature. */
LILALINK_API void UnregisterFeature(const FString& Feature);
/** The features with registered methods. */
LILALINK_API TArray<FString> ExternalFeatures();
} // namespace LilaLink
