#pragma once

#include "core/Export.h"

#include "cmds/Command.h"
#include "scene/Components.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * The last adjustable command run from the UI, kept so it can be re-run with other flags (Maya's in-view editor,
 * Blender's redo panel). Re-running undoes it and executes it again, so it stays a single undo step.
 */
namespace lila::scene::lastOp
{
struct Op
{
    std::string command;
    json call;
    ComponentMode modeBefore = ComponentMode::Object;
    ComponentMode modeAfter = ComponentMode::Object;
    uint64_t serial = 0; // UndoStack serial right after it ran
    bool empty = false;  // the last run changed nothing, so there is no undo step to take back
};

enum class Change
{
    Recorded, // a new op ran
    Rerun,    // the op was re-run with new flags
    Cleared,  // forgotten
    Other,    // some other command ran from the UI
};

/** Starts listening to executes. Idempotent. */
LILA_SCENE_API void install();

/** The last recorded op, or null. It may be stale; see live(). */
LILA_SCENE_API const Op* current();
/** True while the op is still the top undo step, so it can be re-run. */
LILA_SCENE_API bool live();
LILA_SCENE_API void clear();

/**
 * Undoes the op and runs it again with `flags` merged into its call. On failure the previous result is put back
 * and the CommandError is rethrown.
 */
LILA_SCENE_API void rerun(const json& flags, CallSource source = CallSource::Script);

/** Flags of the op's command the UI can edit: Bool, Int, Float and String. */
LILA_SCENE_API std::vector<const FlagDef*> editableFlags(const std::string& command);
/** The value the op ran with for a flag: its call's value, else the flag default. */
LILA_SCENE_API json flagValue(const Op& op, const FlagDef& flag);

/** Modifier kind (as for addModifier) that does the op non-destructively, or empty. */
LILA_SCENE_API std::string modifierEquivalent(const std::string& command);
/** Undoes the op and adds its modifier equivalent, with matching parameters, to the meshes it worked on. */
LILA_SCENE_API void makeNonDestructive();

LILA_SCENE_API int addListener(std::function<void(Change)> fn);
LILA_SCENE_API void removeListener(int id);
} // namespace lila::scene::lastOp
