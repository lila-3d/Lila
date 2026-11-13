#include "LilaLinkCrowd.h"

#include "LilaLinkMethodRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Avoidance/MassAvoidanceTrait.h"
#include "Avoidance/MassNavigationObstacleTrait.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "MassActorSubsystem.h"
#include "MassAssortedFragmentsTrait.h"
#include "MassVisualizer.h"
#include "MassCommonFragments.h"
#include "MassCrowdFragments.h"
#include "MassCrowdMemberTrait.h"
#include "MassCrowdSettings.h"
#include "MassCrowdVisualizationTrait.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityQuery.h"
#include "MassEntityUtils.h"
#include "MassEntityZoneGraphSpawnPointsGenerator.h"
#include "MassExecutionContext.h"
#include "MassLODTrait.h"
#include "MassRepresentationFragments.h"
#include "MassSpawner.h"
#include "MassStateTreeSchema.h"
#include "MassStateTreeTrait.h"
#include "MassZoneGraphNavigationFragments.h"
#include "MassZoneGraphNavigationTrait.h"
#include "Misc/PackageName.h"
#include "Movement/MassMovementTrait.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "SmoothOrientation/MassSmoothOrientationTrait.h"
#include "StateTree.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "Steering/MassSteeringTrait.h"
#include "Tasks/MassZoneGraphFindWanderTarget.h"
#include "Tasks/MassZoneGraphPathFollowTask.h"
#include "Tasks/MassZoneGraphStandTask.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ZoneGraphDelegates.h"
#include "ZoneGraphSettings.h"
#include "ZoneGraphSubsystem.h"
#include "ZoneShapeComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogLilaLinkCrowd, Log, All);

namespace
{
const FName LilaTag(TEXT("Lila"));
const FName ProfileName(TEXT("Lila_Crowd"));

FName ActorTagFor(const FString& Id) { return FName(*(TEXT("LilaCrowd_") + Id)); }

TArray<TSharedPtr<FJsonValue>> ArrayOf(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Obj.IsValid() && Obj->TryGetArrayField(Field, Values) ? *Values : TArray<TSharedPtr<FJsonValue>>();
}

FVector ReadPoint(const TSharedPtr<FJsonValue>& Value)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	return Value.IsValid() && Value->TryGetArray(A) && A->Num() >= 3 ? FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber()) : FVector::ZeroVector;
}

TSharedRef<FJsonValue> PointValue(const FVector& P)
{
	return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(P.X), MakeShared<FJsonValueNumber>(P.Y), MakeShared<FJsonValueNumber>(P.Z)});
}

/** A reflected property's value on an object, for the config and settings fields their classes keep protected. */
template <typename T>
T* Field(UObject* Object, const TCHAR* Name)
{
	const FProperty* P = Object ? Object->GetClass()->FindPropertyByName(Name) : nullptr;
	return P ? P->ContainerPtrToValuePtr<T>(Object) : nullptr;
}

FString AssetName(const FString& Text)
{
	FString Out = Text;
	for (TCHAR& C : Out)
	{
		C = FChar::IsAlnum(C) ? C : TEXT('_');
	}
	return Out;
}

// ZoneGraph -----------------------------------------------------------------------------------------------------

/** The ZoneGraph settings' tag table and lane profiles, protected config properties reached by reflection. */
struct FZoneSettings
{
	UZoneGraphSettings* Settings = GetMutableDefault<UZoneGraphSettings>();
	TArray<FZoneLaneProfile>* Profiles = Field<TArray<FZoneLaneProfile>>(Settings, TEXT("LaneProfiles"));
	FZoneGraphTagInfo* Tags = Field<FZoneGraphTagInfo>(Settings, TEXT("Tags"));

	int32 TagCount() const
	{
		const FProperty* P = UZoneGraphSettings::StaticClass()->FindPropertyByName(TEXT("Tags"));
		return P ? P->ArrayDim : 0;
	}

