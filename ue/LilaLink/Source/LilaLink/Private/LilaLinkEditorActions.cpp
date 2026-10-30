#include "LilaLinkEditorActions.h"

#include "LilaLinkLog.h"
#include "LilaLinkSequence.h"
#include "LilaLinkVat.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "IContentBrowserSingleton.h"
#include "IDesktopPlatform.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#include "LilaLinkTrim.h"
#include "Materials/Material.h"

#define LOCTEXT_NAMESPACE "LilaLinkActions"

namespace
{
void Report(const FText& Message, bool bSuccess)
{
	if (bSuccess)
	{
		UE_LOG(LogLilaLink, Log, TEXT("%s"), *Message.ToString());
	}
	else
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s"), *Message.ToString());
	}
	FNotificationInfo Info(Message);
	Info.ExpireDuration = bSuccess ? 5.f : 8.f;
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

/** "a, b, c and 4 more", short enough for a toast. */
FText NameList(const TArray<FString>& Names)
{
	constexpr int32 MaxNamed = 6;
	TArray<FString> Named(Names.GetData(), FMath::Min(Names.Num(), MaxNamed));
	FString List = FString::Join(Named, TEXT(", "));
	if (Names.Num() > MaxNamed)
	{
		List += FString::Printf(TEXT(" and %d more"), Names.Num() - MaxNamed);
	}
	return FText::FromString(List);
}

TArray<FAssetData> SelectedAssets()
{
	TArray<FAssetData> Selected;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Selected);
	return Selected;
}

template <class T> TArray<FAssetData> SelectedOf()
{
	TArray<FAssetData> Out = SelectedAssets();
	Out.RemoveAll([](const FAssetData& Asset) { return !Asset.IsInstanceOf(T::StaticClass()); });
	return Out;
}

template <class T> TArray<T*> LoadSelected()
{
	TArray<T*> Out;
	for (const FAssetData& Asset : SelectedOf<T>())
	{
		if (T* Object = Cast<T>(Asset.GetAsset()))
		{
			Out.Add(Object);
		}
	}
	return Out;
}

/** Absolute source files from the asset registry tag, so nothing has to load. */
TArray<FString> SourceFilesOf(const FAssetData& Asset)
{
	TArray<FString> Out;
	FString Json;
	if (!Asset.GetTagValue(UObject::SourceFileTagName(), Json) || Json.IsEmpty())
	{
		return Out;
	}
	if (const TOptional<FAssetImportInfo> Info = FAssetImportInfo::FromJson(Json))
	{
		const FString PackageName = Asset.PackageName.ToString();
		for (const FAssetImportInfo::FSourceFile& File : Info->SourceFiles)
		{
			if (!File.RelativeFilename.IsEmpty())
			{
				Out.Add(UAssetImportData::ResolveImportFilename(FStringView(File.RelativeFilename), FStringView(PackageName)));
			}
		}
	}
	return Out;
}

/** Lila exports everything it sends to Unreal under <workspace state or user config>/cache/unreal. */
bool IsInLilaCache(const FString& File)
{
	FString Normalized = File;
	FPaths::NormalizeFilename(Normalized);
	return Normalized.Contains(TEXT("/cache/unreal/"), ESearchCase::IgnoreCase);
}

bool HasLilaSource(const FAssetData& Asset)
{
	return SourceFilesOf(Asset).ContainsByPredicate(&IsInLilaCache);
}

/** Frame names sort by their trailing number, so SM_Blob_10 follows SM_Blob_9. */
bool NaturalLess(const FString& A, const FString& B)
{
	auto Split = [](const FString& S, FString& OutStem, int64& OutNumber) {
		int32 Digits = 0;
		while (Digits < S.Len() && FChar::IsDigit(S[S.Len() - 1 - Digits]))
		{
			++Digits;
		}
		OutStem = S.LeftChop(Digits);
		OutNumber = Digits > 0 ? FCString::Atoi64(*S.Right(Digits)) : -1;
	};
	FString StemA, StemB;
	int64 NumberA = 0, NumberB = 0;
	Split(A, StemA, NumberA);
	Split(B, StemB, NumberB);
	if (!StemA.Equals(StemB, ESearchCase::IgnoreCase))
	{
		return StemA.Compare(StemB, ESearchCase::IgnoreCase) < 0;
	}
	if (NumberA != NumberB)
	{
		return NumberA < NumberB;
	}
	return A.Compare(B, ESearchCase::IgnoreCase) < 0;
}

