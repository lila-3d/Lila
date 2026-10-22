#pragma once

#include "render/FrameState.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"

#include <QRectF>

#include <functional>
#include <string>
#include <vector>

namespace lila::ui::viewportHooks
{
/** Adds scene content to every viewport frame (joints, constraints, curves...). Called on the main thread. */
using Drawer = std::function<void(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)>;

/**
 * Object picking for content the viewport doesn't know: for a click (box empty-ish) return the nearest hit as
 * {name, distance in px}; for a marquee return everything inside with distance 0.
 */
struct PickHit
{
    std::string name;
    double pixels = 0.0;
};
using Picker = std::function<std::vector<PickHit>(QPointF a, QPointF b, bool click, const ViewInfo& view)>;

void addDrawer(Drawer fn);
void addPicker(Picker fn);

const std::vector<Drawer>& drawers();
const std::vector<Picker>& pickers();
} // namespace lila::ui::viewportHooks
