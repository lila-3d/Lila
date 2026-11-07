#include "LilaLinkTerrainMaterial.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionRuntimeVirtualTextureOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureVolume.h"
#include "Components/RuntimeVirtualTextureComponent.h"

namespace
{
const TCHAR* TerrainFunctionName = TEXT("MF_LilaTerrainLayer");
const TCHAR* TerrainVersionTag = TEXT("Lila Terrain Layer v");
constexpr int32 TerrainFunctionVersion = 1;
const TCHAR* TerrainFolder = TEXT("/Game/LilaLink/Landscapes");

struct FTerrainLayer
{
	FName Name;
	FLinearColor Color = FLinearColor(0.5f, 0.5f, 0.5f);
	float Roughness = 0.85f;
	float TileSize = 400.0f;
};

const TCHAR* TerrainColorCode = TEXT(R"(
float3 c = Tint.rgb * Texture2DSample(Texture, TextureSampler, WorldTile.xy).rgb;
return Accum + c * Weight;
)");

const TCHAR* TerrainRoughnessCode = TEXT(R"(
return AccumRoughness + Roughness * Weight;
)");

int32 TerrainVersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(TerrainVersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(TerrainVersionTag)));
		}
	}
	return 0;
}

/**
 * MF_LilaTerrainLayer: adds one weight-blended layer to what the calls before it summed. Accum and AccumRoughness come in
 * (zero for the first call), the layer's tinted texture (projected from above, TileSize cm per repeat) and roughness
 * times Weight go on. Because a landscape's weight-blended layers sum to 1, chaining one call per layer gives the exact mix.
 */
void BuildTerrainFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainMaterial::BuildTerrainFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila terrain layer: adds Weight times a tinted, top-projected texture and roughness to Accum."));
	auto* AccumDefault = G.New<UMaterialExpressionConstant3Vector>(-1500);
	AccumDefault->Constant = FLinearColor(0.0f, 0.0f, 0.0f);
	auto* RoughDefault = G.New<UMaterialExpressionConstant>(-1500);
	RoughDefault->R = 0.0f;
	auto* WeightDefault = G.New<UMaterialExpressionConstant>(-1500);
	WeightDefault->R = 1.0f;
	TMap<FString, UMaterialExpression*> In;
	In.Add(TEXT("Accum"), G.Input(TEXT("Accum"), FunctionInput_Vector3, AccumDefault, TEXT("The colour summed so far: the previous layer's call, or nothing for the first.")));
	In.Add(TEXT("AccumRoughness"), G.Input(TEXT("AccumRoughness"), FunctionInput_Scalar, RoughDefault, TEXT("The roughness summed so far.")));
	In.Add(TEXT("Weight"), G.Input(TEXT("Weight"), FunctionInput_Scalar, WeightDefault, TEXT("The layer's weight: a Landscape Layer Sample of its name.")));
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	In.Add(TEXT("Tint"), G.VectorInput(TEXT("LayerTint"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Layer"), TEXT("Layer colour, multiplied with Texture.")));
	In.Add(TEXT("Roughness"), G.ScalarInput(TEXT("LayerRoughness"), 0.85f, TEXT("Layer"), TEXT("Layer roughness.")));
	In.Add(TEXT("Texture"), G.TextureInput(TEXT("LayerTexture"), White, SAMPLERTYPE_Color, TEXT("Layer"), TEXT("Layer colour texture, projected from above.")));
	UMaterialExpressionFunctionInput* TileSize = G.ScalarInput(TEXT("LayerTileSize"), 400.0f, TEXT("Layer"), TEXT("Texture repeat, cm."));
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

	auto Custom = [&](const FString& Description, const TCHAR* Code, ECustomMaterialOutputType Type, std::initializer_list<const TCHAR*> Names) {
		auto* E = G.New<UMaterialExpressionCustom>(-400);
		E->Description = Description;
		E->Code = Code;
		E->OutputType = Type;
		E->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			E->Inputs.AddDefaulted_GetRef().InputName = Name;
		}
		for (const TCHAR* Name : Names)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(In[Name], TEXT(""), E, Name);
		}
		return E;
	};
	const FString Version = FString::Printf(TEXT("%s%d"), TerrainVersionTag, TerrainFunctionVersion);
	auto* Color = Custom(Version + TEXT(" colour"), TerrainColorCode, CMOT_Float3, {TEXT("Accum"), TEXT("Weight"), TEXT("Tint"), TEXT("Texture"), TEXT("WorldTile")});
	auto* Rough = Custom(Version + TEXT(" roughness"), TerrainRoughnessCode, CMOT_Float1, {TEXT("AccumRoughness"), TEXT("Roughness"), TEXT("Weight")});
	G.Output(Color, TEXT("BaseColor"), TEXT("Accum with this layer's share added."));
	G.Output(Rough, TEXT("Roughness"), TEXT("AccumRoughness with this layer's share added."));
	G.Finish();
}

