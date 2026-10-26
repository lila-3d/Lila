#include "LilaLinkLandscape.h"

#include "LilaLinkLog.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
const FName LilaLayerName(TEXT("Lila"));

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Landscape:") + Id.Right(900)));
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ALandscape* FindTaggedLandscape(UWorld* World, FName Tag)
{
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
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

/** The landscape's layout matches the one asked for: same vertex count and section layout. */
bool SameLayout(ALandscape* Landscape, int32 SizeX, int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::SameLayout);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	FIntRect Extent;
	if (!Info || !Info->GetLandscapeExtent(Extent))
	{
		return false;
	}
	return Extent.Width() + 1 == SizeX && Extent.Height() + 1 == SizeY && Landscape->SubsectionSizeQuads == QuadsPerSection &&
		Landscape->NumSubsections == SectionsPerComponent;
}

/** The "Lila" edit layer's guid, made when the landscape has none. */
FGuid LilaLayer(ALandscape* Landscape)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::LilaLayer);

	int32 Index = Landscape->GetLayerIndex(LilaLayerName);
	if (Index == INDEX_NONE)
	{
		Index = Landscape->CreateLayer(LilaLayerName);
	}
	const ULandscapeEditLayerBase* Layer = Index == INDEX_NONE ? nullptr : Landscape->GetEditLayerConst(Index);
	return Layer ? Layer->GetGuid() : FGuid();
}

ALandscape* CreateLandscape(UWorld* World, const FString& Label, const FVector& Location, const FVector& Scale, int32 SizeX, int32 SizeY,
	int32 QuadsPerSection, int32 SectionsPerComponent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::CreateLandscape);

	// Flat at the encoding's zero: the heights themselves go to the Lila edit layer, which adds to this.
	TArray<uint16> Flat;
	Flat.Init(32768, SizeX * SizeY);
	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(Flat));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	if (!Landscape)
	{
		return nullptr;
	}
	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);
	Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, SectionsPerComponent, QuadsPerSection, HeightDataPerLayers, TEXT(""),
		MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	Landscape->SetActorLabel(Label);
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	return Landscape;
}

TSharedPtr<FJsonValue> SetLandscape(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::SetLandscape);

	FString Id, HeightmapPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setLandscape: give id");
		return nullptr;
	}
	if (!Params->TryGetStringField(TEXT("heightmap"), HeightmapPath))
	{
		OutError = TEXT("setLandscape: give heightmap");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (!Params->TryGetArrayField(TEXT("resolution"), Resolution) || Resolution->Num() != 2)
	{
		OutError = TEXT("setLandscape: give resolution [x, y]");
		return nullptr;
	}
	const int32 SizeX = static_cast<int32>((*Resolution)[0]->AsNumber()), SizeY = static_cast<int32>((*Resolution)[1]->AsNumber());
	int32 QuadsPerSection = 63, SectionsPerComponent = 1;
	Params->TryGetNumberField(TEXT("quadsPerSection"), QuadsPerSection);
	Params->TryGetNumberField(TEXT("sectionsPerComponent"), SectionsPerComponent);
	const int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	if (SizeX < 2 || SizeY < 2 || QuadsPerComponent <= 0 || (SizeX - 1) % QuadsPerComponent != 0 || (SizeY - 1) % QuadsPerComponent != 0)
	{
		OutError = FString::Printf(TEXT("setLandscape: %dx%d isn't a whole number of %d-quad components"), SizeX, SizeY, QuadsPerComponent);
		return nullptr;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *HeightmapPath))
	{
		OutError = TEXT("setLandscape: can't read ") + HeightmapPath;
		return nullptr;
	}
	if (Bytes.Num() != SizeX * SizeY * 2)
	{
		OutError = FString::Printf(TEXT("setLandscape: %s holds %d bytes, %dx%d needs %d"), *HeightmapPath, Bytes.Num(), SizeX, SizeY, SizeX * SizeY * 2);
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(SizeX * SizeY);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setLandscape: no editor world");
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id), MaterialPath;
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	FVector Location = FVector::ZeroVector, Scale(100.0, 100.0, 100.0);
	ReadVector(Params, TEXT("location"), Location);
	ReadVector(Params, TEXT("scale"), Scale);
	int32 GridSize = 0;
	Params->TryGetNumberField(TEXT("gridSize"), GridSize);

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetLandscape", "Lila: Update Landscape"));
	const FName Tag = TagFor(Id);
	ALandscape* Landscape = FindTaggedLandscape(World, Tag);
	bool bCreated = false;
	if (Landscape && !SameLayout(Landscape, SizeX, SizeY, QuadsPerSection, SectionsPerComponent))
	{
		World->EditorDestroyActor(Landscape, true);
		Landscape = nullptr;
	}
	if (!Landscape)
	{
		Landscape = CreateLandscape(World, Label, Location, Scale, SizeX, SizeY, QuadsPerSection, SectionsPerComponent);
		if (!Landscape)
		{
			Transaction.Cancel();
			OutError = TEXT("setLandscape: couldn't spawn a landscape");
			return nullptr;
		}
		Landscape->Tags.AddUnique(Tag);
		bCreated = true;
	}
	else
	{
		Landscape->Modify();
		Landscape->SetActorLocation(Location);
		Landscape->SetActorRelativeScale3D(Scale);
	}
	if (!MaterialPath.IsEmpty())
	{
		const FString ObjectPath = MaterialPath.Contains(TEXT(".")) ? MaterialPath : MaterialPath + TEXT(".") + FPackageName::GetShortName(MaterialPath);
		if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *ObjectPath))
		{
			Landscape->LandscapeMaterial = Material;
			FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial)));
			Landscape->PostEditChangeProperty(Changed);
		}
	}

	const FGuid LayerGuid = LilaLayer(Landscape);
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info || !LayerGuid.IsValid())
	{
		Transaction.Cancel();
		OutError = TEXT("setLandscape: the landscape has no Lila edit layer");
		return nullptr;
	}
	{
		FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid, [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
		FLandscapeEditDataInterface Edit(Info, LayerGuid);
		Edit.SetHeightData(0, 0, SizeX - 1, SizeY - 1, Heights.GetData(), 0, true);
	}
	if (GridSize > 0)
	{
		if (ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>(); Subsystem && Subsystem->IsGridBased())
		{
			Subsystem->ChangeGridSize(Info, static_cast<uint32>(GridSize));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Landscape->GetActorLabel());
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetArrayField(TEXT("resolution"), {MakeShared<FJsonValueNumber>(SizeX), MakeShared<FJsonValueNumber>(SizeY)});
	Out->SetStringField(TEXT("editLayer"), LilaLayerName.ToString());
	UE_LOG(LogLilaLink, Log, TEXT("setLandscape: %s %dx%d (%s)"), *Label, SizeX, SizeY, bCreated ? TEXT("created") : TEXT("updated"));
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveLandscape(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::RemoveLandscape);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeLandscape: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeLandscape: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveLandscape", "Lila: Remove Landscape"));
	bool bRemoved = false;
	if (ALandscape* Landscape = FindTaggedLandscape(World, TagFor(Id)))
	{
		bRemoved = World->EditorDestroyActor(Landscape, true);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddLandscapeMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setLandscape"), LilaLink::Sync(&SetLandscape));
	Methods.Add(TEXT("removeLandscape"), LilaLink::Sync(&RemoveLandscape));
}
