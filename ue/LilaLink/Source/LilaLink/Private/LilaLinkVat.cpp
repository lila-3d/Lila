#include "LilaLinkVat.h"

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
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
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
const TCHAR* const DefaultMaterialFolder = TEXT("/Game/LilaLink/VAT");

struct FVatRequest
{
	FString PackagePath; // /Game/FX
	FString AssetName;   // SM_Blob
	FString Fbx;
	FString Positions;
	FString Normals;
	FString MaterialParent;
	FString MaterialFolder = DefaultMaterialFolder;
	FString LodGroup;
	bool bRebuildMaterial = false;
	bool bSave = false;
	TSharedPtr<FJsonObject> Meta;
};

struct FVatMeta
{
	int32 FrameCount = 1;
	int32 RowsPerFrame = 1;
	int32 Width = 1;
	int32 Height = 1;
	int32 UvChannel = 1;
	int32 NormalBits = 8;
	double Fps = 30.0;
	bool bAbsolute = false;
	/** HDR material input: 1 for EXR values, 0 for PNG texels normalised by the bounds. */
	bool bHdr = true;
	int32 PositionBits = 16;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::OneVector;
	FBox MeshBounds = FBox(ForceInit);
};

double NumberOr(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, double Fallback)
{
	double V = Fallback;
	return O.IsValid() && O->TryGetNumberField(Field, V) ? V : Fallback;
}

FVector VectorOr(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O.IsValid() || !O->TryGetArrayField(Field, A) || A->Num() != 3)
	{
		return Fallback;
	}
	return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
}

FVatMeta ParseMeta(const TSharedPtr<FJsonObject>& M)
{
	FVatMeta Out;
	Out.FrameCount = FMath::Max(1, int32(NumberOr(M, TEXT("frameCount"), 1)));
	Out.RowsPerFrame = FMath::Max(1, int32(NumberOr(M, TEXT("rowsPerFrame"), 1)));
	Out.Width = FMath::Max(1, int32(NumberOr(M, TEXT("width"), 1)));
	Out.Height = FMath::Max(1, int32(NumberOr(M, TEXT("height"), Out.FrameCount * Out.RowsPerFrame)));
	Out.UvChannel = FMath::Clamp(int32(NumberOr(M, TEXT("uvChannel"), 1)), 0, 7);
	Out.NormalBits = int32(NumberOr(M, TEXT("normalBits"), 8));
	Out.Fps = NumberOr(M, TEXT("fps"), 30.0);
	FString Positions, Format;
	M->TryGetStringField(TEXT("positions"), Positions);
	M->TryGetStringField(TEXT("format"), Format);
	Out.bAbsolute = Positions == TEXT("absolute");
	Out.PositionBits = int32(NumberOr(M, TEXT("positionBits"), 16));
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	const TSharedPtr<FJsonObject> Params = M->TryGetObjectField(TEXT("materialParams"), ParamsPtr) ? *ParamsPtr : nullptr;
	Out.bHdr = NumberOr(Params, TEXT("HDR"), Format == TEXT("exr") ? 1.0 : 0.0) > 0.5;
	Out.BoundsMin = VectorOr(Params, TEXT("BoundsMin"), FVector::ZeroVector);
	Out.BoundsMax = VectorOr(Params, TEXT("BoundsMax"), FVector::OneVector);
	const TSharedPtr<FJsonObject>* Bounds = nullptr;
	if (M->TryGetObjectField(TEXT("meshBounds"), Bounds))
	{
		Out.MeshBounds = FBox(VectorOr(*Bounds, TEXT("min"), FVector::ZeroVector), VectorOr(*Bounds, TEXT("max"), FVector::ZeroVector));
	}
	return Out;
}