UMaterialFunction* TerrainFunction(TArray<FString>& OutWarnings)
{
	bool bCreated = false;
	UMaterialFunction* Function = LilaLink::FindOrCreateMaterialFunction(LilaLink::MastersFolderPath(), TerrainFunctionName, bCreated);
	if (!Function)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create %s in %s"), TerrainFunctionName, LilaLink::MastersFolderPath()));
		return nullptr;
	}
	if (bCreated || TerrainVersionOf(Function) < TerrainFunctionVersion)
	{
		BuildTerrainFunction(Function);
	}
	return Function;
}

/**
 * M_<Label>_Terrain: a Landscape Layer Sample per layer feeding a chain of MF_LilaTerrainLayer calls, each layer's tint,
 * roughness, texture and tile size as parameters (defaults from Lila), and with Rvt the same result into a Runtime
 * Virtual Texture Output. Rebuilt on every send so the defaults follow Lila's rules.
 */
UMaterial* TerrainMaterial(const FString& Label, const TArray<FTerrainLayer>& Layers, bool bRvt, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainMaterial::TerrainMaterial);

	UMaterialFunction* Function = TerrainFunction(OutWarnings);
	if (!Function)
	{
		return nullptr;
	}
	const FString Folder = FString(TerrainFolder) / Label;
	const FString Name = TEXT("M_") + Label + TEXT("_Terrain");
	bool bCreated = false;
	UMaterial* Material = LilaLink::FindOrCreateMaterial(Folder, Name, bCreated);
	if (!Material)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *Name, *Folder));
		return nullptr;
	}
	Material->Modify();
	UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
	int32 Row = 0;
	auto Make = [&](TSubclassOf<UMaterialExpression> Class, int32 X) {
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, (Row++) * 140);
	};
	auto* Zero = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -1200));
	Zero->Constant = FLinearColor(0.0f, 0.0f, 0.0f);
	auto* ZeroRough = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass(), -1200));
	ZeroRough->R = 0.0f;
	UMaterialExpression* Color = Zero;
	UMaterialExpression* Rough = ZeroRough;
	FString ColorOut, RoughOut;
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	for (int32 Index = 0; Index < Layers.Num(); ++Index)
	{
		const FTerrainLayer& L = Layers[Index];
		const FString LayerName = L.Name.ToString();
		auto* Sample = Cast<UMaterialExpressionLandscapeLayerSample>(Make(UMaterialExpressionLandscapeLayerSample::StaticClass(), -900));
		Sample->ParameterName = L.Name;
		Sample->PreviewWeight = Index == 0 ? 1.0f : 0.0f;
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -900));
		Tint->ParameterName = *(LayerName + TEXT("Tint"));
		Tint->DefaultValue = L.Color;
		Tint->Group = *LayerName;
		auto* LayerRough = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -900));
		LayerRough->ParameterName = *(LayerName + TEXT("Roughness"));
		LayerRough->DefaultValue = L.Roughness;
		LayerRough->Group = *LayerName;
		auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -900));
		Texture->ParameterName = *(LayerName + TEXT("Texture"));
		Texture->Texture = White;
		Texture->Group = *LayerName;
		auto* Tile = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -900));
		Tile->ParameterName = *(LayerName + TEXT("TileSize"));
		Tile->DefaultValue = L.TileSize;
		Tile->Group = *LayerName;

		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Make(UMaterialExpressionMaterialFunctionCall::StaticClass(), -400 + Index * 300));
		Call->SetMaterialFunction(Function);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, ColorOut, Call, TEXT("Accum"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, RoughOut, Call, TEXT("AccumRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sample, TEXT(""), Call, TEXT("Weight"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Call, TEXT("LayerTint"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(LayerRough, TEXT(""), Call, TEXT("LayerRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Texture, TEXT(""), Call, TEXT("LayerTexture"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tile, TEXT(""), Call, TEXT("LayerTileSize"));
		Color = Call;
		Rough = Call;
		ColorOut = TEXT("BaseColor");
		RoughOut = TEXT("Roughness");
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Color, ColorOut, MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, RoughOut, MP_Roughness);
	if (bRvt)
	{
		UMaterialExpression* Out = Make(UMaterialExpressionRuntimeVirtualTextureOutput::StaticClass(), 600);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, ColorOut, Out, TEXT("BaseColor"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, RoughOut, Out, TEXT("Roughness"));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	OutCreated.AddUnique(Material->GetPathName());
	OutCreated.AddUnique(Function->GetPathName());
	return Material;
}

URuntimeVirtualTexture* TerrainRvt(const FString& Label, TArray<FString>& OutCreated)
{
	const FString Folder = FString(TerrainFolder) / Label;
	const FString Name = TEXT("RVT_") + Label;
	const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	if (URuntimeVirtualTexture* Existing = Data.IsValid() ? Cast<URuntimeVirtualTexture>(Data.GetAsset()) : nullptr)
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(*(Folder / Name));
	if (!Package)
	{
		return nullptr;
	}
	// The default material type (base colour, normal, roughness, specular) holds what the terrain material writes.
	URuntimeVirtualTexture* Rvt = NewObject<URuntimeVirtualTexture>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Rvt);
	Package->MarkPackageDirty();
	OutCreated.AddUnique(Rvt->GetPathName());
	return Rvt;
}