FString FindVatMeta(const FAssetData& Asset)
{
	for (const FString& Source : SourceFilesOf(Asset))
	{
		const FString Meta = FPaths::ChangeExtension(Source, TEXT("json"));
		if (FPaths::FileExists(Meta))
		{
			return Meta;
		}
	}
	return FString();
}

FString PickVatMeta(const FAssetData& Asset)
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		return FString();
	}
	const TArray<FString> Sources = SourceFilesOf(Asset);
	const FString DefaultFolder = Sources.IsEmpty() ? FString() : FPaths::GetPath(Sources[0]);
	TArray<FString> Files;
	const bool bPicked = Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
		FString::Printf(TEXT("VAT bake for %s"), *Asset.AssetName.ToString()), DefaultFolder, FString(), TEXT("VAT metadata (*.json)|*.json"),
		EFileDialogFlags::None, Files);
	return bPicked && !Files.IsEmpty() ? FPaths::ConvertRelativePathToFull(Files[0]) : FString();
}

/** A modal asset picker; null when it was closed without a choice. */
UNiagaraSystem* PickNiagaraSystem()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::PickNiagaraSystem);

	FAssetData Picked;
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("PickSystemTitle", "Choose a Niagara System"))
		.ClientSize(FVector2D(420.f, 520.f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;
	auto Choose = [&Picked, WeakWindow](const FAssetData& Asset) {
		Picked = Asset;
		if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin())
		{
			Pinned->RequestDestroyWindow();
		}
	};

	FAssetPickerConfig Config;
	Config.Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
	Config.Filter.bRecursiveClasses = true;
	Config.SelectionMode = ESelectionMode::Single;
	Config.InitialAssetViewType = EAssetViewType::List;
	Config.bAllowNullSelection = false;
	Config.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateLambda(Choose);
	Config.OnAssetEnterPressed = FOnAssetEnterPressed::CreateLambda([Choose](const TArray<FAssetData>& Assets) {
		if (!Assets.IsEmpty())
		{
			Choose(Assets[0]);
		}
	});

	Window->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().CreateAssetPicker(Config)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PickSystemHint", "Double-click a system, or select one and press Enter."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]);
	GEditor->EditorAddModalWindow(Window);
	return Cast<UNiagaraSystem>(Picked.GetAsset());
}

bool IsNormalMap(const UTexture2D* Texture)
{
	if (Texture->CompressionSettings == TC_Normalmap)
	{
		return true;
	}
	const FString Lower = Texture->GetName().ToLower();
	return Lower.EndsWith(TEXT("_n")) || Lower.EndsWith(TEXT("_normal")) || Lower.EndsWith(TEXT("_nrm"));
}

void SetUpSelectedTexturesForAddress(TextureAddress Address, const FText& Purpose)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::SetUpSelectedTexturesForAddress);

	const TArray<UTexture2D*> Textures = LoadSelected<UTexture2D>();
	if (Textures.IsEmpty())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("TextureAddressTransaction", "Set Up Textures"));
	int32 Normals = 0;
	for (UTexture2D* Texture : Textures)
	{
		Texture->Modify();
		Texture->PreEditChange(nullptr);
		Texture->AddressX = Address;
		Texture->AddressY = Address;
		if (IsNormalMap(Texture))
		{
			Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
			++Normals;
		}
		Texture->PostEditChange();
		Texture->MarkPackageDirty();
	}
	Report(FText::Format(LOCTEXT("TextureAddressDone", "Set up {0} texture(s) for {1}; {2} normal map(s)."), FText::AsNumber(Textures.Num()), Purpose,
		FText::AsNumber(Normals)), true);
}
} // namespace

