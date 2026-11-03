#include "LilaLinkMasonry.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
/** Raised when the graph changes; a master older than this is kept (artists own it) and reported, until regenerate. */
constexpr int32 MasterVersion = 2;
const TCHAR* MasterName = TEXT("M_LilaMasonry");
const TCHAR* FunctionName = TEXT("MF_LilaMasonry");
const TCHAR* NeutralName = TEXT("T_LilaMasonryNeutral");
const TCHAR* VersionTag = TEXT("Lila Masonry v");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

template <class T> T* AssetAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** Shared by every masonry slot: the per-stone random, height, age, damage, wet and moss, wherever this mesh carries them. */
const TCHAR* StoneDataCode = TEXT(R"(
float rnd = 0.5;
float h = saturate(LocalPos.z / max(WallHeight, 1.0));
float age = 0.0;
float dmg = 0.0;
float wet = 0.0;
float moss = 0.0;
if (StoneData > 1.5)
{
    rnd = VColor.r;
    h = VColor.g;
    moss = VColor.b;
    wet = VColor.a;
    age = UV2.x;
    dmg = UV2.y;
}
else if (StoneData > 0.5)
{
    rnd = InstRandom;
    h = InstHeight;
    age = InstAge;
    dmg = InstDamage;
    wet = InstWet;
    moss = InstMoss;
}
)");

const TCHAR* ColorCode = TEXT(R"(
float3 col = Tint.rgb;
if (HasBaseColorMap > 0.5)
{
    if (WorldAligned > 0.5)
    {
        float3 n = normalize(NormalWS);
        float3 w = pow(abs(n), 4.0);
        w /= max(w.x + w.y + w.z, 1e-6);
        float3 p = WorldTile;
        col *= Texture2DSample(BaseColorMap, BaseColorMapSampler, p.yz).rgb * w.x + Texture2DSample(BaseColorMap, BaseColorMapSampler, p.xz).rgb * w.y
            + Texture2DSample(BaseColorMap, BaseColorMapSampler, p.xy).rgb * w.z;
    }
    else
    {
        col *= Texture2DSample(BaseColorMap, BaseColorMapSampler, UV1 * TextureScale + rnd * float2(7.31, 3.17) * RandomOffset).rgb;
    }
}
col *= 1.0 + (rnd - 0.5) * 2.0 * ValueSpread;
col = lerp(col, col * SpreadColor.rgb, saturate(abs(rnd - 0.5) * 2.0) * ColorSpread);
float curv = HasBakedMaps > 0.5 ? Texture2DSample(BakedCurvatureMap, BakedCurvatureMapSampler, UV0).r : 0.5;
col *= 1.0 + max(curv - 0.5, 0.0) * 2.0 * EdgeLighten;
col = lerp(col, col * CavityColor.rgb, saturate((0.5 - curv) * 2.0) * CavityStrength);
float dirt = saturate((DirtHeight - h) / max(DirtFade, 1e-3)) * DirtStrength;
col = lerp(col, col * DirtColor.rgb, dirt);
col *= 1.0 - age * AgeDarken;
col = lerp(col, DamageColor.rgb, saturate(dmg * DamageStrength));
col *= 1.0 - wet * WetDarken;
float up = saturate(normalize(NormalWS).z);
float m = saturate(moss * MossStrength * lerp(1.0, up, MossUp));
return lerp(col, MossColor.rgb, m);
)");

const TCHAR* RoughnessCode = TEXT(R"(
float curv = HasBakedMaps > 0.5 ? Texture2DSample(BakedCurvatureMap, BakedCurvatureMapSampler, UV0).r : 0.5;
float dirt = saturate((DirtHeight - h) / max(DirtFade, 1e-3)) * DirtStrength;
return saturate(BaseRoughness + (0.5 - curv) * CurvatureRoughness + (rnd - 0.5) * RoughnessSpread - dirt * 0.1 - wet * WetRoughness + moss * 0.1);
)");

const TCHAR* OcclusionCode = TEXT(R"(
float ao = HasBakedMaps > 0.5 ? Texture2DSample(BakedAOMap, BakedAOMapSampler, UV0).r : 1.0;
return lerp(1.0, ao, AOStrength);
)");

