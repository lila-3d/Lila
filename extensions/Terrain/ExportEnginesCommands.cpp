// terrainTiles, the command that keeps mesh tiles in the scene, apart from the exporters so tests can build those alone.

#include "ExportEngines.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Profile.h>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;
using EA = ArgType;

void registerExportEngines(ext::Context& ctx)
{
    setEngineScripts(ctx.resources() / "engines");

    ctx.registerCommand({
        .name = "terrainTiles",
        .doc = "Cut a terrain into mesh tiles in the scene, for use in Lila (ruins on a hill, set dressing) or to export yourself.\n\n"
               "Each tile is an LOD group (<name>_x<N>_y<M>, levels _LOD0 at full resolution down by lodRatio) with a reduced "
               "<tile>_collision mesh beside it, all under one group, in world space. Every level keeps its border vertices, so "
               "neighbouring tiles meet at any mix of levels without a crack; skirt hangs a strip down from each edge too. uv set 0 "
               "spans the whole terrain and masks are point attributes. terrainExport(target='mesh') writes the same tiles to "
               "FBX, glTF or USD without keeping them. Returns the group, each tile's nodes and triangle counts.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"tileSize", "ts", EA::Int, int64_t(257), "Samples per tile side; neighbours share their edge row.", {}, {}, 2.0, 65536.0},
                  {"lods", "lod", EA::Int, int64_t(3), "Levels per tile, LOD0 at full resolution.", {}, {}, 1.0, 8.0},
                  {"lodRatio", "lr", EA::Float, 0.25, "Triangles each level keeps of the one before.", {}, {}, 0.001, 1.0},
                  {"skirt", "sk", EA::Float, 0.0, "Depth of a skirt hung from every tile edge, cm; 0 for none.", {}, {}, 0.0},
                  {"collision", "col", EA::Bool, true, "Make a reduced collision mesh per tile."},
                  {"collisionRatio", "cr", EA::Float, 0.1, "Triangles the collision mesh keeps of LOD0.", {}, {}, 0.001, 1.0},
                  {"name", "n", EA::String, "", "Group name and the tiles' stem; defaults to <terrain>_tiles."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError("terrainTiles: give one terrain");
            const TerrainRef t = findTerrainRef(names[0], "terrainTiles");
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = t.name + "_tiles";
            return makeTerrainTiles(a, *t.local, t.world, name);
        },
        .examples = {"t = terrainTiles('Terrain', tileSize=129, lods=3, skirt=200)\nt['tiles'], t['trianglesPerLod']"},
    });
}
} // namespace terrain