bool RequireFile(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bRequired, FString& Out, FString& OutError)
{
	if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
	{
		if (bRequired)
		{
			OutError = FString::Printf(TEXT("importVertexAnimation: give %s"), Field);
		}
		return !bRequired;
	}
	Out = FPaths::ConvertRelativePathToFull(Out);
	if (!FPaths::FileExists(Out))
	{
		OutError = FString::Printf(TEXT("importVertexAnimation: no file at %s"), *Out);
		return false;
	}
	return true;
}

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FVatRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ParseRequest);

	FString ContentPath;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = TEXT("importVertexAnimation: give contentPath, like /Game/FX/SM_Blob");
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("importVertexAnimation: bad contentPath %s: %s"), *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);

	if (!RequireFile(Params, TEXT("fbx"), true, Out.Fbx, OutError) || !RequireFile(Params, TEXT("positions"), true, Out.Positions, OutError) ||
		!RequireFile(Params, TEXT("normals"), false, Out.Normals, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (Params->TryGetObjectField(TEXT("meta"), Meta))
	{
		Out.Meta = *Meta;
	}
	else
	{
		FString MetaFile, Text;
		if (!RequireFile(Params, TEXT("metaFile"), true, MetaFile, OutError))
		{
			return false;
		}
		FFileHelper::LoadFileToString(Text, *MetaFile);
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Out.Meta) || !Out.Meta.IsValid())
		{
			OutError = FString::Printf(TEXT("importVertexAnimation: %s is not JSON"), *MetaFile);
			return false;
		}
	}

	Params->TryGetStringField(TEXT("materialParent"), Out.MaterialParent);
	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("options"), OptionsPtr))
	{
		const TSharedPtr<FJsonObject>& Options = *OptionsPtr;
		FString Folder;
		if (Options->TryGetStringField(TEXT("materialFolder"), Folder) && !Folder.IsEmpty())
		{
			Out.MaterialFolder = Folder.StartsWith(TEXT("/")) ? Folder : Out.PackagePath / Folder;
		}
		Options->TryGetStringField(TEXT("lodGroup"), Out.LodGroup);
		Options->TryGetBoolField(TEXT("rebuildMaterial"), Out.bRebuildMaterial);
		Options->TryGetBoolField(TEXT("save"), Out.bSave);
	}
	return true;
}

template <class T> T* FindAssetAt(const FString& PackagePath, const FString& Name)
{
	const FString ObjectPath = PackagePath / Name + TEXT(".") + Name;
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** The legacy FBX factory, which reimports over an existing mesh in place and takes the degenerate-triangle setting. */
UStaticMesh* ImportMesh(const FVatRequest& R, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportMesh);

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
	// Soup padding is degenerate in the rest pose and opens up when the texture moves it.
	Static->bRemoveDegenerates = false;
	Static->NormalImportMethod = FBXNIM_ImportNormals;
	if (!R.LodGroup.IsEmpty())
	{
		Static->StaticMeshLODGroup = FName(*R.LodGroup);
	}

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
	OutErrors.Add(TEXT("The rest mesh FBX imported no static mesh; see the editor log"));
	return nullptr;
}

/** Build settings the VAT needs, and bounds that hold every frame. Rebuilds the mesh. */
void ConfigureMesh(UStaticMesh* Mesh, const FVatMeta& Meta)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ConfigureMesh);

	Mesh->Modify();
	for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
	{
		FMeshBuildSettings& Build = Mesh->GetSourceModel(Lod).BuildSettings;
		Build.bRecomputeNormals = false;
		Build.bRecomputeTangents = false;
		Build.bRemoveDegenerates = false;
		// Half precision UVs cannot address thousands of columns.
		Build.bUseFullPrecisionUVs = true;
		Build.bGenerateLightmapUVs = false;
	}
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = false;
	Mesh->SetNaniteSettings(Nanite);

	if (Meta.MeshBounds.IsValid)
	{
		const FStaticMeshRenderData* Render = Mesh->GetRenderData();
		const FBox Rest = Render ? Render->Bounds.GetBox() : FBox(FVector::ZeroVector, FVector::ZeroVector);
		Mesh->SetPositiveBoundsExtension((Meta.MeshBounds.Max - Rest.Max).ComponentMax(FVector::ZeroVector));
		Mesh->SetNegativeBoundsExtension((Rest.Min - Meta.MeshBounds.Min).ComponentMax(FVector::ZeroVector));
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
}

void ApplyDataSettings(UTexture2D* Texture, TextureCompressionSettings Compression)
{
	Texture->SRGB = false;
	Texture->CompressionSettings = Compression;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->Filter = TF_Nearest;
	Texture->NeverStream = true;
	Texture->LODGroup = TEXTUREGROUP_16BitData;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
}

