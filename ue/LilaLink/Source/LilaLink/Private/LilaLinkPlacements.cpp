#include "LilaLinkPlacements.h"

#include "LilaLinkComponents.h"
#include "LilaLinkLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/Scene.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "Misc/PackageName.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "LilaLinkPlacements"

namespace
{
const TCHAR* GroupPrefix = TEXT("LilaGroup:");
const TCHAR* NodePrefix = TEXT("LilaNode:");
const FName InstancedTag(TEXT("LilaInstanced"));
const FName LightTag(TEXT("LilaLight"));
const TCHAR* AssemblyPrefix = TEXT("LilaAssembly:");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FName GroupTag(const FString& Group) { return FName(*(FString(GroupPrefix) + Group)); }
FName NodeTag(const FString& Id) { return FName(*(FString(NodePrefix) + Id)); }

/** The value after a tag prefix on the actor, or empty. */
FString TagValue(const AActor* Actor, const TCHAR* Prefix)
{
	for (const FName& Tag : Actor->Tags)
	{
		FString S = Tag.ToString();
		if (S.StartsWith(Prefix))
		{
			return S.RightChop(FCString::Strlen(Prefix));
		}
	}
	return FString();
}

/** A node id as Lila sends it (a number), or a string id. */
FString IdOf(const TSharedPtr<FJsonObject>& P)
{
	TSharedPtr<FJsonValue> V = P->TryGetField(TEXT("id"));
	if (!V.IsValid())
	{
		return FString();
	}
	if (V->Type == EJson::Number)
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(V->AsNumber()));
	}
	return V->AsString();
}

FVector VectorField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!P->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return Fallback;
	}
	return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
}

/** The placement's custom primitive data: floats a material reads per actor or instance (index 0 is a stable random). */
TArray<float> CustomDataOf(const TSharedPtr<FJsonObject>& P)
{
	TArray<float> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (P->TryGetArrayField(TEXT("customData"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Add(static_cast<float>(V->AsNumber()));
		}
	}
	return Out;
}

/** The placement's Lighting Channels 0, 1 and 2, when Lila sent them. */
TOptional<FLightingChannels> ChannelsOf(const TSharedPtr<FJsonObject>& P)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!P->TryGetArrayField(TEXT("lightingChannels"), Values) || Values->Num() != 3)
	{
		return {};
	}
	FLightingChannels Channels;
	Channels.bChannel0 = (*Values)[0]->AsBool();
	Channels.bChannel1 = (*Values)[1]->AsBool();
	Channels.bChannel2 = (*Values)[2]->AsBool();
	return Channels;
}

/** Collision "none" turns the placed components' collision off; anything else blocks like a placed static mesh. */
void SetCollides(UPrimitiveComponent* Component, bool bCollide)
{
	if (bCollide)
	{
		Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	else
	{
		Component->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}
}

bool SameChannels(const FLightingChannels& A, const FLightingChannels& B)
{
	return A.bChannel0 == B.bChannel0 && A.bChannel1 == B.bChannel1 && A.bChannel2 == B.bChannel2;
}

/** location (cm), rotation (quat x y z w) and scale, already in Unreal's axes. */
FTransform TransformOf(const TSharedPtr<FJsonObject>& P)
{
	FQuat Rotation = FQuat::Identity;
	const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
	if (P->TryGetArrayField(TEXT("rotation"), Q) && Q->Num() == 4)
	{
		Rotation = FQuat((*Q)[0]->AsNumber(), (*Q)[1]->AsNumber(), (*Q)[2]->AsNumber(), (*Q)[3]->AsNumber());
		Rotation.Normalize();
	}
	return FTransform(Rotation, VectorField(P, TEXT("location"), FVector::ZeroVector), VectorField(P, TEXT("scale"), FVector::OneVector));
}

TArray<AActor*> TaggedActors(UWorld* World, const FName& Tag)
{
	TArray<AActor*> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(Tag))
		{
			Out.Add(*It);
		}
	}
	return Out;
}

