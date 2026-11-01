#include "LilaLinkImposter.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"
#include "LilaLinkSettings.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraPositionWS.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
const TCHAR* const DefaultMaterialFolder = TEXT("/Game/LilaLink/Imposter");
const TCHAR* const MaterialName = TEXT("M_LilaImposter");

struct FImposterRequest
{
	FString PackagePath;
	FString AssetName;
	FString Folder;
	FString Fbx;
	FString BaseColor;
	FString Normal;
	FString MaterialFolder = DefaultMaterialFolder;
	int32 Frames = 12;
	bool bHemi = true;
	bool bRebuildMaterial = false;
	bool bSave = false;
};

template <class T> T* FindAssetAt(const FString& PackagePath, const FString& Name)
{
	const FString ObjectPath = PackagePath / Name + TEXT(".") + Name;
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** A file the bake's metadata lists under files, beside the metadata, or Fallback there. */
FString BakeFile(const FString& Folder, const TSharedPtr<FJsonObject>& Files, const TCHAR* Key, const FString& Fallback)
{
	FString Listed;
	if (!Files.IsValid() || !Files->TryGetStringField(Key, Listed) || Listed.IsEmpty())
	{
		Listed = Fallback;
	}
	return FPaths::ConvertRelativePathToFull(Folder, Listed);
}

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FImposterRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ParseRequest);

	FString ContentPath, MetaFile;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = TEXT("importImposter: give contentPath, like /Game/Imposters/SM_Tree");
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("importImposter: bad contentPath %s: %s"), *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);

	if (!Params->TryGetStringField(TEXT("metaFile"), MetaFile) || MetaFile.IsEmpty())
	{
		OutError = TEXT("importImposter: give metaFile, the bake's .json");
		return false;
	}
	MetaFile = FPaths::ConvertRelativePathToFull(MetaFile);
	FString Text;
	TSharedPtr<FJsonObject> Meta;
	if (!FFileHelper::LoadFileToString(Text, *MetaFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Meta) || !Meta.IsValid())
	{
		OutError = FString::Printf(TEXT("importImposter: %s is not an imposter bake's .json"), *MetaFile);
		return false;
	}
	const FString Folder = FPaths::GetPath(MetaFile);
	const FString Name = FPaths::GetBaseFilename(MetaFile);
	const TSharedPtr<FJsonObject>* FilesPtr = nullptr;
	const TSharedPtr<FJsonObject> Files = Meta->TryGetObjectField(TEXT("files"), FilesPtr) ? *FilesPtr : nullptr;
	Out.Fbx = BakeFile(Folder, Files, TEXT("mesh"), Name + TEXT("_Card.fbx"));
	Out.BaseColor = BakeFile(Folder, Files, TEXT("baseColor"), Name + TEXT("_BaseColor.png"));
	Out.Normal = BakeFile(Folder, Files, TEXT("normal"), Name + TEXT("_Normal.png"));
	for (const FString* File : {&Out.Fbx, &Out.BaseColor, &Out.Normal})
	{
		if (!FPaths::FileExists(*File))
		{
			OutError = FString::Printf(TEXT("importImposter: no file at %s"), **File);
			return false;
		}
	}
	double Frames = 12;
	Meta->TryGetNumberField(TEXT("frames"), Frames);
	Out.Frames = FMath::Max(2, int32(Frames));
	FString Layout;
	Meta->TryGetStringField(TEXT("layout"), Layout);
	Out.bHemi = Layout != TEXT("octahedral");

	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("options"), OptionsPtr))
	{
		const TSharedPtr<FJsonObject>& Options = *OptionsPtr;
		FString MaterialFolder;
		if (Options->TryGetStringField(TEXT("materialFolder"), MaterialFolder) && !MaterialFolder.IsEmpty())
		{
			Out.MaterialFolder = MaterialFolder.StartsWith(TEXT("/")) ? MaterialFolder : Out.PackagePath / MaterialFolder;
		}
		Options->TryGetBoolField(TEXT("rebuildMaterial"), Out.bRebuildMaterial);
		Options->TryGetBoolField(TEXT("save"), Out.bSave);
	}
	return true;
}

