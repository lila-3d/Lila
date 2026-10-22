#include "LilaLinkMaterials.h"

#include "LilaLinkLog.h"

#include "AssetImportTask.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/TextureFactory.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
using LilaLink::FMaterialRequest;
using LilaLink::FSlotResult;

enum class ETextureUse : uint8
{
	Color,
	Linear,
	Normal,
};

double Number(const TSharedPtr<FJsonObject>& In, const TCHAR* Field, double Default)
{
	double V = Default;
	return In.IsValid() && In->TryGetNumberField(Field, V) ? V : Default;
}

FLinearColor Color(const TSharedPtr<FJsonObject>& In, const TCHAR* Field, const FLinearColor& Default)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!In.IsValid() || !In->TryGetArrayField(Field, Values) || Values->Num() < 3)
	{
		return Default;
	}
	return FLinearColor(float((*Values)[0]->AsNumber()), float((*Values)[1]->AsNumber()), float((*Values)[2]->AsNumber()));
}

FString String(const TSharedPtr<FJsonObject>& In, const TCHAR* Field)
{
	FString S;
	return In.IsValid() && In->TryGetStringField(Field, S) ? S : FString();
}

FString AssetNameFor(FName Slot)
{
	return ObjectTools::SanitizeObjectName(Slot.ToString());
}

UMaterialInterface* MaterialAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() && Data.IsInstanceOf(UMaterialInterface::StaticClass()) ? Cast<UMaterialInterface>(Data.GetAsset()) : nullptr;
}

/** Every material and material instance under /Game, by asset name; gathered once per import. */
struct FProjectMaterials
{
	bool bGathered = false;
	TMultiMap<FName, FAssetData> ByName;

	TArray<FAssetData> Find(FName Name)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::FindInProject);

		if (!bGathered)
		{
			bGathered = true;
			FARFilter Filter;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			TArray<FAssetData> Assets;
			IAssetRegistry::GetChecked().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				ByName.Add(A.AssetName, A);
			}
		}
		TArray<FAssetData> Out;
		ByName.MultiFind(Name, Out);
		return Out;
	}
};