/** What one removal took away: an instanced holder counts its instances, an actor counts one. */
int32 Weight(AActor* Actor)
{
	if (Actor->Tags.Contains(InstancedTag))
	{
		if (const UInstancedStaticMeshComponent* C = Actor->FindComponentByClass<UInstancedStaticMeshComponent>())
		{
			return C->GetInstanceCount();
		}
	}
	return 1;
}

int32 DestroyActors(UWorld* World, const TArray<AActor*>& Actors)
{
	int32 Removed = 0;
	for (AActor* Actor : Actors)
	{
		Removed += Weight(Actor);
		World->EditorDestroyActor(Actor, true);
	}
	return Removed;
}

FString ShortGroupName(const FString& Group)
{
	int32 Bar = INDEX_NONE;
	return Group.FindLastChar(TEXT('|'), Bar) ? Group.RightChop(Bar + 1) : Group;
}

/** Where placed actors' attribute components go: data assets beside the mesh, warnings and assets made back to Lila. */
struct FComponentSink
{
	FString Folder;
	TArray<FString> Warnings;
	TArray<FString> Assets;
};

void ApplyPlacementComponents(AActor* Actor, const TSharedPtr<FJsonObject>& Placement, FComponentSink& Sink)
{
	TArray<LilaLink::FLilaComponent> Components;
	if (LilaLink::ParseComponents(Placement, Components))
	{
		LilaLink::ApplyComponentsToActor(Actor, Components, Sink.Folder, Sink.Warnings, Sink.Assets);
	}
}

/** The physical material for a Lila body material, made beside the meshes the first time: PM_<material>. */
UPhysicalMaterial* PhysicalMaterialFor(const TSharedPtr<FJsonObject>& Physics, const FString& Folder)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PhysicalMaterialFor);

	FString Material;
	if (!Physics->TryGetStringField(TEXT("material"), Material) || Material.IsEmpty())
	{
		return nullptr;
	}
	const FString Name = TEXT("PM_") + Material;
	const FString PackageName = Folder / TEXT("PhysicalMaterials") / Name;
	UPhysicalMaterial* PM = LoadObject<UPhysicalMaterial>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!PM)
	{
		UPackage* Package = CreatePackage(*PackageName);
		PM = NewObject<UPhysicalMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(PM);
	}
	double Friction = PM->Friction, Restitution = PM->Restitution, Density = PM->Density * 1000.0;
	Physics->TryGetNumberField(TEXT("friction"), Friction);
	Physics->TryGetNumberField(TEXT("restitution"), Restitution);
	Physics->TryGetNumberField(TEXT("density"), Density);
	PM->Modify();
	PM->Friction = static_cast<float>(Friction);
	PM->Restitution = static_cast<float>(Restitution);
	// Lila's density is kg/m3, Unreal's g/cm3.
	PM->Density = static_cast<float>(Density / 1000.0);
	PM->MarkPackageDirty();
	return PM;
}

/**
 * A placement's body settings on its component: the physical material, and for an actor of its own whether it
 * simulates and its mass. Instances share one component, which can't simulate per instance.
 */
void ApplyPhysics(UPrimitiveComponent* Component, const TSharedPtr<FJsonObject>& Placement, const FString& Folder, bool bInstanced, const FString& Label,
	FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyPhysics);

	const TSharedPtr<FJsonObject>* Physics = nullptr;
	if (!Placement->TryGetObjectField(TEXT("physics"), Physics))
	{
		return;
	}
	if (UPhysicalMaterial* PM = PhysicalMaterialFor(*Physics, Folder))
	{
		Component->SetPhysMaterialOverride(PM);
	}
	bool bSimulate = false;
	(*Physics)->TryGetBoolField(TEXT("simulate"), bSimulate);
	if (bInstanced)
	{
		if (bSimulate)
		{
			Sink.Warnings.Add(FString::Printf(TEXT("%s: instanced members can't simulate physics; send it as actors"), *Label));
		}
		return;
	}
	if (bSimulate)
	{
		Component->SetMobility(EComponentMobility::Movable);
	}
	Component->SetSimulatePhysics(bSimulate);
	double Mass = 0.0;
	if ((*Physics)->TryGetNumberField(TEXT("mass"), Mass) && Mass > 0.0)
	{
		Component->SetMassOverrideInKg(NAME_None, static_cast<float>(Mass), true);
	}
}