	/** The tag of that name, made in the first free slot when the project has none. */
	FZoneGraphTag Tag(const FString& Name, TArray<FString>& Warnings)
	{
		const FName Key(*Name);
		const int32 Count = TagCount();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Tags[Index].Name == Key)
			{
				return Tags[Index].Tag;
			}
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Tags[Index].Name.IsNone())
			{
				Tags[Index].Name = Key;
				Tags[Index].Tag = FZoneGraphTag(static_cast<uint8>(Index));
				Tags[Index].Color = FColor::MakeRandomSeededColor(static_cast<int32>(GetTypeHash(Name)));
				return Tags[Index].Tag;
			}
		}
		Warnings.Add(FString::Printf(TEXT("no free ZoneGraph tag slot for %s"), *Name));
		return FZoneGraphTag::None;
	}

	/** The tag's name, or none when the project has no tag in that slot. */
	FName NameOf(const FZoneGraphTag Tag) const
	{
		const int32 Count = TagCount();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Tags[Index].Tag == Tag && !Tags[Index].Name.IsNone())
			{
				return Tags[Index].Name;
			}
		}
		return NAME_None;
	}
};

/** A crowd lane's end that meets others: where its trimmed end sits and the direction away from the meeting point. */
struct FLaneEnd
{
	int32 Lane = INDEX_NONE;
	bool bStart = true;
	FVector Away = FVector::XAxisVector;
};

/** Lanes whose ends meet (within a centimetre) share a node; a node of two or more ends becomes a small polygon. */
struct FCrowdNetwork
{
	struct FLane
	{
		TArray<FVector> Points;
		bool bCrossing = false;
	};
	TArray<FLane> Lanes;
	TArray<FVector> Nodes;
	TArray<TArray<FLaneEnd>> NodeEnds;

	int32 NodeAt(const FVector& P)
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (FVector::DistSquared(Nodes[Index], P) < 1.0)
			{
				return Index;
			}
		}
		NodeEnds.AddDefaulted();
		return Nodes.Add(P);
	}

	void Build(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::BuildNetwork);

		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			FLane Lane;
			for (const TSharedPtr<FJsonValue>& P : ArrayOf(Obj, TEXT("points")))
			{
				const FVector Point = ReadPoint(P);
				if (Lane.Points.IsEmpty() || FVector::DistSquared(Lane.Points.Last(), Point) > 1.0)
				{
					Lane.Points.Add(Point);
				}
			}
			if (Lane.Points.Num() < 2)
			{
				continue;
			}
			Obj->TryGetBoolField(TEXT("crossing"), Lane.bCrossing);
			Lanes.Add(MoveTemp(Lane));
		}
		for (int32 L = 0; L < Lanes.Num(); ++L)
		{
			const TArray<FVector>& P = Lanes[L].Points;
			NodeEnds[NodeAt(P[0])].Add({L, true, (P[1] - P[0]).GetSafeNormal()});
			NodeEnds[NodeAt(P.Last())].Add({L, false, (P[P.Num() - 2] - P.Last()).GetSafeNormal()});
		}
	}
};

/** How far each lane end stops short of a node it shares, so the node's polygon has room: under a third of the end segment. */
double TrimAt(const FCrowdNetwork& Net, const FLaneEnd& End)
{
	const TArray<FVector>& P = Net.Lanes[End.Lane].Points;
	const double Segment = End.bStart ? FVector::Dist(P[0], P[1]) : FVector::Dist(P.Last(), P[P.Num() - 2]);
	return FMath::Min(75.0, Segment * 0.3);
}

// StateTree and Mass assets --------------------------------------------------------------------------------------

struct FGroup
{
	FString Name;
	double Share = 1.0;
	double Speed = 130.0;
	double SpeedVariance = 0.1;
	double Pause = 2.0;
	double Radius = 40.0;
};