/** The volume tagged for this landscape, made when missing, fitted to the landscape's bounds in its rotation. */
ARuntimeVirtualTextureVolume* TerrainRvtVolume(UWorld* World, ALandscape* Landscape, URuntimeVirtualTexture* Rvt, FName Tag)
{
	ARuntimeVirtualTextureVolume* Volume = nullptr;
	for (TActorIterator<ARuntimeVirtualTextureVolume> It(World); It && !Volume; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Volume = *It;
		}
	}
	if (!Volume)
	{
		Volume = World->SpawnActor<ARuntimeVirtualTextureVolume>();
		if (!Volume)
		{
			return nullptr;
		}
		Volume->Tags.AddUnique(Tag);
		Volume->SetActorLabel(Landscape->GetActorLabel() + TEXT("_RVT"));
	}
	Volume->Modify();
	URuntimeVirtualTextureComponent* Component = Volume->VirtualTextureComponent;
	Component->SetVirtualTexture(Rvt);
	Component->SetBoundsAlignActor(Landscape);
	const FQuat Rotation = Landscape->GetActorQuat();
	const FTransform Local(Rotation, Landscape->GetActorLocation());
	const FBox Bounds = Landscape->GetCompleteBounds().TransformBy(Local.Inverse()).ExpandBy(FVector(0.0, 0.0, 1.0));
	Volume->SetActorTransform(FTransform(Rotation, Local.TransformPosition(Bounds.Min), Bounds.GetSize()));
	return Volume;
}