/** A data texture from the file, created or updated in place. */
UTexture2D* ImportDataTexture(const FString& File, const FString& PackagePath, const FString& Name, TextureCompressionSettings Compression,
	TArray<FString>& OutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportDataTexture);

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
	ApplyDataSettings(Texture, Compression);
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
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/**
 * The shared VAT material. Frame = floor(AutoPlay ? Time * FrameRate * PlayRate (wrapped or clamped by Loop) : Frame);
 * the UV channel's V moves down Frame * RowsPerFrame / TextureHeight rows, the position texel decodes as
 * HDR ? texel : lerp(BoundsMin, BoundsMax, texel) into a local offset (Absolute subtracts the vertex's own position), and
 * World Position Offset is that offset in world space. The normal texel goes to the pixel shader through a vertex
 * interpolator as a world-space normal.
 */
void BuildVatMaterial(UMaterial* Material, int32 UvChannel, UTexture* Positions, UTexture* Normals)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildVatMaterial);

	UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
	int32 Row = 0;
	auto Make = [&](TSubclassOf<UMaterialExpression> Class, int32 X = -600) {
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, (Row++) * 120);
	};
	auto Scalar = [&](const TCHAR* Name, float Value, const TCHAR* Group) {
		auto* E = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -1400));
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Value, const TCHAR* Group) {
		auto* E = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -1400));
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	};
	auto Binary = [&](TSubclassOf<UMaterialExpression> Class, UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut) {
		UMaterialExpression* E = Make(Class);
		// Every binary math node here keeps its inputs as A then B.
		TArrayView<FExpressionInput*> Inputs = E->GetInputsView();
		Inputs[0]->Connect(AOut, A);
		if (B)
		{
			Inputs[1]->Connect(BOut, B);
		}
		return E;
	};
	auto Sample = [&](const TCHAR* Name, UTexture* Texture, UMaterialExpression* Uv) {
		auto* E = Cast<UMaterialExpressionTextureSampleParameter2D>(Make(UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300));
		E->ParameterName = Name;
		E->Group = TEXT("Vertex Animation");
		E->Texture = Texture;
		E->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
		E->SamplerSource = SSM_FromTextureAsset;
		// Sampled in the vertex shader, which has no derivatives.
		E->MipValueMode = TMVM_MipLevel;
		E->ConstMipValue = 0;
		E->Coordinates.Connect(0, Uv);
		return E;
	};

	const TCHAR* Playback = TEXT("Playback");
	const TCHAR* Layout = TEXT("Vertex Animation");
	UMaterialExpression* FrameRate = Scalar(TEXT("FrameRate"), 30.0f, Playback);
	UMaterialExpression* PlayRate = Scalar(TEXT("PlayRate"), 1.0f, Playback);
	UMaterialExpression* AutoPlay = Scalar(TEXT("AutoPlay"), 1.0f, Playback);
	UMaterialExpression* Loop = Scalar(TEXT("Loop"), 1.0f, Playback);
	UMaterialExpression* ManualFrame = Scalar(TEXT("Frame"), 0.0f, Playback);
	UMaterialExpression* FrameCount = Scalar(TEXT("FrameCount"), 1.0f, Layout);
	UMaterialExpression* RowsPerFrame = Scalar(TEXT("RowsPerFrame"), 1.0f, Layout);
	UMaterialExpression* TextureHeight = Scalar(TEXT("TextureHeight"), 1.0f, Layout);
	UMaterialExpression* Absolute = Scalar(TEXT("Absolute"), 0.0f, Layout);
	UMaterialExpression* Hdr = Scalar(TEXT("HDR"), 1.0f, Layout);
	UMaterialExpression* BoundsMin = Vector(TEXT("BoundsMin"), FLinearColor(0, 0, 0, 0), Layout);
	UMaterialExpression* BoundsMax = Vector(TEXT("BoundsMax"), FLinearColor(1, 1, 1, 0), Layout);

	UMaterialExpression* Time = Make(UMaterialExpressionTime::StaticClass());
	UMaterialExpression* Played = Binary(UMaterialExpressionMultiply::StaticClass(), Binary(UMaterialExpressionMultiply::StaticClass(), Time, 0, FrameRate, 0), 0, PlayRate, 0);
	UMaterialExpression* LastFrame = Binary(UMaterialExpressionSubtract::StaticClass(), FrameCount, 0, nullptr, 0);
	Cast<UMaterialExpressionSubtract>(LastFrame)->ConstB = 1.0f;
	UMaterialExpression* Wrapped = Binary(UMaterialExpressionFmod::StaticClass(), Played, 0, FrameCount, 0);
	UMaterialExpression* Held = Binary(UMaterialExpressionMin::StaticClass(), Played, 0, LastFrame, 0);
	auto* LoopLerp = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass()));
	LoopLerp->A.Connect(0, Held);
	LoopLerp->B.Connect(0, Wrapped);
	LoopLerp->Alpha.Connect(0, Loop);
	auto* AutoLerp = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass()));
	AutoLerp->A.Connect(0, ManualFrame);
	AutoLerp->B.Connect(0, LoopLerp);
	AutoLerp->Alpha.Connect(0, AutoPlay);
	auto* Floor = Cast<UMaterialExpressionFloor>(Make(UMaterialExpressionFloor::StaticClass()));
	Floor->Input.Connect(0, AutoLerp);
	UMaterialExpression* NotBelow = Binary(UMaterialExpressionMax::StaticClass(), Floor, 0, nullptr, 0);
	Cast<UMaterialExpressionMax>(NotBelow)->ConstB = 0.0f;
	UMaterialExpression* Frame = Binary(UMaterialExpressionMin::StaticClass(), NotBelow, 0, LastFrame, 0);

	UMaterialExpression* Rows = Binary(UMaterialExpressionMultiply::StaticClass(), Frame, 0, RowsPerFrame, 0);
	UMaterialExpression* VOffset = Binary(UMaterialExpressionDivide::StaticClass(), Rows, 0, TextureHeight, 0);
	auto* Zero = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass()));
	Zero->R = 0.0f;
	UMaterialExpression* Shift = Binary(UMaterialExpressionAppendVector::StaticClass(), Zero, 0, VOffset, 0);
	auto* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Make(UMaterialExpressionTextureCoordinate::StaticClass()));
	TexCoord->CoordinateIndex = UvChannel;
	UMaterialExpression* Uv = Binary(UMaterialExpressionAdd::StaticClass(), TexCoord, 0, Shift, 0);

	UMaterialExpression* PosTexel = Sample(TEXT("PositionTexture"), Positions, Uv);
	auto* Unnormalised = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass()));
	Unnormalised->A.Connect(0, BoundsMin);
	Unnormalised->B.Connect(0, BoundsMax);
	Unnormalised->Alpha.Connect(0, PosTexel);
	auto* Value = Cast<UMaterialExpressionLinearInterpolate>(Make(UMaterialExpressionLinearInterpolate::StaticClass()));
	Value->A.Connect(0, Unnormalised);
	Value->B.Connect(0, PosTexel);
	Value->Alpha.Connect(0, Hdr);
	auto* Local = Cast<UMaterialExpressionLocalPosition>(Make(UMaterialExpressionLocalPosition::StaticClass()));
	Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
	UMaterialExpression* RestPart = Binary(UMaterialExpressionMultiply::StaticClass(), Local, 0, Absolute, 0);
	UMaterialExpression* Offset = Binary(UMaterialExpressionSubtract::StaticClass(), Value, 0, RestPart, 0);
	auto* OffsetWorld = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass()));
	OffsetWorld->TransformSourceType = TRANSFORMSOURCE_Local;
	OffsetWorld->TransformType = TRANSFORM_World;
	OffsetWorld->Input.Connect(0, Offset);
	UMaterialEditingLibrary::ConnectMaterialProperty(OffsetWorld, TEXT(""), MP_WorldPositionOffset);

	Material->bTangentSpaceNormal = Normals == nullptr;
	if (Normals)
	{
		UMaterialExpression* NrmTexel = Sample(TEXT("NormalTexture"), Normals, Uv);
		UMaterialExpression* Doubled = Binary(UMaterialExpressionMultiply::StaticClass(), NrmTexel, 0, nullptr, 0);
		Cast<UMaterialExpressionMultiply>(Doubled)->ConstB = 2.0f;
		UMaterialExpression* Unpacked = Binary(UMaterialExpressionSubtract::StaticClass(), Doubled, 0, nullptr, 0);
		Cast<UMaterialExpressionSubtract>(Unpacked)->ConstB = 1.0f;
		auto* NormalWorld = Cast<UMaterialExpressionTransform>(Make(UMaterialExpressionTransform::StaticClass()));
		NormalWorld->TransformSourceType = TRANSFORMSOURCE_Local;
		NormalWorld->TransformType = TRANSFORM_World;
		NormalWorld->Input.Connect(0, Unpacked);
		auto* ToPixel = Cast<UMaterialExpressionVertexInterpolator>(Make(UMaterialExpressionVertexInterpolator::StaticClass()));
		ToPixel->Input.Connect(0, NormalWorld);
		UMaterialEditingLibrary::ConnectMaterialProperty(ToPixel, TEXT(""), MP_Normal);
	}

	UMaterialEditingLibrary::ConnectMaterialProperty(Vector(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Surface")), TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(TEXT("Roughness"), 0.55f, TEXT("Surface")), TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(TEXT("Metallic"), 0.0f, TEXT("Surface")), TEXT(""), MP_Metallic);

	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_NiagaraMeshParticles);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

/** materialParent when given, else the generated material for this UV channel and normal choice (built once, kept after). */
UMaterialInterface* ParentMaterial(const FVatRequest& R, const FVatMeta& Meta, UTexture* Positions, UTexture* Normals, TArray<FString>& OutCreated,
	TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ParentMaterial);

	if (!R.MaterialParent.IsEmpty())
	{
		const FString Path = R.MaterialParent.Contains(TEXT(".")) ? R.MaterialParent : R.MaterialParent + TEXT(".") + FPackageName::GetShortName(R.MaterialParent);
		if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path))
		{
			return Parent;
		}
		OutErrors.Add(FString::Printf(TEXT("No material at %s"), *R.MaterialParent));
		return nullptr;
	}
	const FString Name = FString::Printf(TEXT("M_LilaVAT_UV%d%s"), Meta.UvChannel, Normals ? TEXT("") : TEXT("_NoNormals"));
	UMaterial* Material = FindAssetAt<UMaterial>(R.MaterialFolder, Name);
	if (Material && !R.bRebuildMaterial)
	{
		return Material;
	}
	if (!Material)
	{
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>(GetTransientPackage());
		Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.MaterialFolder, UMaterial::StaticClass(), Factory));
		if (!Material)
		{
			OutErrors.Add(FString::Printf(TEXT("Could not create material %s in %s"), *Name, *R.MaterialFolder));
			return nullptr;
		}
	}
	BuildVatMaterial(Material, Meta.UvChannel, Positions, Normals);
	OutCreated.AddUnique(Material->GetPathName());
	return Material;
}

