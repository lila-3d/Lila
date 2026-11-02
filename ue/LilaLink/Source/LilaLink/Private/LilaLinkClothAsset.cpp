#include "LilaLinkCloth.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Chaos/CollectionPropertyFacade.h"
#include "ChaosClothAsset/ClothAsset.h"
#include "ChaosClothAsset/ClothGeometryTools.h"
#include "ChaosClothAsset/CollectionClothFacade.h"
#include "ClothConfig.h"
#include "Dom/JsonObject.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/SkeletalMesh.h"
#include "FileHelpers.h"
#include "GeometryCollection/ManagedArrayCollection.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
using UE::Geometry::FDynamicMesh3;

TArray<FVector3f> ReadPoints(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	TArray<FVector3f> Out;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(Field, List))
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TArray<TSharedPtr<FJsonValue>>& P = V->AsArray();
			Out.Emplace(float(P[0]->AsNumber()), float(P.Num() > 1 ? P[1]->AsNumber() : 0.0), float(P.Num() > 2 ? P[2]->AsNumber() : 0.0));
		}
	return Out;
}

TArray<FIntVector3> ReadTriangles(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	TArray<FIntVector3> Out;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(Field, List))
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TArray<TSharedPtr<FJsonValue>>& T = V->AsArray();
			Out.Emplace(int32(T[0]->AsNumber()), int32(T[1]->AsNumber()), int32(T[2]->AsNumber()));
		}
	return Out;
}

TArray<float> ReadFloats(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	TArray<float> Out;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(Field, List))
		for (const TSharedPtr<FJsonValue>& V : *List)
			Out.Add(float(V->AsNumber()));
	return Out;
}

/** A weighted property as a Dataflow config node writes it: low to high across the weight map (or flat). */
void SetWeighted(::Chaos::Softs::FCollectionPropertyMutableFacade& Properties, const FName& Name, float Low, float High, const FString& WeightMap = FString(),
                 ::Chaos::Softs::ECollectionPropertyFlags Extra = ::Chaos::Softs::ECollectionPropertyFlags::None)
{
	int32 Key = Properties.GetKeyNameIndex(Name);
	if (Key == INDEX_NONE)
		Key = Properties.AddProperty(Name);
	Properties.SetFlags(Key, ::Chaos::Softs::ECollectionPropertyFlags::Enabled | ::Chaos::Softs::ECollectionPropertyFlags::Interpolable | Extra);
	Properties.SetWeightedValue(Key, Low, High);
	Properties.SetStringValue(Key, WeightMap);
}

void SetInt(::Chaos::Softs::FCollectionPropertyMutableFacade& Properties, const FName& Name, int32 Value, ::Chaos::Softs::ECollectionPropertyFlags Extra = ::Chaos::Softs::ECollectionPropertyFlags::None)
{
	int32 Key = Properties.GetKeyNameIndex(Name);
	if (Key == INDEX_NONE)
		Key = Properties.AddProperty(Name);
	Properties.SetFlags(Key, ::Chaos::Softs::ECollectionPropertyFlags::Enabled | Extra);
	Properties.SetValue(Key, Value);
}

