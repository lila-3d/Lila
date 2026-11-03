using UnrealBuildTool;

public class LilaLinkSettlement : ModuleRules
{
	public LilaLinkSettlement(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Json",
			"AssetRegistry",
			"GeometryCore",
			"LilaLink",
			"PCG",
			"ZoneGraph",
		});
	}
}
