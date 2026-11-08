#include "LilaLinkSettlement.h"

#include "LilaLinkMethodRegistry.h"

#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

DEFINE_LOG_CATEGORY_STATIC(LogLilaLinkSettlementCells, Log, All);

namespace
{
const FName CellsLilaTag(TEXT("LilaLink"));
const FName SettlementDataParameter(TEXT("SettlementData"));

FName CellTag(const FString& Id, const FString& Cell)
{
	return FName(*(TEXT("LilaLink.SettlementCell:") + (Id + TEXT("/") + Cell).Right(900)));
}

FString CellPrefix(const FString& Id)
{
	return TEXT("LilaLink.SettlementCell:") + (Id + TEXT("/")).Right(900);
}

UWorld* CellsEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FVector ReadVector(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(Field, Values) || Values->Num() < 3)
	{
		return FVector::ZeroVector;
	}
	return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
}

/** Points the cell's graph at its data asset through the graph's SettlementData parameter, whichever object type it has. */
bool SetDataParameter(UPCGComponent* Component, const FString& AssetPath)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementCells::SetDataParameter);

	UPCGGraphInstance* Instance = Component ? Component->GetGraphInstance() : nullptr;
	if (!Instance)
	{
		return false;
	}
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	const FSoftObjectPath Path(ObjectPath);
	if (Instance->SetGraphParameter<TSoftObjectPtr<UObject>>(SettlementDataParameter, TSoftObjectPtr<UObject>(Path)) == EPropertyBagResult::Success)
	{
		return true;
	}
	if (Instance->SetGraphParameter<FSoftObjectPath>(SettlementDataParameter, Path) == EPropertyBagResult::Success)
	{
		return true;
	}
	UObject* Loaded = Path.TryLoad();
	return Loaded && Instance->SetGraphParameter<UObject*>(SettlementDataParameter, Loaded) == EPropertyBagResult::Success;
}

TSharedPtr<FJsonValue> SetSettlementCell(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementCells::SetSettlementCell);

	FString Id, Cell, AssetPath, Label, GraphPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || !Params->TryGetStringField(TEXT("cell"), Cell) || !Params->TryGetStringField(TEXT("asset"), AssetPath))
	{
		OutError = TEXT("setSettlementCell: give id, cell and asset");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("graph"), GraphPath);
	UWorld* World = CellsEditorWorld();
	if (!World)
	{
		OutError = TEXT("setSettlementCell: no editor world");
		return nullptr;
	}
	const TSharedPtr<FJsonValue> Data = LilaLinkSettlement::WriteSettlementData(Params, OutError);
	if (!Data.IsValid())
	{
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetSettlementCell", "Lila: Update Settlement Cell"));
	const FName Tag = CellTag(Id, Cell);
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It && !Actor; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Actor = *It;
		}
	}
	const FVector Center = ReadVector(Params, TEXT("center"));
	const double Size = Params->HasField(TEXT("size")) ? Params->GetNumberField(TEXT("size")) : 12800.0;
	bool bSpawned = false;
	if (!Actor)
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Center), Spawn);
		if (!Actor)
		{
			OutError = TEXT("setSettlementCell: could not spawn the cell actor");
			return nullptr;
		}
		// The box gives the actor (and its PCG component) the cell's bounds, so World Partition and PCG see the cell.
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, TEXT("Cell"), RF_Transactional);
		Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Box->SetHiddenInGame(true);
		Actor->SetRootComponent(Box);
		Actor->AddInstanceComponent(Box);
		Box->RegisterComponent();
		UPCGComponent* Component = NewObject<UPCGComponent>(Actor, TEXT("PCG"), RF_Transactional);
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		Actor->Tags.Add(CellsLilaTag);
		Actor->Tags.Add(Tag);
		Actor->SetFolderPath(FName(TEXT("Lila/Settlement")));
		bSpawned = true;
	}
	Actor->Modify();
	Actor->SetActorLocation(Center);
	if (UBoxComponent* Box = Actor->FindComponentByClass<UBoxComponent>())
	{
		Box->Modify();
		Box->SetBoxExtent(FVector(0.5 * Size, 0.5 * Size, FMath::Max(0.5 * Size, 50000.0)));
	}
	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	bool bGenerated = false;
	UPCGComponent* Component = Actor->FindComponentByClass<UPCGComponent>();
	if (Component && !GraphPath.IsEmpty())
	{
		UPCGGraphInterface* Graph = LoadObject<UPCGGraphInterface>(nullptr, *GraphPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Graph && !GraphPath.Contains(TEXT(".")))
		{
			Graph = LoadObject<UPCGGraphInterface>(nullptr, *(GraphPath + TEXT(".") + FPackageName::GetShortName(GraphPath)), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Graph)
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("no PCG graph at %s; the cell has its data but no graph"), *GraphPath)));
		}
		else
		{
			Component->Modify();
			if (Component->GetGraph() != Graph)
			{
				Component->SetGraphLocal(Graph);
			}
			if (!SetDataParameter(Component, AssetPath))
			{
				Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("%s has no SettlementData parameter (an object or soft object); add one and load the cell's data from it"), *GraphPath)));
			}
			Component->GenerateLocal(true);
			bGenerated = true;
		}
	}
	UE_LOG(LogLilaLinkSettlementCells, Log, TEXT("setSettlementCell: %s %s (%s)"), *Id, *Cell, bSpawned ? TEXT("new") : TEXT("updated"));

	TSharedRef<FJsonObject> Out = Data->AsObject().ToSharedRef();
	Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Out->SetBoolField(TEXT("spawned"), bSpawned);
	Out->SetBoolField(TEXT("generated"), bGenerated);
	Out->SetArrayField(TEXT("warnings"), Warnings);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveSettlementCells(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementCells::RemoveSettlementCells);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeSettlementCells: give id");
		return nullptr;
	}
	UWorld* World = CellsEditorWorld();
	if (!World)
	{
		OutError = TEXT("removeSettlementCells: no editor world");
		return nullptr;
	}
	TSet<FName> Tags;
	const TArray<TSharedPtr<FJsonValue>>* Cells = nullptr;
	const bool bAll = !Params->TryGetArrayField(TEXT("cells"), Cells);
	if (!bAll)
	{
		for (const TSharedPtr<FJsonValue>& V : *Cells)
		{
			Tags.Add(CellTag(Id, V->AsString()));
		}
	}
	const FString Prefix = CellPrefix(Id);
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveSettlementCells", "Lila: Remove Settlement Cells"));
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		for (const FName& Tag : It->Tags)
		{
			if (bAll ? Tag.ToString().StartsWith(Prefix, ESearchCase::IgnoreCase) : Tags.Contains(Tag))
			{
				Doomed.Add(*It);
				break;
			}
		}
	}
	int32 Removed = 0;
	for (AActor* Actor : Doomed)
	{
		Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), Removed);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLinkSettlement::RegisterCellMethods()
{
	LilaLink::RegisterMethod(TEXT("setSettlementCell"), &SetSettlementCell, CellsFeature);
	LilaLink::RegisterMethod(TEXT("removeSettlementCells"), &RemoveSettlementCells, CellsFeature);
}
