#pragma once

#include "core/Export.h"
#include "link/RpcClient.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::link
{
namespace fs = std::filesystem;

/** One placed part of a Nanite Assembly, in Unreal space (cm, Z up, quaternion xyzw). */
struct AssemblyInstance
{
    int part = 0;
    double location[3] = {0, 0, 0};
    double rotation[4] = {0, 0, 0, 1};
    double scale[3] = {1, 1, 1};
};

/**
 * A USD Nanite Assembly (UE 5.7+): a root prim with NaniteAssemblyRootAPI holding a point instancer whose prototypes
 * are parts with NaniteAssemblyExternalRefAPI pointing at the given static mesh object paths. Importing the file makes
 * one static mesh drawing every instance.
 */
LILA_SCENE_API bool writeNaniteAssemblyUsda(const fs::path& path, const std::string& root, const std::vector<std::string>& parts,
                                            const std::vector<AssemblyInstance>& instances, std::string* err);

/**
 * The instances of a placementPlan's groups as one assembly, a part per group (in group order), placements
 * converted from their Unreal-space JSON. Groups flagged remove or without placements give no part.
 */
LILA_SCENE_API std::vector<AssemblyInstance> assemblyInstances(const json& groups, std::vector<size_t>& partGroups);
} // namespace lila::link
