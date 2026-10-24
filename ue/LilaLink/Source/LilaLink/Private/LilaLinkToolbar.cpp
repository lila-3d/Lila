#include "LilaLinkToolbar.h"

#include "LilaLinkEditorActions.h"
#include "LilaLinkEditorSettings.h"
#include "LilaLinkSettings.h"
#include "LilaLinkStyle.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISettingsModule.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "LilaLinkToolbar"

namespace
{
FName MenuOwner()
{
	static const FName Owner(TEXT("LilaLinkToolbar"));
	return Owner;
}

FLilaLinkToolbarHooks Hooks;
FDelegateHandle StartupHandle;

bool IsToolbarMenuEnabled()
{
	return GetDefault<ULilaLinkEditorUserSettings>()->bShowToolbarMenu;
}

void HideToolbarMenu()
{
	ULilaLinkEditorUserSettings* Settings = GetMutableDefault<ULilaLinkEditorUserSettings>();
	Settings->bShowToolbarMenu = false;
	Settings->SaveConfig();
}

/** Opened by the object's own container, category and section, which are the class name and not the display name. */
void ShowSettingsFor(const UDeveloperSettings* Settings)
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
	{
		SettingsModule->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
	}
}

FSlateIcon AppIcon(const TCHAR* Name)
{
	return FSlateIcon(FAppStyle::GetAppStyleSetName(), Name);
}

FUIAction Action(void (*Execute)(), bool (*CanExecute)() = nullptr)
{
	return FUIAction(FExecuteAction::CreateStatic(Execute), CanExecute ? FCanExecuteAction::CreateStatic(CanExecute) : FCanExecuteAction());
}

