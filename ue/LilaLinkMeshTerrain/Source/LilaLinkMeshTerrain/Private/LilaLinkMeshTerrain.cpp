#include "LilaLinkMeshTerrain.h"

#include "LilaLinkMethodRegistry.h"

#include "Dom/JsonObject.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "MeshPartition.h"
#include "MeshPartitionChannel.h"
#include "MeshPartitionDefinition.h"
#include "Misc/FileHelper.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionEditorSubsystem.h"
#include "MeshPartitionHeightmapImporter.h"
#include "MeshPartitionModifierComponent.h"
#include "Components/SplineComponent.h"
#include "Misc/PackageName.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
#include "Modifiers/MeshPartitionSplineModifier.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
using UE::MeshPartition::AMeshPartition;
using UE::MeshPartition::UMeshPartitionDefinition;
using UE::MeshPartition::UMeshPartitionEditorComponent;
using UE::MeshPartition::UMeshProviderModifier;
using UE::MeshPartition::UModifierComponent;
using UE::MeshPartition::USplineModifier;

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

/** A paint layer as a Mesh Partition channel: its name and one weight byte per heightmap mesh vertex. */
struct FLilaChannel
{
	FName Name;
	TArray<uint8> Weights;
};

bool ReadChannels(const TSharedPtr<FJsonObject>& Params, FInt32Point Resolution, TArray<FLilaChannel>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("channels"), Values))
	{
		return true;
	}
	const int64 Expected = int64(Resolution.X + 1) * int64(Resolution.Y + 1);
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Name, File;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Name) || !(*Object)->TryGetStringField(TEXT("file"), File))
		{
			OutError = TEXT("setMeshTerrain: each channel needs name and file");
			return false;
		}
		FLilaChannel& C = Out.AddDefaulted_GetRef();
		C.Name = FName(*Name);
		if (!FFileHelper::LoadFileToArray(C.Weights, *File) || C.Weights.Num() != Expected)
		{
			OutError = FString::Printf(TEXT("setMeshTerrain: channel %s: %s is not %lld bytes"), *Name, *File, Expected);
			return false;
		}
	}
	return true;
}

/**
 * Declares the channels on the terrain's definition, so they are rasterized for materials to sample. The engine's default
 * definition is shared, so a terrain still on it gets its own copy first.
 */
void DeclareChannels(AMeshPartition* Mesh, const TArray<FLilaChannel>& Channels, TArray<FString>& Warnings)
{
	if (!Mesh || Channels.IsEmpty())
	{
		return;
	}
	UMeshPartitionDefinition* Definition = Mesh->GetMeshPartitionDefinition();
	if (!Definition || Definition == UMeshPartitionDefinition::GetDefaultMegaMeshDefinition())
	{
		const UMeshPartitionDefinition* Source = Definition ? Definition : UMeshPartitionDefinition::GetDefaultMegaMeshDefinition();
		Definition = DuplicateObject<UMeshPartitionDefinition>(Source, Mesh, MakeUniqueObjectName(Mesh, UMeshPartitionDefinition::StaticClass(), TEXT("LilaDefinition")));
		Mesh->Modify();
		Mesh->SetMeshPartitionDefinition(Definition);
	}
	// The channel list has no setter; it is reached through reflection like the details panel does.
	FStructProperty* MapProperty = FindFProperty<FStructProperty>(UMeshPartitionDefinition::StaticClass(), TEXT("ChannelMap"));
	FArrayProperty* DescsProperty = MapProperty ? FindFProperty<FArrayProperty>(MapProperty->Struct, TEXT("ChannelDescs")) : nullptr;
	if (!DescsProperty)
	{
		Warnings.Add(TEXT("this engine's Mesh Partition definition has no ChannelMap.ChannelDescs, so the channels were written to the mesh but not declared"));
		return;
	}
	Definition->Modify();
	void* Map = MapProperty->ContainerPtrToValuePtr<void>(Definition);
	TArray<UE::MeshPartition::FChannelDesc>& Descs = *DescsProperty->ContainerPtrToValuePtr<TArray<UE::MeshPartition::FChannelDesc>>(Map);
	for (const FLilaChannel& C : Channels)
	{
		if (!Descs.ContainsByPredicate([&](const UE::MeshPartition::FChannelDesc& D) { return D.Name == C.Name; }))
		{
			if (Descs.Num() >= UE::MeshPartition::FChannelPacking::MaxNumberPackedChannels)
			{
				Warnings.Add(FString::Printf(TEXT("channel %s left out: the definition already has %d channels"), *C.Name.ToString(), Descs.Num()));
				continue;
			}
			Descs.AddDefaulted_GetRef().Name = C.Name;
		}
	}
	FPropertyChangedEvent Changed(MapProperty);
	Definition->PostEditChangeProperty(Changed);
}

