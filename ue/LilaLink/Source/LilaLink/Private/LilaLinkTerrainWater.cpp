#include "LilaLinkTerrainWater.h"

#include "LilaLinkLog.h"
#include "LilaWaterData.h"

#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
/** On every actor this file makes, so third-party water setups can find them by tag. */
const FName WaterTag(TEXT("LilaWater"));

/** The tag shared by every actor of one terrain's water. */
FName TerrainTag(const FString& Id)
{
	return FName(*FString::Printf(TEXT("LilaLink.Water:%08X"), FCrc::StrCrc32(*Id)));
}

FName KeyTag(const FString& Key)
{
	return FName(*(TEXT("LilaLink.WaterKey:") + Key.Right(900)));
}

FString KeyOf(const AActor* Actor)
{
	const FString Prefix = TEXT("LilaLink.WaterKey:");
	for (const FName& Tag : Actor->Tags)
	{
		const FString S = Tag.ToString();
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return S.RightChop(Prefix.Len());
		}
	}
	return FString();
}

UWorld* WaterEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool ReadWaterVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

float Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Fallback = 0.0)
{
	double V = Fallback;
	Object->TryGetNumberField(Field, V);
	return float(V);
}

/** Every field that isn't geometry or per-point data, as text, for ULilaWaterData::Values. */
void CopyValues(const TSharedPtr<FJsonObject>& Object, ULilaWaterData* Data)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::CopyValues);

	Data->Values.Reset();
	for (const auto& [Name, Value] : Object->Values)
	{
		if (Name == TEXT("points") || Name == TEXT("position") || Name == TEXT("id") || Name == TEXT("kind") || Name == TEXT("feature") || !Value.IsValid())
		{
			continue;
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			Data->Values.Add(FName(*Name), Text);
		}
		else if (Value->Type == EJson::Number)
		{
			Data->Values.Add(FName(*Name), FString::SanitizeFloat(Value->AsNumber()));
		}
		else if (Value->Type == EJson::Boolean)
		{
			Data->Values.Add(FName(*Name), Value->AsBool() ? TEXT("true") : TEXT("false"));
		}
		else if (Value->Type == EJson::Array)
		{
			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& Part : Value->AsArray())
			{
				Parts.Add(Part->Type == EJson::Number ? FString::SanitizeFloat(Part->AsNumber()) : Part->AsString());
			}
			Data->Values.Add(FName(*Name), FString::Join(Parts, TEXT(",")));
		}
	}
}

ULilaWaterData* EnsureData(AActor* Actor)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::EnsureData);

	if (ULilaWaterData* Data = ULilaWaterData::FindOn(Actor))
	{
		Data->Modify();
		return Data;
	}
	ULilaWaterData* Data = NewObject<ULilaWaterData>(Actor, TEXT("LilaWaterData"), RF_Transactional);
	Actor->AddInstanceComponent(Data);
	Data->RegisterComponent();
	return Data;
}

/** The actor for one water feature: the existing one when it has the right kind of root, else a new one. */
AActor* EnsureActor(UWorld* World, AActor* Existing, bool bSpline, const FVector& Location, const FName& Terrain, const FString& Key, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::EnsureActor);

	if (Existing)
	{
		const bool bHasSpline = Cast<USplineComponent>(Existing->GetRootComponent()) != nullptr;
		if (bHasSpline == bSpline)
		{
			Existing->Modify();
			Existing->SetActorLocation(Location);
			return Existing;
		}
		World->EditorDestroyActor(Existing, true);
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags |= RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), Spawn);
	if (!Actor)
	{
		OutError = TEXT("setWater: could not spawn an actor");
		return nullptr;
	}
	USceneComponent* Root = bSpline ? NewObject<USplineComponent>(Actor, TEXT("LilaWaterSpline"), RF_Transactional)
									: NewObject<USceneComponent>(Actor, TEXT("LilaWaterLocator"), RF_Transactional);
	Root->SetMobility(EComponentMobility::Static);
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorLocation(Location);
	Actor->Tags.Add(WaterTag);
	Actor->Tags.Add(Terrain);
	Actor->Tags.Add(KeyTag(Key));
	return Actor;
}

