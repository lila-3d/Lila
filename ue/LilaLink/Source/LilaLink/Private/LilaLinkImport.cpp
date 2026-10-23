#include "LilaLinkMethods.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterials.h"
#include "LilaLinkSettings.h"

#include "AbcImportSettings.h"
#include "AssetImportTask.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EditorReimportHandler.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "IAssetTools.h"
#include "InterchangeGenericAnimationPipeline.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeGenericTexturePipeline.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeManager.h"
#include "InterchangeMeshDefinitions.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
enum class EKind : uint8
{
	Static,
	Skeletal,
	Animation,
	GeometryCache,
};

struct FImportRequest
{
	FString File;
	FString PackagePath; // /Game/Props
	FString AssetName;   // SM_Crate
	EKind Kind = EKind::Static;
	FString Skeleton;
	FString LodGroup;
	bool bImportMaterials = true;
	bool bImportTextures = true;
	bool bCombineMeshes = true;
	bool bAutoCollision = false;
	bool bGenerateLightmapUVs = false;
	bool bComputeNormals = false;
	bool bNanite = false;
	bool bImportAnimations = false;
	/** Reimport keeps the asset's stored settings unless this is set. */
	bool bReapplyOptions = false;
	bool bReplace = false;
	bool bSave = false;
	LilaLink::FMaterialRequest Materials;
};

struct FImportOutcome
{
	TArray<FString> Assets;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	FString Method;
	TArray<LilaLink::FSlotResult> Slots;
};

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FImportRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ParseRequest);

	if (!Params->TryGetStringField(TEXT("file"), Out.File) || Out.File.IsEmpty())
	{
		OutError = TEXT("importAsset: give file");
		return false;
	}
	Out.File = FPaths::ConvertRelativePathToFull(Out.File);
	if (!FPaths::FileExists(Out.File))
	{
		OutError = FString::Printf(TEXT("importAsset: no file at %s"), *Out.File);
		return false;
	}

	FString ContentPath;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = TEXT("importAsset: give contentPath, like /Game/Props/SM_Crate");
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("importAsset: bad contentPath %s: %s"), *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);

	FString Kind = TEXT("static");
	Params->TryGetStringField(TEXT("kind"), Kind);
	if (Kind == TEXT("static"))
	{
		Out.Kind = EKind::Static;
	}
	else if (Kind == TEXT("skeletal"))
	{
		Out.Kind = EKind::Skeletal;
	}
	else if (Kind == TEXT("animation"))
	{
		Out.Kind = EKind::Animation;
	}
	else if (Kind == TEXT("geometryCache"))
	{
		Out.Kind = EKind::GeometryCache;
	}
	else
	{
		OutError = FString::Printf(TEXT("importAsset: kind must be static, skeletal, animation or geometryCache, not '%s'"), *Kind);
		return false;
	}

	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	TSharedPtr<FJsonObject> Options = Params->TryGetObjectField(TEXT("options"), OptionsPtr) ? *OptionsPtr : MakeShared<FJsonObject>();
	Options->TryGetStringField(TEXT("skeleton"), Out.Skeleton);
	Options->TryGetStringField(TEXT("lodGroup"), Out.LodGroup);
	Options->TryGetBoolField(TEXT("importMaterials"), Out.bImportMaterials);
	Options->TryGetBoolField(TEXT("importTextures"), Out.bImportTextures);
	Options->TryGetBoolField(TEXT("combineMeshes"), Out.bCombineMeshes);
	Options->TryGetBoolField(TEXT("autoCollision"), Out.bAutoCollision);
	Options->TryGetBoolField(TEXT("generateLightmapUVs"), Out.bGenerateLightmapUVs);
	Options->TryGetBoolField(TEXT("nanite"), Out.bNanite);
	Options->TryGetBoolField(TEXT("importAnimations"), Out.bImportAnimations);
	Options->TryGetBoolField(TEXT("reapplyOptions"), Out.bReapplyOptions);
	Options->TryGetBoolField(TEXT("save"), Out.bSave);

	FString Normals = TEXT("import");
	Options->TryGetStringField(TEXT("normals"), Normals);
	Out.bComputeNormals = Normals == TEXT("compute");

	FString Existing = TEXT("reimport");
	Options->TryGetStringField(TEXT("existing"), Existing);
	Out.bReplace = Existing == TEXT("replace");

	if (!LilaLink::ParseMaterialRequest(Params, Options, Out.PackagePath, Out.Materials, OutError))
	{
		return false;
	}

	if (Out.Kind == EKind::Animation && Out.Skeleton.IsEmpty())
	{
		OutError = TEXT("importAsset: animation imports need options.skeleton");
		return false;
	}
	return true;
}