/** Writes the channels into a section's base mesh as weight layers, sampled at each vertex's place in the heightmap mesh. */
void WriteChannels(AActor* Section, const TArray<FLilaChannel>& Channels, FInt32Point Resolution, const FVector& MeshSize)
{
	UMeshProviderModifier* Provider = Section ? Section->FindComponentByClass<UMeshProviderModifier>() : nullptr;
	if (!Provider || !Provider->GetMesh() || Channels.IsEmpty())
	{
		return;
	}
	UE::Geometry::FDynamicMesh3 Mesh(*Provider->GetMesh());
	Mesh.EnableAttributes();
	Mesh.Attributes()->SetNumWeightLayers(Channels.Num());
	const FTransform ToImport = Section->GetActorTransform();
	const int32 Stride = Resolution.X + 1;
	for (int32 Index = 0; Index < Channels.Num(); ++Index)
	{
		UE::Geometry::FDynamicMeshWeightAttribute* Layer = Mesh.Attributes()->GetWeightLayer(Index);
		Layer->SetName(Channels[Index].Name);
		const TArray<uint8>& W = Channels[Index].Weights;
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			// The importer's mesh is centred on the origin: vertex k of Resolution.X sits at (k / X - 0.5) * MeshSize.X.
			const FVector P = ToImport.TransformPosition(Mesh.GetVertex(Vid));
			const double K = FMath::Clamp((P.X / MeshSize.X + 0.5) * Resolution.X, 0.0, double(Resolution.X));
			const double L = FMath::Clamp((P.Y / MeshSize.Y + 0.5) * Resolution.Y, 0.0, double(Resolution.Y));
			const int32 K0 = FMath::Min(int32(K), Resolution.X - 1), L0 = FMath::Min(int32(L), Resolution.Y - 1);
			const double FK = K - K0, FL = L - L0;
			auto At = [&](int32 X, int32 Y) { return double(W[Y * Stride + X]) / 255.0; };
			const float Value = float(FMath::Lerp(FMath::Lerp(At(K0, L0), At(K0 + 1, L0), FK), FMath::Lerp(At(K0, L0 + 1), At(K0 + 1, L0 + 1), FK), FL));
			Layer->SetValue(Vid, &Value);
		}
	}
	Provider->SetMesh(MoveTemp(Mesh), false);
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
	TArray<FLilaChannel> Channels;
	if (!ReadChannels(Params, MeshResolution, Channels, OutError))
	{
		return nullptr;
	}
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

	for (AActor* Section : NewSections)
	{
		WriteChannels(Section, Channels, MeshResolution, MeshSize);
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

	DeclareChannels(Result, Channels, Warnings);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Result ? Result->GetActorLabel() : FString());
	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	for (const FLilaChannel& C : Channels)
	{
		ChannelValues.Add(MakeShared<FJsonValueString>(C.Name.ToString()));
	}
	Out->SetArrayField(TEXT("channels"), ChannelValues);
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

FName RoadsTagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.MeshTerrainRoads:") + Id.Right(900)));
}

template <typename T>
T* PrivateMember(UObject* Object, const TCHAR* Name)
{
	FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
	return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
}

/**
 * Roads on a Lila Mesh Terrain as Mesh Partition spline modifiers, one actor per road: the spline through the road's
 * points, a plateau of its half width and its falloff beyond, writing the road's channel and, when raise or lower is
 * on, pulling the surface to the road. Sending again replaces them.
 */