UStaticMesh* ImportCard(const FImposterRequest& R, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ImportCard);

	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bAutomatedImportShouldDetectType = false;
	Options->bImportMesh = true;
	Options->bImportAsSkeletal = false;
	Options->MeshTypeToImport = FBXIT_StaticMesh;
	Options->OriginalImportType = FBXIT_StaticMesh;
	Options->bImportAnimations = false;
	Options->bImportMaterials = false;
	Options->bImportTextures = false;
	UFbxStaticMeshImportData* Static = Options->StaticMeshImportData;
	Static->bCombineMeshes = true;
	Static->bAutoGenerateCollision = false;
	Static->bGenerateLightmapUVs = false;
	Static->bBuildNanite = false;

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.Fbx;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Factory = NewObject<UFbxFactory>(GetTransientPackage());
	Task->Options = Options;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});

	for (UObject* Object : Task->GetObjects())
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			return Mesh;
		}
	}
	if (UStaticMesh* Existing = FindAssetAt<UStaticMesh>(R.PackagePath, R.AssetName))
	{
		return Existing;
	}
	OutErrors.Add(TEXT("The card FBX imported no static mesh; see the editor log"));
	return nullptr;
}

/** The card's centre and half width in its own space, from its rest bounds (the card is a square through the centre). */
TPair<FVector, double> CardShape(const UStaticMesh* Mesh)
{
	const FStaticMeshRenderData* Render = Mesh->GetRenderData();
	const FBox Box = Render ? Render->Bounds.GetBox() : FBox(FVector(-50), FVector(50));
	return {Box.GetCenter(), FMath::Max(Box.GetExtent().GetMax(), UE_KINDA_SMALL_NUMBER)};
}

/** No Nanite, and bounds that hold the card whichever way the material turns it (its corners reach Radius * sqrt 2). */
void ConfigureCard(UStaticMesh* Mesh)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ConfigureCard);

	Mesh->Modify();
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = false;
	Mesh->SetNaniteSettings(Nanite);
	const auto [Centre, Radius] = CardShape(Mesh);
	const FStaticMeshRenderData* Render = Mesh->GetRenderData();
	const FBox Rest = Render ? Render->Bounds.GetBox() : FBox(Centre, Centre);
	const FVector Reach(Radius * UE_SQRT_2);
	Mesh->SetPositiveBoundsExtension((Centre + Reach - Rest.Max).ComponentMax(FVector::ZeroVector));
	Mesh->SetNegativeBoundsExtension((Rest.Min - (Centre - Reach)).ComponentMax(FVector::ZeroVector));
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
}

UTexture2D* ImportAtlas(const FString& File, const FString& PackagePath, const FString& Name, bool bColor, TArray<FString>& OutAssets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ImportAtlas);

	FImage Image;
	if (!FImageUtils::LoadImage(*File, Image))
	{
		OutErrors.Add(FString::Printf(TEXT("Could not read %s"), *File));
		return nullptr;
	}
	UTexture2D* Texture = FindAssetAt<UTexture2D>(PackagePath, Name);
	const bool bCreated = Texture == nullptr;
	if (bCreated)
	{
		UPackage* Package = CreatePackage(*(PackagePath / Name));
		Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	}
	Texture->Modify();
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Image);
	Texture->SRGB = bColor;
	// Normals keep depth in alpha, and block compression smears both across a view's edge.
	Texture->CompressionSettings = bColor ? TC_Default : TC_VectorDisplacementmap;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	if (!Texture->AssetImportData)
	{
		Texture->AssetImportData = NewObject<UAssetImportData>(Texture, TEXT("AssetImportData"));
	}
	Texture->AssetImportData->Update(File);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Texture);
	}
	OutAssets.AddUnique(Texture->GetPathName());
	return Texture;
}

