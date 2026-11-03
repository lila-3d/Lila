#pragma once

#include "core/Export.h"
#include "groom/HairCards.h"
#include "groom/Locks.h"
#include "groom/Shell.h"
#include "groom/Style.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;
} // namespace lila::scene

/**
 * Grooming in the scene. A groom is its guides (a curve node with a groomSurface), the style modifiers they run through
 * (groomSplit, groomCurl, ... chained curve to curve) and the groom node that makes the locks mesh; groomShell and
 * hairCards read the same chain. The stack is DG history:
 *
 *   HairGuides.worldSpace -> groomSplit -> groomCurl -> groom.inCurve -> HairLocks.inMesh
 *                                                         groom.outCurve -> groomShell -> HairShell.inMesh
 */
namespace lila::groom
{
/** The mesh a curve's roots grow from (its groomSurface attr); empty for none. */
LILA_SCENE_API std::string groomSurfaceOf(const scene::Node& curve);
LILA_SCENE_API CardOptions cardOptionsOf(const scene::Node& n);
LILA_SCENE_API LockOptions lockOptionsOf(const scene::Node& groom);
LILA_SCENE_API ShellOptions shellOptionsOf(const scene::Node& shell);
/** A style node's settings; kind from its type. */
LILA_SCENE_API StyleOptions styleOptionsOf(const scene::Node& style);
LILA_SCENE_API bool isStyleNode(const scene::Node& n);

/** The groom node the curve feeds, through its style stack; null when it feeds none. */
LILA_SCENE_API scene::Node* groomOf(scene::Graph& g, const scene::Node& curve);
/** The guides curve at the start of a groom's stack; null when nothing feeds it. */
LILA_SCENE_API scene::Node* guidesOf(scene::Graph& g, const scene::Node& groom);
/** The style nodes between the guides and the groom node, first applied first. */
LILA_SCENE_API std::vector<scene::Node*> styleStack(scene::Graph& g, const scene::Node& groom);
/** A groom from any part of it: the groom node, its guides, a style node, its locks or shell mesh. Null for none. */
LILA_SCENE_API scene::Node* findGroom(scene::Graph& g, const scene::Node& part);

/** The guides' mirror plane in world space, a point on it and its unit normal, when their symmetry is on. */
LILA_SCENE_API std::optional<std::pair<D3, D3>> mirrorPlane(const scene::Node& curve);
/**
 * Moves a world-space dab onto the guides' own side when it lands on their mirrored copies, so either side can be
 * brushed. ownLocal is the curve's own splines in its object space. True when it moved the dab.
 */
LILA_SCENE_API bool mirrorDabToSource(const scene::Node& curve, const Splines& ownLocal, D3& center, D3& normal, D3& delta, D3& view);

/** Lock brush inputs from the groom the guides feed: collision radii in the curve's space and gravity into it. */
LILA_SCENE_API void lockBrushSettings(scene::Graph& g, scene::Node& guides, const Splines& s, const glm::dmat4& worldToCurve, Settings& o);

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::groom
