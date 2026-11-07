using System.IO;
using UnrealBuildTool;

public class LilaLinkMeshTerrain : ModuleRules
{
	public LilaLinkMeshTerrain(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// MeshPartition's public headers include Engine/Internal ones, which a project plugin cannot see otherwise.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source", "Runtime", "Engine", "Internal"));

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
			"MaterialEditor",
		});
	}
}