TSharedRef<SWidget> BuildMenu()
{
	FMenuBuilder Menu(true, nullptr);

	Menu.BeginSection(TEXT("LilaStatus"), LOCTEXT("StatusSection", "Status"));
	Menu.AddMenuEntry(Hooks.Status ? Hooks.Status() : FText::GetEmpty(),
		LOCTEXT("StatusTip", "The localhost link Lila sends assets through. Its port and import options are under Settings: Lila Link."),
		AppIcon(TEXT("Icons.Info")), FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([] { return false; })));
	if (Hooks.CanRestart && Hooks.CanRestart())
	{
		Menu.AddMenuEntry(LOCTEXT("Restart", "Restart Link"),
			LOCTEXT("RestartTip", "Drops any connection and listens again, on the first free port from the one in Settings: Lila Link."),
			AppIcon(TEXT("Icons.Refresh")), FUIAction(FExecuteAction::CreateLambda([] {
				if (Hooks.Restart)
				{
					Hooks.Restart();
				}
			})));
	}
	Menu.EndSection();

	Menu.BeginSection(TEXT("LilaAssets"), LOCTEXT("AssetsSection", "Assets"));
	Menu.AddMenuEntry(LOCTEXT("Reimport", "Reimport Selected From Lila Cache"),
		LOCTEXT("ReimportTip",
			"Reruns the import of each asset selected in the Content Browser from the file Lila last exported for it, with the settings it was "
			"imported with. Assets whose source is not in Lila's export cache are skipped and named."),
		AppIcon(TEXT("Icons.Refresh")), Action(&LilaLinkActions::ReimportSelected, &LilaLinkActions::CanReimportSelected));
	Menu.AddMenuEntry(LOCTEXT("ShowSource", "Show Source File"),
		LOCTEXT("ShowSourceTip", "Opens the file browser on the file the selected asset was imported from."), AppIcon(TEXT("Icons.FolderOpen")),
		Action(&LilaLinkActions::ShowSourceFile, &LilaLinkActions::CanShowSourceFile));
	Menu.EndSection();

	Menu.BeginSection(TEXT("LilaVfx"), LOCTEXT("VfxSection", "VFX"));
	Menu.AddMenuEntry(LOCTEXT("VatMesh", "Set Up Selected Mesh For VAT"),
		LOCTEXT("VatMeshTip",
			"Makes each selected static mesh play a Lila vertex animation bake: imports the bake's position and normal textures beside it, sets "
			"the mesh's build settings and bounds for VAT, and puts a VAT material instance on every slot. The bake's .json is taken from beside "
			"the mesh's source file, or asked for. The mesh must be the bake's rest mesh."),
		AppIcon(TEXT("ClassIcon.StaticMesh")), Action(&LilaLinkActions::SetUpSelectedMeshesForVat, &LilaLinkActions::HasSelectedMeshes));
	Menu.AddMenuEntry(LOCTEXT("VatTextures", "Set Up Selected Textures As VAT Data"),
		LOCTEXT("VatTexturesTip",
			"Data texture settings on the selected textures: no sRGB, nearest filtering, no mips, never streamed, clamped. Position textures get "
			"HDR compression from EXR or 16-bit sources and vector displacement from 8-bit ones; names containing nrm or normal are treated as "
			"normal textures."),
		AppIcon(TEXT("ClassIcon.Texture2D")), Action(&LilaLinkActions::SetUpSelectedTexturesAsVatData, &LilaLinkActions::HasSelectedTextures));
	Menu.AddMenuEntry(LOCTEXT("NiagaraMeshes", "Fill Niagara Mesh Renderer From Selected Meshes"),
		LOCTEXT("NiagaraMeshesTip",
			"Puts the selected static meshes, in name order, into every mesh renderer of a Niagara system for a mesh index flipbook. Uses the "
			"Niagara system in the selection, or asks for one. Pivot, scale and rotation on the renderer's first mesh carry over to all."),
		AppIcon(TEXT("ClassIcon.ParticleSystem")), Action(&LilaLinkActions::FillNiagaraFromSelectedMeshes, &LilaLinkActions::HasSelectedMeshes));
	Menu.EndSection();

	Menu.BeginSection(TEXT("LilaTextures"), LOCTEXT("TexturesSection", "Textures"));
	Menu.AddMenuEntry(LOCTEXT("TexturesUnique", "Set Up Selected Textures For Unique UVs"),
		LOCTEXT("TexturesUniqueTip",
			"Address Clamp on the selected textures, for art on a mesh's own unwrap, so the mips do not bleed the far edge in. Normal maps (a _N, "
			"_Normal or _NRM suffix, or normal map compression) also go in the world normal map texture group. Compression and sRGB are left as they "
			"are."),
		AppIcon(TEXT("ClassIcon.Texture2D")), Action(&LilaLinkActions::SetUpSelectedTexturesForUniqueUVs, &LilaLinkActions::HasSelectedTextures));
	Menu.AddMenuEntry(LOCTEXT("TexturesTiling", "Set Up Selected Textures For Tiling"),
		LOCTEXT("TexturesTilingTip",
			"Address Wrap on the selected textures, for art that repeats across a surface, so the mips filter across the seam. Normal maps (a _N, "
			"_Normal or _NRM suffix, or normal map compression) also go in the world normal map texture group. Compression and sRGB are left as they "
			"are."),
		AppIcon(TEXT("ClassIcon.Texture2D")), Action(&LilaLinkActions::SetUpSelectedTexturesForTiling, &LilaLinkActions::HasSelectedTextures));
	Menu.EndSection();

	Menu.BeginSection(TEXT("LilaSettings"), LOCTEXT("SettingsSection", "Settings"));
	Menu.AddMenuEntry(LOCTEXT("LinkSettings", "Settings: Lila Link"),
		LOCTEXT("LinkSettingsTip", "The link server's port and import options, under Editor Preferences, Plugins, Lila Link."),
		AppIcon(TEXT("Icons.Settings")), FUIAction(FExecuteAction::CreateLambda([] { ShowSettingsFor(GetDefault<ULilaLinkSettings>()); })));
	Menu.AddMenuEntry(LOCTEXT("EditorSettings", "Editor Preferences"), LOCTEXT("EditorSettingsTip", "Per-developer settings for this menu. Not checked in."),
		AppIcon(TEXT("Icons.Toolbar.Settings")),
		FUIAction(FExecuteAction::CreateLambda([] { ShowSettingsFor(GetDefault<ULilaLinkEditorUserSettings>()); })));
	Menu.AddMenuEntry(LOCTEXT("HideMenu", "Hide This Menu"),
		LOCTEXT("HideMenuTip", "Removes the Lila button from your toolbar. Turn it back on under Editor Preferences, Plugins, Lila Link Editor."),
		AppIcon(TEXT("Icons.Visibility")), Action(&HideToolbarMenu));
	Menu.EndSection();

	return Menu.MakeWidget();
}

void RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(MenuOwner());

	UToolMenu* ToolBar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.PlayToolBar"));
	if (!ToolBar)
	{
		return;
	}

	FToolMenuEntry Entry = FToolMenuEntry::InitComboButton(TEXT("LilaMenu"),
		FUIAction(FExecuteAction(), FCanExecuteAction(), FIsActionChecked(), FIsActionButtonVisible::CreateStatic(&IsToolbarMenuEnabled)),
		FOnGetContent::CreateStatic(&BuildMenu), LOCTEXT("LilaToolbar", "Lila"), LOCTEXT("LilaToolbarTip", "Lila link tools."),
		FSlateIcon(FLilaLinkStyle::GetStyleSetName(), FLilaLinkStyle::GetMenuIconName()));

	// The style that gives a toolbar button its label beside the icon.
	Entry.StyleNameOverride = TEXT("CalloutToolbar");

	ToolBar->FindOrAddSection(TEXT("LilaLink")).AddEntry(Entry);
}
} // namespace

void LilaLinkToolbar::Register(FLilaLinkToolbarHooks InHooks)
{
	Hooks = MoveTemp(InHooks);
	FLilaLinkStyle::Register();
	if (UToolMenus::IsToolMenuUIEnabled())
	{
		StartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
	}
}

void LilaLinkToolbar::Unregister()
{
	if (StartupHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(StartupHandle);
		StartupHandle.Reset();
	}
	UToolMenus::UnregisterOwner(MenuOwner());
	FLilaLinkStyle::Unregister();
	Hooks = FLilaLinkToolbarHooks();
}

#undef LOCTEXT_NAMESPACE