/** The mesh collides with its UCX_ hulls, or with its own triangles when the body is a static triangle mesh. */
void ApplyMeshCollision(UStaticMesh* Mesh, const TArray<TSharedPtr<FJsonValue>>& Placements)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyMeshCollision);

	const TSharedPtr<FJsonObject>* First = nullptr;
	const TSharedPtr<FJsonObject>* Physics = nullptr;
	FString Collision;
	if (Placements.IsEmpty() || !Placements[0].IsValid() || !Placements[0]->TryGetObject(First) || !(*First)->TryGetObjectField(TEXT("physics"), Physics) ||
		!(*Physics)->TryGetStringField(TEXT("collision"), Collision))
	{
		return;
	}
	UBodySetup* Body = Mesh->GetBodySetup();
	if (!Body)
	{
		return;
	}
	const ECollisionTraceFlag Flag = Collision == TEXT("complexAsSimple") ? CTF_UseComplexAsSimple : CTF_UseDefault;
	if (Body->CollisionTraceFlag != Flag)
	{
		Body->Modify();
		Body->CollisionTraceFlag = Flag;
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
		Mesh->MarkPackageDirty();
	}
}

/** An actor per placement, matched to the existing ones by node id. */
void PlaceActors(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bCollide, const TArray<TSharedPtr<FJsonValue>>& Placements,
	TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceActors);

	TArray<AActor*> Holders = Existing.FilterByPredicate([](const AActor* A) { return A->Tags.Contains(InstancedTag); });
	Removed += DestroyActors(World, Holders);
	TMap<FString, AStaticMeshActor*> ByNode;
	for (AActor* A : Existing)
	{
		if (AStaticMeshActor* S = Cast<AStaticMeshActor>(A); S && !A->Tags.Contains(InstancedTag))
		{
			ByNode.Add(TagValue(S, NodePrefix), S);
		}
	}
	const FName Folder(*(TEXT("Lila/") + ShortGroupName(Label)));
	TSet<FString> Sent;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		Sent.Add(Id);
		const FTransform Transform = TransformOf(*P);
		AStaticMeshActor*& Actor = ByNode.FindOrAdd(Id);
		if (!Actor)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
			if (!Actor)
			{
				continue;
			}
			Actor->Tags.Add(GroupTag(Group));
			Actor->Tags.Add(NodeTag(Id));
			Actor->SetFolderPath(Folder);
			++Spawned;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorTransform(Transform);
			Actor->SetFolderPath(Folder);
			++Updated;
		}
		FString Name;
		if ((*P)->TryGetStringField(TEXT("name"), Name))
		{
			Actor->SetActorLabel(Name);
		}
		Actor->GetStaticMeshComponent()->Modify();
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		SetCollides(Actor->GetStaticMeshComponent(), bCollide);
		const TArray<float> Custom = CustomDataOf(*P);
		for (int32 K = 0; K < Custom.Num(); ++K)
		{
			Actor->GetStaticMeshComponent()->SetCustomPrimitiveDataFloat(K, Custom[K]);
		}
		if (const TOptional<FLightingChannels> Channels = ChannelsOf(*P))
		{
			Actor->GetStaticMeshComponent()->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
		}
		ApplyPlacementComponents(Actor, *P, Sink);
		ApplyPhysics(Actor->GetStaticMeshComponent(), *P, Sink.Folder, false, Label, Sink);
	}
	for (const TPair<FString, AStaticMeshActor*>& Pair : ByNode)
	{
		if (Pair.Value && !Sent.Contains(Pair.Key))
		{
			World->EditorDestroyActor(Pair.Value, true);
			++Removed;
		}
	}
}

/** Instances share their component's Lighting Channels: the first placement's, with a warning when others differ. */
void ApplyInstancedChannels(UInstancedStaticMeshComponent* Component, const TSharedPtr<FJsonObject>& First, const TArray<TSharedPtr<FJsonValue>>& Placements,
	const FString& Label, FComponentSink& Sink)
{
	const TOptional<FLightingChannels> Channels = ChannelsOf(First);
	if (!Channels)
	{
		return;
	}
	Component->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			const TOptional<FLightingChannels> Other = ChannelsOf(*P);
			if (Other && !SameChannels(*Other, *Channels))
			{
				Sink.Warnings.Add(FString::Printf(TEXT("%s: instances share one component, so all take the first member's lighting channels"), *Label));
				return;
			}
		}
	}
}

