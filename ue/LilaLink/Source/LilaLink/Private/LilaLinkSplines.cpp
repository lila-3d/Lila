#include "LilaLinkSplines.h"

#include "LilaLinkLog.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
/** Marks the components and actors this file makes, so a re-send replaces only those. */
const FName LilaTag(TEXT("LilaLink"));

struct FLilaSplinePoint
{
	FVector Position = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	FVector Up = FVector::UpVector;
	FVector Scale = FVector::OneVector;
	double Roll = 0.0;
};

struct FLilaSpline
{
	bool bClosed = false;
	TArray<FLilaSplinePoint> Points;
};

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Spline:") + Id.Right(900)));
}

/** Names of the landscape spline objects made for an id, so they can be found again in a shared splines component. */
FString LandscapePrefix(const FString& Id)
{
	return FString::Printf(TEXT("LilaLink_%08X_"), FCrc::StrCrc32(*Id));
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

bool ParseSplines(const TSharedPtr<FJsonObject>& Params, TArray<FLilaSpline>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Splines = nullptr;
	if (!Params->TryGetArrayField(TEXT("splines"), Splines) || Splines->IsEmpty())
	{
		OutError = TEXT("setSpline: give splines");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Splines)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = TEXT("setSpline: each spline needs at least two points");
			return false;
		}
		FLilaSpline& Spline = Out.AddDefaulted_GetRef();
		(*Object)->TryGetBoolField(TEXT("closed"), Spline.bClosed);
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* PointObject = nullptr;
			if (!PointValue.IsValid() || !PointValue->TryGetObject(PointObject))
			{
				OutError = TEXT("setSpline: a point is not an object");
				return false;
			}
			FLilaSplinePoint& Point = Spline.Points.AddDefaulted_GetRef();
			if (!ReadVector(*PointObject, TEXT("position"), Point.Position))
			{
				OutError = TEXT("setSpline: a point has no position");
				return false;
			}
			ReadVector(*PointObject, TEXT("arriveTangent"), Point.ArriveTangent);
			ReadVector(*PointObject, TEXT("leaveTangent"), Point.LeaveTangent);
			ReadVector(*PointObject, TEXT("up"), Point.Up);
			ReadVector(*PointObject, TEXT("scale"), Point.Scale);
			(*PointObject)->TryGetNumberField(TEXT("roll"), Point.Roll);
		}
	}
	return true;
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FindTagged(UWorld* World, FName Tag)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

ALandscape* FindLandscape(UWorld* World, const FString& Label)
{
	ALandscape* First = nullptr;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (Label.IsEmpty() || It->GetActorLabel() == Label || It->GetName() == Label)
		{
			return *It;
		}
		First = First ? First : *It;
	}
	return Label.IsEmpty() ? First : nullptr;
}

