// Create > Settlement: lays out a settlement in each selected mesh's outline, or a square one when nothing is selected.

#include "Settlement.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
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
    menus::addItem(ctx, {.id = "settlement.menu.create", .menu = "Create", .action = "settlement.create"});
}
} // namespace settlement