void LilaLinkActions::ReimportSelected()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::ReimportSelected);

	TArray<UObject*> Objects;
	TArray<FString> NotFromLila, Missing;
	for (const FAssetData& Asset : SelectedAssets())
	{
		const TArray<FString> Sources = SourceFilesOf(Asset);
		if (!Sources.ContainsByPredicate(&IsInLilaCache))
		{
			NotFromLila.Add(Asset.AssetName.ToString());
			continue;
		}
		if (!Sources.ContainsByPredicate([](const FString& File) { return FPaths::FileExists(File); }))
		{
			Missing.Add(Asset.AssetName.ToString());
			continue;
		}
		if (UObject* Object = Asset.GetAsset())
		{
			Objects.Add(Object);
		}
	}

	FText Message;
	bool bSuccess = !Objects.IsEmpty();
	if (bSuccess)
	{
		bSuccess = FReimportManager::Instance()->ReimportMultiple(Objects, false, true, FString(), nullptr, INDEX_NONE, false, true);
		Message = bSuccess ? FText::Format(LOCTEXT("ReimportStarted", "Reimporting {0} asset(s) from the Lila cache."), FText::AsNumber(Objects.Num()))
						   : LOCTEXT("ReimportFailed", "Reimport from the Lila cache failed; see the Output Log.");
	}
	else
	{
		Message = LOCTEXT("ReimportNothing", "Nothing to reimport from the Lila cache.");
	}
	if (!NotFromLila.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("ReimportNotLila", "{0}\nNot imported from Lila: {1}"), Message, NameList(NotFromLila));
	}
	if (!Missing.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("ReimportMissing", "{0}\nCache file gone, send it from Lila again: {1}"), Message, NameList(Missing));
	}
	Report(Message, bSuccess);
}

bool LilaLinkActions::CanReimportSelected()
{
	return SelectedAssets().ContainsByPredicate(&HasLilaSource);
}

void LilaLinkActions::ShowSourceFile()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::ShowSourceFile);

	for (const FAssetData& Asset : SelectedAssets())
	{
		for (const FString& Source : SourceFilesOf(Asset))
		{
			if (FPaths::FileExists(Source))
			{
				FPlatformProcess::ExploreFolder(*Source);
				return;
			}
			if (FPaths::DirectoryExists(FPaths::GetPath(Source)))
			{
				FPlatformProcess::ExploreFolder(*FPaths::GetPath(Source));
				Report(FText::Format(LOCTEXT("SourceGone", "{0} is no longer there; showing its folder."), FText::FromString(Source)), false);
				return;
			}
			Report(FText::Format(LOCTEXT("SourceFolderGone", "Neither {0} nor its folder exists any more."), FText::FromString(Source)), false);
			return;
		}
	}
}

bool LilaLinkActions::CanShowSourceFile()
{
	return SelectedAssets().ContainsByPredicate([](const FAssetData& Asset) { return !SourceFilesOf(Asset).IsEmpty(); });
}

void LilaLinkActions::SetUpSelectedMeshesForVat()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::SetUpSelectedMeshesForVat);

	// Every bake is found or picked before the transaction opens, so no file dialog sits inside it.
	TArray<TPair<FAssetData, FString>> Jobs;
	TArray<FString> Skipped;
	for (const FAssetData& Asset : SelectedOf<UStaticMesh>())
	{
		FString Meta = FindVatMeta(Asset);
		if (Meta.IsEmpty())
		{
			Meta = PickVatMeta(Asset);
		}
		if (Meta.IsEmpty())
		{
			Skipped.Add(Asset.AssetName.ToString());
			continue;
		}
		Jobs.Emplace(Asset, Meta);
	}
	if (Jobs.IsEmpty())
	{
		Report(LOCTEXT("VatNoBake", "No VAT bake chosen; nothing set up."), false);
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("VatMeshTransaction", "Set Up Mesh For VAT"));
	TArray<FString> Done, Errors;
	for (const TPair<FAssetData, FString>& Job : Jobs)
	{
		UStaticMesh* Mesh = Cast<UStaticMesh>(Job.Key.GetAsset());
		if (!Mesh)
		{
			continue;
		}
		TArray<FString> Assets, MeshErrors;
		if (LilaLink::SetUpVatOnMesh(Mesh, Job.Value, Assets, MeshErrors))
		{
			Done.Add(Mesh->GetName());
		}
		for (const FString& Error : MeshErrors)
		{
			Errors.Add(FString::Printf(TEXT("%s: %s"), *Mesh->GetName(), *Error));
			UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *Mesh->GetName(), *Error);
		}
	}

	FText Message = FText::Format(LOCTEXT("VatMeshDone", "Set up {0} mesh(es) for VAT."), FText::AsNumber(Done.Num()));
	if (!Errors.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("VatMeshErrors", "{0}\n{1}"), Message, FText::FromString(Errors[0]));
	}
	if (!Skipped.IsEmpty())
	{
		Message = FText::Format(LOCTEXT("VatMeshSkipped", "{0}\nNo bake chosen for: {1}"), Message, NameList(Skipped));
	}
	Report(Message, Errors.IsEmpty() && !Done.IsEmpty());
}