UMaterialInstanceConstant* MakeInstance(const FVatRequest& R, UMaterialInterface* Parent, TArray<FString>& OutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::MakeInstance);

	const FString Name = TEXT("MI_") + R.AssetName;
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
	OutCreated.AddUnique(Instance->GetPathName());
	return Instance;
}

void SetInstanceParameters(UMaterialInstanceConstant* Instance, const FVatMeta& Meta, UTexture* Positions, UTexture* Normals)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetInstanceParameters);

	// A user's own parent may have only some of these; the ones it lacks are skipped.
	auto Names = [&](EMaterialParameterType Type) {
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Instance->GetAllParameterInfoOfType(Type, Infos, Ids);
		TSet<FName> Out;
		for (const FMaterialParameterInfo& Info : Infos)
		{
			Out.Add(Info.Name);
		}
		return Out;
	};
	const TSet<FName> Scalars = Names(EMaterialParameterType::Scalar);
	const TSet<FName> Vectors = Names(EMaterialParameterType::Vector);
	const TSet<FName> Textures = Names(EMaterialParameterType::Texture);
	auto SetScalar = [&](const TCHAR* Name, float Value) {
		if (Scalars.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Name, Value);
		}
	};
	auto SetVector = [&](const TCHAR* Name, const FVector& Value) {
		if (Vectors.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name, FLinearColor(FVector3f(Value)));
		}
	};
	auto SetTexture = [&](const TCHAR* Name, UTexture* Value) {
		if (Value && Textures.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Name, Value);
		}
	};
	SetTexture(TEXT("PositionTexture"), Positions);
	SetTexture(TEXT("NormalTexture"), Normals);
	SetScalar(TEXT("HDR"), Meta.bHdr ? 1.0f : 0.0f);
	SetVector(TEXT("BoundsMin"), Meta.BoundsMin);
	SetVector(TEXT("BoundsMax"), Meta.BoundsMax);
	SetScalar(TEXT("FrameCount"), float(Meta.FrameCount));
	SetScalar(TEXT("RowsPerFrame"), float(Meta.RowsPerFrame));
	SetScalar(TEXT("TextureHeight"), float(Meta.Height));
	SetScalar(TEXT("FrameRate"), float(Meta.Fps));
	SetScalar(TEXT("Absolute"), Meta.bAbsolute ? 1.0f : 0.0f);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
}