/**
 * One holder actor whose (H)ISM component carries every placement as an instance, rebuilt in order each time. Instances
 * can't hold components, so the holder takes the first placement's.
 */
void PlaceInstanced(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bHierarchical, bool bCollide,
	const TArray<TSharedPtr<FJsonValue>>& Placements, TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceInstanced);

	AActor* Holder = nullptr;
	TArray<AActor*> Others;
	for (AActor* A : Existing)
	{
		if (!Holder && A->Tags.Contains(InstancedTag))
		{
			Holder = A;
		}
		else
		{
			Others.Add(A);
		}
	}
	Removed += DestroyActors(World, Others);
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transactional;
		Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		if (!Holder)
		{
			return;
		}
		Holder->Tags.Add(GroupTag(Group));
		Holder->Tags.Add(InstancedTag);
		Holder->SetFolderPath(FName(TEXT("Lila")));
	}
	Holder->Modify();
	Holder->SetActorLabel(TEXT("Lila_") + ShortGroupName(Label));

	UInstancedStaticMeshComponent* Component = Holder->FindComponentByClass<UInstancedStaticMeshComponent>();
	const bool bRightKind = Component && Component->IsA<UHierarchicalInstancedStaticMeshComponent>() == bHierarchical;
	const int32 Before = Component && bRightKind ? Component->GetInstanceCount() : 0;
	if (Component && !bRightKind)
	{
		Removed += Component->GetInstanceCount();
		Holder->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		Component = nullptr;
	}
	if (!Component)
	{
		UClass* Class = bHierarchical ? UHierarchicalInstancedStaticMeshComponent::StaticClass() : UInstancedStaticMeshComponent::StaticClass();
		Component = NewObject<UInstancedStaticMeshComponent>(Holder, Class, MakeUniqueObjectName(Holder, Class, TEXT("LilaInstances")), RF_Transactional);
		Holder->SetRootComponent(Component);
		Holder->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	Component->Modify();
	Component->SetStaticMesh(Mesh);
	SetCollides(Component, bCollide);
	Component->ClearInstances();
	// One bulk add and one custom data write: per-instance calls rebuild state each time and crawl at thousands of instances.
	TArray<FTransform> Transforms;
	TArray<TArray<float>> Custom;
	Transforms.Reserve(Placements.Num());
	Custom.Reserve(Placements.Num());
	int32 CustomFloats = 0;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			Transforms.Add(TransformOf(*P));
			CustomFloats = FMath::Max(CustomFloats, Custom.Add_GetRef(CustomDataOf(*P)).Num());
		}
	}
	const int32 Count = Transforms.Num();
	Component->SetNumCustomDataFloats(CustomFloats);
	Component->AddInstances(Transforms, false);
	if (CustomFloats > 0 && Count > 0)
	{
		// Every instance gets as many floats as the placement with the most, the missing ones 0.
		TArray<float> Flat;
		Flat.SetNumZeroed(Count * CustomFloats);
		for (int32 I = 0; I < Count; ++I)
		{
			FMemory::Memcpy(&Flat[I * CustomFloats], Custom[I].GetData(), Custom[I].Num() * sizeof(float));
		}
		Component->SetCustomData(0, Count - 1, Flat, true);
	}
	Updated += FMath::Min(Before, Count);
	Spawned += FMath::Max(0, Count - Before);
	Removed += FMath::Max(0, Before - Count);
	const TSharedPtr<FJsonObject>* First = nullptr;
	if (!Placements.IsEmpty() && Placements[0].IsValid() && Placements[0]->TryGetObject(First))
	{
		ApplyPlacementComponents(Holder, *First, Sink);
		ApplyInstancedChannels(Component, *First, Placements, Label, Sink);
		ApplyPhysics(Component, *First, Sink.Folder, true, Label, Sink);
	}
}