/**
 * The camera-facing card corner for UV0, as an offset from the vertex's own position. Up close the card grows to the
 * bounding sphere's silhouette, at most the sqrt(2) the mesh bounds allow.
 */
const TCHAR* const CardHlsl = TEXT(R"(
float3 V = Cam - Centre;
float D2 = max(dot(V, V), 1e-6);
float3 d = V * rsqrt(D2);
float3 UpWorld = abs(d.z) > 0.999 ? float3(1, 0, 0) : float3(0, 0, 1);
float3 Right = normalize(cross(UpWorld, d));
float3 Up = cross(d, Right);
float Size = Radius * rsqrt(max(1.0 - Radius * Radius / D2, 0.5));
return Centre + (Right * (Uv.x * 2.0 - 1.0) + Up * (1.0 - Uv.y * 2.0)) * Size - Local;
)");

/**
 * Base colour and coverage, and LocalNormal, blended from the three baked views around the camera's direction from
 * Centre. Each view is read where the pixel's ray crosses that view's plane, shifted by one parallax step along the ray
 * using the depth in the normal atlas's alpha. The blend is a plain weighted sum, so it needs no dither or TAA. The view
 * encoding matches Lila's bake: cell (x, y), y from the top row, is the octahedral (or hemi-octahedral) direction at
 * (x, y) / (Frames - 1) * 2 - 1, and Right and Up are the ones the bake used for it.
 */
const TCHAR* const BlendHlsl = TEXT(R"(
float3 V = normalize(Cam - Centre);
float2 p;
if (Hemi > 0.5)
{
	V.z = max(V.z, 0.0);
	float3 a = V / max(abs(V.x) + abs(V.y) + V.z, 1e-6);
	p = float2(a.x + a.y, a.x - a.y);
}
else
{
	float3 a = V / max(abs(V.x) + abs(V.y) + abs(V.z), 1e-6);
	p = a.xy;
	if (a.z < 0.0)
		p = (1.0 - abs(a.yx)) * (step(0.0, a.xy) * 2.0 - 1.0);
}
float2 f = clamp((p * 0.5 + 0.5) * (Frames - 1.0), 0.0, Frames - 1.0);
float2 b = min(floor(f), max(Frames - 2.0, 0.0));
float2 t = f - b;
float2 Cells[3] = {b, b + (t.x > t.y ? float2(1, 0) : float2(0, 1)), b + 1.0};
float Weights[3] = {1.0 - max(t.x, t.y), abs(t.x - t.y), min(t.x, t.y)};

float3 From = Cam - Centre;
float3 Ray = normalize(P - Cam);
float4 Sum = 0;
float3 N = 0;
[unroll] for (int i = 0; i < 3; ++i)
{
	float2 o = Cells[i] / max(Frames - 1.0, 1.0) * 2.0 - 1.0;
	float3 d;
	if (Hemi > 0.5)
	{
		float x = (o.x + o.y) * 0.5;
		float y = (o.x - o.y) * 0.5;
		d = float3(x, y, 1.0 - abs(x) - abs(y));
	}
	else
	{
		d = float3(o, 1.0 - abs(o.x) - abs(o.y));
		if (d.z < 0.0)
			d.xy = (1.0 - abs(o.yx)) * (step(0.0, o) * 2.0 - 1.0);
	}
	d = normalize(d);
	float3 UpWorld = abs(d.z) > 0.999 ? float3(1, 0, 0) : float3(0, 0, 1);
	float3 Right = normalize(cross(UpWorld, d));
	float3 Up = cross(d, Right);
	float Facing = min(dot(Ray, d), -0.1);
	float3 H = From - Ray * (dot(From, d) / Facing);
	float2 Uv = float2(dot(H, Right), -dot(H, Up)) / (2.0 * Radius) + 0.5;
	float Depth = Texture2DSample(Normal, NormalSampler, (Cells[i] + saturate(Uv)) / Frames).a;
	H += Ray * (Radius * (1.0 - 2.0 * Depth) * Parallax / Facing);
	Uv = float2(dot(H, Right), -dot(H, Up)) / (2.0 * Radius) + 0.5;
	float2 At = (Cells[i] + saturate(Uv)) / Frames;
	float4 C = Texture2DSample(BaseColor, BaseColorSampler, At);
	float w = C.a * Weights[i] * (all(Uv == saturate(Uv)) ? 1.0 : 0.0);
	Sum += float4(C.rgb, 1.0) * w;
	N += (Texture2DSample(Normal, NormalSampler, At).xyz * 2.0 - 1.0) * w;
}
LocalNormal = N / max(length(N), 1e-4);
return float4(Sum.rgb / max(Sum.a, 1e-4), Sum.a);
)");

