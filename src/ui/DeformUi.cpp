#include "ui/DeformUi.h"

#include "ui/Actions.h"

namespace lila::ui
{
namespace
{
void add(const char* id, const char* label, const char* command, nlohmann::json call, const char* python, const char* doc)
{
    actions::add({.id = id, .label = label, .category = "Deform", .doc = doc, .python = python, .command = command, .call = std::move(call)});
}
} // namespace

void registerDeformActions()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    add("deform.blendShape", "Blend Shape", "blendShape", nlohmann::json::object(), "blendShape()",
        "Select the target meshes, then the base last: the base gets a morph target per target, exported to Unreal as morph targets.");
    add("deform.addBlendTarget", "Add Blend Target", "addBlendTarget", nlohmann::json::object(), "addBlendTarget()",
        "Select the base mesh (or its blendShape) first, then the new targets.");
    add("deform.extractBlendTarget", "Extract Blend Target", "extractBlendTarget", nlohmann::json::object(), "extractBlendTarget()",
        "Builds a mesh in the selected blendTarget's full shape, to sculpt and add back.");
    add("deform.lattice", "Lattice", "lattice", nlohmann::json::object(), "lattice()",
        "Wraps the selected meshes in a lattice; edit its points in vertex mode to reshape them.");
    for (const char* kind : {"bend", "twist", "taper", "squash", "sine", "wave"})
    {
        std::string k = kind;
        actions::add({.id = "deform." + k, .label = std::string(1, char(kind[0] - 'a' + 'A')) + k.substr(1), .category = "Deform",
                      .doc = "Adds a " + k + " deformer to the selected meshes with a handle to place it.",
                      .python = "nonlinear(type=\"" + k + "\")", .command = "nonlinear", .call = {{"type", k}}});
    }
    add("deform.deltaMush", "Delta Mush", "deltaMush", nlohmann::json::object(), "deltaMush()",
        "Smooths skinning and deformer artefacts on the selected meshes while keeping their detail.");
    add("deform.wrap", "Wrap", "wrap", nlohmann::json::object(), "wrap()",
        "Select the meshes to deform, then the driver last: they follow the driver's surface.");
    add("deform.cage", "Cage", "cage", nlohmann::json::object(), "cage()",
        "Select the meshes, then a closed cage mesh last: editing the cage reshapes them smoothly.");
    add("deform.softMod", "Soft Modification", "softMod", nlohmann::json::object(), "softMod()",
        "Adds a handle that moves nearby points with a soft falloff.");
    add("deform.rebind", "Rebind Deformer", "rebindDeformer", nlohmann::json::object(), "rebindDeformer()",
        "Makes the selected deformers' current state their rest state.");
}
} // namespace lila::ui
