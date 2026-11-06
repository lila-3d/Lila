#include "LilaLinkTerrainVolume.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetToolsModule.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLandscapeVisibilityMask.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
const FName LilaLayerName(TEXT("Lila"));
const TCHAR* FunctionName = TEXT("MF_LilaTerrainVolume");
const TCHAR* MasterName = TEXT("M_LilaTerrainVolume");
const TCHAR* VersionTag = TEXT("Lila Terrain Volume v");
constexpr int32 FunctionVersion = 1;
const TCHAR* LandscapeFolder = TEXT("/Game/LilaLink/Landscapes");
const FLinearColor GroundColorDefault(0.25f, 0.22f, 0.18f);
constexpr float GroundRoughnessDefault = 0.9f;

const TCHAR* TriplanarCode = TEXT(R"(
float3 w = pow(abs(Normal) + 1e-4, max(Sharpness, 1.0));
w /= (w.x + w.y + w.z);
float3 cx = Texture2DSample(Texture, TextureSampler, WorldTile.yz).rgb;
float3 cy = Texture2DSample(Texture, TextureSampler, WorldTile.xz).rgb;
float3 cz = Texture2DSample(Texture, TextureSampler, WorldTile.xy).rgb;
return Tint.rgb * (cx * w.x + cy * w.y + cz * w.z);
)");

int32 VersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(VersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(VersionTag)));
		}
	}
	return 0;
}

/**
 * MF_LilaTerrainVolume: a ground surface for meshes that sit in a landscape, a tinted texture projected from all three
 * axes (TileSize cm per repeat, Sharpness for how hard the projections part) and a roughness. Its tint and roughness
 * inputs default to GroundColor and GroundRoughness, the names the generated landscape material uses.
 */
void BuildFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::BuildFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila terrain volume: a tinted triplanar texture and roughness for caves, overhangs and arches."));
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	TMap<FString, UMaterialExpression*> In;
	In.Add(TEXT("Tint"), G.VectorInput(TEXT("GroundColor"), GroundColorDefault, TEXT("Ground"), TEXT("Colour, multiplied with Texture.")));
	UMaterialExpressionFunctionInput* Rough = G.ScalarInput(TEXT("GroundRoughness"), GroundRoughnessDefault, TEXT("Ground"), TEXT("Roughness."));
	In.Add(TEXT("Texture"), G.TextureInput(TEXT("GroundTexture"), White, SAMPLERTYPE_Color, TEXT("Ground"), TEXT("Colour texture, projected along each axis.")));
	UMaterialExpressionFunctionInput* TileSize = G.ScalarInput(TEXT("GroundTileSize"), 400.0f, TEXT("Ground"), TEXT("Texture repeat, cm."));
	In.Add(TEXT("Sharpness"), G.ScalarInput(TEXT("TriplanarSharpness"), 4.0f, TEXT("Ground"), TEXT("How sharply the three projections part.")));
	In.Add(TEXT("Normal"), G.Make(UMaterialExpressionVertexNormalWS::StaticClass(), -1600));
	// World position is large-world (double); divide and wrap it before the Custom node, which takes floats.
	UMaterialExpression* World = G.Make(UMaterialExpressionWorldPosition::StaticClass(), -1600);
	auto* Divide = G.New<UMaterialExpressionDivide>(-1400);
	UMaterialEditingLibrary::ConnectMaterialExpressions(World, TEXT(""), Divide, TEXT("A"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(TileSize, TEXT(""), Divide, TEXT("B"));
	auto* Wrap = G.New<UMaterialExpressionFmod>(-1300);
	UMaterialEditingLibrary::ConnectMaterialExpressions(Divide, TEXT(""), Wrap, TEXT("A"));
	auto* Period = G.New<UMaterialExpressionConstant>(-1400);
	Period->R = 256.0f;
	UMaterialEditingLibrary::ConnectMaterialExpressions(Period, TEXT(""), Wrap, TEXT("B"));
	In.Add(TEXT("WorldTile"), Wrap);

	auto* Color = G.New<UMaterialExpressionCustom>(-400);
	Color->Description = FString::Printf(TEXT("%s%d colour"), VersionTag, FunctionVersion);
	Color->Code = TriplanarCode;
	Color->OutputType = CMOT_Float3;
	Color->Inputs.Reset();
	const TCHAR* Names[] = {TEXT("Tint"), TEXT("Texture"), TEXT("WorldTile"), TEXT("Normal"), TEXT("Sharpness")};
	for (const TCHAR* Name : Names)
	{
		Color->Inputs.AddDefaulted_GetRef().InputName = Name;
	}
	for (const TCHAR* Name : Names)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(In[Name], TEXT(""), Color, Name);
	}
	G.Output(Color, TEXT("BaseColor"), TEXT("The projected, tinted texture."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness."));
	G.Finish();
}

/** M_LilaTerrainVolume on MF_LilaTerrainVolume, both in the masters folder, built when new or older than this plugin. */
UMaterial* Master(TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::Master);

	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), FunctionName, MasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	if (Pair.bCreated || VersionOf(Pair.Function) < FunctionVersion)
	{
		BuildFunction(Pair.Function);
		UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Pair.Material, Pair.Function);
		if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}))
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), FunctionName, MasterName));
		}
		Pair.Material->PostEditChange();
		Pair.Material->MarkPackageDirty();
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	return Pair.Material;
}

