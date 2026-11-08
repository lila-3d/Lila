using UnrealBuildTool;

public class LilaLinkPCG : ModuleRules
{
	public LilaLinkPCG(ReadOnlyTargetRules Target) : base(Target)
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
		});
	}
}
