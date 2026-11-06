// Level > Terrain entries for biomes, material layers, their preview and ground cover.

#include "MaterialNodes.h"

#include <lila/Actions.h>
#include <lila/Extension.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
void registerSurfaceUi(ext::Context& ctx)
{
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    } commands[] = {
        {"terrain.biomes", "Terrain Biomes", "terrainBiomes", "Work out temperature, moisture and biomes over the selected terrain.", "terrainBiomes('Terrain')"},
        {"terrain.materialRules", "Terrain Layers", "terrainMaterialRules", "Lay grass, rock, dirt, snow and the rest over the selected terrain by rules.",
         "terrainMaterialRules('Terrain')"},
        {"terrain.materialPreview", "Preview Terrain Layers", "terrainMaterialPreview", "Show the selected terrain's layers in the viewport through its material.",
         "terrainMaterialPreview('Terrain')"},
        {"terrain.satmap", "Bake Terrain Colour", "terrainSatmap", "Bake the selected terrain's layer colours to an image.", "terrainSatmap('Terrain')"},
        {"terrain.groundCover", "Ground Cover", "terrainGroundCover", "Scatter rocks, trees, grass and debris over the selected terrain by rules.",
         "terrainGroundCover('Terrain')"},
    };
    for (const auto& c : commands)
        actions::add(ctx, {.id = c.id, .label = c.label, .category = "Terrain", .doc = c.doc, .python = c.python, .command = c.command, .call = nlohmann::json::object()});
    menus::addItem(ctx, {.id = "terrain.menu.surfaceSep", .menu = "Level/Terrain", .kind = menus::Kind::Separator});
    for (const auto& c : commands)
        menus::addItem(ctx, {.id = std::string("terrain.menu.") + c.id, .menu = "Level/Terrain", .action = c.id});
}
} // namespace terrain