/** Expressions placed down a column of the material graph. */
struct FImposterGraph
{
	UMaterial* Material = nullptr;
	int32 Row = 0;

	template <class T> T* New(int32 X = -600)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, (Row++) * 120));
	}

	UMaterialExpression* Scalar(const TCHAR* Name, float Value, const TCHAR* Group)
	{
		auto* E = New<UMaterialExpressionScalarParameter>(-1400);
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	}

	UMaterialExpression* Vector(const TCHAR* Name, const FLinearColor& Value, const TCHAR* Group)
	{
		auto* E = New<UMaterialExpressionVectorParameter>(-1400);
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	}

	UMaterialExpression* Mask(UMaterialExpression* Input, int32 OutputIndex, bool R, bool G, bool B, bool A)
	{
		auto* E = New<UMaterialExpressionComponentMask>();
		E->Input.Connect(OutputIndex, Input);
		E->R = R;
		E->G = G;
		E->B = B;
		E->A = A;
		return E;
	}

	UMaterialExpression* Custom(const TCHAR* Description, ECustomMaterialOutputType Type, const FString& Code, TArrayView<const TPair<const TCHAR*, UMaterialExpression*>> Inputs,
							   TArrayView<const FCustomOutput> Extra = {})
	{
		auto* E = New<UMaterialExpressionCustom>(-900);
		E->Description = Description;
		E->OutputType = Type;
		E->Code = Code;
		E->Inputs.Reset();
		for (const TPair<const TCHAR*, UMaterialExpression*>& In : Inputs)
		{
			FCustomInput& Added = E->Inputs.AddDefaulted_GetRef();
			Added.InputName = In.Key;
			Added.Input.Connect(0, In.Value);
		}
		E->AdditionalOutputs = TArray<FCustomOutput>(Extra);
		E->RebuildOutputs();
		return E;
	}
};

/**
 * M_LilaImposter: in the vertex shader World Position Offset turns the card (its corners by UV0) to face the camera; the
 * pixel shader blends base colour, coverage and normal from the three nearest baked views (BlendHlsl). The normal atlas is
 * in the mesh's space. Masked and two sided.
 */
