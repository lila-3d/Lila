#include "LilaLinkTrim.h"

#include "LilaLinkLog.h"
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
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
/** Raised when the graph changes; a master older than this is kept (artists own it) and reported, until regenerate. */
constexpr int32 MasterVersion = 2;
const TCHAR* MasterName = TEXT("M_LilaTrim");
const TCHAR* VersionTag = TEXT("Lila Trim v");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

template <class T> T* AssetAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** Edge and cavity masks from the baked curvature (0.5 flat, above convex) and cavity maps, shared by every output. */
const TCHAR* MasksCode = TEXT(R"(
float curv = Texture2DSample(SheetCurvatureMap, SheetCurvatureMapSampler, SheetUV).r;
float cavity = Texture2DSample(SheetCavityMap, SheetCavityMapSampler, SheetUV).r;
float edge = saturate((curv - 0.5 - EdgeThreshold) / max(EdgeSoftness, 1e-3)) * EdgeWear;
float dirt = saturate(saturate((0.5 - curv) * 2.0) + (1.0 - cavity)) * CavityStrength;
float blend = UseVertexBlend > 0.5 ? saturate(VColor.r * VertexBlend) : 0.0;
float2 detailUV = SheetUV * float2(SheetWidthCm, SheetHeightCm) / max(DetailScaleCm, 1e-3);
)");

const TCHAR* ColorCode = TEXT(R"(
float3 col = Tint.rgb;
if (UseDetail > 0.5)
{
    float3 d = Texture2DSample(DetailBaseColorMap, DetailBaseColorMapSampler, detailUV).rgb;
    col *= lerp(float3(1, 1, 1), d * 2.0, DetailStrength);
}
col = lerp(col, SecondaryTint.rgb, blend);
col = lerp(col, EdgeTint.rgb, edge);
return lerp(col, col * DirtColor.rgb, dirt);
)");

const TCHAR* RoughnessCode = TEXT(R"(
float r = BaseRoughness;
if (UseDetail > 0.5)
    r += (Texture2DSample(DetailORMMap, DetailORMMapSampler, detailUV).g - 0.5) * DetailStrength;
r = lerp(r, SecondaryRoughness, blend);
r = lerp(r, EdgeRoughness, edge);
return saturate(r + dirt * DirtRoughness);
)");

const TCHAR* MetallicCode = TEXT(R"(
return saturate(lerp(lerp(Metallic, 0.0, blend), EdgeMetallic, edge));
)");

const TCHAR* OcclusionCode = TEXT(R"(
float ao = Texture2DSample(SheetAOMap, SheetAOMapSampler, SheetUV).r;
return lerp(1.0, ao, AOStrength);
)");

/** The sheet's baked normal (BC5 keeps only xy) under the tiling detail normal, RNM blended. */
const TCHAR* NormalCode = TEXT(R"(
float2 xy = Texture2DSample(SheetNormalMap, SheetNormalMapSampler, SheetUV).xy * 2.0 - 1.0;
float3 nrm = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
if (UseDetail > 0.5)
{
    float2 dxy = (Texture2DSample(DetailNormalMap, DetailNormalMapSampler, detailUV).xy * 2.0 - 1.0) * DetailNormalStrength * (1.0 - edge);
    float3 d = float3(dxy, sqrt(saturate(1.0 - dot(dxy, dxy))));
    float3 n1 = nrm + float3(0, 0, 1);
    float3 n2 = d * float3(-1, -1, 1);
    nrm = n1 * dot(n1, n2) / n1.z - n2;
}
return normalize(nrm);
)");

/** A 4x4 linear texture of one grey level and compression: defaults whose sampler type matches the parameter's. */
UTexture* NeutralTexture(const TCHAR* Name, uint8 Level, TextureCompressionSettings Compression, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::NeutralTexture);

	const FString Folder = LilaLink::MastersFolderPath();
	if (UTexture* Existing = AssetAt<UTexture>(Folder, Name))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(*(Folder / Name));
	UTexture2D* Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	TArray<uint8> Pixels;
	Pixels.Init(Level, 4 * 4 * 4);
	for (int32 K = 3; K < Pixels.Num(); K += 4)
	{
		Pixels[K] = 255;
	}
	Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
	Texture->SRGB = false;
	Texture->CompressionSettings = Compression;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