void CreateClothAsset(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCloth::CreateClothAsset);

	const FString ContentPath = Params->GetStringField(TEXT("contentPath"));
	const TArray<FVector3f> Points2D = ReadPoints(Params, TEXT("vertices2D"));
	const TArray<FIntVector3> Tris2D = ReadTriangles(Params, TEXT("triangles2D"));
	const TArray<FVector3f> Points3D = ReadPoints(Params, TEXT("vertices3D"));
	const TArray<FIntVector3> Tris3D = ReadTriangles(Params, TEXT("triangles3D"));
	const TArray<float> Pattern = ReadFloats(Params, TEXT("pattern"));
	const TArray<float> MaxDistance = ReadFloats(Params, TEXT("maxDistance"));
	if (Tris2D.Num() == 0 || Tris2D.Num() != Tris3D.Num() || Pattern.Num() != Tris2D.Num())
	{
		Call->Reply(nullptr, TEXT("createClothAsset: the 2D and 3D triangles and the pattern per triangle must match"));
		return;
	}

	// The flat pattern and the drape as two meshes with the same triangles; a polygroup layer says which panel each is.
	FDynamicMesh3 Mesh2D, Mesh3D;
	Mesh2D.EnableAttributes();
	Mesh2D.Attributes()->SetNumPolygroupLayers(1);
	for (const FVector3f& P : Points2D)
		Mesh2D.AppendVertex(FVector3d(P));
	for (const FVector3f& P : Points3D)
		Mesh3D.AppendVertex(FVector3d(P));
	for (int32 T = 0; T < Tris2D.Num(); ++T)
	{
		const int32 A = Mesh2D.AppendTriangle(Tris2D[T].X, Tris2D[T].Y, Tris2D[T].Z);
		const int32 B = Mesh3D.AppendTriangle(Tris3D[T].X, Tris3D[T].Y, Tris3D[T].Z);
		if (A < 0 || B < 0)
		{
			Call->Reply(nullptr, FString::Printf(TEXT("createClothAsset: triangle %d isn't manifold"), T));
			return;
		}
		Mesh2D.Attributes()->GetPolygroupLayer(0)->SetValue(A, int32(Pattern[T]));
	}

	using namespace UE::Chaos::ClothAsset;
	TSharedRef<FManagedArrayCollection> Collection = MakeShared<FManagedArrayCollection>();
	FCollectionClothFacade Cloth(Collection);
	Cloth.DefineSchema();
	TMap<int, int32> VertexMap;
	FClothGeometryTools::BuildSimMeshFromDynamicMeshes(Collection, Mesh2D, Mesh3D, 0, false, false, false, VertexMap);

	FString MeshPath;
	if (Params->TryGetStringField(TEXT("skeletalMesh"), MeshPath) && !MeshPath.IsEmpty())
		Cloth.SetSkeletalMeshSoftObjectPathName(FSoftObjectPath(MeshPath));
	FClothGeometryTools::BindMeshToRootBone(Collection, true, false);

	// Max distance as a weight map, scaled to the largest distance: each 3D vertex takes the nearest sample's.
	const float Largest = FMath::Max(1.f, MaxDistance.Num() ? FMath::Max(MaxDistance) : 1.f);
	Cloth.AddWeightMap(TEXT("MaxDistance"));
	{
		TArrayView<float> Weights = Cloth.GetWeightMap(TEXT("MaxDistance"));
		const TConstArrayView<FVector3f> Sim3D = Cloth.GetSimPosition3D();
		for (int32 V = 0; V < Sim3D.Num(); ++V)
		{
			float Best = TNumericLimits<float>::Max(), Value = 0.f;
			for (int32 S = 0; S < Points3D.Num() && S < MaxDistance.Num(); ++S)
			{
				const float D = FVector3f::DistSquared(Sim3D[V], Points3D[S]);
				if (D < Best)
				{
					Best = D;
					Value = MaxDistance[S];
				}
			}
			Weights[V] = Value / Largest;
		}
	}

	FString Material;
	Params->TryGetStringField(TEXT("material"), Material);
	FClothGeometryTools::CopySimMeshToRenderMesh(Collection, FSoftObjectPath(Material), false);

	::Chaos::Softs::FCollectionPropertyMutableFacade Properties(Collection);
	Properties.DefineSchema();
	SetWeighted(Properties, TEXT("MaxDistance"), 0.f, Largest, TEXT("MaxDistance"), ::Chaos::Softs::ECollectionPropertyFlags::Intrinsic);
	const TSharedPtr<FJsonObject>* In = nullptr;
	if (Params->TryGetObjectField(TEXT("config"), In))
	{
		double X = 0.0;
		if ((*In)->TryGetNumberField(TEXT("density"), X))
		{
			SetInt(Properties, TEXT("MassMode"), int32(EClothMassMode::Density), ::Chaos::Softs::ECollectionPropertyFlags::Intrinsic);
			SetWeighted(Properties, TEXT("MassValue"), float(X), float(X), FString(), ::Chaos::Softs::ECollectionPropertyFlags::Intrinsic);
		}
		if ((*In)->TryGetNumberField(TEXT("edgeStiffness"), X))
			SetWeighted(Properties, TEXT("EdgeSpringStiffness"), float(X), float(X));
		if ((*In)->TryGetNumberField(TEXT("bendStiffness"), X))
			SetWeighted(Properties, TEXT("BendingSpringStiffness"), float(X), float(X));
		if ((*In)->TryGetNumberField(TEXT("damping"), X))
			SetWeighted(Properties, TEXT("DampingCoefficient"), float(X), float(X));
		if ((*In)->TryGetNumberField(TEXT("friction"), X))
			SetWeighted(Properties, TEXT("FrictionCoefficient"), float(X), float(X));
		if ((*In)->TryGetNumberField(TEXT("collisionThickness"), X))
			SetWeighted(Properties, TEXT("CollisionThickness"), float(X), float(X));
	}

	const FString PackageName = ContentPath;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	UChaosClothAsset* Asset = FindObject<UChaosClothAsset>(Package, *AssetName);
	if (!Asset)
	{
		Asset = NewObject<UChaosClothAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	FText Error, Verbose;
	TArray<TSharedRef<const FManagedArrayCollection>> Collections{Collection};
	Asset->Build(Collections, nullptr, &Error, &Verbose);
	Asset->MarkPackageDirty();
	if (!Error.IsEmpty())
	{
		Call->Reply(nullptr, FString::Printf(TEXT("createClothAsset: %s %s"), *Error.ToString(), *Verbose.ToString()));
		return;
	}
	bool bSave = false;
	if (Params->TryGetBoolField(TEXT("save"), bSave) && bSave)
		UEditorLoadingAndSavingUtils::SavePackages({Package}, true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Asset->GetPathName());
	Result->SetNumberField(TEXT("simVertices"), Cloth.GetNumSimVertices3D());
	Result->SetNumberField(TEXT("patterns"), Cloth.GetNumSimPatterns());
	Call->Reply(MakeShared<FJsonValueObject>(Result));
}
} // namespace

void LilaLink::AddClothAssetMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("createClothAsset"), &CreateClothAsset);
}
