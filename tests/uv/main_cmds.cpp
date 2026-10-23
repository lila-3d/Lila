#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "uv/UvCommands.h"

int main(int argc, char** argv)
{
    lila::mainq::init();
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    lila::uv::registerCommands();
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