FString ObjectPathOf(const FImportRequest& R)
{
	return R.PackagePath / R.AssetName + TEXT(".") + R.AssetName;
}

UObject* FindExisting(const FImportRequest& R)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPathOf(R)));
	return Data.IsValid() ? Data.GetAsset() : nullptr;
}

/** Starts from the project's FBX pipeline asset so project-wide tweaks carry over, then applies the request. */
UInterchangeGenericAssetsPipeline* MakePipeline(const FImportRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::MakePipeline);

	UInterchangeGenericAssetsPipeline* Pipeline = nullptr;
	for (const TCHAR* Path : {TEXT("/Interchange/Pipelines/DefaultFBXOBJAssetsPipeline.DefaultFBXOBJAssetsPipeline"), TEXT("/Interchange/Pipelines/DefaultAssetsPipeline.DefaultAssetsPipeline")})
	{
		if (UInterchangeGenericAssetsPipeline* Template = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Pipeline = DuplicateObject<UInterchangeGenericAssetsPipeline>(Template, GetTransientPackage());
			break;
		}
	}
	if (!Pipeline)
	{
		Pipeline = NewObject<UInterchangeGenericAssetsPipeline>(GetTransientPackage());
	}

	Pipeline->bUseSourceNameForAsset = false;
	Pipeline->AssetName = R.AssetName;
	Pipeline->bSceneNameSubFolder = false;
	Pipeline->bAssetTypeSubFolders = false;

	UInterchangeGenericCommonMeshesProperties* Common = Pipeline->CommonMeshesProperties;
	Common->ForceAllMeshAsType = R.Kind == EKind::Static ? EInterchangeForceMeshType::IFMT_StaticMesh : EInterchangeForceMeshType::IFMT_SkeletalMesh;
	Common->bRecomputeNormals = R.bComputeNormals;
	Common->bImportLods = true;

	UInterchangeGenericCommonSkeletalMeshesAndAnimationsProperties* SkelCommon = Pipeline->CommonSkeletalMeshesAndAnimationsProperties;
	SkelCommon->bImportOnlyAnimations = R.Kind == EKind::Animation;
	if (!R.Skeleton.IsEmpty())
	{
		SkelCommon->Skeleton = TSoftObjectPtr<USkeleton>(FSoftObjectPath(R.Skeleton));
	}

	UInterchangeGenericMeshPipeline* Mesh = Pipeline->MeshPipeline;
	Mesh->bImportStaticMeshes = R.Kind == EKind::Static;
	Mesh->bImportSkeletalMeshes = R.Kind == EKind::Skeletal;
	Mesh->CombineStaticMeshesBehavior = R.bCombineMeshes ? EInterchangeCombineStaticMeshesBehavior::All : EInterchangeCombineStaticMeshesBehavior::DoNotCombine;
	Mesh->bGenerateLightmapUVs = R.bGenerateLightmapUVs;
	Mesh->bBuildNanite = R.bNanite;
	Mesh->LodGroup = R.LodGroup.IsEmpty() ? NAME_None : FName(*R.LodGroup);
	// UCX_ and friends still import; this only stops the fallback hull when there are none.
	Mesh->bCollision = true;
	Mesh->bImportCollisionAccordingToMeshName = true;
	if (!R.bAutoCollision)
	{
		Mesh->Collision = EInterchangeMeshCollision::None;
	}

	Pipeline->AnimationPipeline->bImportAnimations = R.Kind == EKind::Animation || R.bImportAnimations;

	// Managed materials are matched and made after the import, by slot name.
	Pipeline->MaterialPipeline->bImportMaterials = R.bImportMaterials && !R.Materials.bManaged;
	if (Pipeline->MaterialPipeline->TexturePipeline)
	{
		Pipeline->MaterialPipeline->TexturePipeline->bImportTextures = R.bImportTextures && !R.Materials.bManaged;
	}
	return Pipeline;
}

UInterchangeAssetImportData* InterchangeDataOf(UObject* Asset)
{
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		return Cast<UInterchangeAssetImportData>(Mesh->GetAssetImportData());
	}
	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
	{
		return Cast<UInterchangeAssetImportData>(Mesh->GetAssetImportData());
	}
	return nullptr;
}

