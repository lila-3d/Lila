#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "LilaExampleComponentData.generated.h"

/**
 * EXAMPLE: a data asset Lila's "dataAsset" export mode creates and fills. In Lila, set a component's Unreal export to
 * dataAsset with class /Script/LilaLinkRuntime.LilaExampleComponentData; LilaLink makes DA_<object>_<component> beside
 * the imported asset, sets the properties named like the attributes (gold, lootTable, spawnOffset) and references it
 * from the asset's ULilaComponentUserData. Copy it into your game module to start your own.
 */
UCLASS(BlueprintType)
class LILALINKRUNTIME_API ULilaExampleComponentData : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loot")
	int32 Gold = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loot")
	FName LootTable;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loot")
	FVector SpawnOffset = FVector::ZeroVector;
};
