// Puppeteer's entry point: one registration call per subsystem, scene first so the UI can find its types and commands.

#include "Puppeteer.h"

using namespace lila;

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    if (!puppeteer::registerCharacter(ctx) || !puppeteer::registerRigElements(ctx))
        return 1;
    puppeteer::registerControllers(ctx);
    puppeteer::registerJointTools(ctx);
    puppeteer::registerFullBody(ctx);
    puppeteer::registerSpinnerOps(ctx);
    puppeteer::registerControllerDrawing(ctx);
    puppeteer::registerSpinner(ctx);
    puppeteer::registerModes(ctx);

    // Animate-mode panels (anim/): add further anim registration calls here.
    puppeteer::registerAnimPanels(ctx);
    return 0;
}
