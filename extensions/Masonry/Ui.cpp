// Create > Masonry Wall: builds a wall along the selected mesh's loose edges.

#include "Masonry.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace masonry
{
void registerUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "masonry.wall",
                       .label = "Masonry Wall",
                       .category = "Create",
                       .run =
                           [] {
                               nlohmann::json names = nlohmann::json::array();
                               scene::Graph& g = scene::Graph::current();
                               for (scene::NodeId id : g.selection())
                                   if (scene::Node* n = g.node(id))
                                       names.push_back(n->path());
                               try
                               {
                                   for (const nlohmann::json& n : names)
                                       ops::execute("masonryWall", {{"args", {n}}}, CallSource::UI);
                               }
                               catch (const std::exception& e)
                               {
                                   log::write(LogLevel::Warning, e.what());
                               }
                           },
                       .doc = "Lay a brick wall along each selected mesh's loose edges: courses, bonded corners and a mortar core.",
                       .python = "masonryWall('Footprint', bond='running', height=200)"});
    menus::addItem(ctx, {.id = "masonry.menu.wall", .menu = "Create", .action = "masonry.wall"});
}
} // namespace masonry
