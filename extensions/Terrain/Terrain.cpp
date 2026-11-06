// Terrain: heightfield terrain as geometry nodes, with commands that build and read it and their UI.

#include "ExportEngines.h"
#include "Roads.h"
#include "Terrain.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!terrain::registerNodes(ctx))
        return 1;
    terrain::registerCommands(ctx);
    terrain::registerExportEngines(ctx);
    terrain::registerWater(ctx);
    terrain::registerUi(ctx);
    if (!terrain::registerRoads(ctx))
        return 1;
    return 0;
}
