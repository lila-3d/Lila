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
#include "LandscapeComponent.h"
#include "LandscapeEditLayer.h"
#include "LandscapeHeightfieldCollisionComponent.h"
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
#include "Materials/MaterialExpressionVertexColor.h"
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
constexpr int32 FunctionVersion = 2;
const TCHAR* LandscapeFolder = TEXT("/Game/LilaLink/Landscapes");
const FLinearColor GroundColorDefault(0.25f, 0.22f, 0.18f);
constexpr float GroundRoughnessDefault = 0.9f;

// Triplanar, optionally hex-tiled (Mikkelsen) to break up repeats on big faces, darkened in cavities by the vertex colour
// Lila's cliff meshes carry (G is openness: white, or no colour, leaves it as it is).
const TCHAR* TriplanarCode = TEXT(R"(
struct FLilaHex
{
	float2 Hash(float2 p) { return frac(sin(float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)))) * 43758.5453); }
	float3 Sample(Texture2D T, SamplerState S, float2 uv, float hex)
	{
		float2 dx = ddx(uv), dy = ddy(uv);
		if (hex < 0.5)
			return T.SampleGrad(S, uv, dx, dy).rgb;
		float2 st = uv * 3.4641016;
		float2 sk = float2(st.x, -0.57735027 * st.x + 1.15470054 * st.y);
		float2 base = floor(sk);
		float3 f = float3(frac(sk), 0.0);
		f.z = 1.0 - f.x - f.y;
		float s = step(0.0, -f.z);
		float s2 = 2.0 * s - 1.0;
		float3 w = float3(-f.z * s2, s - f.y * s2, s - f.x * s2);
		float2 v1 = base + float2(s, s), v2 = base + float2(s, 1.0 - s), v3 = base + float2(1.0 - s, s);
		w = pow(max(w, 0.0), 7.0);
		w /= max(w.x + w.y + w.z, 1e-5);
		return T.SampleGrad(S, uv + Hash(v1), dx, dy).rgb * w.x + T.SampleGrad(S, uv + Hash(v2), dx, dy).rgb * w.y + T.SampleGrad(S, uv + Hash(v3), dx, dy).rgb * w.z;
	}
};
FLilaHex L;
float3 w = pow(abs(Normal) + 1e-4, max(Sharpness, 1.0));
w /= (w.x + w.y + w.z);
float3 c = 0;
if (w.x > 0.01) c += L.Sample(Texture, TextureSampler, WorldTile.yz, HexTile) * w.x;
if (w.y > 0.01) c += L.Sample(Texture, TextureSampler, WorldTile.xz, HexTile) * w.y;
if (w.z > 0.01) c += L.Sample(Texture, TextureSampler, WorldTile.xy, HexTile) * w.z;
return Tint.rgb * c * lerp(1.0, Masks.g, saturate(CavityStrength));
)");

