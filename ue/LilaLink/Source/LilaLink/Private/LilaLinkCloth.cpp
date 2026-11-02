#include "LilaLinkCloth.h"

#include "ChaosCloth/ChaosClothConfig.h"
#include "ClothingAsset.h"
#include "ClothingAssetFactoryInterface.h"
#include "ClothingSystemEditorInterfaceModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshModel.h"

namespace
{
void ApplyClothing(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCloth::ApplyClothing);

	const FString MeshPath = Params->GetStringField(TEXT("mesh"));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		Call->Reply(nullptr, FString::Printf(TEXT("no skeletal mesh at %s"), *MeshPath));
		return;
	}
	const FString Name = Params->HasField(TEXT("name")) ? Params->GetStringField(TEXT("name")) : Mesh->GetName() + TEXT("_Cloth");
	const int32 Lod = Params->HasField(TEXT("lod")) ? int32(Params->GetNumberField(TEXT("lod"))) : 0;
	const int32 Section = Params->HasField(TEXT("section")) ? int32(Params->GetNumberField(TEXT("section"))) : 0;

	// Replace clothing a previous send made: unbind it everywhere, then drop it from the mesh.
	TArray<TObjectPtr<UClothingAssetBase>>& Existing = Mesh->GetMeshClothingAssets();
	for (int32 i = Existing.Num() - 1; i >= 0; --i)
		if (Existing[i] && Existing[i]->GetName() == Name)
		{
			Existing[i]->UnbindFromSkeletalMesh(Mesh, Lod, Section);
			Existing.RemoveAt(i);
		}

	FClothingSystemEditorInterfaceModule& Editor = FModuleManager::LoadModuleChecked<FClothingSystemEditorInterfaceModule>(TEXT("ClothingSystemEditorInterface"));
	UClothingAssetFactoryBase* Factory = Editor.GetClothingAssetFactory();
	if (!Factory)
	{
		Call->Reply(nullptr, TEXT("no clothing asset factory"));
		return;
	}
	FSkeletalMeshClothBuildParams Build;
	Build.AssetName = Name;
	Build.LodIndex = Lod;
	Build.SourceSection = Section;
	Build.bRemoveFromMesh = false;
	UClothingAssetCommon* Cloth = Cast<UClothingAssetCommon>(Factory->CreateFromSkeletalMesh(Mesh, Build));
	if (!Cloth || Cloth->LodData.Num() == 0)
	{
		Call->Reply(nullptr, FString::Printf(TEXT("couldn't make clothing from section %d of LOD %d"), Section, Lod));
		return;
	}
	Mesh->AddClothingAsset(Cloth);

	FClothPhysicalMeshData& Physical = Cloth->LodData[0].PhysicalMeshData;
	TArray<FVector4f> Samples;
	for (const TSharedPtr<FJsonValue>& V : Params->GetArrayField(TEXT("maxDistance")))
	{
		const TArray<TSharedPtr<FJsonValue>>& S = V->AsArray();
		if (S.Num() == 4)
			Samples.Emplace(float(S[0]->AsNumber()), float(S[1]->AsNumber()), float(S[2]->AsNumber()), float(S[3]->AsNumber()));
	}
	FPointWeightMap& MaxDistance = Physical.FindOrAddWeightMap(EWeightMapTargetCommon::MaxDistance);
	MaxDistance.Values.SetNum(Physical.Vertices.Num());
	for (int32 v = 0; v < Physical.Vertices.Num(); ++v)
	{
		const FVector3f P = Physical.Vertices[v];
		float Best = TNumericLimits<float>::Max(), Value = 0.f;
		for (const FVector4f& S : Samples)
		{
			const float D = FVector3f::DistSquared(P, FVector3f(S.X, S.Y, S.Z));
			if (D < Best)
			{
				Best = D;
				Value = S.W;
			}
		}
		MaxDistance.Values[v] = Value;
	}

	if (UChaosClothConfig* Config = Cloth->GetClothConfig<UChaosClothConfig>())
	{
		const TSharedPtr<FJsonObject>* In = nullptr;
		if (Params->TryGetObjectField(TEXT("config"), In))
		{
			double X = 0.0;
			if ((*In)->TryGetNumberField(TEXT("density"), X))
			{
				Config->MassMode = EClothMassMode::Density;
				Config->Density = float(X);
			}
			if ((*In)->TryGetNumberField(TEXT("edgeStiffness"), X))
				Config->EdgeStiffnessWeighted = {float(X), float(X)};
			if ((*In)->TryGetNumberField(TEXT("bendStiffness"), X))
				Config->BendingStiffnessWeighted = {float(X), float(X)};
			if ((*In)->TryGetNumberField(TEXT("damping"), X))
				Config->DampingCoefficient = float(X);
			if ((*In)->TryGetNumberField(TEXT("friction"), X))
				Config->FrictionCoefficient = float(X);
			if ((*In)->TryGetNumberField(TEXT("collisionThickness"), X))
				Config->CollisionThickness = float(X);
		}
	}

	Cloth->InvalidateAllCachedData();
	Cloth->BindToSkeletalMesh(Mesh, Lod, Section, 0);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	bool bSave = false;
	if (Params->TryGetBoolField(TEXT("save"), bSave) && bSave)
		UEditorLoadingAndSavingUtils::SavePackages({Mesh->GetPackage()}, true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Cloth->GetPathName());
	Result->SetNumberField(TEXT("vertices"), Physical.Vertices.Num());
	Call->Reply(MakeShared<FJsonValueObject>(Result));
}
} // namespace

void LilaLink::AddClothMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("applyClothing"), &ApplyClothing);
}
