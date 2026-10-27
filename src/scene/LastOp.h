#pragma once

#include "core/Export.h"

#include "ops/Command.h"
#include "scene/Components.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

/**
 * The last adjustable command run from the UI, kept so it can be re-run with other flags. Each re-run is an undo step
 * of its own ("Adjust Bevel: offset") that replaces the op's result, so undo walks back through the settings before
 * it undoes the op, and redo walks forward through them again.
 */
namespace lila::scene::lastOp
{
struct Op
{
    std::string command;
    json call;
    ComponentMode modeBefore = ComponentMode::Object;
    ComponentMode modeAfter = ComponentMode::Object;
    /** Components selected in modeAfter when the op, or the adjustment the stack is on, finished: what it made. */
    std::map<NodeId, components::Ids> made;
};

enum class Change
{
    Recorded, // a new op ran
    Rerun,    // the op was re-run with new flags
    Restored, // undo or redo landed on one of the op's settings
    Cleared,  // forgotten
    Other,    // the op stopped being reachable(): another edit went on top, or undo went below it
};

/** Starts listening to executes. Idempotent. */
LILA_SCENE_API void install();

/** The last recorded op, or null. It may be stale; see live(). */
LILA_SCENE_API const Op* current();
/** True while the op, or an adjustment of it, is the newest undo step, so it can be re-run. */
LILA_SCENE_API bool live();
/**
 * True while the op, or an adjustment of it, is the newest undo step other than selection changes. rerun() and
 * makeNonDestructive() accept this: they first undo the selection steps above the op and drop them, since component
 * ids they name may not survive a re-run.
 */
LILA_SCENE_API bool reachable();
LILA_SCENE_API void clear();

/**
 * Runs the op again with `flags` merged into its call, as a new undo step that replaces its result. Returns the
 * command's result, or null when the flags change nothing. `drag` marks one tick of a continuous edit (a handle or
 * slider drag): drag re-runs replace one another's step until endDrag(), so the whole drag undoes at once. On failure
 * the previous result is put back and the CommandError is rethrown.
 */
LILA_SCENE_API json rerun(const json& flags, CallSource source = CallSource::Script, bool drag = false);
/** Ends a drag of re-runs. Without `keep` the drag's step is undone and dropped, as when the drag is cancelled. */
LILA_SCENE_API void endDrag(bool keep = true);
/**
 * Called by a running command whose flag hit a limit (a bevel offset clamped by the mesh): the op records the value
 * actually used, so adjusting it cannot run past the limit.
 */
LILA_SCENE_API void limitFlag(const std::string& name, const json& value);

/** Flags of the op's command the UI can edit: Bool, Int, Float, String with choices, and the Vec3 and free-text String flags its option override lists under adjust. */
LILA_SCENE_API std::vector<const FlagDef*> editableFlags(const std::string& command);
/** The value the op ran with for a flag: its call's value, else the flag default. */
LILA_SCENE_API json flagValue(const Op& op, const FlagDef& flag);

/** "polyInsertLoop" reads as "Insert Loop". */
LILA_SCENE_API std::string displayName(const std::string& command);

/** Modifier kind (as for addModifier) that does the op non-destructively, or empty. */
LILA_SCENE_API std::string modifierEquivalent(const std::string& command);
/** Undoes the op and adds its modifier equivalent, with matching parameters, to the meshes it worked on. */
LILA_SCENE_API void makeNonDestructive();

LILA_SCENE_API int addListener(std::function<void(Change)> fn);
LILA_SCENE_API void removeListener(int id);
} // namespace lila::scene::lastOp
