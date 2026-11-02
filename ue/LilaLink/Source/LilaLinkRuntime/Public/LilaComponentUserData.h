#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"

#include "LilaComponentUserData.generated.h"

class UDataAsset;

/** One Lila component's values as text, by attribute name, with how it was meant to reach Unreal. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaComponentValues
{
	GENERATED_BODY()

	/** Numbers, true/false, strings, enum labels; vectors and lists as comma-separated values ("0,0.5,1"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lila")
	TMap<FName, FString> Values;

	/** The export mode Lila chose: assetUserData, dataAsset, or component/actorTags kept here because no actor received them. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Mode;

	/** The class Lila named for component and dataAsset modes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lila")
	FString ClassPath;

	/** dataAsset mode: the data asset holding the values. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lila")
	TObjectPtr<UDataAsset> DataAsset;
};

/**
 * Lila attribute components on a mesh asset or an actor's component, by component name (the heading in Lila's
 * Attribute Editor; "" holds attributes added without one). LilaLink rewrites it on every send.
 */
UCLASS(BlueprintType)
class LILALINKRUNTIME_API ULilaComponentUserData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lila")
	TMap<FString, FLilaComponentValues> Components;

	/** On an actor's root component: the "Component.attr=value" tags LilaLink gave the actor, replaced on the next send. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TArray<FName> ActorTags;

	/** The value of Component's Attribute as text; false when either is missing. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	bool GetValue(const FString& Component, FName Attribute, FString& OutValue) const;

	/** The value as a number; Fallback when it is missing or not a number. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	double GetNumber(const FString& Component, FName Attribute, double Fallback = 0.0) const;

	/** dataAsset mode: the component's data asset, or null. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	UDataAsset* GetDataAsset(const FString& Component) const;

	/** The Lila user data on an asset or component that carries asset user data (static mesh, actor component), or null. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	static ULilaComponentUserData* FindOn(UObject* Owner);
};