/** EXR stays half float, 16-bit PNG keeps its precision as RGBA32F, 8-bit PNG stays RGBA8. */
TextureCompressionSettings PositionCompression(const FVatMeta& Meta)
{
	if (Meta.bHdr)
	{
		return TC_HDR;
	}
	return Meta.PositionBits <= 8 ? TC_VectorDisplacementmap : TC_HDR_F32;
}

TSharedPtr<FJsonValue> Strings(const TArray<FString>& In)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& S : In)
	{
		Values.Add(MakeShared<FJsonValueString>(S));
	}
	return MakeShared<FJsonValueArray>(Values);
}

/** The position and normal textures, the material and its instance, assigned to every slot of Mesh when there is one. */
void BuildVatAssets(const FVatRequest& R, const FVatMeta& Meta, UStaticMesh* Mesh, TArray<FString>& Assets, TArray<FString>& Errors,
	TFunctionRef<void(const TCHAR*)> Progress)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildVatAssets);

	Progress(TEXT("importing the textures"));
	const FString Base = R.AssetName.StartsWith(TEXT("SM_")) ? R.AssetName.RightChop(3) : R.AssetName;
	UTexture2D* Positions = ImportDataTexture(R.Positions, R.PackagePath, TEXT("T_") + Base + TEXT("_VATPos"), PositionCompression(Meta), Assets, Errors);
	UTexture2D* Normals = nullptr;
	if (!R.Normals.IsEmpty())
	{
		Normals = ImportDataTexture(R.Normals, R.PackagePath, TEXT("T_") + Base + TEXT("_VATNrm"), Meta.NormalBits > 8 ? TC_HDR : TC_VectorDisplacementmap, Assets, Errors);
	}

	if (Mesh && Positions)
	{
		Progress(TEXT("building the material"));
		if (UMaterialInterface* Parent = ParentMaterial(R, Meta, Positions, Normals, Assets, Errors))
		{
			if (UMaterialInstanceConstant* Instance = MakeInstance(R, Parent, Assets, Errors))
			{
				SetInstanceParameters(Instance, Meta, Positions, Normals);
				Mesh->Modify();
				for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
				{
					Mesh->SetMaterial(Slot, Instance);
				}
				Mesh->MarkPackageDirty();
			}
		}
	}
}

