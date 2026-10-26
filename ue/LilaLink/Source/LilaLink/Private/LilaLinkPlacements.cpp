#include "LilaLinkPlacements.h"

#include "LilaLinkLog.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
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

/** An actor per placement, matched to the existing ones by node id. */
void PlaceActors(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, const TArray<TSharedPtr<FJsonValue>>& Placements, TArray<AActor*> Existing,
	int32& Spawned, int32& Updated, int32& Removed)
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

/** One holder actor whose (H)ISM component carries every placement as an instance, rebuilt in order each time. */
void PlaceInstanced(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bHierarchical, const TArray<TSharedPtr<FJsonValue>>& Placements,
	TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed)
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
	TArray<AActor*> Existing = TaggedActors(World, GroupTag(Group));
	if (Mode == TEXT("actors"))
	{
		PlaceActors(World, Group, Label, Mesh, *Placements, Existing, Spawned, Updated, Removed);
	}
	else
	{
		PlaceInstanced(World, Group, Label, Mesh, Mode == TEXT("hism"), *Placements, Existing, Spawned, Updated, Removed);
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
	Methods.Add(TEXT("removePlacements"), LilaLink::Sync(&RemovePlacements));
	Methods.Add(TEXT("listPlacements"), LilaLink::Sync(&ListPlacements));
}

#undef LOCTEXT_NAMESPACE
