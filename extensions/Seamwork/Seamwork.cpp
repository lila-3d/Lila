// Seamwork: contact joins between objects and the ground. Node types, commands, live fields and the Level menu entries.

#include "Seamwork.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!seamwork::registerNodes(ctx) || !seamwork::registerScatterNodes(ctx) || !seamwork::registerStripNodes(ctx))
        return 1;
    seamwork::registerCommands(ctx);
    seamwork::registerScatterCommands(ctx);
    seamwork::registerStripCommands(ctx);
    seamwork::registerExportCommands(ctx);
    seamwork::registerLiveUpdates(ctx);
    seamwork::registerUi(ctx);
    return 0;
}