ESplineMeshAxis::Type AxisFrom(const FString& Name)
{
	return Name.Equals(TEXT("y"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Y
		: Name.Equals(TEXT("z"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Z
		: ESplineMeshAxis::X;
}

/** Replaces the component's points: positions first, then the tangents they need, then the up vectors that turn on them. */
void FillSpline(USplineComponent* Component, const FLilaSpline& Spline)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::FillSpline);

	Component->ClearSplinePoints(false);
	for (const FLilaSplinePoint& Point : Spline.Points)
	{
		Component->AddSplinePoint(Point.Position, ESplineCoordinateSpace::Local, false);
	}
	for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
	{
		const FLilaSplinePoint& Point = Spline.Points[Index];
		Component->SetTangentsAtSplinePoint(Index, Point.ArriveTangent, Point.LeaveTangent, ESplineCoordinateSpace::Local, false);
		Component->SetScaleAtSplinePoint(Index, Point.Scale, false);
	}
	Component->SetClosedLoop(Spline.bClosed, false);
	Component->UpdateSpline();
	for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
	{
		Component->SetUpVectorAtSplinePoint(Index, Spline.Points[Index].Up, ESplineCoordinateSpace::Local, false);
	}
	Component->UpdateSpline();
}

/** A spline mesh per segment of the spline, attached to it, in its space. Returns how many. */
int32 AddSplineMeshes(AActor* Actor, USplineComponent* Spline, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::AddSplineMeshes);

	const int32 Points = Spline->GetNumberOfSplinePoints();
	const int32 Segments = Spline->GetNumberOfSplineSegments();
	for (int32 Segment = 0; Segment < Segments; ++Segment)
	{
		const int32 Next = (Segment + 1) % Points;
		const FName Name = MakeUniqueObjectName(Actor, USplineMeshComponent::StaticClass(), *FString::Printf(TEXT("LilaSplineMesh%d"), Segment));
		USplineMeshComponent* Piece = NewObject<USplineMeshComponent>(Actor, Name, RF_Transactional);
		Piece->ComponentTags.Add(LilaTag);
		Piece->SetMobility(Spline->Mobility);
		Piece->SetupAttachment(Spline);
		Piece->SetStaticMesh(Mesh);
		Piece->SetForwardAxis(Axis, false);
		Piece->SetStartAndEnd(Spline->GetLocationAtSplinePoint(Segment, ESplineCoordinateSpace::Local),
			Spline->GetLeaveTangentAtSplinePoint(Segment, ESplineCoordinateSpace::Local),
			Spline->GetLocationAtSplinePoint(Next, ESplineCoordinateSpace::Local),
			Spline->GetArriveTangentAtSplinePoint(Next, ESplineCoordinateSpace::Local), false);
		const FVector StartScale = Spline->GetScaleAtSplinePoint(Segment), EndScale = Spline->GetScaleAtSplinePoint(Next);
		Piece->SetStartScale(FVector2D(StartScale.Y, StartScale.Z), false);
		Piece->SetEndScale(FVector2D(EndScale.Y, EndScale.Z), false);
		Piece->SetStartRollDegrees(Spline->GetRollAtSplinePoint(Segment, ESplineCoordinateSpace::Local), false);
		Piece->SetEndRollDegrees(Spline->GetRollAtSplinePoint(Next, ESplineCoordinateSpace::Local), false);
		Actor->AddInstanceComponent(Piece);
		Piece->RegisterComponent();
		Piece->UpdateMesh();
	}
	return Segments;
}

void RemoveOurComponents(AActor* Actor, const TArray<UActorComponent*>& Components)
{
	for (UActorComponent* Component : Components)
	{
		Component->Modify();
		Actor->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
	}
}

TSharedPtr<FJsonValue> SetActorSplines(UWorld* World, const FString& Id, const FString& Label, const FString& Folder, const FVector& Location,
	const TArray<FLilaSpline>& Splines, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetActorSplines);

	const FName Tag = TagFor(Id);
	AActor* Actor = FindTagged(World, Tag);
	if (Actor && Actor->IsA<ALandscapeSplineActor>())
	{
		// The curve was a landscape spline before: start over as a plain actor.
		World->EditorDestroyActor(Actor, true);
		Actor = nullptr;
	}
	if (Actor)
	{
		Actor->Modify();
		Actor->SetActorLocation(Location);
	}
	else
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), Spawn);
		if (!Actor)
		{
			OutError = TEXT("setSpline: could not spawn the spline actor");
			return nullptr;
		}
		Actor->Tags.Add(Tag);
		Actor->Tags.Add(LilaTag);
	}
	Actor->SetActorLabel(Label);
	Actor->SetFolderPath(FName(*Folder));

	TArray<USplineComponent*> AllSplines;
	Actor->GetComponents(AllSplines);
	TArray<USplineComponent*> Ours;
	for (USplineComponent* Component : AllSplines)
	{
		if (Component->ComponentHasTag(LilaTag))
		{
			Ours.Add(Component);
		}
	}
	TArray<USplineMeshComponent*> AllMeshes;
	Actor->GetComponents(AllMeshes);
	TArray<UActorComponent*> Stale;
	for (USplineMeshComponent* Component : AllMeshes)
	{
		if (Component->ComponentHasTag(LilaTag))
		{
			Stale.Add(Component);
		}
	}
	for (int32 Index = Splines.Num(); Index < Ours.Num(); ++Index)
	{
		// The first one is the root, and there is always at least one spline.
		Stale.Add(Ours[Index]);
	}
	RemoveOurComponents(Actor, Stale);
	Ours.SetNum(FMath::Min(Ours.Num(), Splines.Num()));

	int32 MeshCount = 0;
	for (int32 Index = 0; Index < Splines.Num(); ++Index)
	{
		USplineComponent* Component = Index < Ours.Num() ? Ours[Index] : nullptr;
		if (!Component)
		{
			const FName Name = MakeUniqueObjectName(Actor, USplineComponent::StaticClass(), *FString::Printf(TEXT("LilaSpline%d"), Index));
			Component = NewObject<USplineComponent>(Actor, Name, RF_Transactional);
			Component->ComponentTags.Add(LilaTag);
			if (USceneComponent* Root = Actor->GetRootComponent())
			{
				Component->SetupAttachment(Root);
			}
			else
			{
				Actor->SetRootComponent(Component);
			}
			Actor->AddInstanceComponent(Component);
			Component->RegisterComponent();
		}
		Component->Modify();
		FillSpline(Component, Splines[Index]);
		if (Mesh)
		{
			MeshCount += AddSplineMeshes(Actor, Component, Mesh, Axis);
		}
	}
	if (Actor->GetRootComponent())
	{
		Actor->SetActorLocation(Location);
	}
	Actor->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Actor->GetPathName());
	Out->SetNumberField(TEXT("components"), Splines.Num());
	Out->SetNumberField(TEXT("segments"), MeshCount);
	return MakeShared<FJsonValueObject>(Out);
}

