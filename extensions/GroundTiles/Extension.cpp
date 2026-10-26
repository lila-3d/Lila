#include "GroundTiles.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!groundtiles::registerScene(ctx))
        return 1;
    groundtiles::registerUi(ctx);
    return 0;
}
