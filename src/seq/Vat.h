#pragma once

#include "core/Export.h"

#include "seq/MeshSequence.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

/**
 * Vertex Animation Textures: an animated mesh (geometry nodes, deformers, anything seq::sample can evaluate) baked
 * into a rest mesh plus textures a vertex shader reads, so the animation plays on a plain static mesh.
 *
 * Files, all named after VatOptions::name in VatOptions::folder:
 *
 *   <name>.fbx       the rest mesh, with one extra UV channel (meta "uvChannel") that addresses each vertex's texels
 *   <name>_pos.exr   positions, RGBA half float (format exr; float with exrBits 32), or <name>_pos.png, RGBA
 *                    normalised by bounds, 16 bit (png16) or 8 bit (png8)
 *   <name>_nrm.png   normals, RGBA 8 or 16 bit, n * 0.5 + 0.5 (normals on)
 *   <name>.json      the metadata below
 *
 * Texture layout. Columns are vertices, rows are frames. A frame takes rowsPerFrame consecutive rows: vertex i sits
 * at column i % width, row frame * rowsPerFrame + i / width, so width is min(vertexCount, maxWidth) and
 * rowsPerFrame = ceil(vertexCount / width). height = frameCount * rowsPerFrame. Row 0 is the first scanline of the
 * file, which is V = 0 in Unreal. Unused texels of the last row of a frame are zero.
 *
 * The extra UV channel holds, for vertex i, Unreal-convention UVs (V down, as Unreal shows them after its importer
 * flips V) of the texel centre in frame 0: U = (i % width + 0.5) / width, V = (i / width + 0.5) / height. A material
 * samples at V + floor(frame) * rowsPerFrame / height with nearest filtering.
 *
 * Values are in Unreal space: the axis profile and scale of the FBX, as Unreal's importer lands them (cm, X forward,
 * Y right, Z up for ue5). positions offset stores P(frame) - P(rest), absolute stores P(frame). EXR stores the value
 * itself (bounds are informational); PNG stores (value - bounds.min) / (bounds.max - bounds.min) per component. A
 * material decodes HDR ? texel : lerp(BoundsMin, BoundsMax, texel). Alpha is 1. Normals are unit vectors in the same
 * space. The metadata's "materialParams" holds those material inputs by name: HDR (1 for EXR, 0 for PNG), BoundsMin,
 * BoundsMax, FrameCount, RowsPerFrame, TextureHeight, FrameRate, PositionTexture and NormalTexture (file names).
 *
 * Topology. constant: every frame has the rest frame's point and face counts, a column per point, normals averaged
 * per point. soup: each frame is triangulated into an unwelded triangle soup, three columns per triangle, padded to
 * the largest frame's triangle count; slots a frame does not use collapse onto one point (zero area), so the rest
 * mesh is the largest soup with split normals. rigid: below. auto picks soup when the topology changes, rigid when
 * every mesh has an int point attribute `piece` and every piece fits a rigid motion within kVatRigidTolerance, and
 * constant otherwise.
 *
 * Rigid (mode rigid): loose pieces that only move and rotate, like a fracture simulation, baked as one transform per
 * piece per frame instead of one value per vertex. Files:
 *
 *   <name>.fbx       the rest mesh with three extra UV channels: meta "uvChannel" addresses the piece's texels,
 *                    meta "pivotChannels" [a, b] hold the piece's pivot as (x, y) in channel a and (z, 0) in channel b
 *   <name>_pos.exr   pivot translation per piece per frame, RGBA (.png when normalised)
 *   <name>_rot.exr   rotation per piece per frame, a unit quaternion in RGBA = xyzw (.png when normalised)
 *   <name>.json      the metadata
 *
 * Pieces come from each mesh's int point attribute `piece` (one piece per distinct value, columns in ascending value
 * order) or, for a mesh without it, from its connected components (columns in order of each component's lowest point
 * index); meshes follow one another. Columns are pieces with the wrapping above (width = min(pieceCount, maxWidth),
 * rowsPerFrame = ceil(pieceCount / width)) and the addressing UV is the texel centre of the piece's column in frame 0,
 * exactly as for a vertex. The pivot is the piece's rest centroid in Unreal space; the pivot channels store it at the
 * UV's full float precision, written so that Unreal's V flip lands (x, y) and (z, 0). Every frame, the rotation q and
 * the frame centre c are a least squares rigid fit of the piece's rest points onto that frame's points (Horn's
 * quaternion method in double precision, c = the frame centroid); q is sign continuous over the frames (dot with the
 * previous frame >= 0, the first frame w >= 0), so neighbouring frames blend with a normalised lerp. The position
 * texel is c - pivot (positions offset) or c (absolute); EXR stores it and q as is, PNG stores the position
 * normalised by bounds like the vertex modes and q * 0.5 + 0.5. EXR is 32-bit float by default here, since half
 * floats would put centimetre steps into a metre-scale offset. The material decodes, per vertex at rest position P:
 *
 *   q = normalize(HDR ? rotTexel : rotTexel * 2 - 1)
 *   c = (Absolute ? 0 : pivot) + (HDR ? posTexel : lerp(BoundsMin, BoundsMax, posTexel))
 *   P' = rotate(q, P - pivot) + c,     World Position Offset = P' - P
 *   rotate(q, v) = v + 2 q.w (q.xyz x v) + 2 q.xyz x (q.xyz x v);  normals and tangents rotate by q the same way
 *
 * There is no normal texture. The metadata records the largest fit error over all pieces and frames ("rigid":
 * {maxResidual (Unreal units), piece, frame}); above kVatRigidTolerance the export logs a warning, since the input
 * is not rigid. materialParams adds PieceCount, RotationTexture, PivotChannelXY and PivotChannelZ, and has no
 * NormalTexture. "bounds" is the range of the position texels and "meshBounds" every vertex over the whole bake.
 */
