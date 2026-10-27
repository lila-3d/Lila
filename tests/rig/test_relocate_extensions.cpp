#include <doctest/doctest.h>

#include "bake/BakeNodes.h"
#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "scene/Relocate.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <set>

using namespace lila;
using namespace lila::scene;

TEST_CASE("every extension's node types are known to relocateScene")
{
    gn::registerTypes();
    curve::registerTypes();
    tex::registerTypes();
    bake::registerTypes();
    extensions::ExtensionManager& pm = extensions::ExtensionManager::get();
    pm.addSearchPath(LILA_TEST_EXTENSIONS_DIR);
    std::set<std::string> before;
    for (const NodeType* t : types::all())
        before.insert(t->name);
    std::vector<std::string> failed;
    for (const extensions::ExtensionInfo& info : pm.available())
        try
        {
            pm.load(info.name);
        }
        catch (const std::exception& e)
        {
            failed.push_back(info.name + ": " + e.what());
        }
    INFO(json(failed).dump());
    CHECK(failed.empty());
    std::vector<std::string> unknown;
    for (const NodeType* t : types::all())
    {
        if (before.contains(t->name))
            continue;
        if (!relocate::handles(t->name))
            unknown.push_back(t->name);
    }
    INFO("classify these with lila/Relocate.h in the extension's init: " << json(unknown).dump());
    CHECK(unknown.empty());
}
