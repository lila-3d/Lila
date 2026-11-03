#pragma once

#include "core/Export.h"
#include "ui/Actions.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace lila::ui
{
/**
 * Actions for fields, deformer weights, membership, correction and timing, and the deformer display: nonlinear handles
 * drawn bent through their bounds (with falloff), softMod falloff spheres while selected, field shapes (draw override
 * "lila.field"), all pickable; a selected field's value shown on the meshes it drives; and the displacement heatmap
 * (command displacementHeatmap, action "deform.heatmap").
 */
LILA_UI_API void registerDeformFoundationUi();

/** A Deform menu action that runs `command` with `call`. */
inline void addDeformCommand(const char* id, const char* label, const char* command, nlohmann::json call, const char* python, const char* doc)
{
    actions::add({.id = id, .label = label, .category = "Deform", .doc = doc, .python = python, .command = command, .call = std::move(call)});
}
} // namespace lila::ui
