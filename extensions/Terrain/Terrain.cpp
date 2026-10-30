// Terrain: heightfield terrain as geometry nodes, with commands that build and read it and their UI.

#include "Terrain.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!terrain::registerNodes(ctx))
        return 1;
    terrain::registerCommands(ctx);
    terrain::registerUi(ctx);
    return 0;
}
