#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "LilaExampleHealthComponent.generated.h"

UENUM(BlueprintType)
enum class ELilaExampleTeam : uint8
{
	Neutral,
	Red,
	Blue,
};

/**
 * EXAMPLE: a component Lila's "component" export mode fills. In Lila, give an object attributes under one heading
 * named like these properties (maxHealth, regenerates, team as an enum red:blue, teamColor as a colour, displayName),
 * then set the component's Unreal export to component with class /Script/LilaLinkRuntime.LilaExampleHealthComponent.
 * Names match case-insensitively; a bool also matches without its b prefix. Copy it into your game module to start
 * your own.
 */
UCLASS(ClassGroup = (Lila), meta = (BlueprintSpawnableComponent))
class LILALINKRUNTIME_API ULilaExampleHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
	float MaxHealth = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
	bool bRegenerates = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
	ELilaExampleTeam Team = ELilaExampleTeam::Neutral;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
	FLinearColor TeamColor = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Health")
	FString DisplayName;
};
