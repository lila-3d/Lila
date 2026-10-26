// Unreal output for a wall: stone meshes with their LOD chains as assets, stones as instanced components per ground
// chunk, the core as one mesh.

#include "MasonryShip.h"

#include <lila/Fbx.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <format>
#include <map>

namespace masonry
{
namespace
{
using json = nlohmann::json;

json vec(const lila::math::Vector& v) { return json::array({v.x, v.y, v.z}); }

json placementAt(const glm::dmat4& world, std::string id, json customData)
{
    namespace math = lila::math;
    const math::Transform t = math::toUnreal(math::Transform(math::Matrix(world)));
    return {{"id", std::move(id)},
            {"location", vec(t.translation)},
            {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
            {"scale", vec(t.scale3d)},
            {"customData", std::move(customData)}};
}

json asset(const std::string& folder, const std::string& name, int mesh)
{
    return {{"name", name}, {"contentPath", folder + "/" + name}, {"objectPath", folder + "/" + name + "." + name}, {"mesh", mesh}};
}
} // namespace

json shipPlan(const Built& b, const glm::dmat4& world, const std::string& wallKey, const std::string& name, const ShipOptions& o)
{
    LILA_ZONE();

    std::string folder = o.folder.empty() ? "/Game/Lila/Masonry" : o.folder;
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    folder += "/" + name;

    json assets = json::array();
    for (size_t r = 0; r < b.meshes.size(); ++r)
        assets.push_back(asset(folder, std::format("SM_{}_Stone_{}", name, r), int(r)));
    const int coreAsset = int(assets.size());
    assets.push_back(asset(folder, "SM_" + name + "_Core", -1));

    const Chunks chunks = chunkStones(b.stones, o.chunkSize);
    // Groups in (chunk, mesh) order so the plan is the same bytes every time.
    std::map<std::pair<int, int>, json> groups;
    const double top = std::max(1, b.courses - 1);
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const Stone& s = b.stones[k];
        const int chunk = chunks.of[k], mesh = b.reference[k];
        json& g = groups[{chunk, mesh}];
        if (g.is_null())
        {
            const glm::ivec2 c = chunks.cells[size_t(chunk)];
            g = {{"key", std::format("{}:{}_{}:{}", wallKey, c.x, c.y, mesh)},
                 {"label", std::format("{}_{}_{}_Stone_{}", name, c.x, c.y, mesh)},
                 {"asset", mesh},
                 {"mode", o.mode},
                 {"placements", json::array()}};
        }
        g["placements"].push_back(placementAt(world * stoneMatrix(s), std::to_string(k), json::array({s.random, double(s.course) / top})));
    }
    json list = json::array();
    for (auto& [at, g] : groups)
        list.push_back(std::move(g));
    list.push_back({{"key", wallKey + ":core"},
                    {"label", name + "_Core"},
                    {"asset", coreAsset},
                    {"mode", "actors"},
                    {"placements", json::array({placementAt(world, "core", json::array({0.0, 0.0}))})}});

    json cells = json::array();
    for (const glm::ivec2& c : chunks.cells)
        cells.push_back(json::array({c.x, c.y}));
    return {{"assets", std::move(assets)}, {"groups", std::move(list)}, {"chunks", std::move(cells)}, {"folder", folder}};
}

bool writeLodFbx(std::span<const geo::Mesh> levels, const std::string& name, const std::filesystem::path& path, std::string* err)
{
    LILA_ZONE();

    namespace fbx = lila::fbx;
    fbx::ExportScene scene;
    fbx::ExportNode group;
    group.name = name;
    group.lodGroup = levels.size() > 1;
    scene.nodes.push_back(group);
    for (size_t k = 0; k < levels.size(); ++k)
    {
        fbx::ExportNode level;
        level.name = levels.size() > 1 ? std::format("{}_LOD{}", name, k) : name + "_Mesh";
        level.parent = 0;
        level.mesh = std::make_shared<const geo::Mesh>(levels[k]);
        scene.nodes.push_back(std::move(level));
    }
    fbx::ExportOptions opts;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return fbx::writeFbx(scene, path, opts, err);
}
} // namespace masonry