/** Deletes the control points and segments made for an id from a splines component (all of them when Prefix is empty). */
void ClearLandscapeSplines(ULandscapeSplinesComponent* Splines, const FString& Prefix)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::ClearLandscapeSplines);

	Splines->Modify();
	auto Ours = [&Prefix](const UObject* Object) { return Object && (Prefix.IsEmpty() || Object->GetName().StartsWith(Prefix)); };
	const TArray<TObjectPtr<ULandscapeSplineSegment>> Segments = Splines->GetSegments();
	for (ULandscapeSplineSegment* Segment : Segments)
	{
		if (!Ours(Segment))
		{
			continue;
		}
		Segment->Modify();
		Segment->DeleteSplinePoints();
		for (int32 End = 0; End < 2; ++End)
		{
			if (ULandscapeSplineControlPoint* Point = Segment->Connections[End].ControlPoint)
			{
				Point->Modify();
				Point->ConnectedSegments.Remove(FLandscapeSplineConnection(Segment, End));
			}
		}
		Splines->GetSegments().Remove(Segment);
	}
	const TArray<TObjectPtr<ULandscapeSplineControlPoint>> Points = Splines->GetControlPoints();
	for (ULandscapeSplineControlPoint* Point : Points)
	{
		if (Ours(Point) && Point->ConnectedSegments.IsEmpty())
		{
			Point->Modify();
			Point->DeleteSplinePoints();
			Splines->GetControlPoints().Remove(Point);
		}
	}
}