int32 VersionOf(UMaterial* Material)
{
	for (UMaterialExpression* E : Material->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(VersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(VersionTag)));
		}
	}
	return 0;
}

void BuildMaster(UMaterial* Material, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::BuildMaster);

	int32 Row = 0;
	auto NextY = [&Row] { return (Row++) * 120; };
	auto Make = [&](UClass* Class, int32 X) { return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, NextY()); };
	TMap<FString, UMaterialExpression*> Inputs;
	auto Scalar = [&](const TCHAR* Param, float Value, const TCHAR* Group) {
		auto* E = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -1400));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		E->Group = Group;
		Inputs.Add(Param, E);
	};
	auto Vector = [&](const TCHAR* Param, const FLinearColor& Value, const TCHAR* Group) {
		auto* E = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -1400));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		E->Group = Group;
		Inputs.Add(Param, E);
	};
	auto Texture = [&](const TCHAR* Param, UTexture* Default, EMaterialSamplerType Type, const TCHAR* Group) {
		auto* E = Cast<UMaterialExpressionTextureObjectParameter>(Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -1400));
		E->ParameterName = Param;
		E->Texture = Default;
		E->SamplerType = Type;
		E->Group = Group;
		Inputs.Add(Param, E);
	};
	// A static switch between 1 and 0: an instance that leaves it off folds the branch it gates out of the shader.
	auto Switch = [&](const TCHAR* Param, bool bDefault, const TCHAR* Group) {
		auto* S = Cast<UMaterialExpressionStaticSwitchParameter>(Make(UMaterialExpressionStaticSwitchParameter::StaticClass(), -1200));
		S->ParameterName = Param;
		S->DefaultValue = bDefault;
		S->Group = Group;
		auto* On = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass(), -1400));
		On->R = 1.0f;
		auto* Off = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass(), -1400));
		Off->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(On, TEXT(""), S, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Off, TEXT(""), S, TEXT("False"));
		Inputs.Add(Param, S);
		return S;
	};

	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	UTexture* Grey = NeutralTexture(TEXT("T_LilaTrimNeutral"), 128, TC_Masks, OutCreated);
	UTexture* WhiteMask = NeutralTexture(TEXT("T_LilaTrimWhite"), 255, TC_Masks, OutCreated);
	UTexture* Height = NeutralTexture(TEXT("T_LilaTrimHeight"), 128, TC_Grayscale, OutCreated);
	Texture(TEXT("SheetNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Sheet"));
	Texture(TEXT("SheetAOMap"), WhiteMask, SAMPLERTYPE_Masks, TEXT("Sheet"));
	Texture(TEXT("SheetCurvatureMap"), Grey, SAMPLERTYPE_Masks, TEXT("Sheet"));
	Texture(TEXT("SheetCavityMap"), WhiteMask, SAMPLERTYPE_Masks, TEXT("Sheet"));
	Texture(TEXT("SheetHeightMap"), Height, SAMPLERTYPE_LinearGrayscale, TEXT("Sheet"));
	Texture(TEXT("DetailBaseColorMap"), Grey, SAMPLERTYPE_Color, TEXT("Detail"));
	Texture(TEXT("DetailNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Detail"));
	Texture(TEXT("DetailORMMap"), Grey, SAMPLERTYPE_Masks, TEXT("Detail"));
	Texture(TEXT("SheetBaseColorMap"), Grey, SAMPLERTYPE_Color, TEXT("Painted"));
	Texture(TEXT("SheetORMMap"), Grey, SAMPLERTYPE_Masks, TEXT("Painted"));

	Scalar(TEXT("SheetWidthCm"), 400.0f, TEXT("Sheet"));
	Scalar(TEXT("SheetHeightCm"), 400.0f, TEXT("Sheet"));
	Scalar(TEXT("AOStrength"), 1.0f, TEXT("Sheet"));
	Vector(TEXT("Tint"), FLinearColor(0.35f, 0.37f, 0.4f), TEXT("Paint"));
	Scalar(TEXT("BaseRoughness"), 0.55f, TEXT("Paint"));
	Scalar(TEXT("Metallic"), 0.0f, TEXT("Paint"));
	Vector(TEXT("EdgeTint"), FLinearColor(0.62f, 0.6f, 0.58f), TEXT("Edges"));
	Scalar(TEXT("EdgeWear"), 1.0f, TEXT("Edges"));
	Scalar(TEXT("EdgeThreshold"), 0.05f, TEXT("Edges"));
	Scalar(TEXT("EdgeSoftness"), 0.15f, TEXT("Edges"));
	Scalar(TEXT("EdgeRoughness"), 0.3f, TEXT("Edges"));
	Scalar(TEXT("EdgeMetallic"), 1.0f, TEXT("Edges"));
	Vector(TEXT("DirtColor"), FLinearColor(0.45f, 0.4f, 0.33f), TEXT("Dirt"));
	Scalar(TEXT("CavityStrength"), 0.5f, TEXT("Dirt"));
	Scalar(TEXT("DirtRoughness"), 0.2f, TEXT("Dirt"));
	Scalar(TEXT("DetailScaleCm"), 100.0f, TEXT("Detail"));
	Scalar(TEXT("DetailStrength"), 0.5f, TEXT("Detail"));
	Scalar(TEXT("DetailNormalStrength"), 1.0f, TEXT("Detail"));
	Vector(TEXT("SecondaryTint"), FLinearColor(0.3f, 0.25f, 0.2f), TEXT("Vertex Blend"));
	Scalar(TEXT("SecondaryRoughness"), 0.85f, TEXT("Vertex Blend"));
	Scalar(TEXT("VertexBlend"), 1.0f, TEXT("Vertex Blend"));
	Scalar(TEXT("ParallaxHeight"), 0.02f, TEXT("Parallax"));
	Switch(TEXT("UseDetail"), false, TEXT("Detail"));
	Switch(TEXT("UseVertexBlend"), false, TEXT("Vertex Blend"));
	Inputs.Add(TEXT("VColor"), Make(UMaterialExpressionVertexColor::StaticClass(), -1400));

	// The sheet uv: uv0, or offset along the tangent-space view by the baked height when parallax is on (Unreal's bump offset).
	{
		auto* UV0 = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass(), -1600));
		UV0->CoordinateIndex = 0;
		auto* Offset = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -1500));
		Offset->Description = TEXT("Sheet parallax");
		Offset->Code = TEXT("float h = Texture2DSample(SheetHeightMap, SheetHeightMapSampler, UV).r;\n"
							"float3 v = mul(Parameters.TangentToWorld, Parameters.CameraVector);\n"
							"return UV + v.xy * (h - 0.5) * ParallaxHeight;");
		Offset->OutputType = CMOT_Float2;
		Offset->Inputs.Reset();
		for (const TCHAR* Name : {TEXT("SheetHeightMap"), TEXT("UV"), TEXT("ParallaxHeight")})
		{
			Offset->Inputs.AddDefaulted_GetRef().InputName = Name;
		}
		UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[TEXT("SheetHeightMap")], TEXT(""), Offset, TEXT("SheetHeightMap"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV0, TEXT(""), Offset, TEXT("UV"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[TEXT("ParallaxHeight")], TEXT(""), Offset, TEXT("ParallaxHeight"));
		auto* Pick = Cast<UMaterialExpressionStaticSwitchParameter>(Make(UMaterialExpressionStaticSwitchParameter::StaticClass(), -1300));
		Pick->ParameterName = TEXT("UseParallax");
		Pick->DefaultValue = false;
		Pick->Group = TEXT("Parallax");
		UMaterialEditingLibrary::ConnectMaterialExpressions(Offset, TEXT(""), Pick, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(UV0, TEXT(""), Pick, TEXT("False"));
		Inputs.Add(TEXT("SheetUV"), Pick);
	}

	const TArray<const TCHAR*> MaskInputs = {TEXT("SheetUV"), TEXT("SheetCurvatureMap"), TEXT("SheetCavityMap"), TEXT("EdgeThreshold"),
		TEXT("EdgeSoftness"), TEXT("EdgeWear"), TEXT("CavityStrength"), TEXT("UseVertexBlend"), TEXT("VColor"), TEXT("VertexBlend"),
		TEXT("SheetWidthCm"), TEXT("SheetHeightCm"), TEXT("DetailScaleCm")};
	auto With = [&](std::initializer_list<const TCHAR*> Extra) {
		TArray<const TCHAR*> All = MaskInputs;
		All.Append(Extra.begin(), int32(Extra.size()));
		return All;
	};
	auto CustomOf = [&](const FString& Description, const FString& Code, ECustomMaterialOutputType Type, const TArray<const TCHAR*>& Names) {
		auto* E = Cast<UMaterialExpressionCustom>(Make(UMaterialExpressionCustom::StaticClass(), -400));
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
			if (UMaterialExpression** From = Inputs.Find(Name))
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(*From, TEXT(""), E, Name);
			}
		}
		return E;
	};

	const FString Version = FString::Printf(TEXT("%s%d"), VersionTag, MasterVersion);
	auto* Color = CustomOf(Version + TEXT(" colour"), FString(MasksCode) + ColorCode, CMOT_Float3,
		With({TEXT("Tint"), TEXT("UseDetail"), TEXT("DetailBaseColorMap"), TEXT("DetailStrength"), TEXT("SecondaryTint"), TEXT("EdgeTint"), TEXT("DirtColor")}));
	auto* Roughness = CustomOf(Version + TEXT(" roughness"), FString(MasksCode) + RoughnessCode, CMOT_Float1,
		With({TEXT("BaseRoughness"), TEXT("UseDetail"), TEXT("DetailORMMap"), TEXT("DetailStrength"), TEXT("SecondaryRoughness"), TEXT("EdgeRoughness"),
			TEXT("DirtRoughness")}));
	auto* Metallic = CustomOf(Version + TEXT(" metallic"), FString(MasksCode) + MetallicCode, CMOT_Float1,
		With({TEXT("Metallic"), TEXT("EdgeMetallic")}));
	auto* Occlusion = CustomOf(Version + TEXT(" occlusion"), OcclusionCode, CMOT_Float1, {TEXT("SheetUV"), TEXT("SheetAOMap"), TEXT("AOStrength")});
	auto* Normal = CustomOf(Version + TEXT(" normal"), FString(MasksCode) + NormalCode, CMOT_Float3,
		With({TEXT("SheetNormalMap"), TEXT("UseDetail"), TEXT("DetailNormalMap"), TEXT("DetailNormalStrength")}));

	// UsePaintedMaps: the sheet's texture set from Lila (base colour and ORM) stands in for the procedural paint.
	auto Painted = [&](UMaterialExpression* Procedural, const TCHAR* Map, const TCHAR* Code, ECustomMaterialOutputType Type) -> UMaterialExpression* {
		auto* Sample = CustomOf(Version + TEXT(" painted"), Code, Type, {TEXT("SheetUV"), Map});
		auto* Pick = Cast<UMaterialExpressionStaticSwitchParameter>(Make(UMaterialExpressionStaticSwitchParameter::StaticClass(), -200));
		Pick->ParameterName = TEXT("UsePaintedMaps");
		Pick->DefaultValue = false;
		Pick->Group = TEXT("Painted");
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sample, TEXT(""), Pick, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Procedural, TEXT(""), Pick, TEXT("False"));
		return Pick;
	};
	UMaterialEditingLibrary::ConnectMaterialProperty(
		Painted(Color, TEXT("SheetBaseColorMap"), TEXT("return Texture2DSample(SheetBaseColorMap, SheetBaseColorMapSampler, SheetUV).rgb;"), CMOT_Float3),
		TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(
		Painted(Roughness, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).g;"), CMOT_Float1), TEXT(""),
		MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(
		Painted(Metallic, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).b;"), CMOT_Float1), TEXT(""),
		MP_Metallic);
	UMaterialEditingLibrary::ConnectMaterialProperty(
		Painted(Occlusion, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).r;"), CMOT_Float1), TEXT(""),
		MP_AmbientOcclusion);
	UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);

	Material->bUsedWithInstancedStaticMeshes = true;
	Material->bUsedWithNanite = true;
	Material->PostEditChange();
	Material->MarkPackageDirty();
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

void SetParameters(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::SetParameters);

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
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
					Instance, FName(*Name), FLinearColor(float(C[0]->AsNumber()), float(C[1]->AsNumber()), float(C[2]->AsNumber())));
			}
		}
	}
	const TSharedPtr<FJsonObject>* Switches = nullptr;
	if (In->TryGetObjectField(TEXT("switches"), Switches))
	{
		for (const auto& [Name, Value] : (*Switches)->Values)
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, FName(*Name), Value->AsBool());
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
}