void ImportVertexAnimation(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportVertexAnimation);

	FVatRequest R;
	FString Error;
	if (!ParseRequest(Params, R, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}
	const FVatMeta Meta = ParseMeta(R.Meta);
	TArray<FString> Assets, Warnings, Errors;

	Call->Progress(TEXT("importing the rest mesh"));
	UStaticMesh* Mesh = ImportMesh(R, Errors);
	if (Mesh)
	{
		ConfigureMesh(Mesh, Meta);
		Assets.Add(Mesh->GetPathName());
	}

	BuildVatAssets(R, Meta, Mesh, Assets, Errors, [&Call](const TCHAR* Stage) { Call->Progress(Stage); });

	for (const FString& E : Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *R.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("importVertexAnimation %s/%s: %d asset(s), %d frame(s) of %dx%d"), *R.PackagePath, *R.AssetName, Assets.Num(), Meta.FrameCount,
		Meta.Width, Meta.Height);
	if (!Assets.IsEmpty() && (R.bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Assets, Errors);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("assets"), Strings(Assets));
	Out->SetField(TEXT("warnings"), Strings(Warnings));
	Out->SetField(TEXT("errors"), Strings(Errors));
	Out->SetStringField(TEXT("method"), TEXT("vat"));
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

void LilaLink::AddVatMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importVertexAnimation"), &ImportVertexAnimation);
}

