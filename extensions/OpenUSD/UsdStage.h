#pragma once

#include <lila/Command.h>

#include <nlohmann/json.hpp>
#include <pxr/usd/usd/common.h>

#include <string>
#include <string_view>

/**
 * Live USD stages. A usdStage node holds a composed stage whose root layer sublayers an edit layer over the source
 * file; the composed prims are built as Lila nodes under it, each linked to its prim (a prim inside an LOD level
 * through its level's variant). Saving exports those nodes with the USD writer into a stage in memory, hashes every
 * prim's properties by group and compares them with the hashes taken when the nodes were last built or written, then
 * writes only the groups that changed, as overs on the linked prims in the edit layer. A graph listener collects the
 * nodes changed in between, so a save exports only those and what depends on them, unless a batch or an undo means
 * anything may have changed. The source files are never written. Changes that reach the stage's layers from
 * elsewhere (a reload, a script, another tool writing a file) arrive as UsdNotice::ObjectsChanged and rebuild only
 * the prims they touch, keeping their nodes' modifier stacks and skins.
 */
namespace lila::usd::stage
{
/** Runs a usdStage command or one of the events the front forwards. False when name is none of them. */
bool run(std::string_view name, const Args& a, nlohmann::json& result);

/**
 * The composed stage of a usdStage node, or of the stage an object belongs to; empty picks the selected stage, or the
 * only one. Its edit target is the edit layer, and what is authored through it reaches the nodes like any outside
 * change. Throws CommandError.
 */
pxr::UsdStagePtr liveStage(const std::string& node);
} // namespace lila::usd::stage
