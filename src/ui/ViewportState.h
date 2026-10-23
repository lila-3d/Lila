#pragma once

#include "ui/ViewInfo.h"

#include <QPointF>

#include <optional>
#include <string>

namespace lila::ui
{
class Gizmo;

/** A viewport as callbacks see it (cursor badge predicates, for one), without the widget itself. */
struct ViewportState
{
    ViewInfo view;
    /** Which viewport panel, in creation order. */
    int panel = 0;
    /** Widget pixels. */
    QPointF cursor;
    /** The gizmo in use for the current tool; null when none applies. */
    const Gizmo* gizmo = nullptr;
    /** The active viewport tool's name, empty for the transform tools. */
    std::string tool;
    /** Alt navigation is under way. */
    bool navigating = false;
    /**
     * Tab is held for drag selection: true when the next (or current) stroke selects, false when it deselects. Unset
     * while Tab is up.
     */
    std::optional<bool> tabSelects;
};
} // namespace lila::ui
