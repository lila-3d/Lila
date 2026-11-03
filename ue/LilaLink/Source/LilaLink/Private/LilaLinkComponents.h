#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;

namespace LilaLink
{
/** One Lila attribute component as importAsset and placeInstances receive it: {name, mode, class?, attrs}. */
struct FLilaComponent
{
	FString Name;
	FString Mode;
	FString Class;
	TSharedRef<FJsonObject> Attrs = MakeShared<FJsonObject>();
};

/** A value as user data and tags hold it: numbers without a needless fraction, true/false, lists comma-separated. */
FString ComponentValueText(const TSharedPtr<FJsonValue>& V);

/** Owner's "components" list. False when Owner has none (a Lila that predates them), so nothing is touched. */
bool ParseComponents(const TSharedPtr<FJsonObject>& Owner, TArray<FLilaComponent>& Out);

/**
 * An imported asset's components: assetUserData into its ULilaComponentUserData, dataAsset as DA_<asset>_<component> in
 * Folder referenced from it, custom through its exporter. component and actorTags need a placed actor, so the user
 * data keeps their values for whatever places the asset. Data assets made or changed are added to OutAssets.
 */
void ApplyComponentsToAsset(UObject* Asset, const TArray<FLilaComponent>& Components, const FString& Folder, TArray<FString>& OutWarnings,
	TArray<FString>& OutAssets);

/**
 * A placed actor's components: actorTags as "Component.attr=value" tags, component as an actor component of its class
 * (reused when the actor has one), assetUserData and dataAsset in the root component's ULilaComponentUserData, custom
 * through its exporter.
 */
void ApplyComponentsToActor(AActor* Actor, const TArray<FLilaComponent>& Components, const FString& Folder, TArray<FString>& OutWarnings,
	TArray<FString>& OutAssets);

/** The example C++ exporter, "LilaExample" (LilaLinkExampleExporter.cpp). */
void RegisterExampleExporter();
void UnregisterExampleExporter();
} // namespace LilaLink