bool LilaLinkActions::HasSelectedMeshes()
{
	return !SelectedOf<UStaticMesh>().IsEmpty();
}

void LilaLinkActions::SetUpSelectedTexturesAsVatData()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::SetUpSelectedTexturesAsVatData);

	const TArray<UTexture2D*> Textures = LoadSelected<UTexture2D>();
	if (Textures.IsEmpty())
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("VatTextureTransaction", "Set Up Textures As VAT Data"));
	int32 Normals = 0;
	for (UTexture2D* Texture : Textures)
	{
		const FString Lower = Texture->GetName().ToLower();
		const bool bNormals = Lower.Contains(TEXT("nrm")) || Lower.Contains(TEXT("normal"));
		LilaLink::SetUpVatDataTexture(Texture, bNormals);
		Normals += bNormals ? 1 : 0;
	}
	Report(FText::Format(LOCTEXT("VatTextureDone", "Set up {0} position and {1} normal texture(s) as VAT data."), FText::AsNumber(Textures.Num() - Normals),
		FText::AsNumber(Normals)), true);
}

bool LilaLinkActions::HasSelectedTextures()
{
	return !SelectedOf<UTexture2D>().IsEmpty();
}

void LilaLinkActions::FillNiagaraFromSelectedMeshes()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkActions::FillNiagaraFromSelectedMeshes);

	TArray<UStaticMesh*> Meshes = LoadSelected<UStaticMesh>();
	if (Meshes.IsEmpty())
	{
		return;
	}
	Meshes.Sort([](const UStaticMesh& A, const UStaticMesh& B) { return NaturalLess(A.GetName(), B.GetName()); });

	const TArray<UNiagaraSystem*> SelectedSystems = LoadSelected<UNiagaraSystem>();
	UNiagaraSystem* System = SelectedSystems.Num() == 1 ? SelectedSystems[0] : PickNiagaraSystem();
	if (!System)
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("NiagaraMeshesTransaction", "Fill Niagara Mesh Renderer"));
	const TArray<FString> Updated = LilaLink::FillNiagaraMeshRenderers(System, Meshes, FString(), INDEX_NONE);
	if (Updated.IsEmpty())
	{
		Report(FText::Format(LOCTEXT("NiagaraNoRenderer", "{0} has no mesh renderer to fill."), FText::FromString(System->GetName())), false);
		return;
	}
	Report(FText::Format(LOCTEXT("NiagaraDone", "Put {0} mesh(es) on {1}: {2}"), FText::AsNumber(Meshes.Num()), FText::FromString(System->GetName()),
		NameList(Updated)), true);
}

void LilaLinkActions::SetUpSelectedTexturesForUniqueUVs()
{
	SetUpSelectedTexturesForAddress(TA_Clamp, LOCTEXT("TextureUnique", "unique UVs (Clamp)"));
}

void LilaLinkActions::SetUpSelectedTexturesForTiling()
{
	SetUpSelectedTexturesForAddress(TA_Wrap, LOCTEXT("TextureTiling", "tiling (Wrap)"));
}

void LilaLinkActions::GenerateTrimMaster()
{
	TArray<FString> Created, Warnings;
	bool bBuilt = false;
	int32 Version = 0;
	UMaterial* Master = LilaLink::TrimMaster(true, bBuilt, Version, Created, Warnings);
	if (!Master)
	{
		Report(FText::Format(LOCTEXT("TrimMasterFailed", "Trim master not made: {0}"), FText::FromString(FString::Join(Warnings, TEXT("; ")))), false);
		return;
	}
	Report(FText::Format(LOCTEXT("TrimMasterMade", "Generated {0}"), FText::FromString(Master->GetPathName())), true);
}

#undef LOCTEXT_NAMESPACE
