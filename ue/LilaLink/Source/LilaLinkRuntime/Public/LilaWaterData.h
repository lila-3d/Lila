#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "LilaWaterData.generated.h"

/** What a Lila water spline carries at one of its points. Lengths and heights cm, Unreal space. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaWaterPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Width = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Depth = 0.0f;

	/** World Z of the water surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float WaterLevel = 0.0f;

	/** World Z of the river bed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float BedHeight = 0.0f;

	/** m/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float FlowSpeed = 0.0f;

	/** m3/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Discharge = 0.0f;

	/** Unit direction the water flows, world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FVector FlowDirection = FVector::ForwardVector;

	/** Distance from the river's source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Distance = 0.0f;
};

/**
 * Placement data of a Lila terrain's water on a plain actor: a river or shoreline spline (one FLilaWaterPoint per spline
 * point, by index) or a locator (lake level, sea level, spring, waterfall top or bottom). Drive any water system from it;
 * LilaLink rewrites it on every send.
 */
UCLASS(BlueprintType, ClassGroup = (Lila), meta = (BlueprintSpawnableComponent))
class LILALINKRUNTIME_API ULilaWaterData : public UActorComponent
{
	GENERATED_BODY()

public:
	/** river, lakeShore, seaShore, lakeLevel, seaLevel, spring, waterfallTop or waterfallBottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Kind;

	/** The feature's id in Lila, such as main_2 or tarn. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString FeatureId;

	/** The Lila feature it came from (the river or lake command's id). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Feature;

	/** World Z of the water surface, for locators and shorelines. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float WaterLevel = 0.0f;

	/** Per spline point, by index; empty on locators. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaWaterPoint> Points;

	/** Everything else Lila knows about it as text: mouth, downstream, river, drop, area, maxDepth, volume, lakeKind, pool. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TMap<FName, FString> Values;

	/** The point data at a spline input key, blended between the points either side; default data when there are none. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	FLilaWaterPoint GetPointAtInputKey(float InputKey) const;

	/** A value as a number; Fallback when it is missing or not a number. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	double GetNumber(FName Name, double Fallback = 0.0) const;

	/** The actor's Lila water data, or null. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	static ULilaWaterData* FindOn(const AActor* Actor);
};
