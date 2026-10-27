#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Split Mesh: cut a mesh into pieces along planes, each plane only within its disc, so a plane at the shoulder cuts the
 * arm off without slicing through the legs below it. The base of anim proxies, but works on any mesh.
 */
namespace lila::scene::split
{
struct Plane
{
    glm::dvec3 origin{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    /** How far from origin the plane cuts; 0 or less fits it to the cross-section loop around origin. */
    double radius = 0.0;
};

struct Options
{
    /** Close the holes each cut leaves with a face. */
    bool cap = true;
    /** Shells no plane cuts (eyes, teeth) join the piece nearest to them instead of being pieces of their own. */
    bool mergeLoose = true;
    /** Points this close to a plane count as on it. */
    double tolerance = 1e-5;
};

struct Result
{
    std::vector<geo::Mesh> pieces;
    /** Per piece, the source face ids it holds parts of, ascending. */
    std::vector<std::vector<uint32_t>> sourceFaces;
    /** The radius each plane cut with, after fitting. */
    std::vector<double> radii;
    std::vector<std::string> warnings;
};

/** The radius radius 0 fits the plane to: the smallest cross-section loop around origin, a little enlarged. 0 when it misses. */
LILA_SCENE_API double fitRadius(const geo::Mesh& mesh, const Plane& plane);

/**
 * Cuts the mesh (planes in its own space) along each plane's disc and returns the connected pieces, in order of their
 * lowest source face. Pieces keep every attribute; caps copy them from a face beside the hole.
 */
LILA_SCENE_API Result splitByPlanes(const geo::Mesh& mesh, std::span<const Plane> planes, const Options& opts = {});

/** Volume enclosed by a closed mesh (signed by winding: outward faces give a positive volume). */
LILA_SCENE_API double enclosedVolume(const geo::Mesh& mesh);

/** How a piece is rebuilt after the split. */
struct Remesh
{
    enum class Method
    {
        None, // kept true to the source
        Quad,
        Voxel,
        Decimate,
    };
    Method method = Method::None;
    int targetFaces = 300;
    /** Voxels along the longest side. */
    int resolution = 32;
    /** Triangles kept, 0 to 1. */
    double ratio = 0.25;
    /** Decimate: triangles to keep, overriding ratio when positive. */
    int targetTriangles = 0;
};

LILA_SCENE_API const char* methodName(Remesh::Method m);
/** none, quadRemesh, voxelRemesh, decimate. Throws std::invalid_argument for anything else. */
LILA_SCENE_API Remesh::Method parseMethod(const std::string& name);
/** Reads {remesh, targetFaces, resolution, ratio} over base. */
LILA_SCENE_API Remesh remeshFromJson(const json& j, Remesh base = {});
/**
 * Per-piece tuning as the adjust popup edits it: pieceFaces "0:150 3:80" sets those pieces' face count (quad remesh
 * faces, decimate triangles), exactPieces "1 4" keeps those pieces exact (no remesh). Unreadable entries are skipped.
 */
LILA_SCENE_API void applyPieceOverrides(std::vector<Remesh>& pieces, const std::string& pieceFaces, const std::string& exactPieces);
/** The piece rebuilt; the piece itself when the method is None or the rebuild fails. */
LILA_SCENE_API geo::Mesh remeshPiece(const geo::Mesh& piece, const Remesh& r);

/**
 * World planes where the joints a click would select meet on a skinned mesh: one per pair of neighbouring regions of
 * its dominant-influence map (helpers count as the joint their selectAs names), fitted to the boundary between them
 * and facing the child. Regions under minFaces faces are left out. Empty for an unskinned mesh.
 */
LILA_SCENE_API std::vector<Plane> suggestPlanes(Graph& g, NodeId mesh, int minFaces = 8);

/** Planes as [{origin, normal, radius}], world space. A {joint} entry sits at the joint facing along its bone. */
LILA_SCENE_API std::vector<Plane> planesFromJson(Graph& g, const json& j);
LILA_SCENE_API json planesToJson(std::span<const Plane> planes);

/**
 * A symmetry plane for split guides, world space: the mesh's own mirror symmetry when it has one (its axis, through its
 * pivot or its custom offset), else the object's x plane through its pivot. Needs no symmetry map, so it works on meshes
 * that are not quite symmetrical.
 */
LILA_SCENE_API Plane symmetryPlane(Graph& g, NodeId mesh);
/** p reflected across sym (origin and normal reflected; the radius kept). */
LILA_SCENE_API Plane reflect(const Plane& p, const Plane& sym);

/** splitMesh, suggestSplitPlanes. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::split