/** MI_<Label>_Volume on the master, coloured like the landscape's own material where it has GroundColor and GroundRoughness. */
UMaterialInterface* VolumeInstance(const FString& Label, UMaterialInterface* LandscapeMaterial, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::VolumeInstance);

	UMaterial* Parent = Master(OutCreated, OutWarnings);
	if (!Parent)
	{
		return nullptr;
	}
	const FString Folder = FString(LandscapeFolder) / Label;
	const FString Name = TEXT("MI_") + Label + TEXT("_Volume");
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Folder / Name + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *Name, *Folder));
			return Parent;
		}
		OutCreated.AddUnique(Instance->GetPathName());
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	FLinearColor Color = GroundColorDefault;
	float Roughness = GroundRoughnessDefault;
	if (LandscapeMaterial)
	{
		LandscapeMaterial->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("GroundColor")), Color);
		LandscapeMaterial->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("GroundRoughness")), Roughness);
	}
	UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("GroundColor"), Color);
	UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("GroundRoughness"), Roughness);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
	return Instance;
}

/**
 * Holes render only through a Landscape Visibility Mask in the landscape (or hole) material. A material Lila generated
 * gets one on its opacity mask; anyone else's is left alone and reported. Returns the material that now draws the holes.
 */
UMaterialInterface* EnsureHoleMaterial(ALandscape* Landscape, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::EnsureHoleMaterial);

	UMaterialInterface* Used = Landscape->LandscapeHoleMaterial ? Landscape->LandscapeHoleMaterial.Get() : Landscape->LandscapeMaterial.Get();
	UMaterial* Base = Used ? Used->GetMaterial() : nullptr;
	if (!Base)
	{
		OutWarnings.Add(TEXT("the landscape has no material, so its holes cut collision only"));
		return nullptr;
	}
	if (Base->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionLandscapeVisibilityMask>())
	{
		return Used;
	}
	if (!Base->GetPathName().StartsWith(LandscapeFolder))
	{
		OutWarnings.Add(FString::Printf(TEXT("%s has no Landscape Visibility Mask, so the holes cut collision only; add one on Opacity Mask (Masked) "
										 "or set a Landscape Hole Material"),
			*Base->GetPathName()));
		return nullptr;
	}
	Base->Modify();
	UMaterialExpression* Mask = UMaterialEditingLibrary::CreateMaterialExpression(Base, UMaterialExpressionLandscapeVisibilityMask::StaticClass(), -400, -300);
	Base->BlendMode = BLEND_Masked;
	UMaterialEditingLibrary::ConnectMaterialProperty(Mask, TEXT(""), MP_OpacityMask);
	Base->PostEditChange();
	Base->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Base);
	return Used;
}

