// Foliage: the plant modelling tool sets, Tree Generator and Blades, loaded as one extension.

#include "Blades.h"
#include "TreeGenerator.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!treegen::registerScene(ctx))
        return 1;
    treegen::registerUi(ctx);
    blades::registerCommands(ctx);
    blades::registerUi(ctx);
    return 0;
}