/** The asset's stored pipelines with material import off, so a reimport keeps its settings but never makes materials. */
TArray<UObject*> StoredPipelinesWithoutMaterials(UObject* Existing)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::StoredPipelinesWithoutMaterials);

	TArray<UObject*> Out;
	UInterchangeAssetImportData* Data = InterchangeDataOf(Existing);
	if (!Data)
	{
		return Out;
	}
	for (UObject* Stored : Data->GetPipelines())
	{
		if (!Stored)
		{
			continue;
		}
		UObject* Copy = DuplicateObject<UObject>(Stored, GetTransientPackage());
		if (UInterchangeGenericAssetsPipeline* Generic = Cast<UInterchangeGenericAssetsPipeline>(Copy))
		{
			Generic->MaterialPipeline->bImportMaterials = false;
			if (Generic->MaterialPipeline->TexturePipeline)
			{
				Generic->MaterialPipeline->TexturePipeline->bImportTextures = false;
			}
		}
		Out.Add(Copy);
	}
	return Out;
}

void CollectResults(const UE::Interchange::FAssetImportResultRef& Result, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::CollectResults);

	for (UObject* Object : Result->GetImportedObjects())
	{
		if (Object)
		{
			Out.Assets.AddUnique(Object->GetPathName());
		}
	}
	if (UInterchangeResultsContainer* Results = Result->GetResults())
	{
		for (UInterchangeResult* Item : Results->GetResults())
		{
			if (!Item)
			{
				continue;
			}
			if (Item->GetResultType() == EInterchangeResultType::Warning)
			{
				Out.Warnings.Add(Item->GetText().ToString());
			}
			else if (Item->GetResultType() == EInterchangeResultType::Error)
			{
				Out.Errors.Add(Item->GetText().ToString());
			}
		}
	}
}

/** An Interchange import in flight, polled from the core ticker until it is done. */
struct FInterchangeJob
{
	FImportRequest Request;
	FImportOutcome Outcome;
	TSharedPtr<FLilaLinkCall, ESPMode::ThreadSafe> Call;
	TSharedPtr<UE::Interchange::FImportResult, ESPMode::ThreadSafe> Result;
	TStrongObjectPtr<UInterchangeGenericAssetsPipeline> Pipeline;
	TArray<TStrongObjectPtr<UObject>> StoredPipelines;
	double NextProgressSeconds = 0.0;
};

void FinishImport(const FImportRequest& Request, FImportOutcome& Outcome, const FLilaLinkCallRef& Call);

/**
 * Starts the import without waiting for it: the synchronous Interchange API pumps game-thread tasks while it waits,
 * which must not happen from inside one. False when Interchange cannot read the file.
 */
bool StartInterchangeImport(const FImportRequest& R, UObject* Existing, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::StartInterchangeImport);

	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UE::Interchange::FScopedSourceData Source(R.File);
	if (!Manager.CanTranslateSourceData(Source.GetSourceData()))
	{
		UE_LOG(LogLilaLink, Log, TEXT("Interchange cannot read %s; using the FBX factory"), *R.File);
		return false;
	}

	TSharedRef<FInterchangeJob> Job = MakeShared<FInterchangeJob>();
	Job->Request = R;
	Job->Call = Call;

	FImportAssetParameters Params;
	Params.bIsAutomated = true;
	Params.bReplaceExisting = true;
	Params.DestinationName = R.AssetName;

	if (!Existing || R.bReapplyOptions)
	{
		Job->Pipeline.Reset(MakePipeline(R));
		Params.OverridePipelines.Add(FSoftObjectPath(Job->Pipeline.Get()));
	}
	else if (R.Materials.bManaged)
	{
		for (UObject* Stored : StoredPipelinesWithoutMaterials(Existing))
		{
			Job->StoredPipelines.Emplace(Stored);
			Params.OverridePipelines.Add(FSoftObjectPath(Stored));
		}
	}

	FString ContentPath = R.PackagePath;
	if (Existing)
	{
		Params.ReimportAsset = Existing;
		ContentPath.Reset();
		Job->Outcome.Method = TEXT("interchange-reimport");
	}
	else
	{
		Job->Outcome.Method = TEXT("interchange");
	}

	UE_LOG(LogLilaLink, Log, TEXT("%s: starting %s of %s into %s"), *Call->Describe(), *Job->Outcome.Method, *R.File, *ObjectPathOf(R));
	Job->Result = Manager.ImportAssetAsync(ContentPath, Source.GetSourceData(), Params);
	Call->Progress(TEXT("importing"));
	Job->NextProgressSeconds = FPlatformTime::Seconds() + 1.0;

	FTSTicker::GetCoreTicker().AddTicker(TEXT("LilaLinkImport"), 0.05f, [Job](float)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::PollInterchange);

		if (Job->Result->GetStatus() != UE::Interchange::FImportResult::EStatus::Done)
		{
			const double Now = FPlatformTime::Seconds();
			if (Now >= Job->NextProgressSeconds)
			{
				Job->NextProgressSeconds = Now + 1.0;
				Job->Call->Progress(TEXT("importing"));
			}
			return true;
		}
		CollectResults(Job->Result.ToSharedRef(), Job->Outcome);
		if (Job->Outcome.Assets.IsEmpty() && Job->Outcome.Errors.IsEmpty())
		{
			Job->Outcome.Errors.Add(TEXT("Interchange imported nothing; see LogInterchange in the editor log"));
		}
		FinishImport(Job->Request, Job->Outcome, Job->Call.ToSharedRef());
		return false;
	});
	return true;
}