TSharedPtr<FJsonValue> SetMeshTerrainRoads(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SetMeshTerrainRoads);

	FString Id;
	const TArray<TSharedPtr<FJsonValue>>* Roads = nullptr;
	if (!Params->TryGetStringField(TEXT("id"), Id) || !Params->TryGetArrayField(TEXT("roads"), Roads))
	{
		OutError = TEXT("setMeshTerrainRoads: give id and roads");
		return nullptr;
	}
	FVector Location = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	UWorld* World = EditorWorld();
	AMeshPartition* Mesh = World ? FindTagged(World, TagFor(Id)) : nullptr;
	if (!Mesh)
	{
		OutError = TEXT("setMeshTerrainRoads: no Mesh Terrain from this terrain; send it with terrainSendToUnreal(kind='meshTerrain') first");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetMeshTerrainRoads", "Lila: Update Mesh Terrain Roads"));
	const FName Tag = RoadsTagFor(Id);
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
	const TArray<FName> Channels = Mesh->GetMeshPartitionDefinition() ? Mesh->GetMeshPartitionDefinition()->GetChannelNames() : TArray<FName>();
	UMeshPartitionEditorComponent* Editor = Cast<UMeshPartitionEditorComponent>(Mesh->GetMeshPartitionComponent());
	TArray<FString> Warnings;
	int32 Made = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Roads)
	{
		const TSharedPtr<FJsonObject>* Road = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Road) || !(*Road)->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = TEXT("setMeshTerrainRoads: each road needs at least two points");
			return nullptr;
		}
		FString RoadId, Layer;
		(*Road)->TryGetStringField(TEXT("id"), RoadId);
		(*Road)->TryGetStringField(TEXT("layer"), Layer);
		double HalfWidth = 300.0, Falloff = 500.0;
		bool bRaise = false, bLower = false, bClosed = false;
		(*Road)->TryGetNumberField(TEXT("halfWidth"), HalfWidth);
		(*Road)->TryGetNumberField(TEXT("sideFalloff"), Falloff);
		(*Road)->TryGetBoolField(TEXT("raiseTerrain"), bRaise);
		(*Road)->TryGetBoolField(TEXT("lowerTerrain"), bLower);
		(*Road)->TryGetBoolField(TEXT("closed"), bClosed);

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
		Actor->SetActorLabel(TEXT("Road_") + RoadId);
		Actor->Tags.Add(Tag);
		USplineComponent* Spline = NewObject<USplineComponent>(Actor, TEXT("Spline"), RF_Transactional);
		Actor->AddInstanceComponent(Spline);
		Actor->SetRootComponent(Spline);
		Spline->RegisterComponent();
		Spline->ClearSplinePoints(false);
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* Point = nullptr;
			FVector Position, Arrive = FVector::ZeroVector, Leave = FVector::ZeroVector;
			if (!PointValue->TryGetObject(Point) || !ReadVector(*Point, TEXT("position"), Position))
			{
				OutError = TEXT("setMeshTerrainRoads: a point has no position");
				return nullptr;
			}
			Spline->AddSplinePoint(Location + Position, ESplineCoordinateSpace::World, false);
			const int32 Index = Spline->GetNumberOfSplinePoints() - 1;
			if (ReadVector(*Point, TEXT("arriveTangent"), Arrive) && ReadVector(*Point, TEXT("leaveTangent"), Leave))
			{
				Spline->SetTangentsAtSplinePoint(Index, Arrive, Leave, ESplineCoordinateSpace::World, false);
			}
		}
		Spline->SetClosedLoop(bClosed, false);
		Spline->UpdateSpline();

		USplineModifier* Modifier = NewObject<USplineModifier>(Actor, TEXT("RoadModifier"), RF_Transactional);
		Modifier->SetPriority(1.0);
		Modifier->SetAffectedMeshPartition(Mesh);
		Actor->AddInstanceComponent(Modifier);
		Modifier->AttachToComponent(Spline, FAttachmentTransformRules::KeepWorldTransform);
		Modifier->SetUseSplineScaleForFalloff(false);
		Modifier->SetFalloffDistance(float(FMath::Max(Falloff, 0.1)));
		Modifier->SetMeshClosedInterior(false);
		if (float* Plateau = PrivateMember<float>(Modifier, TEXT("PlateauDistance")))
		{
			*Plateau = float(HalfWidth);
		}
		using UE::MeshPartition::ESplineModifierWriteMode;
		ESplineModifierWriteMode Mode = ESplineModifierWriteMode(0);
		if (bRaise || bLower)
		{
			Mode |= ESplineModifierWriteMode::Positions;
			if (UE::MeshPartition::ESplineModifierBlendMode* Blend = PrivateMember<UE::MeshPartition::ESplineModifierBlendMode>(Modifier, TEXT("BlendMode")))
			{
				// Max moves only the surface below the road up to it, Min only the surface above it down.
				*Blend = bRaise && bLower ? UE::MeshPartition::ESplineModifierBlendMode::Normal
						 : bRaise		  ? UE::MeshPartition::ESplineModifierBlendMode::Max
										  : UE::MeshPartition::ESplineModifierBlendMode::Min;
			}
		}
		if (!Layer.IsEmpty())
		{
			if (Channels.Contains(FName(*Layer)))
			{
				Mode |= ESplineModifierWriteMode::Weights;
				if (TArray<UE::MeshPartition::FSplineModifierWeightEntry>* Entries = PrivateMember<TArray<UE::MeshPartition::FSplineModifierWeightEntry>>(Modifier, TEXT("WeightChannels")))
				{
					UE::MeshPartition::FSplineModifierWeightEntry& Entry = Entries->AddDefaulted_GetRef();
					Entry.WeightChannelName = UE::MeshPartition::FChannelName(FName(*Layer));
					Entry.Value = 1.0;
				}
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("%s: the Mesh Terrain has no channel %s, so the road paints none (send the mask as a layer)"), *RoadId, *Layer));
			}
		}
		if (uint8* WriteMode = PrivateMember<uint8>(Modifier, TEXT("WriteMode")))
		{
			*WriteMode = uint8(Mode);
		}
		Modifier->SetSplineComponent(Spline, true);
		Modifier->RegisterComponent();
		Modifier->UpdateSplineData();
		if (Editor)
		{
			Editor->OnModifierAssigned();
		}
		++Made;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("modifiers"), Made);
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
		if (It->ActorHasTag(Tag) || It->ActorHasTag(RoadsTagFor(Id)))
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
	LilaLink::RegisterMethod(TEXT("setMeshTerrainRoads"), &SetMeshTerrainRoads, TEXT("meshTerrainRoads"));
}