UTexture* ImportTexture(const FString& File, const FString& Folder, ETextureUse Use, bool bFlipGreen, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ImportTexture);

	const FString Name = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(File));
	FAssetData Existing = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	if (Existing.IsValid())
	{
		if (UTexture* Texture = Cast<UTexture>(Existing.GetAsset()))
		{
			// One texture asset per file, so the latest send decides its green convention.
			if (Use == ETextureUse::Normal && Texture->bFlipGreenChannel != bFlipGreen)
			{
				Texture->Modify();
				Texture->bFlipGreenChannel = bFlipGreen;
				Texture->PostEditChange();
			}
			return Texture;
		}
	}
	if (!FPaths::FileExists(File))
	{
		OutWarnings.Add(FString::Printf(TEXT("Texture %s not found; the material is made without it"), *File));
		return nullptr;
	}

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = File;
	Task->DestinationPath = Folder;
	Task->DestinationName = Name;
	Task->bReplaceExisting = false;
	Task->bAutomated = true;
	Task->bSave = false;
	// An explicit factory imports synchronously, without going through Interchange.
	Task->Factory = NewObject<UTextureFactory>(GetTransientPackage());
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});

	UTexture* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Texture = Texture ? Texture : Cast<UTexture>(Object);
	}
	if (!Texture)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not import texture %s"), *File));
		return nullptr;
	}
	Texture->Modify();
	Texture->SRGB = Use == ETextureUse::Color;
	Texture->CompressionSettings = Use == ETextureUse::Normal ? TC_Normalmap : TC_Default;
	Texture->bFlipGreenChannel = bFlipGreen;
	Texture->PostEditChange();
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/** Builds a material from Lila's inputs: parameters for the constants, texture parameters for the maps. */
UMaterial* CreateMaterial(const FString& Name, const FMaterialRequest& R, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::CreateMaterial);

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>(GetTransientPackage());
	UMaterial* Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.MaterialFolder, UMaterial::StaticClass(), Factory));
	if (!Material)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create material %s in %s"), *Name, *R.MaterialFolder));
		return nullptr;
	}
	OutCreated.AddUnique(Material->GetPathName());

	const int32 UvSet = FMath::Clamp(int32(Number(In, TEXT("uvSet"), 0.0)), 0, 7);
	int32 Row = 0;
	auto NextY = [&Row] { return (Row++) * 160; };

	auto Scalar = [&](const TCHAR* Param, float Value) {
		auto* E = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		return E;
	};
	auto Vector = [&](const TCHAR* Param, const FLinearColor& Value) {
		auto* E = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		return E;
	};
	auto Map = [&](const TCHAR* Field, const TCHAR* Param, ETextureUse Use, bool bFlipGreen = false) -> UMaterialExpressionTextureSampleParameter2D* {
		const FString File = String(In, Field);
		if (File.IsEmpty() || !R.bImportTextures)
		{
			return nullptr;
		}
		UTexture* Texture = ImportTexture(File, R.MaterialFolder, Use, bFlipGreen, OutCreated, OutWarnings);
		if (!Texture)
		{
			return nullptr;
		}
		auto* E = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->Texture = Texture;
		E->SamplerType = Use == ETextureUse::Color ? SAMPLERTYPE_Color : Use == ETextureUse::Normal ? SAMPLERTYPE_Normal : SAMPLERTYPE_LinearColor;
		E->ConstCoordinate = uint8(UvSet);
		return E;
	};
	auto Multiply = [&](UMaterialExpression* A, const TCHAR* AOut, UMaterialExpression* B) {
		UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -200, NextY());
		UMaterialEditingLibrary::ConnectMaterialExpressions(A, AOut, E, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(B, TEXT(""), E, TEXT("B"));
		return E;
	};

	const FString Blend = String(In, TEXT("blendMode"));
	Material->BlendMode = Blend == TEXT("masked") ? BLEND_Masked : Blend == TEXT("translucent") ? BLEND_Translucent : BLEND_Opaque;
	Material->TwoSided = In.IsValid() && In->HasTypedField<EJson::Boolean>(TEXT("twoSided")) && In->GetBoolField(TEXT("twoSided"));
	Material->OpacityMaskClipValue = float(Number(In, TEXT("opacityMaskClipValue"), 0.3333));

	UMaterialExpressionTextureSampleParameter2D* BaseMap = Map(TEXT("baseColorMap"), TEXT("BaseColorMap"), ETextureUse::Color);
	if (BaseMap)
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(BaseMap, TEXT("RGB"), MP_BaseColor);
	}
	else
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Vector(TEXT("BaseColor"), Color(In, TEXT("baseColor"), FLinearColor(0.5f, 0.5f, 0.5f))), TEXT(""), MP_BaseColor);
	}

	// A packed map wins over the separate ones, as in Lila. MRO swaps occlusion and metallic.
	const bool bMro = String(In, TEXT("packedLayout")) == TEXT("mro");
	if (UMaterialExpressionTextureSampleParameter2D* Orm = Map(TEXT("ormMap"), bMro ? TEXT("MROMap") : TEXT("ORMMap"), ETextureUse::Linear))
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, bMro ? TEXT("B") : TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, bMro ? TEXT("R") : TEXT("B"), MP_Metallic);
	}
	else
	{
		UMaterialExpression* Metallic = Map(TEXT("metallicMap"), TEXT("MetallicMap"), ETextureUse::Linear);
		UMaterialEditingLibrary::ConnectMaterialProperty(Metallic ? Metallic : Scalar(TEXT("Metallic"), float(Number(In, TEXT("metallic"), 0.0))), Metallic ? TEXT("R") : TEXT(""), MP_Metallic);
		UMaterialExpression* Roughness = Map(TEXT("roughnessMap"), TEXT("RoughnessMap"), ETextureUse::Linear);
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness ? Roughness : Scalar(TEXT("Roughness"), float(Number(In, TEXT("roughness"), 0.55))), Roughness ? TEXT("R") : TEXT(""), MP_Roughness);
		if (UMaterialExpression* Ao = Map(TEXT("ambientOcclusionMap"), TEXT("AmbientOcclusionMap"), ETextureUse::Linear))
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Ao, TEXT("R"), MP_AmbientOcclusion);
		}
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(TEXT("Specular"), float(Number(In, TEXT("specular"), 0.5))), TEXT(""), MP_Specular);

	if (UMaterialExpression* Normal = Map(TEXT("normalMap"), TEXT("NormalMap"), ETextureUse::Normal, String(In, TEXT("normalFormat")) == TEXT("opengl")))
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT("RGB"), MP_Normal);
	}

	const float Intensity = float(Number(In, TEXT("emissiveIntensity"), 1.0));
	if (UMaterialExpression* EmissiveMap = Map(TEXT("emissiveMap"), TEXT("EmissiveMap"), ETextureUse::Color))
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Multiply(EmissiveMap, TEXT("RGB"), Scalar(TEXT("EmissiveIntensity"), Intensity)), TEXT(""), MP_EmissiveColor);
	}
	else
	{
		const FLinearColor Emissive = Color(In, TEXT("emissiveColor"), FLinearColor::Black) * Intensity;
		if (!Emissive.Equals(FLinearColor::Black))
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Vector(TEXT("EmissiveColor"), Emissive), TEXT(""), MP_EmissiveColor);
		}
	}

	if (Material->BlendMode != BLEND_Opaque)
	{
		const EMaterialProperty Target = Material->BlendMode == BLEND_Masked ? MP_OpacityMask : MP_Opacity;
		UMaterialExpression* Opacity = Scalar(TEXT("Opacity"), float(Number(In, TEXT("opacity"), 1.0)));
		UMaterialEditingLibrary::ConnectMaterialProperty(BaseMap ? Multiply(BaseMap, TEXT("A"), Opacity) : Opacity, TEXT(""), Target);
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();
	return Material;
}