bool LilaLink::SetUpVatOnMesh(UStaticMesh* Mesh, const FString& MetaFile, TArray<FString>& OutAssets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetUpVatOnMesh);

	FString Text;
	FFileHelper::LoadFileToString(Text, *MetaFile);
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		OutErrors.Add(FString::Printf(TEXT("%s is not a VAT metadata file"), *MetaFile));
		return false;
	}

	FVatRequest R;
	R.PackagePath = FPackageName::GetLongPackagePath(Mesh->GetPackage()->GetName());
	R.AssetName = Mesh->GetName();
	R.Meta = Json;

	const FString Folder = FPaths::GetPath(MetaFile);
	const FString Name = FPaths::GetBaseFilename(MetaFile);
	const TSharedPtr<FJsonObject>* Files = nullptr;
	Json->TryGetObjectField(TEXT("files"), Files);
	auto BakeFile = [&](const TCHAR* Key, std::initializer_list<FString> Fallbacks) {
		FString Listed;
		if (Files && (*Files)->TryGetStringField(Key, Listed) && !Listed.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(Folder, Listed);
		}
		for (const FString& Fallback : Fallbacks)
		{
			if (FPaths::FileExists(Folder / Fallback))
			{
				return FPaths::ConvertRelativePathToFull(Folder / Fallback);
			}
		}
		return FString();
	};
	R.Positions = BakeFile(TEXT("positions"), {Name + TEXT("_pos.exr"), Name + TEXT("_pos.png")});
	R.Normals = BakeFile(TEXT("normals"), {Name + TEXT("_nrm.png")});
	if (R.Positions.IsEmpty() || !FPaths::FileExists(R.Positions))
	{
		OutErrors.Add(FString::Printf(TEXT("No position texture beside %s"), *MetaFile));
		return false;
	}
	if (!R.Normals.IsEmpty() && !FPaths::FileExists(R.Normals))
	{
		OutErrors.Add(FString::Printf(TEXT("No normal texture at %s; set up without normals"), *R.Normals));
		R.Normals.Reset();
	}

	const FVatMeta Meta = ParseMeta(R.Meta);
	if (Mesh->GetNumUVChannels(0) <= Meta.UvChannel)
	{
		OutErrors.Add(FString::Printf(TEXT("%s has %d UV channel(s); the bake addresses its texels through UV %d, so it is not the mesh this bake was made from"),
			*Mesh->GetName(), Mesh->GetNumUVChannels(0), Meta.UvChannel));
		return false;
	}
	ConfigureMesh(Mesh, Meta);
	OutAssets.AddUnique(Mesh->GetPathName());
	BuildVatAssets(R, Meta, Mesh, OutAssets, OutErrors, [](const TCHAR*) {});
	return OutErrors.IsEmpty();
}

void LilaLink::SetUpVatDataTexture(UTexture2D* Texture, bool bNormals)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetUpVatDataTexture);

	TextureCompressionSettings Compression = TC_VectorDisplacementmap;
	switch (Texture->Source.GetFormat())
	{
	case TSF_RGBA16F:
	case TSF_R16F:
		Compression = TC_HDR;
		break;
	case TSF_RGBA16:
	case TSF_G16:
	case TSF_RGBA32F:
	case TSF_R32F:
		Compression = bNormals ? TC_HDR : TC_HDR_F32;
		break;
	default:
		break;
	}
	Texture->Modify();
	Texture->PreEditChange(nullptr);
	ApplyDataSettings(Texture, Compression);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
}
