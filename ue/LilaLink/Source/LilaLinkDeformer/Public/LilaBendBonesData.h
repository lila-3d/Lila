#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "LilaBendBonesData.generated.h"

class USkeletalMesh;
class USkinnedMeshComponent;

/** One bend bones chain: its bones top to end, and what the deformer needs of its rest. */
USTRUCT()
struct FLilaBendBonesChain
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Bend Bones")
	TArray<FName> Bones;

	/** Each bone's curve normal at rest, in the bone's own space, so its twist can be measured. */
	UPROPERTY()
	TArray<FVector> RestNormals;

	/** Arc length along the rest curve at each bone. */
	UPROPERTY()
	TArray<double> RestKnotLength;
};

/**
 * Lila Bend Bones on a skeletal mesh: its chains and each LOD 0 vertex's place on them, made by LilaLink's
 * unrealBendBones and read by the Lila Bend Bones data interface in the mesh's Deformer Graph.
 */
UCLASS(MinimalAPI)
class ULilaBendBonesData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Bend Bones")
	TArray<FLilaBendBonesChain> Chains;

	UPROPERTY(VisibleAnywhere, Category = "Bend Bones")
	int32 SamplesPerSpan = 16;

	/** 0 keeps the mesh's thickness as chains stretch, 1 thins it to keep its volume. */
	UPROPERTY(EditAnywhere, Category = "Bend Bones", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Volume = 1.0f;

	/** 0 rides the curve, 1 spreads the inside and outside of a bend evenly. */
	UPROPERTY(EditAnywhere, Category = "Bend Bones", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EvenBend = 1.0f;

	/** LOD 0 vertices bound; the deformer runs only where the mesh has this many. */
	UPROPERTY(VisibleAnywhere, Category = "Bend Bones")
	int32 VertexCount = 0;

	/**
	 * Two entries per vertex, three float4 each: (chain as uint bits or ~0u, segment as uint bits, weight, fraction),
	 * (offset in the curve frame, strand fraction), (the rest frame there as a quaternion).
	 */
	UPROPERTY()
	TArray<FVector4f> Entries;

	/** Bumped whenever the binding changes, so deformers upload it again. */
	UPROPERTY()
	int32 Revision = 0;
};

UCLASS()
class LILALINKDEFORMER_API ULilaBendBonesLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Binds the mesh's LOD 0 to bend bones chains from Lila: Json is unrealBendBones' payload (chains of bone names,
	 * volume, evenBend, samplesPerSpan, blend, each point's position and chain weights, the rest joints). Vertices take
	 * the chain weights of the Lila point at their position; places are found on curves through the reference pose.
	 * Returns a JSON report: vertices, matched, unmatched, restError (cm between Lila's rest joints and the reference
	 * pose), warnings, errors. Editor only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Bend Bones")
	static FString BindBendBones(USkeletalMesh* Mesh, const FString& Json);

	/**
	 * The Deformer Graph that runs Lila Bend Bones, made at AssetPath (/Game/...) when missing: a kernel reading the Lila
	 * Bend Bones data interface and the mesh's rest tangents, writing the bent positions and turned tangents. Returns
	 * the graph, or null with OutError saying why. Editor only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Bend Bones")
	static UObject* MakeBendBonesDeformer(const FString& AssetPath, FString& OutError);

	/**
	 * Writes the component's deformed LOD 0 positions (component space) to File as JSON once the GPU has them, to
	 * compare with Lila. Returns false when its deformer can't read back. Editor only.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Bend Bones")
	static bool WriteDeformedPositions(USkinnedMeshComponent* Component, const FString& File);
};