/** Tangent space: the baked normal (far stones, shells) under the tiling detail normal, RNM blended. BC5 keeps only xy. */
const TCHAR* NormalCode = TEXT(R"(
float3 nrm = float3(0, 0, 1);
if (UseBakedNormal > 0.5 && HasBakedMaps > 0.5)
{
    float2 xy = Texture2DSample(BakedNormalMap, BakedNormalMapSampler, UV0).xy * 2.0 - 1.0;
    nrm = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}
if (HasDetailNormal > 0.5 && WorldAligned < 0.5)
{
    float2 dxy = (Texture2DSample(DetailNormalMap, DetailNormalMapSampler, UV1 * TextureScale).xy * 2.0 - 1.0) * DetailNormalStrength;
    float3 d = float3(dxy, sqrt(saturate(1.0 - dot(dxy, dxy))));
    float3 n1 = nrm + float3(0, 0, 1);
    float3 n2 = d * float3(-1, -1, 1);
    nrm = n1 * dot(n1, n2) / n1.z - n2;
}
return normalize(nrm);
)");

/** A 4x4 mid grey linear Masks texture: the default of the baked AO and curvature parameters, whose sampler type it matches. */
UTexture* NeutralMask(TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::NeutralMask);

	const FString Folder = LilaLink::MastersFolderPath();
	if (UTexture* Existing = AssetAt<UTexture>(Folder, NeutralName))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(*(Folder / NeutralName));
	UTexture2D* Texture = NewObject<UTexture2D>(Package, NeutralName, RF_Public | RF_Standalone | RF_Transactional);
	TArray<uint8> Pixels;
	Pixels.Init(128, 4 * 4 * 4);
	for (int32 K = 3; K < Pixels.Num(); K += 4)
	{
		Pixels[K] = 255;
	}
	Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
	Texture->SRGB = false;
	Texture->CompressionSettings = TC_Masks;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/** The version a master's function was built at, from its Custom nodes' description; 0 when none says. */
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
 * MF_LilaMasonry: per-mesh inputs and the parameter inputs feeding four Custom nodes, one per output (BaseColor,
 * Roughness, AmbientOcclusion, tangent-space Normal). Every parameter input defaults to the parameter of its name.
 */