// Mobile (ES3.1): biplanar, the two strongest projections only, no hex tiling.
const TCHAR* BiplanarCode = TEXT(R"(
float3 a = abs(Normal);
float2 uvA, uvB;
float2 w;
if (a.z >= a.x && a.z >= a.y) { uvA = WorldTile.xy; w.x = a.z; if (a.x > a.y) { uvB = WorldTile.yz; w.y = a.x; } else { uvB = WorldTile.xz; w.y = a.y; } }
else if (a.x >= a.y) { uvA = WorldTile.yz; w.x = a.x; if (a.z > a.y) { uvB = WorldTile.xy; w.y = a.z; } else { uvB = WorldTile.xz; w.y = a.y; } }
else { uvA = WorldTile.xz; w.x = a.y; if (a.z > a.x) { uvB = WorldTile.xy; w.y = a.z; } else { uvB = WorldTile.yz; w.y = a.x; } }
w = pow(w, max(Sharpness, 1.0));
w /= max(w.x + w.y, 1e-5);
float3 c = Texture2DSample(Texture, TextureSampler, uvA).rgb * w.x + Texture2DSample(Texture, TextureSampler, uvB).rgb * w.y;
return Tint.rgb * c * lerp(1.0, Masks.g, saturate(CavityStrength));
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
	In.Add(TEXT("HexTile"), G.ScalarInput(TEXT("GroundHexTile"), 0.0f, TEXT("Ground"), TEXT("1 breaks up tiling with hex-tile stochastic sampling (off on mobile).")));
	In.Add(TEXT("CavityStrength"), G.ScalarInput(TEXT("CavityStrength"), 0.5f, TEXT("Ground"), TEXT("How much cavities (vertex colour G, 1 open) darken the colour.")));
	UMaterialExpression* VertexColor = G.Make(UMaterialExpressionVertexColor::StaticClass(), -1600);
	In.Add(TEXT("Masks"), VertexColor);
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

	auto Custom = [&](const TCHAR* Suffix, const TCHAR* Code, std::initializer_list<const TCHAR*> Names) {
		auto* E = G.New<UMaterialExpressionCustom>(-400);
		E->Description = FString::Printf(TEXT("%s%d %s"), VersionTag, FunctionVersion, Suffix);
		E->Code = Code;
		E->OutputType = CMOT_Float3;
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
	auto* Full = Custom(TEXT("colour"), TriplanarCode,
		{TEXT("Tint"), TEXT("Texture"), TEXT("WorldTile"), TEXT("Normal"), TEXT("Sharpness"), TEXT("HexTile"), TEXT("CavityStrength"), TEXT("Masks")});
	auto* Mobile = Custom(TEXT("colour mobile"), BiplanarCode,
		{TEXT("Tint"), TEXT("Texture"), TEXT("WorldTile"), TEXT("Normal"), TEXT("Sharpness"), TEXT("CavityStrength"), TEXT("Masks")});
	G.Output(G.FeatureLevelSwitch(Full, Mobile), TEXT("BaseColor"), TEXT("The projected, tinted texture (triplanar; biplanar on mobile)."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness."));
	G.Output(VertexColor, TEXT("Strata"), TEXT("Cliff meshes: where in its rock layer a point is, 0 bottom to 1 top (vertex colour R)."), 1);
	G.Output(VertexColor, TEXT("Openness"), TEXT("Cliff meshes: 1 open, 0 deep in a cavity (vertex colour G)."), 2);
	G.Output(VertexColor, TEXT("UpFacing"), TEXT("Cliff meshes: 1 on ledges and tops (vertex colour B)."), 3);
	G.Output(VertexColor, TEXT("ScatterZones"), TEXT("Cliff meshes: where scatter can sit (vertex colour A)."), 4);
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

/** MI_<Label><Suffix> on the master, coloured like the landscape's own material where it has GroundColor and GroundRoughness. */
UMaterialInterface* VolumeInstance(const FString& Label, UMaterialInterface* LandscapeMaterial, TArray<FString>& OutCreated, TArray<FString>& OutWarnings,
	const TCHAR* Suffix = TEXT("_Volume"))
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::VolumeInstance);

	UMaterial* Parent = Master(OutCreated, OutWarnings);
	if (!Parent)
	{
		return nullptr;
	}
	const FString Folder = FString(LandscapeFolder) / Label;
	const FString Name = TEXT("MI_") + Label + Suffix;
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
 * Merges the edit layers now and rebuilds every component's collision from the result. The merge refreshes the collision
 * layer data but the editor never rebuilds the physics heightfield from it, so without this, traces still hit the holes.
 */
void RebuildCollisionAfterMerge(ALandscape* Landscape)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::RebuildCollisionAfterMerge);

	Landscape->ForceUpdateLayersContent();
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info)
	{
		return;
	}
	Info->ForEachLandscapeProxy([](ALandscapeProxy* Proxy) {
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			if (!Component)
			{
				continue;
			}
			Component->UpdateCollisionLayerData();
			if (ULandscapeHeightfieldCollisionComponent* Collision = Component->GetCollisionComponent())
			{
				Collision->RecreateCollision();
			}
		}
		return true;
	});
}

/**
 * Holes exist only through a Landscape Visibility Mask in the landscape (or hole) material: without one Unreal drops the
 * visibility data, collision included. A material Lila generated gets one on its opacity mask; anyone else's is left
 * alone and reported. Returns the material that now draws the holes.
 */
