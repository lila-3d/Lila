#pragma once

#include "core/Export.h"

#include <functional>
#include <string>

class QMouseEvent;

namespace lila::ui
{
enum class Tool
{
    Select,
    Move,
    Rotate,
    Scale,
};

/** Snap modes, held like Maya: X grid, C curve/edge, V vertex. */
enum SnapFlags : unsigned
{
    SnapNone = 0,
    SnapGrid = 1 << 0,
    SnapEdge = 1 << 1,
    SnapVertex = 1 << 2,
    SnapRotateHold = 1 << 3, // J held: flips the rotate increment snap for the drag, like Maya
};

/** A manipulator tool's increment snap, from the general prefs /manipulator/snap/<move|rotate|scale>/{enabled,step}. */
struct IncrementSnap
{
    bool enabled = false;
    double step = 0.0;
    /** Move only (/manipulator/snap/move/absolute): land on a world grid of step instead of stepping from the start. */
    bool absolute = false;
};

/** App-wide tool state. Every viewport reads it; hotkeys and menus change it. */
namespace tools
{
LILA_UI_API Tool current();
LILA_UI_API void setCurrent(Tool t);
LILA_UI_API const char* name(Tool t);
/** Bumped when a tool is picked while already current: manipulators drop the handle MMB drags remember. */
LILA_UI_API unsigned handleResets();

/** Pivot editing: hold D, or toggle with Insert. */
LILA_UI_API bool pivotEdit();
LILA_UI_API void setPivotEdit(bool on);

LILA_UI_API unsigned snaps();
LILA_UI_API void setSnap(SnapFlags flag, bool on);

/** The increment snap in effect for t right now, with J's hold applied to rotate. */
LILA_UI_API IncrementSnap incrementSnap(Tool t);
/** "move", "rotate" or "scale": the pref key segment for t's increment snap. */
LILA_UI_API const char* snapKey(Tool t);

/** Called after any change; returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** True while B is held: LMB drag in the viewport sets the soft-selection radius. */
LILA_UI_API bool softSelectAdjusting();
/** A radius drag happened while the soft select key was down, so releasing it doesn't toggle. */
LILA_UI_API void markSoftSelectAdjusted();
/**
 * A brush tool's press starts a radius drag: B held (or the tool's own armed key) with LMB, which drags from the current
 * radius, or MMB, which starts from the smallest. Marks the soft select key used.
 */
LILA_UI_API bool brushRadiusDrag(const QMouseEvent* e, bool armed);

LILA_UI_API void registerActions();
} // namespace tools
} // namespace lila::ui