TSharedPtr<FJsonValue> PlaceInstances(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceInstances);

	FString Group, MeshPath, Mode = TEXT("actors");
	const TArray<TSharedPtr<FJsonValue>>* Placements = nullptr;
	if (!Params->TryGetStringField(TEXT("group"), Group) || !Params->TryGetStringField(TEXT("mesh"), MeshPath) ||
		!Params->TryGetArrayField(TEXT("placements"), Placements))
	{
		OutError = TEXT("placeInstances: give group, mesh and placements");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("mode"), Mode);
	FString Collision;
	Params->TryGetStringField(TEXT("collision"), Collision);
	const bool bCollide = Collision != TEXT("none");
	FString Label = Group;
	Params->TryGetStringField(TEXT("label"), Label);
	if (Mode != TEXT("actors") && Mode != TEXT("ism") && Mode != TEXT("hism"))
	{
		OutError = FString::Printf(TEXT("placeInstances: mode is actors, ism or hism, not '%s'"), *Mode);
		return nullptr;
	}
	const FString ObjectPath = MeshPath.Contains(TEXT(".")) ? MeshPath : MeshPath + TEXT(".") + FPackageName::GetShortName(MeshPath);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("placeInstances: no static mesh at %s"), *ObjectPath);
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placeInstances: no level is open");
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("PlaceInstances", "Lila: Place Instances"));
	int32 Spawned = 0, Updated = 0, Removed = 0;
	FComponentSink Sink;
	Sink.Folder = FPackageName::GetLongPackagePath(Mesh->GetPackage()->GetName());
	TArray<AActor*> Existing = TaggedActors(World, GroupTag(Group));
	ApplyMeshCollision(Mesh, *Placements);
	if (Mode == TEXT("actors"))
	{
		PlaceActors(World, Group, Label, Mesh, bCollide, *Placements, Existing, Spawned, Updated, Removed, Sink);
	}
	else
	{
		PlaceInstanced(World, Group, Label, Mesh, Mode == TEXT("hism"), bCollide, *Placements, Existing, Spawned, Updated, Removed, Sink);
	}
	for (const FString& W : Sink.Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeInstances %s: %s"), *Label, *W);
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}
	UE_LOG(LogLilaLink, Log, TEXT("placeInstances %s: %d spawned, %d updated, %d removed (%s)"), *Label, Spawned, Updated, Removed, *Mode);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("removed"), Removed);
	Out->SetStringField(TEXT("mode"), Mode);
	TArray<TSharedPtr<FJsonValue>> Warnings, Assets;
	for (const FString& W : Sink.Warnings)
	{
		Warnings.Add(MakeShared<FJsonValueString>(W));
	}
	for (const FString& A : Sink.Assets)
	{
		Assets.Add(MakeShared<FJsonValueString>(A));
	}
	Out->SetArrayField(TEXT("warnings"), Warnings);
	Out->SetArrayField(TEXT("assets"), Assets);
	return Obj(Out);
}

double NumberField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, double Fallback)
{
	double V = Fallback;
	P->TryGetNumberField(Field, V);
	return V;
}

bool BoolField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, bool Fallback)
{
	bool V = Fallback;
	P->TryGetBoolField(Field, V);
	return V;
}

UClass* LightClassFor(const FString& Kind)
{
	if (Kind == TEXT("spot"))
	{
		return ASpotLight::StaticClass();
	}
	if (Kind == TEXT("directional"))
	{
		return ADirectionalLight::StaticClass();
	}
	if (Kind == TEXT("area") || Kind == TEXT("disc"))
	{
		return ARectLight::StaticClass();
	}
	return APointLight::StaticClass();
}

