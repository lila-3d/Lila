#include "ui/CurveUi.h"

#include "ops/Command.h"
#include "ui/Actions.h"

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

void cmd(std::string id, std::string label, std::string category, const char* command, json call, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = std::move(category), .doc = std::move(doc), .python = py,
                  .command = command, .call = std::move(call)});
}
} // namespace

void registerPlanarCurveUi()
{
    cmd("curve.text", "Text", "Curves", "textCurves", json::object(),
        "Makes text as closed Bezier curves from a font, kept live: edit the text, font and spacing on its curveText node.");
    cmd("curve.bevelPlus", "Bevel Plus", "Curves", "bevelPlus", json::object(),
        "Turns the selected closed planar curves into a bevelled solid; curves inside others become holes. Live history.");
    cmd("curve.booleanUnion", "Curve Union", "Curves", "curveBoolean", {{"operation", "union"}},
        "Joins the selected closed planar curves into one outline, live.");
    cmd("curve.booleanSubtract", "Curve Subtract", "Curves", "curveBoolean", {{"operation", "subtract"}},
        "Cuts the other selected closed curves out of the first selected, live.");
    cmd("curve.booleanIntersect", "Curve Intersect", "Curves", "curveBoolean", {{"operation", "intersect"}},
        "Keeps what the selected closed curves all cover, live.");
    cmd("curve.booleanXor", "Curve Difference (Xor)", "Curves", "curveBoolean", {{"operation", "xor"}},
        "Keeps what an odd number of the selected closed curves cover, live.");
    cmd("curve.offsetRegion", "Offset Region", "Curves", "offsetRegion", json::object(),
        "Grows or shrinks the region the selected closed curves bound, merging where loops meet, live.");
    cmd("curve.insetSkeleton", "Inset Along Skeleton", "Curves", "insetCurve", json::object(),
        "Insets the selected closed curves along the straight skeleton: mitred corners, splitting where the shape pinches. Live.");
    cmd("curve.centerLine", "Center Line", "Curves", "curveCenterLine", json::object(),
        "Makes the centre lines (straight skeleton) of the region the selected closed curves bound, live.");
    cmd("curve.roof", "Roof from Footprint", "Curves", "roof", json::object(),
        "Builds a roof over the selected closed footprint curves: hips, valleys and ridges at one pitch, gables where asked. Live.");
    cmd("mesh.insetRegion", "Inset Region", "Mesh", "polyInsetRegion", json::object(),
        "Insets each flat region of the selected faces as one along the straight skeleton, splitting where it pinches.");
}
} // namespace lila::ui