namespace lila::seq
{
struct VatOptions
{
    /** Range, fps, scale, profile, world space, recentre, modifiers. format and triangulate are ignored. */
    SequenceOptions sampling;
    std::string folder;
    /** File base name; {name} is the first mesh. */
    std::string name = "{name}";
    /** exr | png16 | png8 ("png" reads as png16) */
    std::string format = "exr";
    /** auto | constant | soup | rigid */
    std::string mode = "auto";
    /** offset | absolute */
    std::string positions = "offset";
    bool normals = true;
    /** 8 | 16 */
    int normalBits = 8;
    /** The frame whose shape is the rest mesh; NaN means the first sampled frame. */
    double restFrame = std::numeric_limits<double>::quiet_NaN();
    /** Widest texture before a frame wraps onto more rows. */
    int maxWidth = 4096;
    /** EXR channel depth, 16 (half) or 32 (float); 0 means 32 for rigid and 16 otherwise. */
    int exrBits = 0;
};

/** Largest rigid fit error, in Unreal units (cm for ue5), that auto accepts as rigid; rigid warns above it. */
inline constexpr double kVatRigidTolerance = 0.01;

/** The int point attribute naming each point's rigid piece. */
inline constexpr const char* kVatPieceAttr = "piece";

/** Tallest texture Unreal takes. */
inline constexpr int kVatMaxHeight = 16384;

/** "maxWidth" from the vat prefs (4096 unless set), clamped to 16 to 16384. */
LILA_SCENE_API int vatDefaultMaxWidth();

/** Options from a json object of the exportVertexAnimation flags; `sampling` is the already parsed range. Throws std::invalid_argument. */
LILA_SCENE_API VatOptions vatFromJson(const nlohmann::json& j, const SequenceOptions& sampling);

struct VatResult
{
    fs::path mesh;
    fs::path positions;
    fs::path normals; // empty with normals off and in rigid mode
    fs::path rotations; // rigid mode only
    fs::path meta;
    nlohmann::json metadata;

    /** {mesh, positions, normals?, rotations?, meta}, generic paths. */
    nlohmann::json files() const;
};

/** materialParams as one line, to type into a material by hand. */
LILA_SCENE_API std::string vatMaterialSummary(const nlohmann::json& materialParams);

/** Main thread. Samples and writes every file and logs vatMaterialSummary. Throws std::runtime_error (a constant mode mismatch names the frame) or progress::Cancelled. */
LILA_SCENE_API VatResult exportVat(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const VatOptions& o);

/** The paths exportVat writes, without evaluating; metadata holds only what the options decide. */
LILA_SCENE_API VatResult plannedVat(scene::Graph& g, const std::vector<scene::NodeId>& meshes, const VatOptions& o);

/** A decoded texture: width * height RGBA texels, row 0 first. PNG texels are 0 to 1. */
struct VatImage
{
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
};

/** Reads an EXR or a PNG (8 or 16 bit RGBA, as written here). Empty image and *err set on failure. */
LILA_SCENE_API VatImage readVatImage(const fs::path& file, std::string* err = nullptr);
} // namespace lila::seq
