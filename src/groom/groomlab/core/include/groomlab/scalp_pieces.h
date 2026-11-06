// SPDX-License-Identifier: Apache-2.0
#pragma once

// Several scalps on one groom: a body and separate ears, a head and its eyebrow patches, each a
// mesh of its own in the host. The groom's scalp ("scalp", Maya's inMesh) comes first and the
// others follow in the host's order, joined into one mesh (combineScalps) that holds them all:
// the session binds everything to it as it binds a single scalp (guides, parts, flow, vertex maps,
// following the scalp, simulation), it is the graph's "scalp" external, and every strand's root
// indexes it. The groom's own scalp keeps its triangles and points first, so what is rooted on it
// stays where it was when other scalps are added.
//
// A Scalp node picks one of them by its `input` ("scalp", or another scalp's name): the session
// registers each scalp's own mesh (scalpPiece: its triangles cut out of the whole mesh, refined as
// the whole is) as the external scalpPieceExternal(name), and the node hands it on. A Scatter fed
// one grows strands on that scalp alone, each root placed on the piece and carried to the whole
// mesh (toWholeRoot), so a branch per scalp (its own Scalp, Scatter and Interpolate) merges into
// one output. Operators reading roots on a mesh input (Interpolate's scalp) read the whole mesh
// (wholeScalp).
//
// Subdivision: each scalp is refined at its own level (GroomSession::scalpSubdivisions: the most
// the Scalp nodes reading it ask for, match render taking its own render level,
// ScalpPiece::renderSubdivision; a scalp no Scalp node reads, the groom's own scalp's), set on the
// joined mesh's pieces (withScalpLevels, ScalpPiece::subdivision): the refined mesh holds each
// scalp's own refinement in turn (subdivision.h), so a scalp's strands, maps and guide roots sit
// where they would on that scalp alone. A groom with one scalp has no pieces, and everything is
// as it always was.

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "groomlab/scalp_mesh.h"

namespace groomlab {

using MeshPtr = std::shared_ptr<const ScalpMesh>;  // as graph.h

// The groom's own scalp's name among its scalps (a Scalp node's default input).
constexpr const char* kMainScalpName = "scalp";

// The groom's other scalps by name, in order (GroomData::scalps).
using NamedScalps = std::vector<std::pair<std::string, MeshPtr>>;

// Whether `name` can name another scalp: letters, digits and _ (not starting with a digit), not
// "scalp".
bool validScalpName(const std::string& name);

// The external a scalp's own mesh is registered as: "scalp:<name>".
std::string scalpPieceExternal(const std::string& name);

// `main` followed by each of `extras` (null meshes, invalid names and names taken already are left
// out) as one mesh with ScalpMesh::pieces naming them: points and triangles in order; UVs where
// any has them (zeros on a scalp without); color sets where any has them (0 0 0 1 on a scalp
// without); a rest pose where any has one (a scalp without rests at its points); polygons where
// any has them (one triangle each on a scalp without); the render subdivision the most of theirs.
// `main` itself when no extra is left (a groom of one scalp), or when `main` is null.
MeshPtr combineScalps(const MeshPtr& main, const NamedScalps& extras);

// `joined` (a mesh of several scalps) with each scalp's own Catmull-Clark levels from `levels` by
// name (ScalpPiece::subdivision): -1 for a scalp at `most` (the levels the whole is refined by) or
// not named. `joined` itself when its pieces have those already (or it has none).
MeshPtr withScalpLevels(const MeshPtr& joined, const std::map<std::string, int>& levels, int most);

// Triangles [first, first + count) of `mesh` as a mesh of their own: the points they use (in
// their order), their corners' UVs and color sets, the rest pose, the polygons that lie within
// the range (none when the range cuts one) and the render subdivision.
ScalpMesh sliceScalp(const ScalpMesh& mesh, std::size_t first, std::size_t count);

// Each scalp of `whole` (the "scalp" external: the joined mesh, refined or not, at rest or not)
// as a mesh of its own, by name: its triangles of the cage (whole->cage(), else whole itself)
// with its render subdivision, refined as whole is (its triangles cut out of whole, the piece of
// the cage as its cage), whole() set to `whole`. Empty when whole holds one scalp.
std::map<std::string, MeshPtr> scalpPieces(const MeshPtr& whole);

// The mesh roots on `mesh` land on: its whole() for a scalp's piece, else itself.
const ScalpMesh* wholeScalp(const ScalpMesh* mesh);
MeshPtr wholeScalp(const MeshPtr& mesh);

// A root on `mesh` as a root of its whole mesh (face + wholeFaceOffset()); unchanged on any other.
inline StrandRoot toWholeRoot(const ScalpMesh& mesh, StrandRoot root) {
    if (root.face >= 0) root.face += mesh.wholeFaceOffset();
    return root;
}
// A root of the whole mesh on the piece `mesh` (face -1 when it is on another scalp); unchanged on
// a mesh that is no piece.
StrandRoot toPieceRoot(const ScalpMesh& mesh, StrandRoot root);

}  // namespace groomlab