struct FSlotView
{
	FName Name;
	TObjectPtr<UMaterialInterface>* Material = nullptr;
};

TArray<FSlotView> SlotsOf(UObject* Asset)
{
	TArray<FSlotView> Out;
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		for (FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			Out.Add({M.MaterialSlotName, &M.MaterialInterface});
		}
	}
	else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
	{
		for (FSkeletalMaterial& M : Skel->GetMaterials())
		{
			Out.Add({M.MaterialSlotName, &M.MaterialInterface});
		}
	}
	return Out;
}

/** The material a slot should have, and how it was chosen. */
UMaterialInterface* Resolve(FName Slot, const FMaterialRequest& R, FProjectMaterials& Project, FString& OutHow, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::Resolve);

	const FString Name = AssetNameFor(Slot);
	// A kept material named after the slot is found again by name below, so only real reassignments count.
	if (R.bKeep)
	{
		if (const FSoftObjectPath* Previous = R.Previous.Find(Slot))
		{
			UMaterialInterface* Kept = Cast<UMaterialInterface>(Previous->TryLoad());
			if (Kept && Kept != UMaterial::GetDefaultMaterial(MD_Surface) && Kept->GetName() != Name)
			{
				OutHow = TEXT("kept");
				return Kept;
			}
		}
	}
	if (Slot == LilaLink::DefaultSlotName)
	{
		OutHow = TEXT("default");
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}
	for (const FString& Folder : {R.MaterialFolder, R.AssetFolder})
	{
		if (UMaterialInterface* Found = MaterialAt(Folder, Name))
		{
			OutHow = TEXT("found");
			return Found;
		}
	}
	TArray<FAssetData> Matches = Project.Find(FName(*Name));
	if (Matches.Num() == 1)
	{
		OutHow = TEXT("found");
		return Cast<UMaterialInterface>(Matches[0].GetAsset());
	}
	if (Matches.Num() > 1)
	{
		TArray<FString> Paths;
		for (const FAssetData& M : Matches)
		{
			Paths.Add(M.PackageName.ToString());
		}
		OutWarnings.Add(FString::Printf(TEXT("Slot %s: %d materials named %s (%s); put the one to use in %s"), *Slot.ToString(), Matches.Num(), *Name,
			*FString::Join(Paths, TEXT(", ")), *R.MaterialFolder));
		OutHow = TEXT("ambiguous");
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}
	if (R.bCreate)
	{
		const TSharedPtr<FJsonObject>* Inputs = R.Inputs.Find(Slot);
		if (UMaterial* Created = CreateMaterial(Name, R, Inputs ? *Inputs : nullptr, OutCreated, OutWarnings))
		{
			OutHow = TEXT("created");
			return Created;
		}
	}
	OutHow = TEXT("none");
	return UMaterial::GetDefaultMaterial(MD_Surface);
}
} // namespace