/** Lila's light settings on the component: colour, temperature, intensity in its units, falloff, cone, size, shadows. */
void ApplyLightSettings(ULightComponent* Light, const TSharedPtr<FJsonObject>& P, const FString& Label, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyLightSettings);

	FString Kind, Units = TEXT("unitless");
	P->TryGetStringField(TEXT("kind"), Kind);
	P->TryGetStringField(TEXT("units"), Units);
	const FVector Color = VectorField(P, TEXT("color"), FVector::OneVector);
	Light->SetLightColor(FLinearColor(Color.X, Color.Y, Color.Z), false);
	Light->SetUseTemperature(BoolField(P, TEXT("useTemperature"), false));
	Light->SetTemperature(NumberField(P, TEXT("temperature"), 6500.0));
	Light->SetCastShadows(BoolField(P, TEXT("castShadows"), true));
	double Intensity = NumberField(P, TEXT("intensity"), 1.0);
	if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light))
	{
		ELightUnits LightUnits = ELightUnits::Unitless;
		if (Units == TEXT("candela"))
		{
			LightUnits = ELightUnits::Candelas;
		}
		else if (Units == TEXT("lumens"))
		{
			LightUnits = ELightUnits::Lumens;
		}
		else if (Units == TEXT("nits"))
		{
			LightUnits = ELightUnits::Nits;
		}
		else if (Units == TEXT("watts"))
		{
			// 683 lm/W, the peak luminous efficacy other apps convert radiant watts with.
			LightUnits = ELightUnits::Lumens;
			Intensity *= 683.0;
		}
		else if (Units == TEXT("lux"))
		{
			Warnings.Add(FString::Printf(TEXT("%s: lux has no meaning on a local light; sent unitless"), *Label));
		}
		Local->SetIntensityUnits(LightUnits);
		if (const double Range = NumberField(P, TEXT("range"), 0.0); Range > 0.0)
		{
			Local->SetAttenuationRadius(Range);
		}
	}
	Light->SetIntensity(Intensity);
	if (UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
	{
		Point->SetSourceRadius(NumberField(P, TEXT("sourceRadius"), 0.0));
	}
	if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
	{
		Spot->SetInnerConeAngle(NumberField(P, TEXT("innerConeAngle"), 0.0));
		Spot->SetOuterConeAngle(NumberField(P, TEXT("outerConeAngle"), 45.0));
	}
	if (URectLightComponent* Rect = Cast<URectLightComponent>(Light))
	{
		const double Width = NumberField(P, TEXT("width"), 100.0);
		Rect->SetSourceWidth(Width);
		Rect->SetSourceHeight(Kind == TEXT("disc") ? Width : NumberField(P, TEXT("height"), 100.0));
		if (Kind == TEXT("disc"))
		{
			Warnings.Add(FString::Printf(TEXT("%s: Unreal has no disc light; sent as a square rect light"), *Label));
		}
	}
	if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Light))
	{
		Directional->SetLightSourceAngle(NumberField(P, TEXT("sourceRadius"), 0.0));
	}
	if (const TOptional<FLightingChannels> Channels = ChannelsOf(P))
	{
		Light->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
	}
}

TSharedPtr<FJsonValue> PlaceLights(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceLights);

	const TArray<TSharedPtr<FJsonValue>>* Lights = nullptr;
	if (!Params->TryGetArrayField(TEXT("lights"), Lights))
	{
		OutError = TEXT("placeLights: give lights");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placeLights: no level is open");
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("PlaceLights", "Lila: Place Lights"));
	TMap<FString, ALight*> ByNode;
	for (AActor* A : TaggedActors(World, LightTag))
	{
		if (ALight* L = Cast<ALight>(A))
		{
			ByNode.Add(TagValue(L, NodePrefix), L);
		}
	}
	int32 Spawned = 0, Updated = 0;
	TArray<FString> Warnings;
	for (const TSharedPtr<FJsonValue>& V : *Lights)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		FString Kind, Name = Id;
		(*P)->TryGetStringField(TEXT("kind"), Kind);
		(*P)->TryGetStringField(TEXT("name"), Name);
		UClass* Class = LightClassFor(Kind);
		// The actor's X axis is where the light shines; Z is Lila's up, along a rect light's height.
		const FVector Forward = VectorField(*P, TEXT("forward"), FVector::ForwardVector);
		const FVector Up = VectorField(*P, TEXT("up"), FVector::UpVector);
		const FTransform Transform(FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat(), VectorField(*P, TEXT("location"), FVector::ZeroVector));
		ALight*& Actor = ByNode.FindOrAdd(Id);
		if (Actor && !Actor->IsA(Class))
		{
			World->EditorDestroyActor(Actor, true);
			Actor = nullptr;
		}
		if (!Actor)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			Actor = Cast<ALight>(World->SpawnActor(Class, &Transform, SpawnParams));
			if (!Actor)
			{
				continue;
			}
			Actor->Tags.Add(LightTag);
			Actor->Tags.Add(NodeTag(Id));
			++Spawned;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorTransform(Transform);
			++Updated;
		}
		Actor->SetFolderPath(FName(TEXT("Lila/Lights")));
		Actor->SetActorLabel(Name);
		if (ULightComponent* Light = Actor->GetLightComponent())
		{
			Light->Modify();
			ApplyLightSettings(Light, *P, Name, Warnings);
		}
	}
	for (const FString& W : Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeLights: %s"), *W);
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}
	UE_LOG(LogLilaLink, Log, TEXT("placeLights: %d spawned, %d updated"), Spawned, Updated);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return Obj(Out);
}

