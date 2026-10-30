// Level > Settlement, whose Create lays out a settlement in each selected mesh's outline or a square one when nothing is
// selected, and the Level Toolkit's Settlement section.

#include "Settlement.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace settlement
{
void registerUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "settlement.create",
                       .label = "Settlement",
                       .category = "Create",
                       .run =
                           [] {
                               nlohmann::json names = nlohmann::json::array();
                               scene::Graph& g = scene::Graph::current();
                               for (scene::NodeId id : g.selection())
                                   if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                                       names.push_back(n->path());
                               try
                               {
                                   if (names.empty())
                                       ops::execute("settlementCreate", nlohmann::json::object(), CallSource::UI);
                                   for (const nlohmann::json& n : names)
                                       ops::execute("settlementCreate", {{"args", {n}}}, CallSource::UI);
                               }
                               catch (const std::exception& e)
                               {
                                   log::write(LogLevel::Warning, e.what());
                               }
                           },
                       .doc = "Lay out streets, lots and building envelopes inside each selected mesh's loose-edge outline, or in a "
                              "400 m square when nothing is selected.",
                       .python = "settlementCreate(profile='medievalTown', size=40000)"});
    actions::add(ctx, {.id = "settlement.region", .label = "Settle Region", .category = "Settlement",
                       .doc = "Place cities, towns and villages over the whole ground, joined by roads.", .python = "settlementRegion()",
                       .command = "settlementRegion", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "settlement.export", .label = "Export Settlement Layout", .category = "Settlement",
                       .doc = "Write the selected settlement's streets, lots and buildings as JSON.", .python = "settlementExport('Town', path='town.json')",
                       .command = "settlementExport", .call = nlohmann::json::object()});

    levelToolkit::add(ctx, {.id = "settlement",
                            .title = "Settlement",
                            .items = {{"settlement.create", "building-community", "Create", "Town"}, {"settlement.region", "map-2", "Region", "Region"},
                                      {"settlement.export", "file-export", "Export"}},
                            .order = 20});
    menus::addItem(ctx, {.id = "settlement.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Settlement",
                         .before = levelToolkit::kDressSection});
    for (const char* a : {"settlement.create", "settlement.region", "settlement.export"})
        menus::addItem(ctx, {.id = std::string("settlement.menu.") + a, .menu = "Level/Settlement", .action = a});
}
} // namespace settlement
