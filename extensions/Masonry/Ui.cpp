// Level > Masonry, whose Masonry Wall builds a wall along the selected mesh's loose edges, and the Level Toolkit's Masonry section.

#include "Masonry.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
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
    actions::add(ctx, {.id = "masonry.ship", .label = "Send Masonry to Unreal", .category = "Masonry",
                       .doc = "Send the selected wall to Unreal's open level as instanced stones per ground chunk.", .python = "masonryShip('Wall')",
                       .command = "masonryShip", .call = nlohmann::json::object()});

    levelToolkit::add(ctx, {.id = "masonry", .title = "Masonry", .items = {{"masonry.wall", "wall", "Wall", "Wall"}, {"masonry.ship", "send", "Unreal"}}, .order = 30, .docs = "masonry"});
    menus::addItem(ctx, {.id = "masonry.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Masonry",
                         .before = levelToolkit::kDressSection});
    for (const char* a : {"masonry.wall", "masonry.ship"})
        menus::addItem(ctx, {.id = std::string("masonry.menu.") + a, .menu = "Level/Masonry", .action = a});
}
} // namespace masonry
