#pragma once

#include "core/Export.h"

#include "scene/LodGroup.h"
#include "scene/Transform.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lila::scene
{
/**
 * Anim proxies: a proxySet is a group that tells a character "these are my proxies". It names the real meshes it
 * stands in for and its pieces, each bound to one joint (rigidly, or skinned like the real mesh for a true piece), and
 * shows either the pieces or the real meshes. Any mesh can be a piece, so proxies made by hand work exactly like
 * generated ones. Several sets can stand in for one character, one per LOD level. Pieces never export.
 */
class LILA_SCENE_API ProxySet : public Transform
{
public:
    enum Attr
    {
        kDisplay = Transform::kCount,
        kLodLevel,
        kPlanes,
        kMembers,
        kCount
    };
};

namespace proxy
{
inline constexpr const char* kTypeName = "proxySet";
inline constexpr const char* kMembersKind = "proxySetMembers";

/** What a set shows: its pieces in place of the real meshes, or the real meshes. */
enum class Display
{
    Proxy,
    Real,
};

/** A proxy piece stands in rigidly; a true piece is the real surface, split but skinned as the real mesh is. */
enum class Kind
{
    Proxy,
    True,
};

/** A piece's override: follow the set, or show its joint's proxy or true piece whatever the rest shows. */
enum class PieceDisplay
{
    Follow,
    Proxy,
    Real,
};

struct Piece
{
    NodeId mesh = kNoNode;
    NodeId joint = kNoNode;
    Kind kind = Kind::Proxy;
    PieceDisplay display = PieceDisplay::Follow;
};

LILA_SCENE_API const char* displayName(Display d);
LILA_SCENE_API const char* kindName(Kind k);
LILA_SCENE_API const char* pieceDisplayName(PieceDisplay d);

LILA_SCENE_API bool isSet(const Node& n);
/** Every proxy set, in creation order. */
LILA_SCENE_API std::vector<NodeId> all(const Graph& g);
/** Every set standing in for the mesh. */
LILA_SCENE_API std::vector<NodeId> setsOfMesh(const Graph& g, NodeId mesh);
/** The split planes the set was made with, as animProxyFromMesh takes them; empty for a hand-made set. */
LILA_SCENE_API json planesOf(const Graph& g, NodeId set);
/** The live real nodes and pieces; deleted ones drop out. */
LILA_SCENE_API std::vector<NodeId> realOf(const Graph& g, NodeId set);
LILA_SCENE_API std::vector<Piece> pieces(const Graph& g, NodeId set);
LILA_SCENE_API Display display(const Graph& g, NodeId set);

/** The set holding the mesh as a piece, else kNoNode. */
LILA_SCENE_API NodeId setOfPiece(const Graph& g, NodeId mesh);
/**
 * Sets the node belongs to: the set itself, the set of a piece, sets standing in for it or for a real node above it,
 * and for a joint, every set with a piece bound to its skeleton.
 */
LILA_SCENE_API std::vector<NodeId> setsFor(const Graph& g, NodeId node);
/** A proxy set or one of its pieces: left out of FBX, glTF, USD and Unreal link exports. */
LILA_SCENE_API bool excludedFromExport(const Graph& g, NodeId id);
/** Where a click on a rigid piece goes: its joint (kNoNode for anything else, skinned true pieces included). */
LILA_SCENE_API NodeId clickJoint(const Graph& g, NodeId mesh);

/** The set of the sets standing in for one character that the view shows, by their lodLevel against the view's LOD. */
LILA_SCENE_API NodeId shownSet(Graph& g, const std::vector<NodeId>& sets, const lod::View& view);
/** What every set hides in the view: the real nodes of sets showing proxies, and the pieces nothing shows. */
LILA_SCENE_API std::unordered_set<NodeId> hidden(Graph& g, const lod::View& view);

/** Undoable. Pieces are added with addPiece. */
LILA_SCENE_API NodeId create(Graph& g, std::string_view name, const std::vector<NodeId>& real, int lodLevel = -1);
/**
 * Makes the mesh a piece bound to joint (kNoNode: the bone nearest to it), parented under the set when reparent is set.
 * With bind, a proxy piece is parent constrained to its joint and a true piece is skinned with the weights of the
 * set's first skinned real mesh. Undoable. Throws std::invalid_argument.
 */
LILA_SCENE_API void addPiece(Graph& g, NodeId set, NodeId mesh, NodeId joint, Kind kind, bool bind = true, bool reparent = true);
LILA_SCENE_API void removePiece(Graph& g, NodeId set, NodeId mesh);
LILA_SCENE_API void addReal(Graph& g, NodeId set, NodeId node);
LILA_SCENE_API void setDisplay(Graph& g, NodeId set, Display d);
/** Undoable. Throws std::invalid_argument when the mesh isn't one of the set's pieces. */
LILA_SCENE_API void setPieceDisplay(Graph& g, NodeId set, NodeId mesh, PieceDisplay d);

/** The bone nearest to point among the joints influencing the set's real meshes (selectAs followed). */
LILA_SCENE_API NodeId nearestJoint(Graph& g, NodeId set, const glm::dvec3& point);

/** {name, display, lodLevel, real, pieces: [{mesh, joint, kind, display}]}. */
LILA_SCENE_API json info(const Graph& g, NodeId set);

LILA_SCENE_API void registerTypes();
/** createProxySet, addToProxySet, removeFromProxySet, setProxyDisplay, proxySetInfo, animProxyFromMesh. */
LILA_SCENE_API void registerCommands();
} // namespace proxy
} // namespace lila::scene
