#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "LilaInstanceData.generated.h"

/** One attribute's value for every instance, as text, by instance index. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaInstanceColumn
{
	GENERATED_BODY()

	/** Numbers, true/false, strings, enum labels; vectors and lists comma-separated. Empty where an instance has no value. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TArray<FString> Values;
};

/**
 * Per-instance Lila data on an actor holding an instanced static mesh component that LilaLink placed: each instance's Lila
 * key, its attribute component values, and what its custom primitive data slots hold. LilaLink rewrites it on every send.
 */
UCLASS(BlueprintType, ClassGroup = (Lila), meta = (BlueprintSpawnableComponent))
class LILALINKRUNTIME_API ULilaInstanceData : public UActorComponent
{
	GENERATED_BODY()

public:
	/** Each instance's Lila placement key, by instance index. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TArray<FString> InstanceIds;

	/** By attribute: "Component.attr", or the attr's name when it was added without a component. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TMap<FName, FLilaInstanceColumn> Columns;

	/** What each custom primitive data float holds, by slot; a vector's name fills its slots as Name.0, Name.1, Name.2. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TArray<FName> CustomDataNames;

	/** The instance's value of Attribute as text; false when either is missing. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	bool GetValue(int32 InstanceIndex, FName Attribute, FString& OutValue) const;

	/** The value as a number; Fallback when it is missing or not a number. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	double GetNumber(int32 InstanceIndex, FName Attribute, double Fallback = 0.0) const;

	/** The instance index of a Lila placement key, or -1. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	int32 FindInstance(const FString& Id) const;

	/** The custom primitive data slot holding Name (or Name.0 for a vector), or -1. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	int32 FindCustomDataSlot(FName Name) const;

	/** The actor's Lila instance data, or null. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	static ULilaInstanceData* FindOn(const AActor* Actor);
};