template <typename T>
T* AssetIn(const FString& Folder, const FString& Name, bool& bCreated)
{
	const FString PackagePath = Folder / Name;
	UPackage* Package = CreatePackage(*PackagePath);
	Package->FullyLoad();
	T* Asset = FindObject<T>(Package, *Name);
	bCreated = Asset == nullptr;
	if (!Asset)
	{
		Asset = NewObject<T>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	Asset->Modify();
	return Asset;
}

bool Save(UObject* Asset)
{
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
}

/**
 * The lane walker: Wander finds a target along the crowd lanes and its Walk child follows it there, Stand waits pause
 * seconds, then Wander picks the next. With no lane to wander on, Idle stands and tries again.
 */
UStateTree* MakeStateTree(const FString& Folder, const FString& Name, const FGroup& Group, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::MakeStateTree);

	bool bCreated = false;
	UStateTree* Tree = AssetIn<UStateTree>(Folder, Name, bCreated);
	UStateTreeEditorData* Data = NewObject<UStateTreeEditorData>(Tree, NAME_None, RF_Transactional);
	Data->Schema = NewObject<UMassStateTreeSchema>(Data);
	Tree->EditorData = Data;

	UStateTreeState& Root = Data->AddSubTree(FName(TEXT("Root")));
	UStateTreeState& Wander = Root.AddChildState(FName(TEXT("Wander")));
	UStateTreeState& Idle = Root.AddChildState(FName(TEXT("Idle")));
	UStateTreeState& Walk = Wander.AddChildState(FName(TEXT("Walk")));
	UStateTreeState& Stand = Wander.AddChildState(FName(TEXT("Stand")));

	auto& Find = Wander.AddTask<FMassZoneGraphFindWanderTarget>();
	auto& Follow = Walk.AddTask<FMassZoneGraphPathFollowTask>();
	Follow.GetInstanceData().SpeedScale = 1.0f;
	if (!Data->AddPropertyBinding(Find, TEXT("WanderTargetLocation"), Follow, TEXT("TargetLocation")))
	{
		Warnings.Add(FString::Printf(TEXT("%s: could not bind the path follow's target to the wander target"), *Name));
	}
	Stand.AddTask<FMassZoneGraphStandTask>().GetInstanceData().Duration = static_cast<float>(FMath::Max(Group.Pause, 0.1));
	Idle.AddTask<FMassZoneGraphStandTask>().GetInstanceData().Duration = 2.0f;

	Walk.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Stand);
	Stand.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Wander);
	Wander.AddTransition(EStateTreeTransitionTrigger::OnStateFailed, EStateTreeTransitionType::GotoState, &Idle);
	Idle.AddTransition(EStateTreeTransitionTrigger::OnStateCompleted, EStateTreeTransitionType::GotoState, &Wander);

	FStateTreeCompilerLog Log;
	FStateTreeCompiler Compiler(Log);
	if (!Compiler.Compile(*Tree))
	{
		Log.DumpToLog(LogLilaLinkCrowd);
		Warnings.Add(FString::Printf(TEXT("the StateTree %s didn't compile; the editor log has why"), *Name));
		return nullptr;
	}
	Save(Tree);
	return Tree;
}

template <typename TTrait>
TTrait* AddTrait(UMassEntityConfigAsset* Config)
{
	TTrait* Trait = NewObject<TTrait>(Config);
	Config->GetMutableConfig().AddTrait(*Trait);
	return Trait;
}

