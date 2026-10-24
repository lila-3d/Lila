using UnrealBuildTool;

public class LilaLink : ModuleRules
{
	public LilaLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Sockets",
			"Networking",
			"Json",
			"DeveloperSettings",
			"Projects",
			"AssetTools",
			"AssetRegistry",
			"ContentBrowser",
			"InterchangeCore",
			"InterchangeEngine",
			"InterchangeNodes",
			"InterchangePipelines",
			"MaterialEditor",
			"Niagara",
			"NiagaraCore",
			"AlembicLibrary",
			"GeometryCache",
			"ImageCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"DesktopPlatform",
			"Settings",
		});

		// Header-only use: the module is looked up at runtime, so a project without Python still loads.
		PrivateIncludePathModuleNames.Add("PythonScriptPlugin");
	}
}