void BuildFunction(UMaterialFunction* Function, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::BuildFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Masonry: per-stone colour spread, cavity and edge wear from baked maps, dirt by height, and age, damage, wet and moss per stone."));
	TMap<FString, UMaterialExpression*> Inputs;
	auto Scalar = [&](const TCHAR* Param, float Value, const TCHAR* Description) { Inputs.Add(Param, G.ScalarInput(Param, Value, TEXT(""), Description)); };
	auto Vector = [&](const TCHAR* Param, const FLinearColor& Value, const TCHAR* Description) {
		Inputs.Add(Param, G.VectorInput(Param, Value, TEXT(""), Description));
	};
	auto Texture = [&](const TCHAR* Param, UTexture* Default, EMaterialSamplerType Type, const TCHAR* Description) {
		Inputs.Add(Param, G.TextureInput(Param, Default, Type, TEXT(""), Description));
	};
	auto Coord = [&](const TCHAR* Name, int32 Index, const TCHAR* Description) {
		auto* E = G.New<UMaterialExpressionTextureCoordinate>(-1500);
		E->CoordinateIndex = Index;
		Inputs.Add(Name, G.Input(Name, FunctionInput_Vector2, E, Description));
	};
	auto InstanceData = [&](const TCHAR* Name, int32 Index, float Default) {
		auto* E = G.New<UMaterialExpressionPerInstanceCustomData>(-1200);
		E->DataIndex = Index;
		E->ConstDefaultValue = Default;
		Inputs.Add(Name, E);
	};

	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	UTexture* Neutral = NeutralMask(OutCreated);
	Texture(TEXT("BaseColorMap"), White, SAMPLERTYPE_Color, TEXT("Stone colour texture, on UV1 or projected in world space."));
	Texture(TEXT("DetailNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Tiling detail normal on UV1."));
	Texture(TEXT("BakedNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Baked normal on UV0, for far stones and shells."));
	Texture(TEXT("BakedAOMap"), Neutral, SAMPLERTYPE_Masks, TEXT("Baked ambient occlusion on UV0."));
	Texture(TEXT("BakedCurvatureMap"), Neutral, SAMPLERTYPE_Masks, TEXT("Baked curvature on UV0, 0.5 flat."));
	Coord(TEXT("UV0"), 0, TEXT("UVs of the baked maps; UV channel 0 when unconnected."));
	Coord(TEXT("UV1"), 1, TEXT("UVs of the tiling textures; UV channel 1 when unconnected."));
	Coord(TEXT("UV2"), 2, TEXT("Merged stones' age (x) and damage (y); UV channel 2 when unconnected."));
	// World position is large-world (double) and a Custom node takes floats: divide and wrap it with nodes that handle that first.
	{
		UMaterialExpression* World = G.Make(UMaterialExpressionWorldPosition::StaticClass(), -1600);
		auto* Divide = G.New<UMaterialExpressionDivide>(-1400);
		UMaterialEditingLibrary::ConnectMaterialExpressions(World, TEXT(""), Divide, TEXT("A"));
		auto* Wrap = G.New<UMaterialExpressionFmod>(-1300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Divide, TEXT(""), Wrap, TEXT("A"));
		auto* Period = G.New<UMaterialExpressionConstant>(-1400);
		Period->R = 256.0f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Period, TEXT(""), Wrap, TEXT("B"));
		Inputs.Add(TEXT("WorldTile"), Wrap);
		Inputs.Add(TEXT("TileDivide"), Divide);
	}
	Inputs.Add(TEXT("LocalPos"), G.Make(UMaterialExpressionLocalPosition::StaticClass(), -1200));
	Inputs.Add(TEXT("NormalWS"), G.Make(UMaterialExpressionVertexNormalWS::StaticClass(), -1200));
	Inputs.Add(TEXT("VColor"), G.Make(UMaterialExpressionVertexColor::StaticClass(), -1200));
	InstanceData(TEXT("InstRandom"), 0, 0.5f);
	InstanceData(TEXT("InstHeight"), 1, 0.5f);
	InstanceData(TEXT("InstAge"), 2, 0.0f);
	InstanceData(TEXT("InstDamage"), 3, 0.0f);
	InstanceData(TEXT("InstWet"), 4, 0.0f);
	InstanceData(TEXT("InstMoss"), 5, 0.0f);

	// Switches, as scalars so one master serves every slot without a shader permutation each.
	Scalar(TEXT("StoneData"), 1.0f, TEXT("Per-stone data: 0 none (height from the mesh), 1 per instance custom data 0-5 (random, height, age, damage, wet, moss), 2 vertex colour (random, height, moss, wet) and UV2 (age, damage)."));
	Scalar(TEXT("WorldAligned"), 0.0f, TEXT("1 projects BaseColorMap in world space (cores, shells) instead of on UV1."));
	Scalar(TEXT("UseBakedNormal"), 0.0f, TEXT("1 for far stones and shells, whose geometry lacks the detail."));
	Scalar(TEXT("HasBakedMaps"), 0.0f, TEXT("1 when the baked maps are set."));
	Scalar(TEXT("HasBaseColorMap"), 0.0f, TEXT("1 when BaseColorMap is set."));
	Scalar(TEXT("HasDetailNormal"), 0.0f, TEXT("1 when DetailNormalMap is set."));
	Scalar(TEXT("WallHeight"), 200.0f, TEXT("Wall height in cm, for dirt by height when StoneData is 0."));
	Scalar(TEXT("TextureScale"), 1.0f, TEXT("Tiling of the UV1 textures."));
	Scalar(TEXT("TileSize"), 100.0f, TEXT("World-aligned projection size in cm."));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[TEXT("TileSize")], TEXT(""), Inputs[TEXT("TileDivide")], TEXT("B"));
	Scalar(TEXT("RandomOffset"), 1.0f, TEXT("How far each stone's random shifts its UV1 texture."));
	Scalar(TEXT("ValueSpread"), 0.08f, TEXT("Per-stone brightness spread."));
	Scalar(TEXT("ColorSpread"), 0.15f, TEXT("Per-stone spread towards SpreadColor."));
	Scalar(TEXT("EdgeLighten"), 0.25f, TEXT("Lightens convex edges."));
	Scalar(TEXT("CavityStrength"), 0.5f, TEXT("Darkens cavities towards CavityColor."));
	Scalar(TEXT("DirtHeight"), 0.2f, TEXT("Height, 0 to 1 up the wall, below which dirt gathers."));
	Scalar(TEXT("DirtFade"), 0.15f, TEXT("Fade above DirtHeight."));
	Scalar(TEXT("DirtStrength"), 0.5f, TEXT("Dirt amount."));
	Scalar(TEXT("BaseRoughness"), 0.8f, TEXT("Roughness before curvature, spread and dirt."));
	Scalar(TEXT("CurvatureRoughness"), 0.3f, TEXT("Rougher in cavities, smoother on edges."));
	Scalar(TEXT("RoughnessSpread"), 0.1f, TEXT("Per-stone roughness spread."));
	Scalar(TEXT("AOStrength"), 1.0f, TEXT("How much of the baked AO shows."));
	Scalar(TEXT("DetailNormalStrength"), 1.0f, TEXT("Scales the detail normal."));
	Scalar(TEXT("AgeDarken"), 0.25f, TEXT("How much darker the oldest stones are."));
	Scalar(TEXT("DamageStrength"), 0.35f, TEXT("How far freshly broken stone shifts towards DamageColor."));
	Scalar(TEXT("WetDarken"), 0.3f, TEXT("How much darker wet stone is."));
	Scalar(TEXT("WetRoughness"), 0.35f, TEXT("How much smoother wet stone is."));
	Scalar(TEXT("MossStrength"), 1.0f, TEXT("Scales the moss mask."));
	Scalar(TEXT("MossUp"), 0.6f, TEXT("How much moss keeps to upward faces, 0 to 1."));
	Vector(TEXT("Tint"), FLinearColor(0.6f, 0.55f, 0.5f), TEXT("Stone colour, multiplied with BaseColorMap."));
	Vector(TEXT("SpreadColor"), FLinearColor(0.9f, 0.82f, 0.75f), TEXT("Colour some stones drift towards."));
	Vector(TEXT("CavityColor"), FLinearColor(0.55f, 0.52f, 0.48f), TEXT("Multiplied into cavities."));
	Vector(TEXT("DirtColor"), FLinearColor(0.45f, 0.42f, 0.35f), TEXT("Multiplied in where dirt gathers."));
	Vector(TEXT("DamageColor"), FLinearColor(0.72f, 0.68f, 0.62f), TEXT("Freshly broken stone, near what fell."));
	Vector(TEXT("MossColor"), FLinearColor(0.16f, 0.22f, 0.07f), TEXT("Moss."));

	const TArray<const TCHAR*> StoneInputs = {TEXT("StoneData"), TEXT("WallHeight"), TEXT("LocalPos"), TEXT("VColor"), TEXT("InstRandom"), TEXT("InstHeight"),
		TEXT("InstAge"), TEXT("InstDamage"), TEXT("InstWet"), TEXT("InstMoss"), TEXT("UV2")};
	auto With = [&](std::initializer_list<const TCHAR*> Extra) {
		TArray<const TCHAR*> All = StoneInputs;
		All.Append(Extra.begin(), int32(Extra.size()));
		return All;
	};
	auto CustomOf = [&](const FString& Description, const FString& Code, ECustomMaterialOutputType Type, const TArray<const TCHAR*>& Names) {
		auto* E = G.New<UMaterialExpressionCustom>(-400);
		E->Description = Description;
		E->Code = Code;
		E->OutputType = Type;
		E->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			FCustomInput& In = E->Inputs.AddDefaulted_GetRef();
			In.InputName = Name;
		}
		for (const TCHAR* Name : Names)
		{
			if (UMaterialExpression** From = Inputs.Find(Name))
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(*From, TEXT(""), E, Name);
			}
		}
		return E;
	};

	const FString Version = FString::Printf(TEXT("%s%d"), VersionTag, MasterVersion);
	auto* Color = CustomOf(Version + TEXT(" colour"), FString(StoneDataCode) + ColorCode, CMOT_Float3,
		With({TEXT("Tint"), TEXT("HasBaseColorMap"), TEXT("WorldAligned"), TEXT("NormalWS"), TEXT("WorldTile"), TEXT("BaseColorMap"),
			TEXT("UV1"), TEXT("TextureScale"), TEXT("RandomOffset"), TEXT("ValueSpread"), TEXT("SpreadColor"), TEXT("ColorSpread"), TEXT("HasBakedMaps"),
			TEXT("BakedCurvatureMap"), TEXT("UV0"), TEXT("EdgeLighten"), TEXT("CavityColor"), TEXT("CavityStrength"), TEXT("DirtHeight"), TEXT("DirtFade"),
			TEXT("DirtStrength"), TEXT("DirtColor"), TEXT("AgeDarken"), TEXT("DamageColor"), TEXT("DamageStrength"), TEXT("WetDarken"), TEXT("MossColor"),
			TEXT("MossStrength"), TEXT("MossUp")}));
	auto* Roughness = CustomOf(Version + TEXT(" roughness"), FString(StoneDataCode) + RoughnessCode, CMOT_Float1,
		With({TEXT("HasBakedMaps"), TEXT("BakedCurvatureMap"), TEXT("UV0"), TEXT("DirtHeight"), TEXT("DirtFade"), TEXT("DirtStrength"),
			TEXT("BaseRoughness"), TEXT("CurvatureRoughness"), TEXT("RoughnessSpread"), TEXT("WetRoughness")}));
	auto* Occlusion = CustomOf(Version + TEXT(" occlusion"), OcclusionCode, CMOT_Float1,
		{TEXT("HasBakedMaps"), TEXT("BakedAOMap"), TEXT("UV0"), TEXT("AOStrength")});
	auto* Normal = CustomOf(Version + TEXT(" normal"), NormalCode, CMOT_Float3,
		{TEXT("UseBakedNormal"), TEXT("HasBakedMaps"), TEXT("BakedNormalMap"), TEXT("UV0"), TEXT("HasDetailNormal"), TEXT("WorldAligned"),
			TEXT("DetailNormalMap"), TEXT("UV1"), TEXT("TextureScale"), TEXT("DetailNormalStrength")});

	G.Output(Color, TEXT("BaseColor"), TEXT("Stone colour."));
	G.Output(Roughness, TEXT("Roughness"), TEXT("Roughness."));
	G.Output(Occlusion, TEXT("AmbientOcclusion"), TEXT("Baked occlusion scaled by AOStrength."));
	G.Output(Normal, TEXT("Normal"), TEXT("Tangent-space normal."));
	G.Finish();
}