/** Fills a river or shoreline: spline points relative to the actor, and the per-point data by index. */
bool FillWaterSpline(AActor* Actor, const TSharedPtr<FJsonObject>& Spline, ULilaWaterData* Data, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::FillWaterSpline);

	USplineComponent* Component = Cast<USplineComponent>(Actor->GetRootComponent());
	const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
	if (!Component || !Spline->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
	{
		OutError = TEXT("setWater: a spline needs at least two points");
		return false;
	}
	bool bClosed = false;
	Spline->TryGetBoolField(TEXT("closed"), bClosed);
	const FVector Origin = Actor->GetActorLocation();
	Component->Modify();
	Component->ClearSplinePoints(false);
	Data->Points.Reset();
	double LevelSum = 0.0;
	for (const TSharedPtr<FJsonValue>& Value : *Points)
	{
		const TSharedPtr<FJsonObject>* Point = nullptr;
		FVector Position;
		if (!Value.IsValid() || !Value->TryGetObject(Point) || !ReadWaterVector(*Point, TEXT("position"), Position))
		{
			OutError = TEXT("setWater: a spline point has no position");
			return false;
		}
		Component->AddSplinePoint(Position - Origin, ESplineCoordinateSpace::Local, false);
		FLilaWaterPoint& P = Data->Points.AddDefaulted_GetRef();
		P.Width = Number(*Point, TEXT("width"));
		P.Depth = Number(*Point, TEXT("depth"));
		P.WaterLevel = Number(*Point, TEXT("waterLevel"), Position.Z);
		P.BedHeight = Number(*Point, TEXT("bedHeight"), P.WaterLevel - P.Depth);
		P.FlowSpeed = Number(*Point, TEXT("flowSpeed"));
		P.Discharge = Number(*Point, TEXT("discharge"));
		P.Distance = Number(*Point, TEXT("distance"));
		FVector Direction;
		if (ReadWaterVector(*Point, TEXT("flowDirection"), Direction))
		{
			P.FlowDirection = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		}
		LevelSum += P.WaterLevel;
	}
	Component->SetClosedLoop(bClosed, false);
	// Width and depth ride on the point scale too (Y width, Z depth, in metres), for tools that size meshes by scale.
	for (int32 Index = 0; Index < Data->Points.Num(); ++Index)
	{
		const FLilaWaterPoint& P = Data->Points[Index];
		if (P.Width > 0.0f)
		{
			Component->SetScaleAtSplinePoint(Index, FVector(1.0, P.Width * 0.01, FMath::Max(0.01f, P.Depth * 0.01f)), false);
		}
	}
	Component->UpdateSpline();
	Data->WaterLevel = Data->Points.IsEmpty() ? 0.0f : float(LevelSum / Data->Points.Num());
	return true;
}

