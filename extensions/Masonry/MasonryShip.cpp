// Unreal output for a wall: stone meshes with their LOD chains as assets, stones as instanced components per ground
// chunk, the core as one mesh.

#include "MasonryShip.h"

#include <lila/Fbx.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <glm/gtc/matrix_transform.hpp>

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

json asset(const std::string& folder, const std::string& name, const char* kind, int index)
{
    return {{"name", name}, {"contentPath", folder + "/" + name}, {"objectPath", folder + "/" + name + "." + name}, {"kind", kind}, {"index", index}};
}
} // namespace

json shipPlan(const Built& b, const glm::dmat4& world, const std::string& wallKey, const std::string& name, const ShipOptions& o,
              std::span<const Shell> shells)
{
    LILA_ZONE();

    std::string folder = o.folder.empty() ? "/Game/Lila/Masonry" : o.folder;
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    folder += "/" + name;

    const Chunks chunks = chunkStones(b.stones, o.chunkSize);
    const double cell = std::max(o.chunkSize, 1.0);
    json assets = json::array();
    json list = json::array();
    std::vector<std::vector<int>> members(chunks.cells.size());
    for (size_t k = 0; k < b.stones.size(); ++k)
        members[size_t(chunks.of[k])].push_back(int(k));
    json origins = json::array();
    std::vector<glm::dvec3> origin(chunks.cells.size());
    for (size_t c = 0; c < chunks.cells.size(); ++c)
    {
        double lowest = 1e300;
        for (int k : members[c])
            lowest = std::min(lowest, b.stones[size_t(k)].center.y - b.stones[size_t(k)].size.y * 0.5);
        origin[c] = {(chunks.cells[c].x + 0.5) * cell, lowest, (chunks.cells[c].y + 0.5) * cell};
        origins.push_back(json::array({origin[c].x, origin[c].y, origin[c].z}));
    }

    if (o.mode == "lowPoly")
    {
        for (size_t k = 0; k < shells.size(); ++k)
        {
            const Shell& sh = shells[k];
            list.push_back({{"key", std::format("{}:{}_{}", wallKey, sh.cell.x, sh.cell.y)},
                            {"label", std::format("{}_{}_{}", name, sh.cell.x, sh.cell.y)},
                            {"asset", int(assets.size())},
                            {"mode", "actors"},
                            {"placements", json::array({placementAt(world * glm::translate(glm::dmat4(1.0), sh.origin), "shell", json::array({0.0, 0.0}))})}});
            assets.push_back(asset(folder, std::format("SM_{}_{}_{}", name, sh.cell.x, sh.cell.y), "shell", int(k)));
        }
        json cells = json::array();
        for (const Shell& sh : shells)
            cells.push_back(json::array({sh.cell.x, sh.cell.y}));
        return {{"assets", std::move(assets)}, {"groups", std::move(list)}, {"chunks", std::move(cells)}, {"folder", folder}};
    }
    if (o.mode == "merged")
        for (size_t c = 0; c < chunks.cells.size(); ++c)
        {
            const glm::ivec2 at = chunks.cells[c];
            const glm::dmat4 placed = world * glm::translate(glm::dmat4(1.0), origin[c]);
            list.push_back({{"key", std::format("{}:{}_{}", wallKey, at.x, at.y)},
                            {"label", std::format("{}_{}_{}", name, at.x, at.y)},
                            {"asset", int(assets.size())},
                            {"mode", "actors"},
                            {"placements", json::array({placementAt(placed, "chunk", json::array({0.0, 0.0}))})}});
            assets.push_back(asset(folder, std::format("SM_{}_{}_{}", name, at.x, at.y), "chunk", int(c)));
        }
    else
    {
        for (size_t r = 0; r < b.meshes.size(); ++r)
            assets.push_back(asset(folder, std::format("SM_{}_Stone_{}", name, r), "stone", int(r)));
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
        for (auto& [at, g] : groups)
            list.push_back(std::move(g));
    }
    const int coreAsset = int(assets.size());
    assets.push_back(asset(folder, "SM_" + name + "_Core", "core", 0));
    list.push_back({{"key", wallKey + ":core"},
                    {"label", name + "_Core"},
                    {"asset", coreAsset},
                    {"mode", "actors"},
                    {"placements", json::array({placementAt(world, "core", json::array({0.0, 0.0}))})}});

    json cells = json::array();
    for (const glm::ivec2& c : chunks.cells)
        cells.push_back(json::array({c.x, c.y}));
    return {{"assets", std::move(assets)}, {"groups", std::move(list)}, {"chunks", std::move(cells)}, {"origins", std::move(origins)},
            {"members", members}, {"folder", folder}};
}

bool writeLodFbx(std::span<const geo::Mesh> levels, const std::string& name, const std::filesystem::path& path, std::string* err,
                 std::span<const geo::Mesh> collision)
{
    LILA_ZONE();

    namespace fbx = lila::fbx;
    fbx::ExportScene scene;
    if (levels.size() == 1)
    {
        fbx::ExportNode mesh;
        mesh.name = name;
        mesh.mesh = std::make_shared<const geo::Mesh>(levels[0]);
        scene.nodes.push_back(std::move(mesh));
    }
    else
    {
        fbx::ExportNode group;
        group.name = name;
        group.lodGroup = true;
        scene.nodes.push_back(group);
        for (size_t k = 0; k < levels.size(); ++k)
        {
            fbx::ExportNode level;
            level.name = std::format("{}_LOD{}", name, k);
            level.parent = 0;
            level.mesh = std::make_shared<const geo::Mesh>(levels[k]);
            scene.nodes.push_back(std::move(level));
        }
    }
    for (size_t k = 0; k < collision.size(); ++k)
    {
        fbx::ExportNode hull;
        hull.name = std::format("UCX_{}_{:02}", name, k);
        hull.mesh = std::make_shared<const geo::Mesh>(collision[k]);
        scene.nodes.push_back(std::move(hull));
    }
    fbx::ExportOptions opts;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return fbx::writeFbx(scene, path, opts, err);
}
} // namespace masonry