TSharedPtr<FJsonValue> RemovePlacements(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::RemovePlacements);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removePlacements: no level is open");
		return nullptr;
	}
	TSet<FName> Tags;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Params->TryGetArrayField(TEXT("groups"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Tags.Add(GroupTag(V->AsString()));
		}
	}
	if (Params->TryGetArrayField(TEXT("ids"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Tags.Add(NodeTag(V->Type == EJson::Number ? FString::Printf(TEXT("%lld"), static_cast<long long>(V->AsNumber())) : V->AsString()));
		}
	}
	// Groups whose key starts with a prefix go too, unless kept: a source's geometry nodes meshes it no longer instances.
	TArray<FString> Prefixes;
	TSet<FString> Keep;
	if (Params->TryGetArrayField(TEXT("prefixes"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Prefixes.Add(V->AsString());
		}
	}
	if (Params->TryGetArrayField(TEXT("keep"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Keep.Add(V->AsString());
		}
	}
	if (Tags.IsEmpty() && Prefixes.IsEmpty())
	{
		OutError = TEXT("removePlacements: give groups, ids or prefixes");
		return nullptr;
	}
	FScopedTransaction Transaction(LOCTEXT("RemovePlacements", "Lila: Remove Placements"));
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString Group = Prefixes.IsEmpty() ? FString() : TagValue(*It, GroupPrefix);
		if (!Group.IsEmpty() && !Keep.Contains(Group) && Prefixes.ContainsByPredicate([&Group](const FString& P) { return Group.StartsWith(P); }))
		{
			Doomed.Add(*It);
			continue;
		}
		for (const FName& Tag : It->Tags)
		{
			if (Tags.Contains(Tag))
			{
				Doomed.Add(*It);
				break;
			}
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), DestroyActors(World, Doomed));
	return Obj(Out);
}

/** A package name not taken yet: the path itself, else with _2, _3, ... after it. */
FString FreePackageName(const FString& Path)
{
	FString Candidate = Path;
	for (int32 K = 2; FPackageName::DoesPackageExist(Candidate) || FindPackage(nullptr, *Candidate); ++K)
	{
		Candidate = FString::Printf(TEXT("%s_%d"), *Path, K);
	}
	return Candidate;
}

TSharedPtr<FJsonValue> MakeLevelInstance(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::MakeLevelInstance);

	FString Key, Name, Kind = TEXT("levelInstance"), Level;
	const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
	if (!Params->TryGetStringField(TEXT("key"), Key) || !Params->TryGetStringField(TEXT("level"), Level) || !Params->TryGetArrayField(TEXT("groups"), Groups))
	{
		OutError = TEXT("makeLevelInstance: give key, level and groups");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("name"), Name);
	Params->TryGetStringField(TEXT("kind"), Kind);
	UWorld* World = EditorWorld();
	ULevelInstanceSubsystem* Subsystem = World ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
	if (!Subsystem)
	{
		OutError = TEXT("makeLevelInstance: no level is open");
		return nullptr;
	}
	TArray<AActor*> Actors;
	for (const TSharedPtr<FJsonValue>& G : *Groups)
	{
		Actors.Append(TaggedActors(World, GroupTag(G->AsString())));
	}
	if (Actors.IsEmpty())
	{
		OutError = TEXT("makeLevelInstance: none of the groups' actors are in the level");
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("MakeLevelInstance", "Lila: Make Level Instance"));
	const FName Tag(*(FString(AssemblyPrefix) + Key));
	TArray<AActor*> Previous = TaggedActors(World, Tag);
	for (AActor* Old : Previous)
	{
		World->EditorDestroyActor(Old, true);
	}
	FNewLevelInstanceParams Create;
	Create.Type = Kind == TEXT("packedLevelActor") ? ELevelInstanceCreationType::PackedLevelActor : ELevelInstanceCreationType::LevelInstance;
	Create.PivotType = ELevelInstancePivotType::CenterMinZ;
	Create.bAlwaysShowDialog = false;
	Create.LevelPackageName = FreePackageName(Level);
	ILevelInstanceInterface* Made = Subsystem->CreateLevelInstanceFrom(Actors, Create);
	AActor* Actor = Made ? Cast<AActor>(Made) : nullptr;
	if (!Actor)
	{
		OutError = FString::Printf(TEXT("makeLevelInstance: Unreal could not make %s"), *Create.LevelPackageName);
		return nullptr;
	}
	Actor->Tags.Add(Tag);
	if (!Name.IsEmpty())
	{
		Actor->SetActorLabel(Name);
	}
	Actor->SetFolderPath(FName(TEXT("Lila/Assemblies")));
	UE_LOG(LogLilaLink, Log, TEXT("makeLevelInstance %s: %d actors into %s (%s)"), *Name, Actors.Num(), *Create.LevelPackageName, *Kind);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Out->SetStringField(TEXT("level"), Create.LevelPackageName);
	Out->SetStringField(TEXT("kind"), Kind);
	Out->SetNumberField(TEXT("actors"), Actors.Num());
	Out->SetNumberField(TEXT("replaced"), Previous.Num());
	return Obj(Out);
}

