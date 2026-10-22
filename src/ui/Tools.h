#pragma once

#include <functional>
#include <string>

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
Tool current();
void setCurrent(Tool t);
const char* name(Tool t);

/** Pivot editing: hold D, or toggle with Insert. */
bool pivotEdit();
void setPivotEdit(bool on);

unsigned snaps();
void setSnap(SnapFlags flag, bool on);

/** The increment snap in effect for t right now, with J's hold applied to rotate. */
IncrementSnap incrementSnap(Tool t);
/** "move", "rotate" or "scale": the pref key segment for t's increment snap. */
const char* snapKey(Tool t);

/** Called after any change; returns an id for removeListener. */
int addListener(std::function<void()> fn);
void removeListener(int id);

/** True while B is held: LMB drag in the viewport sets the soft-selection radius. */
bool softSelectAdjusting();
/** A radius drag happened while the soft select key was down, so releasing it doesn't toggle. */
void markSoftSelectAdjusted();

void registerActions();
} // namespace tools
} // namespace lila::ui
