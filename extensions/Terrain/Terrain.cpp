// Terrain: heightfield terrain as geometry nodes, with commands that build and read it and their UI.

#include "MaterialNodes.h"
#include "Terrain.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!terrain::registerNodes(ctx) || !terrain::registerSurfaceNodes(ctx))
        return 1;
    terrain::registerCommands(ctx);
    terrain::registerSurfaceCommands(ctx);
    terrain::registerUi(ctx);
    terrain::registerSurfaceUi(ctx);
    return 0;
}
