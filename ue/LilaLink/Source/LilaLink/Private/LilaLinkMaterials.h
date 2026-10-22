#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/SoftObjectPath.h"

namespace LilaLink
{
/** Slot name Lila gives faces with no material; it stays on the engine default material. */
inline const FName DefaultSlotName(TEXT("Default"));

/**
 * How the slots of an import get their materials. Managed requests (params.materials present, as Lila's sync sends)
 * match each slot to an Unreal material by name; others leave materials to Interchange.
 */
struct FMaterialRequest
{
	bool bManaged = false;
	/** Create a material for a slot no Unreal material matches. */
	bool bCreate = true;
	bool bImportTextures = true;
	/** On reimport, keep a material assigned in Unreal to a slot that still exists. */
	bool bKeep = true;
	FString AssetFolder;
	/** Searched first, and where created materials and textures go. */
	FString MaterialFolder;
	/** Lila's inputs per material name: baseColor, roughness, baseColorMap (absolute), ... */
	TMap<FName, TSharedPtr<FJsonObject>> Inputs;
	/** Slot name to material on the asset before a reimport. */
	TMap<FName, FSoftObjectPath> Previous;
};

/** What happened to one slot, for the reply. How is kept, found, created, default, ambiguous or none. */
struct FSlotResult
{
	FString Asset;
	FName Slot;
	FString Material;
	FString How;
};

bool ParseMaterialRequest(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Options, const FString& AssetFolder,
	FMaterialRequest& Out, FString& OutError);

/** Records the slot materials of a static or skeletal mesh about to be reimported. */
void SnapshotMaterials(UObject* Asset, FMaterialRequest& Request);

/** Assigns a material to every slot of the imported meshes. Created materials and textures are added to OutCreated. */
void ApplyMaterials(const FMaterialRequest& Request, const TArray<FString>& ImportedAssets, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots);
} // namespace LilaLink
