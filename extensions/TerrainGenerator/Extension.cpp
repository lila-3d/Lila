#include "TerrainGenerator.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!terrain::registerScene(ctx))
        return 1;
    terrain::registerUi(ctx);
    return 0;
}
