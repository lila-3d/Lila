#include "ui/CurveUi.h"

#include "ops/Command.h"
#include "ui/Actions.h"

namespace lila::ui
{
namespace
{
void addVehicleCommand(std::string id, std::string label, const char* command, std::string doc)
{
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Curves", .doc = std::move(doc),
                  .python = ops::toPython(command, json::object()), .command = command, .call = json::object()});
}
} // namespace

void registerVehicleCurveUi()
{
    registerBlendCurveTool();
    registerTrimCurveTool();
    addVehicleCommand("curve.blend", "Blend Curves", "blendCurve", "Bridges the two selected curves' nearest ends with a blend curve (G0 to G3 each end), live.");
    addVehicleCommand("curve.match", "Match Curve", "matchCurve",
        "Moves the first selected curve's nearest end onto the second curve with G0, G1 or G2 continuity, changing only that end.");
    addVehicleCommand("curve.extend", "Extend to Curves", "extendCurve", "Extends the selected curves' ends to the next curve they would meet.");
    addVehicleCommand("curve.split", "Split at Intersections", "splitCurves", "Splits the selected curves wherever they cross each other.");
    addVehicleCommand("curve.fair", "Fair", "fairCurve", "Evens out the selected curves' curvature within a tolerance, ends and corners kept.");
    addVehicleCommand("curve.network", "Curve Network", "curveNetwork",
        "Surfaces the selected crossing curves with quads through every curve (they become edge loops), live.");
    addVehicleCommand("curve.borderBlend", "Border Blend", "borderBlend",
        "Bridges two selected border loops or edge chains with a strip tangent to the faces on both sides, live.");
}
} // namespace lila::ui
