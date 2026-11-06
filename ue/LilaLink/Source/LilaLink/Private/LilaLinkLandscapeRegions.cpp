#include "LilaLinkLandscapeRegions.h"

#include "LilaLinkLandscape.h"
#include "LilaLinkLog.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
struct FRegionLayer
{
	FName Name;
	FString Weightmap;
};

struct FRegion
{
	FIntRect Rect; // inclusive vertex bounds
	FString Heightmap;
	TArray<FRegionLayer> Layers;

	int32 Width() const { return Rect.Max.X - Rect.Min.X + 1; }
	int32 Height() const { return Rect.Max.Y - Rect.Min.Y + 1; }
};

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

/** The regions, each checked against the landscape's size and its files' sizes before anything changes. */
bool ReadRegions(const TSharedPtr<FJsonObject>& Params, int32 SizeX, int32 SizeY, TArray<FRegion>& Out, TSet<FName>& OutLayers, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscapeRegions::ReadRegions);

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("regions"), Values) || Values->Num() == 0)
	{
		OutError = TEXT("setLandscapeRegions: give regions");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Values)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Rect = nullptr;
		FRegion& R = Out.AddDefaulted_GetRef();
		if (!O.IsValid() || !O->TryGetArrayField(TEXT("rect"), Rect) || Rect->Num() != 4 || !O->TryGetStringField(TEXT("heightmap"), R.Heightmap))
		{
			OutError = TEXT("setLandscapeRegions: each region needs rect [x0, y0, x1, y1] and heightmap");
			return false;
		}
		R.Rect = FIntRect(static_cast<int32>((*Rect)[0]->AsNumber()), static_cast<int32>((*Rect)[1]->AsNumber()), static_cast<int32>((*Rect)[2]->AsNumber()),
			static_cast<int32>((*Rect)[3]->AsNumber()));
		if (R.Rect.Min.X < 0 || R.Rect.Min.Y < 0 || R.Rect.Max.X >= SizeX || R.Rect.Max.Y >= SizeY || R.Rect.Max.X < R.Rect.Min.X || R.Rect.Max.Y < R.Rect.Min.Y)
		{
			OutError = FString::Printf(TEXT("setLandscapeRegions: region %s lies outside %dx%d"), *R.Rect.ToString(), SizeX, SizeY);
			return false;
		}
		const int64 Pixels = int64(R.Width()) * int64(R.Height());
		if (IFileManager::Get().FileSize(*R.Heightmap) != Pixels * 2)
		{
			OutError = FString::Printf(TEXT("setLandscapeRegions: %s isn't %dx%d 16-bit"), *R.Heightmap, R.Width(), R.Height());
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if (O->TryGetArrayField(TEXT("layers"), Layers))
		{
			for (const TSharedPtr<FJsonValue>& L : *Layers)
			{
				const TSharedPtr<FJsonObject> LO = L->AsObject();
				FString Name;
				FRegionLayer& Layer = R.Layers.AddDefaulted_GetRef();
				if (!LO.IsValid() || !LO->TryGetStringField(TEXT("name"), Name) || !LO->TryGetStringField(TEXT("weightmap"), Layer.Weightmap) ||
					IFileManager::Get().FileSize(*Layer.Weightmap) != Pixels)
				{
					OutError = FString::Printf(TEXT("setLandscapeRegions: region %s: layer %s needs a %dx%d 8-bit weightmap"), *R.Rect.ToString(), *Name, R.Width(),
						R.Height());
					return false;
				}
				Layer.Name = FName(*Name);
				OutLayers.Add(Layer.Name);
			}
		}
	}
	return true;
}

