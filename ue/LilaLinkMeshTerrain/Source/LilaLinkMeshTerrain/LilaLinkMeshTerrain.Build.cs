using UnrealBuildTool;

public class LilaLinkMeshTerrain : ModuleRules
{
	public LilaLinkMeshTerrain(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Json",
			"GeometryCore",
			"DynamicMesh",
			"LilaLink",
			"MeshPartition",
			"MeshPartitionEditor",
		});
	}
}