TSharedPtr<FJsonValue> ListPlacements(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ListPlacements);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("listPlacements: no level is open");
		return nullptr;
	}
	TSet<FString> Only;
	const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
	if (Params->TryGetArrayField(TEXT("groups"), Groups))
	{
		for (const TSharedPtr<FJsonValue>& V : *Groups)
		{
			Only.Add(V->AsString());
		}
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString Group = TagValue(*It, GroupPrefix);
		if (Group.IsEmpty() || (Groups && !Only.Contains(Group)))
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), It->GetActorLabel());
		Entry->SetStringField(TEXT("group"), Group);
		if (const UStaticMeshComponent* C = It->FindComponentByClass<UStaticMeshComponent>(); C && C->GetStaticMesh())
		{
			Entry->SetStringField(TEXT("mesh"), C->GetStaticMesh()->GetPathName());
			Entry->SetStringField(TEXT("collision"), C->GetCollisionProfileName().ToString());
		}
		const FVector Location = It->GetActorLocation();
		Entry->SetArrayField(TEXT("location"), {MakeShared<FJsonValueNumber>(Location.X), MakeShared<FJsonValueNumber>(Location.Y), MakeShared<FJsonValueNumber>(Location.Z)});
		if (It->Tags.Contains(InstancedTag))
		{
			Entry->SetNumberField(TEXT("instances"), Weight(*It));
		}
		else
		{
			Entry->SetStringField(TEXT("node"), TagValue(*It, NodePrefix));
		}
		Out.Add(Obj(Entry));
	}
	return MakeShared<FJsonValueArray>(Out);
}
} // namespace

void LilaLink::AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("placeInstances"), LilaLink::Sync(&PlaceInstances));
	Methods.Add(TEXT("placeLights"), LilaLink::Sync(&PlaceLights));
	Methods.Add(TEXT("removePlacements"), LilaLink::Sync(&RemovePlacements));
	Methods.Add(TEXT("listPlacements"), LilaLink::Sync(&ListPlacements));
	Methods.Add(TEXT("makeLevelInstance"), LilaLink::Sync(&MakeLevelInstance));
}

#undef LOCTEXT_NAMESPACE