TSharedPtr<FJsonValue> TrimMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::TrimMaterials);

	FString Folder, Slot;
	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("instances"), Instances))
	{
		OutError = TEXT("trimMaterials: give folder and instances");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("slot"), Slot);
	TArray<FString> Created, Warnings;
	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	bool bBuilt = false;
	int32 Version = 0;
	UMaterial* Master = LilaLink::TrimMaster(bRegenerate, bBuilt, Version, Created, Warnings);
	if (!Master)
	{
		OutError = FString::Printf(TEXT("trimMaterials: %s"), *FString::Join(Warnings, TEXT("; ")));
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Made, Assigned;
	UMaterialInstanceConstant* First = nullptr;
	for (const TSharedPtr<FJsonValue>& V : *Instances)
	{
		const TSharedPtr<FJsonObject> In = V->AsObject();
		FString Name;
		if (!In.IsValid() || !In->TryGetStringField(TEXT("name"), Name))
		{
			Warnings.Add(TEXT("an instance without a name was skipped"));
			continue;
		}
		UMaterialInstanceConstant* Instance = AssetAt<UMaterialInstanceConstant>(Folder, Name);
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Master;
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
		if (Instance->Parent != Master)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
		}
		SetParameters(Instance, In, Warnings);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		Made.Add(Str(Instance->GetPathName()));
		First = First ? First : Instance;
	}

	if (First && !Slot.IsEmpty())
	{
		const FName SlotName(*Slot);
		for (const FString& Path : Strings(Params, TEXT("meshes")))
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("no static mesh %s"), *Path));
				continue;
			}
			bool bChanged = false;
			for (FStaticMaterial& M : Mesh->GetStaticMaterials())
			{
				if (M.MaterialSlotName != SlotName || M.MaterialInterface == First)
				{
					continue;
				}
				if (!bChanged)
				{
					Mesh->Modify();
					bChanged = true;
				}
				M.MaterialInterface = First;
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				A->SetStringField(TEXT("slot"), Slot);
				A->SetStringField(TEXT("material"), First->GetPathName());
				Assigned.Add(Obj(A));
			}
			if (bChanged)
			{
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
			}
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Master->GetPathName());
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
		UE_LOG(LogLilaLink, Warning, TEXT("trimMaterials: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return Obj(Out);
}
} // namespace