UWorld* VolumeEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ALandscape* FindLandscapeFor(UWorld* World, const FString& Id)
{
	const FName Tag(*(TEXT("LilaLink.Landscape:") + Id.Right(900)));
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonValue> SetLandscapeVolume(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::SetLandscapeVolume);

	FString Id, Label, VisibilityPath, MaterialPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setLandscapeVolume: give id");
		return nullptr;
	}
	Label = FPackageName::GetShortName(Id);
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("visibility"), VisibilityPath);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	UWorld* World = VolumeEditorWorld();
	if (!World)
	{
		OutError = TEXT("setLandscapeVolume: no editor world");
		return nullptr;
	}
	ALandscape* Landscape = FindLandscapeFor(World, Id);
	if (!Landscape)
	{
		OutError = TEXT("setLandscapeVolume: no landscape sent for ") + Id + TEXT(" (send the terrain first)");
		return nullptr;
	}

	TArray<FString> Created, Warnings;
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetLandscapeVolume", "Lila: Landscape Volume"));
	int32 Holes = 0;
	FString HoleMaterial;
	if (!VisibilityPath.IsEmpty())
	{
		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		FIntRect Extent;
		if (!Info || !Info->GetLandscapeExtent(Extent))
		{
			OutError = TEXT("setLandscapeVolume: the landscape has no extent");
			return nullptr;
		}
		const int32 SizeX = Extent.Width() + 1, SizeY = Extent.Height() + 1;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *VisibilityPath) || Bytes.Num() != SizeX * SizeY)
		{
			OutError = FString::Printf(TEXT("setLandscapeVolume: %s isn't a %dx%d 8-bit visibility map"), *VisibilityPath, SizeX, SizeY);
			return nullptr;
		}
		for (uint8 B : Bytes)
		{
			Holes += B > 127 ? 1 : 0;
		}
		int32 Index = Landscape->GetLayerIndex(LilaLayerName);
		if (Index == INDEX_NONE)
		{
			Index = Landscape->CreateLayer(LilaLayerName);
		}
		const ULandscapeEditLayerBase* Layer = Index == INDEX_NONE ? nullptr : Landscape->GetEditLayerConst(Index);
		if (!Layer || !ALandscapeProxy::VisibilityLayer)
		{
			OutError = TEXT("setLandscapeVolume: the landscape has no Lila edit layer or visibility layer");
			return nullptr;
		}
		{
			FScopedSetLandscapeEditingLayer Scope(Landscape, Layer->GetGuid(), [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
			FLandscapeEditDataInterface Edit(Info, Layer->GetGuid());
			Edit.SetAlphaData(ALandscapeProxy::VisibilityLayer, Extent.Min.X, Extent.Min.Y, Extent.Max.X, Extent.Max.Y, Bytes.GetData(), 0);
		}
		if (Holes > 0)
		{
			if (UMaterialInterface* M = EnsureHoleMaterial(Landscape, Warnings))
			{
				HoleMaterial = M->GetPathName();
			}
		}
	}

	UMaterialInterface* PieceMaterial = nullptr;
	if (!MaterialPath.IsEmpty())
	{
		const FString ObjectPath = MaterialPath.Contains(TEXT(".")) ? MaterialPath : MaterialPath + TEXT(".") + FPackageName::GetShortName(MaterialPath);
		PieceMaterial = LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
		if (!PieceMaterial)
		{
			Warnings.Add(TEXT("no material ") + MaterialPath + TEXT("; the pieces get the generated one"));
		}
	}
	if (!PieceMaterial)
	{
		PieceMaterial = VolumeInstance(Label, Landscape->LandscapeMaterial, Created, Warnings);
	}
	TArray<TSharedPtr<FJsonValue>> MeshValues;
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (Params->TryGetArrayField(TEXT("meshes"), Meshes))
	{
		for (const TSharedPtr<FJsonValue>& V : *Meshes)
		{
			const FString Path = V->AsString();
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
			if (!Mesh)
			{
				Warnings.Add(TEXT("no static mesh ") + Path);
				continue;
			}
			if (PieceMaterial)
			{
				Mesh->Modify();
				for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
				{
					Mesh->SetMaterial(Slot, PieceMaterial);
				}
				Mesh->MarkPackageDirty();
			}
			MeshValues.Add(MakeShared<FJsonValueString>(Mesh->GetPathName()));
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("holes"), Holes);
	Out->SetStringField(TEXT("holeMaterial"), HoleMaterial);
	Out->SetStringField(TEXT("material"), PieceMaterial ? PieceMaterial->GetPathName() : FString());
	Out->SetArrayField(TEXT("meshes"), MeshValues);
	TArray<TSharedPtr<FJsonValue>> CreatedValues, WarningValues;
	for (const FString& C : Created)
	{
		CreatedValues.Add(MakeShared<FJsonValueString>(C));
	}
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("assets"), CreatedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setLandscapeVolume: %s, %d hole vertices, %d meshes"), *Label, Holes, MeshValues.Num());
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddTerrainVolumeMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setLandscapeVolume"), LilaLink::Sync(&SetLandscapeVolume));
}
