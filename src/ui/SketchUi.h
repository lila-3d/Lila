#pragma once

#include "curve/Sketch.h"
#include "ui/Overlay.h"

#include <QPointF>

#include <cstdint>
#include <vector>

namespace lila::scene
{
class Node;
}

/** What the sketch tool and the curve drawing share: a curve's sketch status, cached, and the constraint glyphs. */
namespace lila::ui::sketchUi
{
/** A curve's sketch as the viewport shows it: degrees of freedom, conflicts, the circles and arcs among its splines. */
struct Display
{
    curve::sketch::Sketch sketch;
    curve::sketch::Status status;
    std::vector<curve::sketch::CircleFit> fits;
    bool ok = true;
    std::vector<uint32_t> conflicting;
    std::vector<uint32_t> invalid;
};

/** The sketch status of a curve node, recomputed only when its splines or sketch change; null without constraints. */
const Display* display(const scene::Node& curve);

/** Fully constrained sketch curves, linear colour for 3D lines (theme token "curveConstrained"). */
glm::vec4 constrainedColor();
/** The same in sRGB for the 2D overlay. */
glm::vec4 constrainedInk();

/** Whether the drawn span at polyline parameter u of spline si is fully constrained. */
bool spanFixed(const Display& d, const curve::Splines& s, size_t si, double u);

/** A constraint's glyph, drawn with lines in a square of `size` pixels around center. */
void drawGlyph(OverlayPainter& p, curve::sketch::Type t, QPointF center, const glm::vec4& color, float size = 12.0f);

/** Short label for a constraint type, e.g. "Horizontal". */
const char* label(curve::sketch::Type t);
} // namespace lila::ui::sketchUi