UFbxImportUI* MakeFbxOptions(const FImportRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::MakeFbxOptions);

	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bAutomatedImportShouldDetectType = false;
	Options->bImportMesh = R.Kind != EKind::Animation;
	Options->bImportAsSkeletal = R.Kind != EKind::Static;
	Options->MeshTypeToImport = R.Kind == EKind::Static ? FBXIT_StaticMesh : (R.Kind == EKind::Skeletal ? FBXIT_SkeletalMesh : FBXIT_Animation);
	Options->OriginalImportType = Options->MeshTypeToImport;
	Options->bImportAnimations = R.Kind == EKind::Animation || R.bImportAnimations;
	Options->bImportMaterials = R.bImportMaterials && !R.Materials.bManaged;
	Options->bImportTextures = R.bImportTextures && !R.Materials.bManaged;
	Options->bCreatePhysicsAsset = R.Kind == EKind::Skeletal;
	if (!R.Skeleton.IsEmpty())
	{
		Options->Skeleton = LoadObject<USkeleton>(nullptr, *R.Skeleton);
	}

	const EFBXNormalImportMethod Normals = R.bComputeNormals ? FBXNIM_ComputeNormals : FBXNIM_ImportNormals;
	UFbxStaticMeshImportData* Static = Options->StaticMeshImportData;
	Static->bCombineMeshes = R.bCombineMeshes;
	Static->bAutoGenerateCollision = R.bAutoCollision;
	Static->bGenerateLightmapUVs = R.bGenerateLightmapUVs;
	Static->bBuildNanite = R.bNanite;
	Static->NormalImportMethod = Normals;
	if (!R.LodGroup.IsEmpty())
	{
		Static->StaticMeshLODGroup = FName(*R.LodGroup);
	}
	Options->SkeletalMeshImportData->NormalImportMethod = Normals;
	return Options;
}

void ImportWithFbxFactory(const FImportRequest& R, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ImportWithFbxFactory);

	Out.Method = TEXT("fbx-factory");
	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.File;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	// An explicit factory keeps AssetTools off the Interchange route.
	Task->Factory = NewObject<UFbxFactory>(GetTransientPackage());
	Task->Options = MakeFbxOptions(R);

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});
	for (const FString& Path : Task->ImportedObjectPaths)
	{
		Out.Assets.AddUnique(Path);
	}
	if (Out.Assets.IsEmpty())
	{
		Out.Errors.Add(TEXT("FBX import produced no assets; see the editor log"));
	}
}

/**
 * An .abc as a Geometry Cache through the Alembic importer, every frame of the file. Over an existing cache the factory
 * reimports in place, so actors and sequencer tracks using it keep working.
 */
void ImportGeometryCache(const FImportRequest& R, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ImportGeometryCache);

	Out.Method = TEXT("alembic");
	// Found by name so the module does not link the importer; it is a plugin a project can turn off.
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/AlembicImporter.AlembicImportFactory"));
	if (!FactoryClass)
	{
		Out.Errors.Add(TEXT("Geometry caches need the Alembic Importer plugin; enable it in this project"));
		return;
	}
	UAbcImportSettings* Settings = NewObject<UAbcImportSettings>(GetTransientPackage());
	Settings->ImportType = EAlembicImportType::GeometryCache;
	Settings->SamplingSettings.SamplingType = EAlembicSamplingType::PerFrame;
	// Lila writes real times, so a range from frame 32 starts 32 frames in; skipping the empty start keeps frame 0 the first sample.
	Settings->SamplingSettings.bSkipEmpty = true;
	Settings->SamplingSettings.FrameStart = 0;
	Settings->SamplingSettings.FrameEnd = 0;
	Settings->NormalGenerationSettings.bRecomputeNormals = R.bComputeNormals;
	Settings->MaterialSettings.bFindMaterials = true;
	Settings->MaterialSettings.bCreateMaterials = false;
	Settings->GeometryCacheSettings.bFlattenTracks = true;

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.File;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	Task->Options = Settings;

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});
	for (const FString& Path : Task->ImportedObjectPaths)
	{
		Out.Assets.AddUnique(Path);
	}
	if (Out.Assets.IsEmpty())
	{
		if (UObject* Existing = FindExisting(R))
		{
			Out.Assets.Add(Existing->GetPathName());
		}
		else
		{
			Out.Errors.Add(TEXT("Alembic import produced no Geometry Cache; see the AlembicImporter messages in the editor log"));
		}
	}
}