UMaterial* LilaLink::TrimMaster(bool bRegenerate, bool& bOutBuilt, int32& OutVersion, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::TrimMaster);

	const FString Folder = LilaLink::MastersFolderPath();
	UMaterial* Material = AssetAt<UMaterial>(Folder, MasterName);
	bOutBuilt = false;
	if (!Material)
	{
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>(GetTransientPackage());
		Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(MasterName, Folder, UMaterial::StaticClass(), Factory));
		if (!Material)
		{
			OutWarnings.Add(FString::Printf(TEXT("Could not create %s in %s"), MasterName, *Folder));
			return nullptr;
		}
		OutCreated.AddUnique(Material->GetPathName());
		BuildMaster(Material, OutCreated);
		bOutBuilt = true;
	}
	else if (bRegenerate)
	{
		Material->Modify();
		UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
		BuildMaster(Material, OutCreated);
		bOutBuilt = true;
	}
	OutVersion = VersionOf(Material);
	if (OutVersion < MasterVersion)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is version %d, older than this plugin's %d; it is kept as edited (regenerate rebuilds it)"),
			MasterName, OutVersion, MasterVersion));
	}
	return Material;
}

void LilaLink::AddTrimMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("trimMaterials"), LilaLink::Sync(&TrimMaterials));
}