TSharedPtr<FJsonValue> SetLandscapeRegions(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscapeRegions::SetLandscapeRegions);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setLandscapeRegions: give id");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (!Params->TryGetArrayField(TEXT("resolution"), Resolution) || Resolution->Num() != 2)
	{
		OutError = TEXT("setLandscapeRegions: give resolution [x, y]");
		return nullptr;
	}
	const int32 SizeX = static_cast<int32>((*Resolution)[0]->AsNumber()), SizeY = static_cast<int32>((*Resolution)[1]->AsNumber());
	int32 QuadsPerSection = 63, SectionsPerComponent = 1;
	Params->TryGetNumberField(TEXT("quadsPerSection"), QuadsPerSection);
	Params->TryGetNumberField(TEXT("sectionsPerComponent"), SectionsPerComponent);
	const int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	if (SizeX < 2 || SizeY < 2 || QuadsPerComponent <= 0 || (SizeX - 1) % QuadsPerComponent != 0 || (SizeY - 1) % QuadsPerComponent != 0)
	{
		OutError = FString::Printf(TEXT("setLandscapeRegions: %dx%d isn't a whole number of %d-quad components"), SizeX, SizeY, QuadsPerComponent);
		return nullptr;
	}
	TArray<FRegion> Regions;
	TSet<FName> LayerNames;
	if (!ReadRegions(Params, SizeX, SizeY, Regions, LayerNames, OutError))
	{
		return nullptr;
	}

	UWorld* World = LilaLink::Landscape::EditorWorld();
	if (!World)
	{
		OutError = TEXT("setLandscapeRegions: no editor world");
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id), MaterialPath;
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	FVector Location = FVector::ZeroVector, Scale(100.0, 100.0, 100.0), PitchYawRoll = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	ReadVector(Params, TEXT("scale"), Scale);
	ReadVector(Params, TEXT("rotation"), PitchYawRoll);
	const FRotator Rotation(PitchYawRoll.X, PitchYawRoll.Y, PitchYawRoll.Z);
	int32 GridSize = 0;
	Params->TryGetNumberField(TEXT("gridSize"), GridSize);
	bool bMakeMaterial = false;
	Params->TryGetBoolField(TEXT("makeMaterial"), bMakeMaterial);

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetLandscapeRegions", "Lila: Update Landscape"));
	ALandscape* Landscape = LilaLink::Landscape::FindLandscape(World, Id);
	bool bCreated = false;
	if (Landscape && !LilaLink::Landscape::HasLayout(Landscape, SizeX, SizeY, QuadsPerSection, SectionsPerComponent))
	{
		World->EditorDestroyActor(Landscape, true);
		Landscape = nullptr;
	}
	if (!Landscape)
	{
		Landscape = LilaLink::Landscape::CreateFlatLandscape(World, Label, Location, Rotation, Scale, SizeX, SizeY, QuadsPerSection, SectionsPerComponent);
		if (!Landscape)
		{
			Transaction.Cancel();
			OutError = TEXT("setLandscapeRegions: couldn't spawn a landscape");
			return nullptr;
		}
		Landscape->Tags.AddUnique(LilaLink::Landscape::LandscapeTag(Id));
		bCreated = true;
	}
	else
	{
		Landscape->Modify();
		Landscape->SetActorLocationAndRotation(Location, Rotation);
		Landscape->SetActorRelativeScale3D(Scale);
	}

	// Layers in the order the first region lists them, which is the export's mask order.
	TArray<FName> Ordered;
	for (const FRegionLayer& L : Regions[0].Layers)
	{
		Ordered.AddUnique(L.Name);
	}
	for (const FName& Name : LayerNames)
	{
		Ordered.AddUnique(Name);
	}
	TArray<FString> Created, Warnings;
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
	else if (bMakeMaterial && Ordered.Num() > 0)
	{
		Material = LilaLink::Landscape::GeneratedMaterial(Label, Ordered, Created, Warnings);
	}
	if (Material && Landscape->LandscapeMaterial != Material)
	{
		Landscape->LandscapeMaterial = Material;
		FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial)));
		Landscape->PostEditChangeProperty(Changed);
	}
	TMap<FName, ULandscapeLayerInfoObject*> Infos;
	for (const FName& Name : Ordered)
	{
		ULandscapeLayerInfoObject* Info = LilaLink::Landscape::LayerInfoFor(Label, Name, Created);
		if (!Info)
		{
			Warnings.Add(TEXT("could not make a layer info for ") + Name.ToString());
			continue;
		}
		if (!Landscape->HasTargetLayer(Name))
		{
			Landscape->AddTargetLayer(Name, FLandscapeTargetLayerSettings(Info));
		}
		else
		{
			Landscape->UpdateTargetLayer(Name, FLandscapeTargetLayerSettings(Info));
		}
		Infos.Add(Name, Info);
	}

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (Info && Ordered.Num() > 0)
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	// Proxies first, so each region's writes land in the streaming proxy that owns them.
	if (GridSize > 0 && Info)
	{
		if (ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>(); Subsystem && Subsystem->IsGridBased())
		{
			Subsystem->ChangeGridSize(Info, static_cast<uint32>(GridSize));
		}
		else
		{
			Warnings.Add(TEXT("gridSize ignored: the level isn't World Partition"));
		}
	}
	const FGuid LayerGuid = LilaLink::Landscape::LilaEditLayer(Landscape);
	if (!Info || !LayerGuid.IsValid())
	{
		Transaction.Cancel();
		OutError = TEXT("setLandscapeRegions: the landscape has no Lila edit layer");
		return nullptr;
	}
	int32 Written = 0;
	{
		FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid, [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
		FLandscapeEditDataInterface Edit(Info, LayerGuid);
		for (const FRegion& R : Regions)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscapeRegions::WriteRegion);

			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *R.Heightmap) || Bytes.Num() != R.Width() * R.Height() * 2)
			{
				Warnings.Add(TEXT("couldn't read ") + R.Heightmap);
				continue;
			}
			TArray<uint16> Heights;
			Heights.SetNumUninitialized(R.Width() * R.Height());
			FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());
			Edit.SetHeightData(R.Rect.Min.X, R.Rect.Min.Y, R.Rect.Max.X, R.Rect.Max.Y, Heights.GetData(), 0, true);
			for (const FRegionLayer& L : R.Layers)
			{
				ULandscapeLayerInfoObject** LayerInfo = Infos.Find(L.Name);
				TArray<uint8> Weights;
				if (LayerInfo && FFileHelper::LoadFileToArray(Weights, *L.Weightmap) && Weights.Num() == R.Width() * R.Height())
				{
					Edit.SetAlphaData(*LayerInfo, R.Rect.Min.X, R.Rect.Min.Y, R.Rect.Max.X, R.Rect.Max.Y, Weights.GetData(), 0);
				}
				else if (LayerInfo)
				{
					Warnings.Add(TEXT("couldn't read ") + L.Weightmap);
				}
			}
			// Hands the region's texture edits over before reading the next, so memory stays at one region.
			Edit.Flush();
			++Written;
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Landscape->GetActorLabel());
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetArrayField(TEXT("resolution"), {MakeShared<FJsonValueNumber>(SizeX), MakeShared<FJsonValueNumber>(SizeY)});
	Out->SetNumberField(TEXT("regions"), Written);
	Out->SetStringField(TEXT("editLayer"), TEXT("Lila"));
	TArray<TSharedPtr<FJsonValue>> LayerValues, CreatedValues, WarningValues;
	for (const FName& Name : Ordered)
	{
		if (Infos.Contains(Name))
		{
			LayerValues.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
	}
	for (const FString& C : Created)
	{
		CreatedValues.Add(MakeShared<FJsonValueString>(C));
	}
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("layers"), LayerValues);
	Out->SetStringField(TEXT("material"), Landscape->LandscapeMaterial ? Landscape->LandscapeMaterial->GetPathName() : FString());
	Out->SetArrayField(TEXT("assets"), CreatedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setLandscapeRegions: %s %dx%d in %d regions (%s)"), *Label, SizeX, SizeY, Written, bCreated ? TEXT("created") : TEXT("updated"));
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddLandscapeRegionMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setLandscapeRegions"), LilaLink::Sync(&SetLandscapeRegions));
}