UMassEntityConfigAsset* MakeConfig(const FString& Folder, const FString& Name, const FGroup& Group, UStateTree* Tree, UStaticMesh* Mesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::MakeConfig);

	bool bCreated = false;
	UMassEntityConfigAsset* Config = AssetIn<UMassEntityConfigAsset>(Folder, Name, bCreated);
	Config->GetMutableConfig() = FMassEntityConfig();

	UMassAssortedFragmentsTrait* Fragments = AddTrait<UMassAssortedFragmentsTrait>(Config);
	if (TArray<FInstancedStruct>* List = Field<TArray<FInstancedStruct>>(Fragments, TEXT("Fragments")))
	{
		List->Add(FInstancedStruct::Make<FTransformFragment>());
		FAgentRadiusFragment Radius;
		Radius.Radius = static_cast<float>(Group.Radius);
		List->Add(FInstancedStruct::Make(Radius));
		// The crowd visualization keeps an actor slot per agent, even when every distance shows instances.
		List->Add(FInstancedStruct::Make<FMassActorFragment>());
	}
	AddTrait<UMassCrowdMemberTrait>(Config);
	UMassStateTreeTrait* Behaviour = AddTrait<UMassStateTreeTrait>(Config);
	if (TObjectPtr<UStateTree>* TreeField = Field<TObjectPtr<UStateTree>>(Behaviour, TEXT("StateTree")))
	{
		*TreeField = Tree;
	}
	AddTrait<UMassZoneGraphNavigationTrait>(Config);
	UMassMovementTrait* Movement = AddTrait<UMassMovementTrait>(Config);
	if (FMassMovementParameters* M = Field<FMassMovementParameters>(Movement, TEXT("Movement")))
	{
		M->DefaultDesiredSpeed = static_cast<float>(Group.Speed);
		M->DefaultDesiredSpeedVariance = static_cast<float>(FMath::Clamp(Group.SpeedVariance, 0.0, 1.0));
		M->MaxSpeed = static_cast<float>(Group.Speed * (1.0 + Group.SpeedVariance) * 1.25);
	}
	AddTrait<UMassSteeringTrait>(Config);
	AddTrait<UMassSmoothOrientationTrait>(Config);
	AddTrait<UMassObstacleAvoidanceTrait>(Config);
	AddTrait<UMassNavigationObstacleTrait>(Config);
	AddTrait<UMassLODCollectorTrait>(Config);
	AddTrait<UMassSimulationLODTrait>(Config);
	if (Mesh)
	{
		UMassCrowdVisualizationTrait* Look = AddTrait<UMassCrowdVisualizationTrait>(Config);
		FMassStaticMeshInstanceVisualizationMeshDesc& Desc = Look->StaticMeshInstanceDesc.Meshes.AddDefaulted_GetRef();
		Desc.Mesh = Mesh;
		// Instances at every distance: a crowd from Lila has no actor templates to swap to up close.
		if (FMassRepresentationParameters* Params = Field<FMassRepresentationParameters>(Look, TEXT("Params")))
		{
			for (int32 Lod = 0; Lod < EMassLOD::Max; ++Lod)
			{
				Params->LODRepresentation[Lod] = Lod == EMassLOD::Off ? EMassRepresentationType::None : EMassRepresentationType::StaticMeshInstance;
			}
		}
	}
	Save(Config);
	return Config;
}

// Methods ------------------------------------------------------------------------------------------------------------

AActor* TaggedActor(UWorld* World, const FName Tag, const UClass* Class)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag) && It->IsA(Class))
		{
			return *It;
		}
	}
	return nullptr;
}

/** The crowd network's ZoneGraph shapes on a fresh actor; returns it, with the lanes and polygons made. */
AActor* MakeLanes(UWorld* World, const FString& Id, const FString& Label, const FCrowdNetwork& Net, const FZoneLaneProfileRef& Profile,
				  const FZoneGraphTag CrowdTag, const FZoneGraphTag CrossingTag, int32& OutPolygons)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::MakeLanes);

	const FName Tag = ActorTagFor(Id);
	if (AActor* Old = TaggedActor(World, Tag, AActor::StaticClass()); Old && !Old->IsA<AMassSpawner>())
	{
		World->EditorDestroyActor(Old, true);
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!Actor)
	{
		return nullptr;
	}
	Actor->Tags.Add(Tag);
	Actor->Tags.Add(LilaTag);
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"), RF_Transactional);
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorLabel(Label + TEXT(" Crowd Lanes"));
	Actor->SetFolderPath(FName(TEXT("Lila/Crowd")));

	auto NewShape = [&](const FZoneShapeType Type, const FZoneGraphTagMask Tags) {
		UZoneShapeComponent* Shape = NewObject<UZoneShapeComponent>(Actor, MakeUniqueObjectName(Actor, UZoneShapeComponent::StaticClass(), TEXT("LilaCrowdShape")), RF_Transactional);
		Shape->ComponentTags.Add(LilaTag);
		Shape->SetupAttachment(Root);
		Shape->SetShapeType(Type);
		Shape->SetCommonLaneProfile(Profile);
		Shape->SetTags(Tags);
		Actor->AddInstanceComponent(Shape);
		Shape->GetMutablePoints().Reset();
		return Shape;
	};

	// Where each lane end stops: short of a node it shares with another lane, else at its own end.
	TMap<TPair<int32, bool>, double> Trim;
	for (const TArray<FLaneEnd>& Ends : Net.NodeEnds)
	{
		if (Ends.Num() >= 2)
		{
			for (const FLaneEnd& E : Ends)
			{
				Trim.Add({E.Lane, E.bStart}, TrimAt(Net, E));
			}
		}
	}
	for (int32 L = 0; L < Net.Lanes.Num(); ++L)
	{
		const FCrowdNetwork::FLane& Lane = Net.Lanes[L];
		FZoneGraphTagMask Tags(CrowdTag);
		if (Lane.bCrossing)
		{
			Tags.Add(CrossingTag);
		}
		UZoneShapeComponent* Shape = NewShape(FZoneShapeType::Spline, Tags);
		TArray<FVector> Points = Lane.Points;
		if (const double* T = Trim.Find({L, true}))
		{
			Points[0] += (Points[1] - Points[0]).GetSafeNormal() * *T;
		}
		if (const double* T = Trim.Find({L, false}))
		{
			const int32 N = Points.Num();
			Points[N - 1] += (Points[N - 2] - Points[N - 1]).GetSafeNormal() * *T;
		}
		for (const FVector& P : Points)
		{
			Shape->GetMutablePoints().Emplace_GetRef(P).Type = FZoneShapePointType::AutoBezier;
		}
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			Shape->UpdatePointRotationAndTangent(Index);
		}
		Shape->RegisterComponent();
		Shape->UpdateShape();
	}
	OutPolygons = 0;
	for (int32 N = 0; N < Net.Nodes.Num(); ++N)
	{
		TArray<FLaneEnd> Ends = Net.NodeEnds[N];
		if (Ends.Num() < 2)
		{
			continue;
		}
		Ends.Sort([](const FLaneEnd& A, const FLaneEnd& B) { return FMath::Atan2(A.Away.Y, A.Away.X) < FMath::Atan2(B.Away.Y, B.Away.X); });
		UZoneShapeComponent* Shape = NewShape(FZoneShapeType::Polygon, FZoneGraphTagMask(CrowdTag));
		for (const FLaneEnd& E : Ends)
		{
			// Polygon points face into the shape, toward the node.
			FZoneShapePoint& Point = Shape->GetMutablePoints().Emplace_GetRef(Net.Nodes[N] + E.Away * TrimAt(Net, E));
			Point.Type = FZoneShapePointType::LaneProfile;
			Point.SetRotationFromForwardAndUp(-E.Away, FVector::UpVector);
		}
		Shape->RegisterComponent();
		Shape->UpdateShape();
		++OutPolygons;
	}
	Actor->MarkPackageDirty();
	return Actor;
}

