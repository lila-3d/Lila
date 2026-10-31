#pragma once

#include <lila/Command.h>

#include <lila/Graph.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/common.h>

#include <string>
#include <string_view>
#include <vector>

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

/** A live stage as the Hydra viewport renderers draw it: its prims, less those Lila's own nodes stand for. */
struct HydraView
{
    scene::NodeId node = scene::kNoNode;
    pxr::UsdStageRefPtr stage;
    /** Stage space to Lila's world: the usdStage node's world matrix with the stage's axes and units. */
    glm::dmat4 toWorld{1.0};
    /** A linked prim's stage-space world matrix is inverse(toWorld) * its node's world matrix * this (turned for cameras and lights). */
    glm::dmat4 toLila{1.0}, turnedToLila{1.0};
    struct Link
    {
        pxr::SdfPath prim;
        scene::NodeId node = scene::kNoNode;
        bool turned = false;
    };
    std::vector<Link> links;
    /** PointInstancers whose instances Lila built as nodes. */
    std::vector<pxr::SdfPath> realized;
    double timeCodesPerSecond = 24.0;
};

/** The scene's live stages, after g.evaluate(). Stages not composed yet are left out. */
std::vector<HydraView> hydraViews(scene::Graph& g);

/** The node built for a prim path ("/World/crate") in any live stage, the selected one's first; kNoNode for none. */
scene::NodeId nodeForPrim(std::string_view path);
} // namespace lila::usd::stage