void BuildImposterMaterial(UMaterial* Material, UTexture* BaseColor, UTexture* Normal)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::BuildImposterMaterial);

	UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
	FImposterGraph G;
	G.Material = Material;
	const TCHAR* Layout = TEXT("Imposter");
	const TCHAR* Surface = TEXT("Surface");
	UMaterialExpression* Frames = G.Scalar(TEXT("Frames"), 12.0f, Layout);
	UMaterialExpression* Hemi = G.Scalar(TEXT("Hemi"), 1.0f, Layout);
	UMaterialExpression* Centre = G.Mask(G.Vector(TEXT("Centre"), FLinearColor(0, 0, 0, 0), Layout), 0, true, true, true, false);
	UMaterialExpression* Radius = G.Scalar(TEXT("Radius"), 50.0f, Layout);
	UMaterialExpression* Parallax = G.Scalar(TEXT("Parallax"), 1.0f, Layout);

	auto* CameraWorld = G.New<UMaterialExpressionCameraPositionWS>();
	auto* Camera = G.New<UMaterialExpressionTransformPosition>();
	Camera->TransformSourceType = TRANSFORMPOSSOURCE_World;
	Camera->TransformType = TRANSFORMPOSSOURCE_Local;
	Camera->Input.Connect(0, CameraWorld);
	auto* Uv = G.New<UMaterialExpressionTextureCoordinate>();
	Uv->CoordinateIndex = 0;
	auto* Local = G.New<UMaterialExpressionLocalPosition>();
	Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;

	const TPair<const TCHAR*, UMaterialExpression*> CardInputs[] = {{TEXT("Cam"), Camera}, {TEXT("Centre"), Centre}, {TEXT("Radius"), Radius}, {TEXT("Uv"), Uv}, {TEXT("Local"), Local}};
	UMaterialExpression* Card = G.Custom(TEXT("Imposter card"), CMOT_Float3, CardHlsl, CardInputs);
	auto* CardWorld = G.New<UMaterialExpressionTransform>();
	CardWorld->TransformSourceType = TRANSFORMSOURCE_Local;
	CardWorld->TransformType = TRANSFORM_World;
	CardWorld->Input.Connect(0, Card);
	UMaterialEditingLibrary::ConnectMaterialProperty(CardWorld, TEXT(""), MP_WorldPositionOffset);

	auto* PixelWorld = G.New<UMaterialExpressionWorldPosition>();
	auto* Pixel = G.New<UMaterialExpressionTransformPosition>();
	Pixel->TransformSourceType = TRANSFORMPOSSOURCE_World;
	Pixel->TransformType = TRANSFORMPOSSOURCE_Local;
	Pixel->Input.Connect(0, PixelWorld);
	auto Atlas = [&](const TCHAR* Name, UTexture* Texture) {
		auto* E = G.New<UMaterialExpressionTextureObjectParameter>(-1400);
		E->ParameterName = Name;
		E->Group = Surface;
		E->Texture = Texture;
		E->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
		return E;
	};
	const TPair<const TCHAR*, UMaterialExpression*> BlendInputs[] = {
		{TEXT("Cam"), Camera},
		{TEXT("P"), Pixel},
		{TEXT("Centre"), Centre},
		{TEXT("Radius"), Radius},
		{TEXT("Frames"), Frames},
		{TEXT("Hemi"), Hemi},
		{TEXT("Parallax"), Parallax},
		{TEXT("BaseColor"), Atlas(TEXT("BaseColor"), BaseColor)},
		{TEXT("Normal"), Atlas(TEXT("Normal"), Normal)}};
	const FCustomOutput BlendOutputs[] = {{TEXT("LocalNormal"), CMOT_Float3}};
	UMaterialExpression* Blend = G.Custom(TEXT("Imposter views"), CMOT_Float4, BlendHlsl, BlendInputs, BlendOutputs);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Mask(Blend, 0, true, true, true, false), TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Mask(Blend, 0, false, false, false, true), TEXT(""), MP_OpacityMask);
	auto* NormalWorld = G.New<UMaterialExpressionTransform>();
	NormalWorld->TransformSourceType = TRANSFORMSOURCE_Local;
	NormalWorld->TransformType = TRANSFORM_World;
	NormalWorld->Input.Connect(1, Blend);
	UMaterialEditingLibrary::ConnectMaterialProperty(NormalWorld, TEXT(""), MP_Normal);
	Material->bTangentSpaceNormal = false;

	UMaterialEditingLibrary::ConnectMaterialProperty(G.Scalar(TEXT("Roughness"), 0.7f, Surface), TEXT(""), MP_Roughness);
	Material->NumCustomizedUVs = 0;
	Material->BlendMode = BLEND_Masked;
	Material->TwoSided = true;
	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_StaticLighting);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

