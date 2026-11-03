// Level > Terrain and the Level Toolkit's Terrain section.

#include "Terrain.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileDialog.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/ViewportTool.h>

#include <QApplication>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
void registerUi(ext::Context& ctx)
{
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    } commands[] = {
        {"terrain.create", "Terrain", "terrainCreate", "Make a fractal noise terrain as a geometry-nodes heightfield.", "terrainCreate(size=[100000, 100000])"},
        {"terrain.erode", "Erode Terrain", "terrainErode", "Carve rivers and shed steep slopes on the selected terrain.", "terrainErode('Terrain', iterations=80)"},
        {"terrain.toMesh", "Terrain to Mesh", "terrainToMesh", "A plain mesh copy of the selected terrain as it is now.", "terrainToMesh('Terrain', step=2)"},
        {"terrain.bake", "Bake Terrain", "terrainBake", "Collapse the selected terrain's node graph into a .lilahf file.", "terrainBake('Terrain')"},
        {"terrain.export", "Export Terrain", "terrainExport", "Write the selected terrain to files for an engine or another tool.", "terrainExport('Terrain', target='files')"},
        {"terrain.sendToUnreal", "Send Terrain to Unreal", "terrainSendToUnreal", "Send the selected terrain to the connected Unreal editor as a Landscape.",
         "terrainSendToUnreal('Terrain')"},
    };
    for (const auto& c : commands)
        actions::add(ctx, {.id = c.id, .label = c.label, .category = "Terrain", .doc = c.doc, .python = c.python, .command = c.command, .call = nlohmann::json::object()});

    actions::add(ctx, {.id = "terrain.import",
                       .label = "Import Terrain",
                       .category = "Terrain",
                       .run =
                           [] {
                               QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "Import Terrain", {},
                                                                          "Elevation and images (*.tif *.tiff *.hgt *.asc *.xyz *.png *.exr);;All files (*)");
                               if (!file.isEmpty())
                                   actions::runWithOptions("terrainImport", {{"args", {file.toStdString()}}});
                           },
                       .doc = "Make a terrain from a DEM tile or a painted height image.",
                       .python = "terrainImport('dem/N46E007.hgt')"});

    registerSculptTool(ctx);
    actions::add(ctx, {.id = "terrain.sculpt",
                       .label = "Sculpt Terrain",
                       .category = "Terrain",
                       .run = [] { viewportTools::activate("terrain.sculptTool"); },
                       .doc = "Brush the selected terrain: raise, lower, smooth, flatten and noise, before or after erosion.",
                       .python = "terrainSculpt('Terrain', op='raise', points=[[0, 0]], radius=3000, strength=300)"});

    menus::addItem(ctx, {.id = "terrain.menu",.menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Terrain",
                         .after = levelToolkit::kBuildSection});
    int separators = 0;
    for (const char* a : {"terrain.create", "terrain.import", "terrain.erode", "terrain.sculpt", "", "terrain.toMesh", "terrain.bake", "terrain.export", "terrain.sendToUnreal"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("terrain.menu.") + a, .menu = "Level/Terrain", .action = a});
        else
            menus::addItem(ctx, {.id = "terrain.menu.sep" + std::to_string(separators++), .menu = "Level/Terrain", .kind = menus::Kind::Separator});
    }

    levelToolkit::add(ctx, {.id = "terrain",
                            .title = "Terrain",
                            .items = {{"terrain.create", "mountain", "Create", "Terrain"}, {"terrain.import", "file-import", "Import"}, {"terrain.erode", "droplet", "Erode", "Erode"}, {"terrain.sculpt", "brush", "Sculpt"}, {},
                                      {"terrain.toMesh", "grid-dots", "To Mesh"}, {"terrain.bake", "file-zip", "Bake"}, {"terrain.export", "file-export", "Export"},
                                      {"terrain.sendToUnreal", "send", "Unreal"}},
                            .order = 10});
}
} // namespace terrain