TSharedPtr<FJsonValue> SetTerrainMaterial(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainMaterial::SetTerrainMaterial);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setTerrainMaterial: give id");
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id);
	Params->TryGetStringField(TEXT("label"), Label);
	bool bRvt = false, bAssign = true;
	Params->TryGetBoolField(TEXT("rvt"), bRvt);
	Params->TryGetBoolField(TEXT("assign"), bAssign);
	TArray<FTerrainLayer> Layers;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Params->TryGetArrayField(TEXT("layers"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			FString Name;
			if (!O.IsValid() || !O->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				continue;
			}
			FTerrainLayer& L = Layers.AddDefaulted_GetRef();
			L.Name = FName(*Name);
			const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
			if (O->TryGetArrayField(TEXT("color"), Color) && Color->Num() == 3)
			{
				L.Color = FLinearColor(float((*Color)[0]->AsNumber()), float((*Color)[1]->AsNumber()), float((*Color)[2]->AsNumber()));
			}
			double Number = 0.0;
			if (O->TryGetNumberField(TEXT("roughness"), Number))
			{
				L.Roughness = float(Number);
			}
			if (O->TryGetNumberField(TEXT("tileSize"), Number) && Number > 0.0)
			{
				L.TileSize = float(Number);
			}
		}
	}
	if (Layers.IsEmpty())
	{
		OutError = TEXT("setTerrainMaterial: give layers");
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("setTerrainMaterial: no editor world");
		return nullptr;
	}
	const FName Tag(*(TEXT("LilaLink.Landscape:") + Id.Right(900)));
	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(World); It && !Landscape; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Landscape = *It;
		}
	}
	if (!Landscape)
	{
		OutError = TEXT("setTerrainMaterial: no landscape for ") + Id + TEXT("; send the terrain first");
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetTerrainMaterial", "Lila: Terrain Material"));
	TArray<FString> Created, Warnings;
	TArray<TSharedPtr<FJsonValue>> LayerNames;
	const TMap<FName, FLandscapeTargetLayerSettings>& Targets = Landscape->GetTargetLayers();
	for (const FTerrainLayer& L : Layers)
	{
		const FLandscapeTargetLayerSettings* Target = Targets.Find(L.Name);
		ULandscapeLayerInfoObject* Info = Target ? Target->LayerInfoObj.Get() : nullptr;
		if (!Info)
		{
			Warnings.Add(TEXT("the landscape has no layer info for ") + L.Name.ToString());
			continue;
		}
		if (Info->GetBlendMethod() != ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending)
		{
			Info->SetBlendMethod(ELandscapeTargetLayerBlendMethod::PremultipliedAlphaBlending, /*bInModify=*/true);
		}
		LayerNames.Add(MakeShared<FJsonValueString>(L.Name.ToString()));
	}

	UMaterial* Material = nullptr;
	URuntimeVirtualTexture* Rvt = nullptr;
	ARuntimeVirtualTextureVolume* Volume = nullptr;
	if (bAssign)
	{
		Material = TerrainMaterial(Label, Layers, bRvt, Created, Warnings);
		if (Material && Landscape->LandscapeMaterial != Material)
		{
			Landscape->Modify();
			Landscape->LandscapeMaterial = Material;
			FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial)));
			Landscape->PostEditChangeProperty(Changed);
		}
	}
	else
	{
		Warnings.Add(TEXT("the landscape keeps its own material; it needs a Landscape Layer Sample per layer (MF_LilaTerrainLayer takes each)"));
		TerrainFunction(Warnings);
	}
	if (bRvt)
	{
		Rvt = TerrainRvt(Label, Created);
		if (!Rvt)
		{
			Warnings.Add(TEXT("could not create RVT_") + Label);
		}
		else
		{
			if (!Landscape->RuntimeVirtualTextures.Contains(Rvt))
			{
				Landscape->Modify();
				Landscape->RuntimeVirtualTextures.Add(Rvt);
				FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, RuntimeVirtualTextures)));
				Landscape->PostEditChangeProperty(Changed);
			}
			Volume = TerrainRvtVolume(World, Landscape, Rvt, Tag);
			if (!Volume)
			{
				Warnings.Add(TEXT("could not place a Runtime Virtual Texture Volume"));
			}
			if (!bAssign)
			{
				Warnings.Add(TEXT("rvt: your material needs a Runtime Virtual Texture Output for RVT_") + Label + TEXT(" to hold anything"));
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("material"), Material ? Material->GetPathName() : FString());
	Out->SetStringField(TEXT("function"), FString(LilaLink::MastersFolderPath()) / TerrainFunctionName);
	Out->SetStringField(TEXT("rvt"), Rvt ? Rvt->GetPathName() : FString());
	Out->SetStringField(TEXT("volume"), Volume ? Volume->GetActorLabel() : FString());
	Out->SetArrayField(TEXT("layers"), LayerNames);
	TArray<TSharedPtr<FJsonValue>> CreatedValues, WarningValues;
	for (const FString& C : Created)
	{
		CreatedValues.Add(MakeShared<FJsonValueString>(C));
	}
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
		UE_LOG(LogLilaLink, Warning, TEXT("setTerrainMaterial: %s"), *W);
	}
	Out->SetArrayField(TEXT("assets"), CreatedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setTerrainMaterial: %s, %d layers%s"), *Label, LayerNames.Num(), bRvt ? TEXT(", RVT") : TEXT(""));
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

UMaterialFunction* LilaLink::TerrainLayerFunction(TArray<FString>& OutWarnings)
{
	return TerrainFunction(OutWarnings);
}

void LilaLink::AddTerrainMaterialMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setTerrainMaterial"), LilaLink::Sync(&SetTerrainMaterial));
}
