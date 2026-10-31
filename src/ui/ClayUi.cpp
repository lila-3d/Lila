#include "ui/ClayUi.h"

#include "ops/Command.h"
#include "ui/Actions.h"

#include <nlohmann/json.hpp>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

void clayAction(std::string id, std::string label, std::string command, json call, std::string icon, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Mesh", .doc = std::move(doc), .python = py,
                  .command = std::move(command), .call = std::move(call), .icon = std::move(icon)});
}
} // namespace

void registerClayUi()
{
    clayAction("clay.create", "Create Clay", "createClay", json::object(), "droplet",
        "Creates a clay object: shapes that melt into each other with smooth joins, shown as one mesh. Starts with a sphere.");
    clayAction("clay.addShape", "Add Clay Shape", "addClayPrimitive", json::object(), "circle-plus",
        "Adds a sphere, capsule, box or other shape to the selected clay object, after its other shapes. Pick the shape and join in the "
        "option box.");
    clayAction("clay.addCut", "Add Clay Cut", "addClayPrimitive", {{"operation", "subtract"}}, "circle-minus",
        "Adds a shape that carves into the clay before it.");
    clayAction("clay.group", "Group Clay Shapes", "groupClay", json::object(), "box-multiple",
        "Groups the selected clay shapes, so they join each other first and a cut among them reaches only the group.");
    clayAction("clay.refresh", "Refresh Clay", "refreshClay", json::object(), "refresh",
        "Rewires a clay object after its shapes were reordered or reparented in the Outliner. Shapes join in Outliner order.");
    clayAction("clay.convert", "Convert Clay to Mesh", "convertClayToMesh", json::object(), "transform",
        "Bakes the selected clay object into a plain mesh at its final voxel size, optionally quad remeshed in the option box.");
}
} // namespace lila::ui