TSharedPtr<FJsonValue> SetWater(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::SetWater);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setWater: give id");
		return nullptr;
	}
	UWorld* World = WaterEditorWorld();
	if (!World)
	{
		OutError = TEXT("setWater: no editor world");
		return nullptr;
	}
	FString Label = TEXT("Terrain");
	Params->TryGetStringField(TEXT("label"), Label);
	FString Folder = TEXT("Lila/Water/") + Label;
	Params->TryGetStringField(TEXT("folder"), Folder);
	const FName Terrain = TerrainTag(Id);

	TMap<FString, AActor*> Existing;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Terrain))
		{
			Existing.Add(KeyOf(*It), *It);
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetWater", "Lila: Update Terrain Water"));
	TSet<FString> Sent;
	int32 Created = 0, Updated = 0;
	auto Place = [&](const TSharedPtr<FJsonObject>& Feature, bool bSpline) -> bool {
		FString FeatureId, Kind, Source;
		Feature->TryGetStringField(TEXT("id"), FeatureId);
		Feature->TryGetStringField(TEXT("kind"), Kind);
		Feature->TryGetStringField(TEXT("feature"), Source);
		const FString Key = (bSpline ? TEXT("spline:") : TEXT("locator:")) + FeatureId;
		if (FeatureId.IsEmpty() || Sent.Contains(Key))
		{
			OutError = FString::Printf(TEXT("setWater: every spline and locator needs its own id (%s)"), *FeatureId);
			return false;
		}
		Sent.Add(Key);
		FVector Location = FVector::ZeroVector;
		if (bSpline)
		{
			const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
			if (Feature->TryGetArrayField(TEXT("points"), Points) && !Points->IsEmpty() && (*Points)[0]->Type == EJson::Object)
			{
				ReadWaterVector((*Points)[0]->AsObject(), TEXT("position"), Location);
			}
		}
		else if (!ReadWaterVector(Feature, TEXT("position"), Location))
		{
			OutError = FString::Printf(TEXT("setWater: locator %s has no position"), *FeatureId);
			return false;
		}
		AActor** Found = Existing.Find(Key);
		AActor* Actor = EnsureActor(World, Found ? *Found : nullptr, bSpline, Location, Terrain, Key, OutError);
		if (!Actor)
		{
			return false;
		}
		(Found && *Found == Actor ? Updated : Created)++;
		Actor->SetActorLabel(Label + TEXT("_") + FeatureId);
		Actor->SetFolderPath(FName(*Folder));
		const FName KindTag(*(TEXT("LilaWater.") + Kind));
		Actor->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("LilaWater."), ESearchCase::IgnoreCase); });
		Actor->Tags.Add(KindTag);
		ULilaWaterData* Data = EnsureData(Actor);
		Data->Kind = FName(*Kind);
		Data->FeatureId = FeatureId;
		Data->Feature = Source.IsEmpty() ? FeatureId : Source;
		CopyValues(Feature, Data);
		if (bSpline)
		{
			return FillWaterSpline(Actor, Feature, Data, OutError);
		}
		Data->Points.Reset();
		Data->WaterLevel = Number(Feature, TEXT("level"), Location.Z);
		Actor->MarkPackageDirty();
		return true;
	};

	for (const TCHAR* Field : {TEXT("splines"), TEXT("locators")})
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Params->TryGetArrayField(Field, List))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Value : *List)
		{
			const TSharedPtr<FJsonObject>* Feature = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Feature) || !Place(*Feature, FCString::Strcmp(Field, TEXT("splines")) == 0))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("setWater: an entry is not an object");
				}
				Transaction.Cancel();
				return nullptr;
			}
		}
	}

	int32 Removed = 0;
	for (const auto& [Key, Actor] : Existing)
	{
		if (!Sent.Contains(Key) && IsValid(Actor))
		{
			Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
		}
	}
	UE_LOG(LogLilaLink, Log, TEXT("setWater: %s (%d created, %d updated, %d removed)"), *Label, Created, Updated, Removed);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("actors"), Sent.Num());
	Out->SetNumberField(TEXT("created"), Created);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("removed"), Removed);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveWater(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainWater::RemoveWater);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeWater: give id");
		return nullptr;
	}
	UWorld* World = WaterEditorWorld();
	if (!World)
	{
		OutError = TEXT("removeWater: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveWater", "Lila: Remove Terrain Water"));
	const FName Terrain = TerrainTag(Id);
	TArray<AActor*> Ours;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Terrain))
		{
			Ours.Add(*It);
		}
	}
	int32 Removed = 0;
	for (AActor* Actor : Ours)
	{
		Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), Removed);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddTerrainWaterMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setWater"), LilaLink::Sync(&SetWater));
	Methods.Add(TEXT("removeWater"), LilaLink::Sync(&RemoveWater));
}