UMaterialInterface* ImposterMaterial(const FImposterRequest& R, UTexture* BaseColor, UTexture* Normal, TArray<FString>& OutAssets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ImposterMaterial);

	UMaterial* Material = FindAssetAt<UMaterial>(R.MaterialFolder, MaterialName);
	if (Material && !R.bRebuildMaterial)
	{
		return Material;
	}
	if (!Material)
	{
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>(GetTransientPackage());
		Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(MaterialName, R.MaterialFolder, UMaterial::StaticClass(), Factory));
		if (!Material)
		{
			OutErrors.Add(FString::Printf(TEXT("Could not create %s in %s"), MaterialName, *R.MaterialFolder));
			return nullptr;
		}
	}
	BuildImposterMaterial(Material, BaseColor, Normal);
	OutAssets.AddUnique(Material->GetPathName());
	return Material;
}

UMaterialInstanceConstant* MakeInstance(const FImposterRequest& R, UMaterialInterface* Parent, TArray<FString>& OutAssets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::MakeInstance);

	const FString Name = TEXT("MI_") + (R.AssetName.StartsWith(TEXT("SM_")) ? R.AssetName.RightChop(3) : R.AssetName);
	UMaterialInstanceConstant* Instance = FindAssetAt<UMaterialInstanceConstant>(R.PackagePath, Name);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.PackagePath, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutErrors.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, *R.PackagePath));
		return nullptr;
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	OutAssets.AddUnique(Instance->GetPathName());
	return Instance;
}

void ImportImposter(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImposter::ImportImposter);

	FImposterRequest R;
	FString Error;
	if (!ParseRequest(Params, R, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}
	TArray<FString> Assets, Errors;

	Call->Progress(TEXT("importing the card"));
	UStaticMesh* Mesh = ImportCard(R, Errors);
	if (Mesh)
	{
		ConfigureCard(Mesh);
		Assets.Add(Mesh->GetPathName());
	}

	Call->Progress(TEXT("importing the atlases"));
	const FString Base = R.AssetName.StartsWith(TEXT("SM_")) ? R.AssetName.RightChop(3) : R.AssetName;
	UTexture2D* BaseColor = ImportAtlas(R.BaseColor, R.PackagePath, TEXT("T_") + Base + TEXT("_BaseColor"), true, Assets, Errors);
	UTexture2D* Normal = ImportAtlas(R.Normal, R.PackagePath, TEXT("T_") + Base + TEXT("_Normal"), false, Assets, Errors);

	if (Mesh && BaseColor && Normal)
	{
		Call->Progress(TEXT("building the material"));
		if (UMaterialInterface* Parent = ImposterMaterial(R, BaseColor, Normal, Assets, Errors))
		{
			if (UMaterialInstanceConstant* Instance = MakeInstance(R, Parent, Assets, Errors))
			{
				const auto [Centre, Radius] = CardShape(Mesh);
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Frames"), float(R.Frames));
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Hemi"), R.bHemi ? 1.0f : 0.0f);
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Centre"), FLinearColor(FVector3f(Centre)));
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Radius"), float(Radius));
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("BaseColor"), BaseColor);
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Normal"), Normal);
				UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
				Instance->MarkPackageDirty();
				Mesh->Modify();
				for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
				{
					Mesh->SetMaterial(Slot, Instance);
				}
				Mesh->MarkPackageDirty();
			}
		}
	}

	for (const FString& E : Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *R.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("importImposter %s/%s: %d asset(s), %dx%d views"), *R.PackagePath, *R.AssetName, Assets.Num(), R.Frames, R.Frames);
	if (!Assets.IsEmpty() && (R.bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Assets, Errors);
	}

	auto Strings = [](const TArray<FString>& In) {
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& S : In)
		{
			Values.Add(MakeShared<FJsonValueString>(S));
		}
		return Values;
	};
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("assets"), Strings(Assets));
	Out->SetArrayField(TEXT("warnings"), {});
	Out->SetArrayField(TEXT("errors"), Strings(Errors));
	Out->SetStringField(TEXT("method"), TEXT("imposter"));
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

void LilaLink::AddImposterMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importImposter"), &ImportImposter);
}