/** M_LilaMasonry: MF_LilaMasonry's four outputs on the material's pins. */
void BuildMaster(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::BuildMaster);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness},
			{TEXT("AmbientOcclusion"), MP_AmbientOcclusion}, {TEXT("Normal"), MP_Normal}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

/** The master and its function, both built when either is missing and rebuilt in place when asked. */
UMaterial* Master(bool bRegenerate, bool& bOutBuilt, int32& OutVersion, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::Master);

	const FString Folder = LilaLink::MastersFolderPath();
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(Folder, FunctionName, MasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	bOutBuilt = Pair.bCreated || bRegenerate;
	if (bOutBuilt)
	{
		BuildFunction(Pair.Function, OutCreated);
		BuildMaster(Pair.Material, Pair.Function, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	OutVersion = VersionOf(Pair.Function);
	if (OutVersion < MasterVersion)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is version %d, older than this plugin's %d; it is kept as edited (regenerate rebuilds it)"),
			MasterName, OutVersion, MasterVersion));
	}
	return Pair.Material;
}

TArray<FString> Strings(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
{
	TArray<FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (O->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Add(V->AsString());
		}
	}
	return Out;
}

/** Sets an instance's parameters from {scalars, vectors, textures, files}; files import next to the instance. */
void SetParameters(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, const FString& Folder, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::SetParameters);

	const TSharedPtr<FJsonObject>* Scalars = nullptr;
	if (In->TryGetObjectField(TEXT("scalars"), Scalars))
	{
		for (const auto& [Name, Value] : (*Scalars)->Values)
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(*Name), float(Value->AsNumber()));
		}
	}
	const TSharedPtr<FJsonObject>* Vectors = nullptr;
	if (In->TryGetObjectField(TEXT("vectors"), Vectors))
	{
		for (const auto& [Name, Value] : (*Vectors)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = Value->AsArray();
			if (C.Num() >= 3)
			{
				const FLinearColor Color(float(C[0]->AsNumber()), float(C[1]->AsNumber()), float(C[2]->AsNumber()));
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, FName(*Name), Color);
			}
		}
	}
	const TSharedPtr<FJsonObject>* Textures = nullptr;
	if (In->TryGetObjectField(TEXT("textures"), Textures))
	{
		for (const auto& [Name, Value] : (*Textures)->Values)
		{
			if (UTexture* Texture = Cast<UTexture>(FSoftObjectPath(Value->AsString()).TryLoad()))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(*Name), Texture);
			}
			else
			{
				OutWarnings.Add(FString::Printf(TEXT("%s: no texture %s for %s"), *Instance->GetName(), *Value->AsString(), *Name));
			}
		}
	}
	const TSharedPtr<FJsonObject>* Files = nullptr;
	if (In->TryGetObjectField(TEXT("files"), Files))
	{
		for (const auto& [Name, Value] : (*Files)->Values)
		{
			const TSharedPtr<FJsonObject> F = Value->AsObject();
			if (!F.IsValid())
			{
				continue;
			}
			bool bNormal = false, bFlip = false;
			F->TryGetBoolField(TEXT("normal"), bNormal);
			F->TryGetBoolField(TEXT("flipGreen"), bFlip);
			if (UTexture* Texture = LilaLink::ImportTextureFile(F->GetStringField(TEXT("file")), Folder, !bNormal, bNormal, bFlip, OutCreated, OutWarnings))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(*Name), Texture);
			}
		}
	}
}