TSharedPtr<FJsonValue> SetLandscapeSplines(UWorld* World, const FString& Id, const FString& Label, const FString& LandscapeLabel, const FVector& Location,
	const TArray<FLilaSpline>& Splines, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis, double Width, bool bDeform, bool bJoin, TArray<FString>& Warnings,
	FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetLandscapeSplines);

	ALandscape* Landscape = FindLandscape(World, LandscapeLabel);
	if (!Landscape)
	{
		OutError = LandscapeLabel.IsEmpty() ? FString(TEXT("setSpline: the level has no landscape; send it as a plain spline actor instead"))
			: FString::Printf(TEXT("setSpline: no landscape labelled %s"), *LandscapeLabel);
		return nullptr;
	}
	const FName Tag = TagFor(Id);
	AActor* Tagged = FindTagged(World, Tag);
	ULandscapeSplinesComponent* Component = nullptr;
	FString Prefix = LandscapePrefix(Id);
	FString OwnerPath;
	if (World->GetWorldPartition())
	{
		// In a partitioned world each curve gets its own landscape spline actor, found again by its tag.
		ALandscapeSplineActor* SplineActor = Cast<ALandscapeSplineActor>(Tagged);
		if (Tagged && !SplineActor)
		{
			World->EditorDestroyActor(Tagged, true);
		}
		if (!SplineActor)
		{
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			SplineActor = Info ? Info->CreateSplineActor(Location) : nullptr;
			if (!SplineActor)
			{
				OutError = TEXT("setSpline: could not make a landscape spline actor");
				return nullptr;
			}
			SplineActor->Tags.Add(Tag);
			SplineActor->Tags.Add(LilaTag);
		}
		SplineActor->Modify();
		SplineActor->SetActorLabel(Label);
		Component = SplineActor->GetSplinesComponent();
		OwnerPath = SplineActor->GetPathName();
	}
	else
	{
		if (Tagged)
		{
			World->EditorDestroyActor(Tagged, true);
		}
		Landscape->Modify();
		if (!Landscape->GetSplinesComponent())
		{
			Landscape->CreateSplineComponent();
		}
		Component = Landscape->GetSplinesComponent();
		OwnerPath = Landscape->GetPathName();
	}
	if (!Component)
	{
		OutError = TEXT("setSpline: the landscape has no splines component");
		return nullptr;
	}
	ClearLandscapeSplines(Component, Prefix);

	const FTransform ToComponent = Component->GetComponentTransform();
	int32 SegmentCount = 0;
	// With join, splines ending at the same place share one control point: a road network's junctions.
	TMap<FIntVector, ULandscapeSplineControlPoint*> Shared;
	for (int32 SplineIndex = 0; SplineIndex < Splines.Num(); ++SplineIndex)
	{
		const FLilaSpline& Spline = Splines[SplineIndex];
		TArray<ULandscapeSplineControlPoint*> Points;
		for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
		{
			const FLilaSplinePoint& In = Spline.Points[Index];
			const FVector WorldPosition = Location + In.Position;
			const FIntVector Key(FMath::RoundToInt(WorldPosition.X), FMath::RoundToInt(WorldPosition.Y), FMath::RoundToInt(WorldPosition.Z));
			if (bJoin)
			{
				if (ULandscapeSplineControlPoint** Found = Shared.Find(Key))
				{
					(*Found)->Width = FMath::Max((*Found)->Width, static_cast<float>(Width * In.Scale.Y));
					Points.Add(*Found);
					continue;
				}
			}
			const FName Name(*FString::Printf(TEXT("%sP%d_%d"), *Prefix, SplineIndex, Index));
			ULandscapeSplineControlPoint* Point = NewObject<ULandscapeSplineControlPoint>(Component, Name, RF_Transactional);
			Component->GetControlPoints().Add(Point);
			Point->Location = ToComponent.InverseTransformPosition(Location + In.Position);
			const FVector Forward = ToComponent.InverseTransformVectorNoScale(In.LeaveTangent.IsNearlyZero() ? In.ArriveTangent : In.LeaveTangent);
			const FVector Up = ToComponent.InverseTransformVectorNoScale(In.Up);
			Point->Rotation = FRotationMatrix::MakeFromXZ(Forward.GetSafeNormal(), Up).Rotator();
			Point->Width = static_cast<float>(Width * In.Scale.Y);
			Point->bRaiseTerrain = bDeform;
			Point->bLowerTerrain = bDeform;
			Points.Add(Point);
			if (bJoin)
			{
				Shared.Add(Key, Point);
			}
		}
		const int32 Segments = Spline.bClosed ? Points.Num() : Points.Num() - 1;
		for (int32 Index = 0; Index < Segments; ++Index)
		{
			const int32 Next = (Index + 1) % Points.Num();
			if (Points[Index] == Points[Next])
			{
				continue;
			}
			const FName Name(*FString::Printf(TEXT("%sS%d_%d"), *Prefix, SplineIndex, Index));
			ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Component, Name, RF_Transactional);
			Component->GetSegments().Add(Segment);
			Segment->Connections[0].ControlPoint = Points[Index];
			Segment->Connections[1].ControlPoint = Points[Next];
			// The end tangent is the far point's forward times -TangentLen, so a spline running on through it takes a negative length.
			// A shared (joined) point faces the way of the spline that made it; one leaving it the other way takes the opposite sign.
			const FVector Along = Points[Next]->Location - Points[Index]->Location;
			const float StartSign = FVector::DotProduct(Points[Index]->Rotation.Vector(), Along) >= 0.0 ? 1.0f : -1.0f;
			const float EndSign = FVector::DotProduct(Points[Next]->Rotation.Vector(), Along) >= 0.0 ? 1.0f : -1.0f;
			Segment->Connections[0].TangentLen = StartSign * static_cast<float>(ToComponent.InverseTransformVector(Spline.Points[Index].LeaveTangent).Size());
			Segment->Connections[1].TangentLen = -EndSign * static_cast<float>(ToComponent.InverseTransformVector(Spline.Points[Next].ArriveTangent).Size());
			Segment->bRaiseTerrain = bDeform;
			Segment->bLowerTerrain = bDeform;
			if (Mesh)
			{
				FLandscapeSplineMeshEntry Entry;
				Entry.Mesh = Mesh;
				Entry.ForwardAxis = Axis;
				Segment->SplineMeshes.Add(Entry);
			}
			Points[Index]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
			Points[Next]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
			++SegmentCount;
		}
		for (ULandscapeSplineControlPoint* Point : TSet<ULandscapeSplineControlPoint*>(Points))
		{
			Point->UpdateSplinePoints();
		}
	}
	Component->MarkRenderStateDirty();
	Component->MarkPackageDirty();
	if (!bDeform)
	{
		Warnings.Add(TEXT("deformTerrain is off: the splines do not shape the landscape"));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), OwnerPath);
	Out->SetNumberField(TEXT("components"), Splines.Num());
	Out->SetNumberField(TEXT("segments"), SegmentCount);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> SetSpline(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetSpline);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setSpline: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setSpline: no editor world");
		return nullptr;
	}
	TArray<FLilaSpline> Splines;
	if (!ParseSplines(Params, Splines, OutError))
	{
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id), Mode = TEXT("actor"), Folder = TEXT("Lila"), LandscapeLabel;
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("mode"), Mode);
	Params->TryGetStringField(TEXT("folder"), Folder);
	Params->TryGetStringField(TEXT("landscape"), LandscapeLabel);
	FVector Location = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	double Width = 200.0;
	Params->TryGetNumberField(TEXT("width"), Width);
	bool bDeform = true, bJoin = false;
	Params->TryGetBoolField(TEXT("deformTerrain"), bDeform);
	Params->TryGetBoolField(TEXT("join"), bJoin);

	TArray<FString> Warnings;
	UStaticMesh* Mesh = nullptr;
	ESplineMeshAxis::Type Axis = ESplineMeshAxis::X;
	const TSharedPtr<FJsonObject>* MeshObject = nullptr;
	if (Params->TryGetObjectField(TEXT("mesh"), MeshObject))
	{
		FString Asset, AxisName;
		(*MeshObject)->TryGetStringField(TEXT("asset"), Asset);
		(*MeshObject)->TryGetStringField(TEXT("forwardAxis"), AxisName);
		Axis = AxisFrom(AxisName);
		const FString ObjectPath = Asset.Contains(TEXT(".")) ? Asset : Asset + TEXT(".") + FPackageName::GetShortName(Asset);
		Mesh = Asset.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *ObjectPath);
		if (!Mesh)
		{
			Warnings.Add(FString::Printf(TEXT("no static mesh at %s; sent without spline meshes"), *Asset));
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetSpline", "Lila: Update Spline"));
	TSharedPtr<FJsonValue> Result = Mode == TEXT("landscape")
		? SetLandscapeSplines(World, Id, Label, LandscapeLabel, Location, Splines, Mesh, Axis, Width, bDeform, bJoin, Warnings, OutError)
		: SetActorSplines(World, Id, Label, Folder, Location, Splines, Mesh, Axis, OutError);
	if (!Result.IsValid())
	{
		Transaction.Cancel();
		return nullptr;
	}
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Result->AsObject()->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setSpline: %s (%s, %d spline(s))"), *Label, *Mode, Splines.Num());
	return Result;
}

TSharedPtr<FJsonValue> RemoveSpline(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::RemoveSpline);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeSpline: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeSpline: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveSpline", "Lila: Remove Spline"));
	bool bRemoved = false;
	if (AActor* Tagged = FindTagged(World, TagFor(Id)))
	{
		bRemoved = World->EditorDestroyActor(Tagged, true);
	}
	const FString Prefix = LandscapePrefix(Id);
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (ULandscapeSplinesComponent* Splines = It->GetSplinesComponent())
		{
			const int32 Before = Splines->GetControlPoints().Num();
			ClearLandscapeSplines(Splines, Prefix);
			bRemoved |= Splines->GetControlPoints().Num() != Before;
			Splines->MarkRenderStateDirty();
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddSplineMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setSpline"), LilaLink::Sync(&SetSpline));
	Methods.Add(TEXT("removeSpline"), LilaLink::Sync(&RemoveSpline));
}