void ReimportWithManager(const FImportRequest& R, UObject* Existing, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ReimportWithManager);

	Out.Method = TEXT("reimport");
	FReimportManager* Manager = FReimportManager::Instance();
	if (!Manager->CanReimport(Existing))
	{
		Out.Errors.Add(FString::Printf(TEXT("%s has no import source to reimport from; pass options.existing=\"replace\""), *Existing->GetPathName()));
		return;
	}
	Manager->UpdateReimportPaths(Existing, {R.File});
	if (Manager->Reimport(Existing, false, false, R.File, nullptr, INDEX_NONE, false, true))
	{
		Out.Assets.Add(Existing->GetPathName());
	}
	else
	{
		Out.Errors.Add(FString::Printf(TEXT("Reimport of %s failed; see the editor log"), *Existing->GetPathName()));
	}
}

TSharedPtr<FJsonValue> ToJson(const FImportOutcome& Out)
{
	auto Strings = [](const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& S : In)
		{
			Values.Add(MakeShared<FJsonValueString>(S));
		}
		return Values;
	};
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("assets"), Strings(Out.Assets));
	Json->SetArrayField(TEXT("warnings"), Strings(Out.Warnings));
	Json->SetArrayField(TEXT("errors"), Strings(Out.Errors));
	Json->SetStringField(TEXT("method"), Out.Method);
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (const LilaLink::FSlotResult& S : Out.Slots)
	{
		TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("asset"), S.Asset);
		Slot->SetStringField(TEXT("slot"), S.Slot.ToString());
		Slot->SetStringField(TEXT("material"), S.Material);
		Slot->SetStringField(TEXT("how"), S.How);
		Slots.Add(MakeShared<FJsonValueObject>(Slot));
	}
	Json->SetArrayField(TEXT("materialSlots"), Slots);
	return MakeShared<FJsonValueObject>(Json);
}

void FinishImport(const FImportRequest& Request, FImportOutcome& Outcome, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::FinishImport);

	if (Request.Materials.bManaged && !Outcome.Assets.IsEmpty())
	{
		Call->Progress(TEXT("assigning materials"));
		TArray<FString> Created;
		LilaLink::ApplyMaterials(Request.Materials, Outcome.Assets, Created, Outcome.Warnings, Outcome.Slots);
		for (const FString& C : Created)
		{
			Outcome.Assets.AddUnique(C);
		}
	}

	for (const FString& W : Outcome.Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s: %s"), *Request.AssetName, *W);
	}
	for (const FString& E : Outcome.Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *Request.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("Imported %s -> %s via %s (%d assets)"), *Request.File, *ObjectPathOf(Request), *Outcome.Method, Outcome.Assets.Num());

	if (!Outcome.Assets.IsEmpty() && (Request.bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Outcome.Assets, Outcome.Errors);
	}
	Call->Reply(ToJson(Outcome));
}
} // namespace

void LilaLink::ImportAsset(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ImportAsset);

	FImportRequest Request;
	FString Error;
	if (!ParseRequest(Params, Request, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}

	if (Request.Kind == EKind::GeometryCache)
	{
		Call->Progress(TEXT("importing"));
		FImportOutcome Outcome;
		ImportGeometryCache(Request, Outcome);
		FinishImport(Request, Outcome, Call);
		return;
	}

	UObject* Existing = Request.bReplace ? nullptr : FindExisting(Request);
	if (Existing && Request.Materials.bManaged)
	{
		LilaLink::SnapshotMaterials(Existing, Request.Materials);
	}
	const bool bInterchangeOwnsExisting = [&]
	{
		TArray<FString> Files;
		return Existing && UInterchangeManager::GetInterchangeManager().CanReimport(Existing, Files);
	}();

	FImportOutcome Outcome;
	if (Existing && !bInterchangeOwnsExisting)
	{
		ReimportWithManager(Request, Existing, Outcome);
	}
	else if (UInterchangeManager::IsInterchangeImportEnabled() && StartInterchangeImport(Request, Existing, Call))
	{
		return;
	}
	else
	{
		ImportWithFbxFactory(Request, Outcome);
	}
	FinishImport(Request, Outcome, Call);
}
