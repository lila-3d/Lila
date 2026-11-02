#include "LilaLinkPlacements.h"

#include "LilaLinkComponents.h"
#include "LilaLinkLog.h"

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
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "LilaLinkPlacements"

namespace
{
const TCHAR* GroupPrefix = TEXT("LilaGroup:");
const TCHAR* NodePrefix = TEXT("LilaNode:");
const FName InstancedTag(TEXT("LilaInstanced"));
const FName LightTag(TEXT("LilaLight"));

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

/** An actor per placement, matched to the existing ones by node id. */
void PlaceActors(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, const TArray<TSharedPtr<FJsonValue>>& Placements, TArray<AActor*> Existing,
	int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
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
void PlaceInstanced(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bHierarchical, const TArray<TSharedPtr<FJsonValue>>& Placements,
	TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
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
	Component->ClearInstances();
	// Every instance gets as many custom data floats as the placement with the most.
	int32 CustomFloats = 0;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			CustomFloats = FMath::Max(CustomFloats, CustomDataOf(*P).Num());
		}
	}
	Component->SetNumCustomDataFloats(CustomFloats);
	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			const int32 Index = Component->AddInstance(TransformOf(*P), true);
			const TArray<float> Custom = CustomDataOf(*P);
			for (int32 K = 0; K < Custom.Num(); ++K)
			{
				Component->SetCustomDataValue(Index, K, Custom[K]);
			}
			++Count;
		}
	}
	Updated += FMath::Min(Before, Count);
	Spawned += FMath::Max(0, Count - Before);
	Removed += FMath::Max(0, Before - Count);
	const TSharedPtr<FJsonObject>* First = nullptr;
	if (!Placements.IsEmpty() && Placements[0].IsValid() && Placements[0]->TryGetObject(First))
	{
		ApplyPlacementComponents(Holder, *First, Sink);
		ApplyInstancedChannels(Component, *First, Placements, Label, Sink);
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
	if (Mode == TEXT("actors"))
	{
		PlaceActors(World, Group, Label, Mesh, *Placements, Existing, Spawned, Updated, Removed, Sink);
	}
	else
	{
		PlaceInstanced(World, Group, Label, Mesh, Mode == TEXT("hism"), *Placements, Existing, Spawned, Updated, Removed, Sink);
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
	if (Tags.IsEmpty())
	{
		OutError = TEXT("removePlacements: give groups or ids");
		return nullptr;
	}
	FScopedTransaction Transaction(LOCTEXT("RemovePlacements", "Lila: Remove Placements"));
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
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

TSharedPtr<FJsonValue> ListPlacements(const TSharedPtr<FJsonObject>&, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ListPlacements);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("listPlacements: no level is open");
		return nullptr;
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString Group = TagValue(*It, GroupPrefix);
		if (Group.IsEmpty())
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), It->GetActorLabel());
		Entry->SetStringField(TEXT("group"), Group);
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
}

#undef LOCTEXT_NAMESPACE