TSharedPtr<FJsonValue> MasonryMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMasonry::MasonryMaterials);

	FString Folder;
	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("instances"), Instances))
	{
		OutError = TEXT("masonryMaterials: give folder and instances");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	bool bBuilt = false;
	int32 Version = 0;
	UMaterialInterface* Parent = nullptr;
	FString ParentPath;
	if (Params->TryGetStringField(TEXT("parent"), ParentPath) && !ParentPath.IsEmpty())
	{
		Parent = Cast<UMaterialInterface>(FSoftObjectPath(ParentPath).TryLoad());
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("masonryMaterials: no material %s"), *ParentPath);
			return nullptr;
		}
	}
	else
	{
		Parent = Master(bRegenerate, bBuilt, Version, Created, Warnings);
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("masonryMaterials: %s"), *FString::Join(Warnings, TEXT("; ")));
			return nullptr;
		}
	}

	TArray<UStaticMesh*> AllMeshes;
	for (const FString& Path : Strings(Params, TEXT("meshes")))
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad()))
		{
			AllMeshes.AddUnique(Mesh);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Made, Assigned;
	TSet<UStaticMesh*> Changed;
	for (const TSharedPtr<FJsonValue>& V : *Instances)
	{
		const TSharedPtr<FJsonObject> In = V->AsObject();
		FString Name, Slot;
		if (!In.IsValid() || !In->TryGetStringField(TEXT("name"), Name) || !In->TryGetStringField(TEXT("slot"), Slot))
		{
			Warnings.Add(TEXT("an instance without name or slot was skipped"));
			continue;
		}
		UMaterialInstanceConstant* Instance = AssetAt<UMaterialInstanceConstant>(Folder, Name);
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Parent;
			Instance = Cast<UMaterialInstanceConstant>(
				FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (!Instance)
			{
				Warnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *Name, *Folder));
				continue;
			}
			Created.AddUnique(Instance->GetPathName());
		}
		Instance->Modify();
		if (Instance->Parent != Parent)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		}
		SetParameters(Instance, In, Folder, Created, Warnings);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		Made.Add(Str(Instance->GetPathName()));

		TArray<UStaticMesh*> Targets;
		const TArray<FString> Only = Strings(In, TEXT("meshes"));
		for (UStaticMesh* Mesh : AllMeshes)
		{
			if (Only.IsEmpty() || Only.Contains(Mesh->GetPathName()))
			{
				Targets.Add(Mesh);
			}
		}
		const FName SlotName(*Slot);
		for (UStaticMesh* Mesh : Targets)
		{
			for (FStaticMaterial& M : Mesh->GetStaticMaterials())
			{
				if (M.MaterialSlotName != SlotName || M.MaterialInterface == Instance)
				{
					continue;
				}
				if (!Changed.Contains(Mesh))
				{
					Mesh->Modify();
					Changed.Add(Mesh);
				}
				M.MaterialInterface = Instance;
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				A->SetStringField(TEXT("slot"), Slot);
				A->SetStringField(TEXT("material"), Instance->GetPathName());
				Assigned.Add(Obj(A));
			}
		}
	}
	for (UStaticMesh* Mesh : Changed)
	{
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Parent->GetPathName());
	Out->SetNumberField(TEXT("masterVersion"), Version);
	Out->SetBoolField(TEXT("built"), bBuilt);
	Out->SetArrayField(TEXT("instances"), Made);
	Out->SetArrayField(TEXT("assigned"), Assigned);
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& C : Created)
	{
		CreatedJson.Add(Str(C));
	}
	for (const FString& W : Warnings)
	{
		WarningsJson.Add(Str(W));
		UE_LOG(LogLilaLink, Warning, TEXT("masonryMaterials: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return Obj(Out);
}
} // namespace

void LilaLink::AddMasonryMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("masonryMaterials"), LilaLink::Sync(&MasonryMaterials));
}