UMaterialInterface* EnsureHoleMaterial(ALandscape* Landscape, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::EnsureHoleMaterial);

	UMaterialInterface* Used = Landscape->LandscapeHoleMaterial ? Landscape->LandscapeHoleMaterial.Get() : Landscape->LandscapeMaterial.Get();
	UMaterial* Base = Used ? Used->GetMaterial() : nullptr;
	if (!Base)
	{
		OutWarnings.Add(TEXT("the landscape has no material, so its holes were not cut (Unreal keeps holes only through a Landscape Visibility Mask)"));
		return nullptr;
	}
	if (Base->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionLandscapeVisibilityMask>())
	{
		return Used;
	}
	if (!Base->GetPathName().StartsWith(LandscapeFolder))
	{
		OutWarnings.Add(FString::Printf(TEXT("%s has no Landscape Visibility Mask, so the holes were not cut; add one on Opacity Mask (Masked) "
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
		// The engine adds the visibility target layer on load only, so a landscape made this session lacks it and its holes
		// would never merge.
		const FName VisibilityName = ALandscapeProxy::VisibilityLayer->GetLayerName();
		if (!Landscape->HasTargetLayer(VisibilityName))
		{
			Landscape->AddTargetLayer(VisibilityName, FLandscapeTargetLayerSettings(ALandscapeProxy::VisibilityLayer));
			Info->UpdateLayerInfoMap(Landscape);
		}
		// The mask first: Unreal skips visibility data for components whose material has none.
		UMaterialInterface* HoleDrawer = nullptr;
		if (Holes > 0)
		{
			HoleDrawer = EnsureHoleMaterial(Landscape, Warnings);
			HoleMaterial = HoleDrawer ? HoleDrawer->GetPathName() : FString();
		}
		else
		{
			UMaterialInterface* Used = Landscape->LandscapeHoleMaterial ? Landscape->LandscapeHoleMaterial.Get() : Landscape->LandscapeMaterial.Get();
			HoleDrawer = Used && Used->GetMaterial()->HasAnyExpressionsInMaterialAndFunctionsOfType<UMaterialExpressionLandscapeVisibilityMask>() ? Used : nullptr;
		}
		if (HoleDrawer)
		{
			{
				FScopedSetLandscapeEditingLayer Scope(Landscape, Layer->GetGuid(), [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
				FLandscapeEditDataInterface Edit(Info, Layer->GetGuid());
				Edit.SetAlphaData(ALandscapeProxy::VisibilityLayer, Extent.Min.X, Extent.Min.Y, Extent.Max.X, Extent.Max.Y, Bytes.GetData(), 0);
			}
			RebuildCollisionAfterMerge(Landscape);
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
/** setCliffMaterial: MI_<Label>_Cliffs on M_LilaTerrainVolume on every slot of the meshes, coloured like landscapeMaterial when given. */
TSharedPtr<FJsonValue> SetCliffMaterial(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainVolume::SetCliffMaterial);

	FString Label;
	if (!Params->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		OutError = TEXT("setCliffMaterial: label is required");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	UMaterialInterface* LandscapeMaterial = nullptr;
	FString LandscapePath;
	if (Params->TryGetStringField(TEXT("landscapeMaterial"), LandscapePath) && !LandscapePath.IsEmpty())
	{
		LandscapeMaterial = LoadObject<UMaterialInterface>(nullptr, *LandscapePath);
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetCliffMaterial", "Lila: Cliff Material"));
	UMaterialInterface* Material = VolumeInstance(Label, LandscapeMaterial, Created, Warnings, TEXT("_Cliffs"));
	TArray<TSharedPtr<FJsonValue>> MeshValues;
	const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
	if (Material && Params->TryGetArrayField(TEXT("meshes"), Meshes))
	{
		for (const TSharedPtr<FJsonValue>& V : *Meshes)
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *V->AsString());
			if (!Mesh)
			{
				Warnings.Add(TEXT("no static mesh ") + V->AsString());
				continue;
			}
			Mesh->Modify();
			for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
			{
				Mesh->SetMaterial(Slot, Material);
			}
			Mesh->MarkPackageDirty();
			MeshValues.Add(MakeShared<FJsonValueString>(Mesh->GetPathName()));
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("material"), Material ? Material->GetPathName() : FString());
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
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddTerrainVolumeMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setLandscapeVolume"), LilaLink::Sync(&SetLandscapeVolume));
	Methods.Add(TEXT("setCliffMaterial"), LilaLink::Sync(&SetCliffMaterial));
}
