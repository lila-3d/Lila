#include "LilaLinkMeshTerrain.h"

#include "LilaLinkMethodRegistry.h"

#include "Dom/JsonObject.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MeshPartition.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionEditorSubsystem.h"
#include "MeshPartitionHeightmapImporter.h"
#include "MeshPartitionModifierComponent.h"
#include "Misc/PackageName.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
using UE::MeshPartition::AMeshPartition;
using UE::MeshPartition::UMeshPartitionEditorComponent;
using UE::MeshPartition::UMeshProviderModifier;
using UE::MeshPartition::UModifierComponent;

const TCHAR* SectionTagPrefix = TEXT("LilaLink.MeshTerrainSection:");

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.MeshTerrain:") + Id.Right(900)));
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

bool ReadPoint(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FInt32Point& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 2)
	{
		return false;
	}
	Out = FInt32Point(static_cast<int32>((*Values)[0]->AsNumber()), static_cast<int32>((*Values)[1]->AsNumber()));
	return true;
}

AMeshPartition* FindTagged(UWorld* World, FName Tag)
{
	for (TActorIterator<AMeshPartition> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

/** The section key the importer's label carries ("Section_X3-Y1" gives "X3-Y1"), or the actor's section tag. */
FString SectionKey(const AActor* Actor)
{
	for (const FName& Tag : Actor->Tags)
	{
		const FString S = Tag.ToString();
		if (S.StartsWith(SectionTagPrefix))
		{
			return S.RightChop(FCString::Strlen(SectionTagPrefix));
		}
	}
	FString Label = Actor->GetActorLabel();
	return Label.RemoveFromStart(TEXT("Section_")) ? Label : FString();
}

/** Base modifier actors Lila made for this id, by section key. */
TMap<FString, AActor*> TaggedSections(UWorld* World, FName Tag)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::TaggedSections);

	TMap<FString, AActor*> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag) && !It->IsA<AMeshPartition>() && It->FindComponentByClass<UMeshProviderModifier>())
		{
			Out.Add(SectionKey(*It), *It);
		}
	}
	return Out;
}

void NotifyMoved(AMeshPartition* Mesh, const TArray<FBox>& Bounds)
{
	if (UMeshPartitionEditorComponent* Editor = Mesh ? Cast<UMeshPartitionEditorComponent>(Mesh->GetMeshPartitionComponent()) : nullptr)
	{
		Editor->OnBoundsChanged(Bounds, UE::MeshPartition::EChangeType::StateChange);
	}
}

