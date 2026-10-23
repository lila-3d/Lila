#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Selection redirect: a joint's selectAs names the joint a viewport mesh click selects instead of it, so a click on a
 * twist or corrective bone's skin picks the limb bone the animator meant. The reference binds by node id while the
 * target lives (renames are free) and is saved by name.
 */
namespace rig
{
inline constexpr const char* kNodeRefKind = "nodeRef";

/** A stored reference to one node (kNoNode for none), for Data attrs such as selectAs. */
LILA_SCENE_API DataPtr makeNodeRef(const Graph& g, NodeId node);
/** The node a nodeRef value names, resolved against owner's namespace first; kNoNode when unset or gone. */
LILA_SCENE_API NodeId resolveNodeRef(const Graph& g, const DataPtr& ref, const std::string& ownerNamespace = {});
LILA_SCENE_API void registerNodeRefKind();

/** The joint's own selectAs target, kNoNode when it has none. */
LILA_SCENE_API NodeId selectAsOf(const Graph& g, NodeId joint);
/** Sets (kNoNode clears) the joint's selectAs. Undoable. Throws std::invalid_argument for a non-joint or itself. */
LILA_SCENE_API void setSelectAs(Graph& g, NodeId joint, NodeId target);
/** Where a click on node lands: its selectAs chain followed to the end, or node itself. Cycles stop where they loop. */
LILA_SCENE_API NodeId selectAsTarget(const Graph& g, NodeId node);

/** Name tokens that mark a helper joint (twist, corrective, muscle and volume bones). */
LILA_SCENE_API const std::vector<std::string>& defaultHelperTokens();
/**
 * Whether a joint name reads as a helper: any of its tokens (split at '_', digits and camelCase humps, compared
 * without case) is one of tokens. "upperarm_twistCor_01_l" has twist; "spine_04_latissimus_l" has latissimus.
 */
LILA_SCENE_API bool isHelperName(std::string_view name, const std::vector<std::string>& tokens = defaultHelperTokens());

struct AutoSelectAsOptions
{
    std::vector<std::string> tokens = defaultHelperTokens();
    /** Replace selectAs already set; off keeps what a template or the user chose. */
    bool overwrite = false;
};

/**
 * The auto rule: each helper joint (by name, or under a helper such as a correctiveRoot) maps to its nearest ancestor
 * that isn't one, preferring an ancestor weighted on the same skins so the click lands on a bone the mesh shows.
 * Joints that aren't helpers are left out. Pure: changes nothing.
 */
LILA_SCENE_API std::vector<std::pair<NodeId, NodeId>> autoSelectAsTargets(Graph& g, const std::vector<NodeId>& joints,
                                                                           const AutoSelectAsOptions& opts = {});
/**
 * Applies autoSelectAsTargets over joints and everything under them (every joint in the scene when empty). Returns
 * how many were set. Undoable. Rig templates call this after building a skeleton.
 */
LILA_SCENE_API int applyAutoSelectAs(Graph& g, const std::vector<NodeId>& joints = {}, const AutoSelectAsOptions& opts = {});

/** What a joint under the cursor is to the click, in marking menu order. */
enum class CandidateRole
{
    Redirect, // where a click goes (the dominant joint's selectAs)
    Weighted, // the joint that dominates the face
    Parent,   // the parent of the click's joint
    Child,    // the child of the click's joint nearest the hit
    Influence, // another joint weighting the face
};

struct SelectCandidate
{
    NodeId joint = kNoNode;
    CandidateRole role = CandidateRole::Weighted;
    /** Share of the face's summed corner weights, 0 for parent and child. */
    float weight = 0.0f;
};

/** Influences below this share of a face's weight are left out of the candidates. */
inline constexpr float kSignificantInfluence = 0.05f;

/**
 * The joints a click on a skinned mesh's face could mean, each once and in role order: the redirect target (when the
 * dominant joint has one), the dominant joint, the click joint's parent and its child nearest hit, then the other
 * joints weighting the face from heaviest down. Empty when the mesh isn't skinned or the face is unweighted.
 */
LILA_SCENE_API std::vector<SelectCandidate> selectionCandidates(Graph& g, NodeId mesh, uint32_t face,
                                                                std::optional<glm::dvec3> hit = std::nullopt, bool redirect = true);
} // namespace rig

/** selectAs, autoSelectAs, selectionCandidates. Called by registerRigCommands. */
LILA_SCENE_API void registerSelectAsCommands();
} // namespace lila::scene