TSharedPtr<FJsonValue> SetMassCrowd(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::SetMassCrowd);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("setMassCrowd: no editor world");
		return nullptr;
	}
	if (GEditor->PlayWorld)
	{
		OutError = TEXT("setMassCrowd: stop Play In Editor first; the crowd goes into the editor level");
		return nullptr;
	}
	const FString Id = Params->GetStringField(TEXT("id"));
	if (Id.IsEmpty())
	{
		OutError = TEXT("setMassCrowd: give id");
		return nullptr;
	}
	FString Label = Id, Folder = TEXT("/Game/Lila/Crowd");
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("folder"), Folder);
	FCrowdNetwork Net;
	Net.Build(ArrayOf(Params, TEXT("lanes")));
	if (Net.Lanes.IsEmpty())
	{
		OutError = TEXT("setMassCrowd: no lanes with two or more points");
		return nullptr;
	}
	TArray<FString> Warnings;
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetMassCrowd", "Lila: Update Mass Crowd"));

	// Tags and the two-lane profile (one lane each way) the crowd lanes use; MassCrowd builds its lane data on the crowd tag.
	FZoneSettings Zone;
	if (!Zone.Profiles || !Zone.Tags)
	{
		OutError = TEXT("setMassCrowd: this ZoneGraph has no lane profile or tag settings to write");
		return nullptr;
	}
	FString CrowdName = TEXT("LilaCrowd"), CrossingName = TEXT("Crossing");
	Params->TryGetStringField(TEXT("crowdTag"), CrowdName);
	Params->TryGetStringField(TEXT("crossingTag"), CrossingName);
	Zone.Settings->Modify();
	const FZoneGraphTag CrowdTag = Zone.Tag(CrowdName, Warnings), CrossingTag = Zone.Tag(CrossingName, Warnings);
	double Width = 150.0;
	Params->TryGetNumberField(TEXT("laneWidth"), Width);
	FZoneLaneProfile* Profile = Zone.Profiles->FindByPredicate([](const FZoneLaneProfile& P) { return P.Name == ProfileName; });
	if (!Profile)
	{
		Profile = &Zone.Profiles->AddDefaulted_GetRef();
		Profile->Name = ProfileName;
	}
	Profile->Lanes.Reset();
	for (const EZoneLaneDirection Direction : {EZoneLaneDirection::Forward, EZoneLaneDirection::Backward})
	{
		FZoneLaneDesc& Lane = Profile->Lanes.AddDefaulted_GetRef();
		Lane.Width = static_cast<float>(Width * 0.5);
		Lane.Direction = Direction;
		Lane.Tags = FZoneGraphTagMask(CrowdTag);
	}
	Zone.Settings->TryUpdateDefaultConfigFile();
	UE::ZoneGraphDelegates::OnZoneGraphTagsChanged.Broadcast();
	if (UMassCrowdSettings* Crowd = GetMutableDefault<UMassCrowdSettings>())
	{
		Crowd->Modify();
		if (FZoneGraphTag* T = Field<FZoneGraphTag>(Crowd, TEXT("CrowdTag")))
		{
			*T = CrowdTag;
		}
		if (FZoneGraphTag* T = Field<FZoneGraphTag>(Crowd, TEXT("CrossingTag")))
		{
			*T = CrossingTag;
		}
		Crowd->TryUpdateDefaultConfigFile();
	}

	int32 Polygons = 0;
	AActor* Lanes = MakeLanes(World, Id, Label, Net, FZoneLaneProfileRef(*Profile), CrowdTag, CrossingTag, Polygons);
	if (!Lanes)
	{
		Transaction.Cancel();
		OutError = TEXT("setMassCrowd: could not spawn the lanes actor");
		return nullptr;
	}
	UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild.Broadcast();

	UStaticMesh* Mesh = nullptr;
	FString MeshPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
	Params->TryGetStringField(TEXT("mesh"), MeshPath);
	if (!MeshPath.IsEmpty())
	{
		Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
		if (!Mesh)
		{
			Warnings.Add(FString::Printf(TEXT("no static mesh at %s; the agents walk unseen"), *MeshPath));
		}
	}
	TArray<FGroup> Groups;
	for (const TSharedPtr<FJsonValue>& Value : ArrayOf(Params, TEXT("groups")))
	{
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		FGroup& G = Groups.AddDefaulted_GetRef();
		G.Name = Obj->GetStringField(TEXT("name"));
		Obj->TryGetNumberField(TEXT("share"), G.Share);
		Obj->TryGetNumberField(TEXT("speed"), G.Speed);
		Obj->TryGetNumberField(TEXT("speedVariance"), G.SpeedVariance);
		Obj->TryGetNumberField(TEXT("pause"), G.Pause);
		Obj->TryGetNumberField(TEXT("radius"), G.Radius);
	}
	if (Groups.IsEmpty())
	{
		Groups.Add({TEXT("walkers")});
	}
	TArray<FMassSpawnedEntityType> Types;
	TArray<TSharedPtr<FJsonValue>> Trees, Configs;
	const FString Base = AssetName(Label);
	for (const FGroup& G : Groups)
	{
		const FString Suffix = Base + TEXT("_") + AssetName(G.Name);
		UStateTree* Tree = MakeStateTree(Folder, TEXT("ST_") + Suffix, G, Warnings);
		if (!Tree)
		{
			continue;
		}
		UMassEntityConfigAsset* Config = MakeConfig(Folder, TEXT("CE_") + Suffix, G, Tree, Mesh);
		FMassSpawnedEntityType& Type = Types.AddDefaulted_GetRef();
		Type.EntityConfig = Config;
		Type.Proportion = static_cast<float>(FMath::Max(G.Share, 0.0));
		Trees.Add(MakeShared<FJsonValueString>(Tree->GetPathName()));
		Configs.Add(MakeShared<FJsonValueString>(Config->GetPathName()));
	}
	if (Types.IsEmpty())
	{
		Transaction.Cancel();
		OutError = TEXT("setMassCrowd: no group's StateTree compiled") + (Warnings.IsEmpty() ? FString() : TEXT(": ") + Warnings.Last());
		return nullptr;
	}

	const FName Tag = ActorTagFor(Id);
	AMassSpawner* Spawner = Cast<AMassSpawner>(TaggedActor(World, Tag, AMassSpawner::StaticClass()));
	if (!Spawner)
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		Spawner = World->SpawnActor<AMassSpawner>(AMassSpawner::StaticClass(), FTransform(Net.Lanes[0].Points[0]), Spawn);
		if (!Spawner)
		{
			Transaction.Cancel();
			OutError = TEXT("setMassCrowd: could not spawn the Mass spawner");
			return nullptr;
		}
		Spawner->Tags.Add(Tag);
		Spawner->Tags.Add(LilaTag);
	}
	Spawner->Modify();
	Spawner->SetActorLabel(Label + TEXT(" Mass Crowd"));
	Spawner->SetFolderPath(FName(TEXT("Lila/Crowd")));
	if (TArray<FMassSpawnedEntityType>* EntityTypes = Field<TArray<FMassSpawnedEntityType>>(Spawner, TEXT("EntityTypes")))
	{
		*EntityTypes = Types;
	}
	double Count = 100.0, MinGap = 300.0, MaxGap = 600.0;
	Params->TryGetNumberField(TEXT("count"), Count);
	Params->TryGetNumberField(TEXT("minGap"), MinGap);
	Params->TryGetNumberField(TEXT("maxGap"), MaxGap);
	if (int32* C = Field<int32>(Spawner, TEXT("Count")))
	{
		*C = FMath::Max(0, FMath::RoundToInt(Count));
	}
	UMassEntityZoneGraphSpawnPointsGenerator* Generator = NewObject<UMassEntityZoneGraphSpawnPointsGenerator>(Spawner);
	if (FZoneGraphTagFilter* Filter = Field<FZoneGraphTagFilter>(Generator, TEXT("TagFilter")))
	{
		Filter->AnyTags = FZoneGraphTagMask(CrowdTag);
		// Agents start on the walkways; they reach the crossings by walking.
		Filter->NotTags = FZoneGraphTagMask(CrossingTag);
	}
	if (float* G = Field<float>(Generator, TEXT("MinGap")))
	{
		*G = static_cast<float>(MinGap);
	}
	if (float* G = Field<float>(Generator, TEXT("MaxGap")))
	{
		*G = static_cast<float>(FMath::Max(MinGap, MaxGap));
	}
	if (TArray<FMassSpawnDataGenerator>* Generators = Field<TArray<FMassSpawnDataGenerator>>(Spawner, TEXT("SpawnDataGenerators")))
	{
		Generators->Reset();
		FMassSpawnDataGenerator& Entry = Generators->AddDefaulted_GetRef();
		Entry.GeneratorInstance = Generator;
		Entry.Proportion = 1.0f;
	}
	Spawner->MarkPackageDirty();
	UE_LOG(LogLilaLinkCrowd, Log, TEXT("setMassCrowd: %s (%d lanes, %d nodes, %d groups, %d agents)"), *Label, Net.Lanes.Num(), Polygons, Types.Num(),
		   FMath::RoundToInt(Count));

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Lanes->GetPathName());
	Out->SetNumberField(TEXT("lanes"), Net.Lanes.Num());
	Out->SetNumberField(TEXT("nodes"), Polygons);
	Out->SetStringField(TEXT("spawner"), Spawner->GetPathName());
	Out->SetArrayField(TEXT("stateTrees"), Trees);
	Out->SetArrayField(TEXT("configs"), Configs);
	TArray<TSharedPtr<FJsonValue>> W;
	for (const FString& Line : Warnings)
	{
		W.Add(MakeShared<FJsonValueString>(Line));
	}
	Out->SetArrayField(TEXT("warnings"), W);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> SampleMassCrowd(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::SampleMassCrowd);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
	Out->SetBoolField(TEXT("playing"), World != nullptr);
	TArray<TSharedPtr<FJsonValue>> Agents;
	if (!World)
	{
		Out->SetArrayField(TEXT("agents"), Agents);
		return MakeShared<FJsonValueObject>(Out);
	}
	double Radius = 100.0;
	Params->TryGetNumberField(TEXT("radius"), Radius);
	FZoneSettings Zone;
	TArray<FString> Ignored;
	const FZoneGraphTag CrowdTag = GetDefault<UMassCrowdSettings>()->CrowdTag;
	FZoneGraphTagFilter CrowdFilter, VehicleFilter;
	CrowdFilter.AnyTags = FZoneGraphTagMask(CrowdTag);
	VehicleFilter.AnyTags = FZoneGraphTagMask(Zone.Tag(TEXT("Vehicle"), Ignored));
	const UZoneGraphSubsystem* Lanes = World->GetSubsystem<UZoneGraphSubsystem>();

	FMassEntityManager& Manager = UE::Mass::Utils::GetEntityManagerChecked(*World);
	FMassEntityQuery Query(Manager.AsShared());
	Query.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	Query.AddRequirement<FMassZoneGraphLaneLocationFragment>(EMassFragmentAccess::ReadOnly);
	Query.AddTagRequirement<FMassCrowdTag>(EMassFragmentPresence::All);
	FMassExecutionContext Context(Manager);
	auto Nearest = [&](const FVector& P, const FZoneGraphTagFilter& Filter) {
		FZoneGraphLaneLocation Location;
		float DistSqr = 0.0f;
		return Lanes && Lanes->FindNearestLane(FBox(P - FVector(Radius), P + FVector(Radius)), Filter, Location, DistSqr) ? FMath::Sqrt(DistSqr) : -1.0f;
	};
	Query.ForEachEntityChunk(Context, [&](FMassExecutionContext& Chunk) {
		const TConstArrayView<FTransformFragment> Transforms = Chunk.GetFragmentView<FTransformFragment>();
		const TConstArrayView<FMassZoneGraphLaneLocationFragment> LaneLocations = Chunk.GetFragmentView<FMassZoneGraphLaneLocationFragment>();
		for (int32 I = 0; I < Chunk.GetNumEntities(); ++I)
		{
			const FVector P = Transforms[I].GetTransform().GetLocation();
			TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetNumberField(TEXT("id"), Chunk.GetEntity(I).Index);
			A->SetField(TEXT("p"), PointValue(P));
			const FZoneGraphLaneHandle Lane = LaneLocations[I].LaneHandle;
			A->SetNumberField(TEXT("lane"), Lane.IsValid() ? Lane.Index : -1);
			TArray<TSharedPtr<FJsonValue>> Names;
			FZoneGraphTagMask Tags;
			if (Lanes && Lane.IsValid() && Lanes->GetLaneTags(Lane, Tags))
			{
				for (uint8 Bit = 0; Bit < 32; ++Bit)
				{
					if (Tags.Contains(FZoneGraphTag(Bit)))
					{
						if (const FName Name = Zone.NameOf(FZoneGraphTag(Bit)); !Name.IsNone())
						{
							Names.Add(MakeShared<FJsonValueString>(Name.ToString()));
						}
					}
				}
			}
			A->SetArrayField(TEXT("tags"), Names);
			A->SetNumberField(TEXT("crowd"), Nearest(P, CrowdFilter));
			A->SetNumberField(TEXT("vehicle"), Nearest(P, VehicleFilter));
			Agents.Add(MakeShared<FJsonValueObject>(A));
		}
	});
	Out->SetArrayField(TEXT("agents"), Agents);
	int32 Instances = 0;
	for (TActorIterator<AMassVisualizer> It(World); It; ++It)
	{
		TArray<UInstancedStaticMeshComponent*> Components;
		It->GetComponents(Components);
		for (const UInstancedStaticMeshComponent* C : Components)
		{
			Instances += C->GetInstanceCount();
		}
	}
	Out->SetNumberField(TEXT("instances"), Instances);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveMassCrowd(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::RemoveMassCrowd);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("removeMassCrowd: no editor world");
		return nullptr;
	}
	const FName Tag = ActorTagFor(Params->GetStringField(TEXT("id")));
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveMassCrowd", "Lila: Remove Mass Crowd"));
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Doomed.Add(*It);
		}
	}
	for (AActor* Actor : Doomed)
	{
		World->EditorDestroyActor(Actor, true);
	}
	UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild.Broadcast();
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), Doomed.Num());
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLinkCrowd::RegisterMethods()
{
	LilaLink::RegisterMethod(TEXT("setMassCrowd"), &SetMassCrowd, Feature);
	LilaLink::RegisterMethod(TEXT("sampleMassCrowd"), &SampleMassCrowd, Feature);
	LilaLink::RegisterMethod(TEXT("removeMassCrowd"), &RemoveMassCrowd, Feature);
}