TSharedPtr<FJsonValue> SetMeshTerrain(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SetMeshTerrain);

	FString Id, Heightmap, MaterialPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty() || !Params->TryGetStringField(TEXT("heightmap"), Heightmap))
	{
		OutError = TEXT("setMeshTerrain: give id and heightmap");
		return nullptr;
	}
	FInt32Point MeshResolution, Sections;
	FVector MeshSize, Location = FVector::ZeroVector, PitchYawRoll = FVector::ZeroVector;
	if (!ReadPoint(Params, TEXT("meshResolution"), MeshResolution) || !ReadPoint(Params, TEXT("sections"), Sections) ||
		!ReadVector(Params, TEXT("meshSize"), MeshSize))
	{
		OutError = TEXT("setMeshTerrain: give meshResolution [x, y], sections [x, y] and meshSize [x, y, z]");
		return nullptr;
	}
	ReadVector(Params, TEXT("location"), Location);
	ReadVector(Params, TEXT("rotation"), PitchYawRoll);
	FString Label = FPackageName::GetShortName(Id);
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	bool bSaveAndUnload = false;
	Params->TryGetBoolField(TEXT("saveAndUnload"), bSaveAndUnload);
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setMeshTerrain: no editor world");
		return nullptr;
	}

	const FName Tag = TagFor(Id);
	AMeshPartition* Old = FindTagged(World, Tag);
	if (Old && bSaveAndUnload)
	{
		OutError = TEXT("setMeshTerrain: saveAndUnload can't update a sent terrain whose sections are unloaded; removeMeshTerrain it first");
		return nullptr;
	}
	TArray<FString> Warnings;
	UMaterialInterface* Material = nullptr;
	if (!MaterialPath.IsEmpty())
	{
		const FString ObjectPath = MaterialPath.Contains(TEXT(".")) ? MaterialPath : MaterialPath + TEXT(".") + FPackageName::GetShortName(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
		if (!Material)
		{
			Warnings.Add(TEXT("no material ") + MaterialPath);
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetMeshTerrain", "Lila: Update Mesh Terrain"));
	TSet<AActor*> Before;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Before.Add(*It);
	}
	UE::MeshPartition::FHeightmapImportParams Import;
	Import.World = World;
	Import.HeightmapFilename = Heightmap;
	Import.MeshResolution = MeshResolution;
	Import.MeshSize = MeshSize;
	Import.SectionsResolution = Sections;
	Import.bSaveAndUnload = bSaveAndUnload;
	Import.LocationVolumesResolution = FInt32Point(0);
	UE::MeshPartition::FHeightmapImporter Importer(Import);
	if (!Importer.Import(nullptr))
	{
		Transaction.Cancel();
		OutError = TEXT("setMeshTerrain: ") + Importer.GetErrorText().ToString();
		return nullptr;
	}
	AMeshPartition* Made = Importer.GetMegaMesh();
	TArray<AActor*> NewSections;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!Before.Contains(*It) && *It != Made && It->FindComponentByClass<UMeshProviderModifier>())
		{
			NewSections.Add(*It);
		}
	}

	const FTransform Place(FRotator(PitchYawRoll.X, PitchYawRoll.Y, PitchYawRoll.Z), Location);
	TArray<FBox> Moved;
	if (bSaveAndUnload)
	{
		Warnings.Add(TEXT("saveAndUnload: the sections stay where the importer put them, centred on the origin from Z 0; move the terrain in Lila to the origin or send without it"));
	}
	else
	{
		for (AActor* Section : NewSections)
		{
			for (const FBox& B : Section->FindComponentByClass<UMeshProviderModifier>()->ComputeBounds())
			{
				Moved.Add(B);
			}
			Section->SetActorTransform(Section->GetActorTransform() * Place);
			for (const FBox& B : Section->FindComponentByClass<UMeshProviderModifier>()->ComputeBounds())
			{
				Moved.Add(B);
			}
		}
	}

	bool bCreated = true;
	AMeshPartition* Result = Made;
	TMap<FString, AActor*> OldSections = Old ? TaggedSections(World, Tag) : TMap<FString, AActor*>();
	TSet<FString> NewKeys;
	for (AActor* Section : NewSections)
	{
		NewKeys.Add(SectionKey(Section));
	}
	bool bSameSections = Old && OldSections.Num() == NewSections.Num();
	for (const FString& Key : NewKeys)
	{
		bSameSections &= OldSections.Contains(Key);
	}

	if (bSameSections)
	{
		// The same sections: new meshes into the old base modifiers, so everything sitting on that Mesh Partition stays.
		TArray<FBox> Changed;
		for (AActor* Section : NewSections)
		{
			AActor* Target = OldSections[SectionKey(Section)];
			UMeshProviderModifier* From = Section->FindComponentByClass<UMeshProviderModifier>();
			UMeshProviderModifier* To = Target->FindComponentByClass<UMeshProviderModifier>();
			Changed.Append(To->ComputeBounds());
			Target->Modify();
			Target->SetActorTransform(Section->GetActorTransform());
			UE::Geometry::FDynamicMesh3 Copy(*From->GetMesh());
			To->SetMesh(MoveTemp(Copy), true);
			Changed.Append(To->ComputeBounds());
			if (Material)
			{
				To->SetMaterial(0, Material);
			}
		}
		for (AActor* Section : NewSections)
		{
			World->EditorDestroyActor(Section, true);
		}
		World->EditorDestroyActor(Made, true);
		Result = Old;
		bCreated = false;
		NotifyMoved(Old, Changed);
	}
	else
	{
		if (Old)
		{
			// New sections: the user's modifiers move to the new Mesh Partition, Lila's old base goes.
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->ActorHasTag(Tag))
				{
					continue;
				}
				TArray<UModifierComponent*> Modifiers;
				It->GetComponents(Modifiers);
				for (UModifierComponent* M : Modifiers)
				{
					if (M->GetAffectedMeshPartition() == Old)
					{
						M->SetAffectedMeshPartition(Made);
					}
				}
			}
			for (const TPair<FString, AActor*>& S : OldSections)
			{
				World->EditorDestroyActor(S.Value, true);
			}
			World->EditorDestroyActor(Old, true);
			Warnings.Add(TEXT("the sections changed, so the Mesh Partition actor was replaced; your modifiers on it moved to the new one"));
		}
		Made->Tags.AddUnique(Tag);
		Made->SetActorLabel(Label);
		for (AActor* Section : NewSections)
		{
			Section->Tags.AddUnique(Tag);
			Section->Tags.AddUnique(FName(*(FString(SectionTagPrefix) + SectionKey(Section))));
			if (Material)
			{
				Section->FindComponentByClass<UMeshProviderModifier>()->SetMaterial(0, Material);
			}
		}
		NotifyMoved(Made, Moved);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Result ? Result->GetActorLabel() : FString());
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetNumberField(TEXT("sections"), NewSections.Num());
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveMeshTerrain(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::RemoveMeshTerrain);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeMeshTerrain: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeMeshTerrain: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveMeshTerrain", "Lila: Remove Mesh Terrain"));
	const FName Tag = TagFor(Id);
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Doomed.Add(*It);
		}
	}
	for (AActor* A : Doomed)
	{
		World->EditorDestroyActor(A, true);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), Doomed.Num() > 0);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLinkMeshTerrain::RegisterMethods()
{
	LilaLink::RegisterMethod(TEXT("setMeshTerrain"), &SetMeshTerrain, Feature);
	LilaLink::RegisterMethod(TEXT("removeMeshTerrain"), &RemoveMeshTerrain, Feature);
}