bool LilaLink::ParseMaterialRequest(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Options, const FString& AssetFolder,
	FMaterialRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ParseMaterialRequest);

	Out.AssetFolder = AssetFolder;
	Options->TryGetBoolField(TEXT("importMaterials"), Out.bCreate);
	Options->TryGetBoolField(TEXT("importTextures"), Out.bImportTextures);
	Options->TryGetBoolField(TEXT("keepMaterials"), Out.bKeep);

	FString Folder;
	Options->TryGetStringField(TEXT("materialFolder"), Folder);
	Folder.TrimStartAndEndInline();
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	Out.MaterialFolder = Folder.IsEmpty() ? AssetFolder / TEXT("Materials") : Folder.StartsWith(TEXT("/")) ? Folder : AssetFolder / Folder;
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(Out.MaterialFolder / TEXT("M"), false, &Reason))
	{
		OutError = FString::Printf(TEXT("importAsset: bad materialFolder %s: %s"), *Out.MaterialFolder, *Reason.ToString());
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	Out.bManaged = Params->TryGetArrayField(TEXT("materials"), Materials);
	if (Materials)
	{
		for (const TSharedPtr<FJsonValue>& V : *Materials)
		{
			const TSharedPtr<FJsonObject>* M = nullptr;
			FString Name;
			if (V.IsValid() && V->TryGetObject(M) && (*M)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
			{
				Out.Inputs.Add(FName(*Name), *M);
			}
		}
	}
	return true;
}

void LilaLink::SnapshotMaterials(UObject* Asset, FMaterialRequest& Request)
{
	for (const FSlotView& S : SlotsOf(Asset))
	{
		if (*S.Material)
		{
			Request.Previous.Add(S.Name, FSoftObjectPath(S.Material->Get()));
		}
	}
}

void LilaLink::ApplyMaterials(const FMaterialRequest& Request, const TArray<FString>& ImportedAssets, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ApplyMaterials);

	FProjectMaterials Project;
	for (const FString& Path : ImportedAssets)
	{
		UObject* Asset = FSoftObjectPath(Path).ResolveObject();
		TArray<FSlotView> Slots = SlotsOf(Asset);
		if (Slots.IsEmpty())
		{
			continue;
		}
		bool bChanged = false;
		for (const FSlotView& S : Slots)
		{
			FString How;
			UMaterialInterface* Material = Resolve(S.Name, Request, Project, How, OutCreated, OutWarnings);
			if (S.Material->Get() != Material)
			{
				if (!bChanged)
				{
					Asset->Modify();
					bChanged = true;
				}
				*S.Material = Material;
			}
			OutSlots.Add({Asset->GetPathName(), S.Name, Material ? Material->GetPathName() : FString(), How});
			UE_LOG(LogLilaLink, Log, TEXT("%s slot %s -> %s (%s)"), *Asset->GetName(), *S.Name.ToString(), Material ? *Material->GetPathName() : TEXT("none"), *How);
		}
		if (bChanged)
		{
			Asset->PostEditChange();
			Asset->MarkPackageDirty();
		}
	}
}
