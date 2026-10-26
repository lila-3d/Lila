#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace masonry
{
namespace geo = lila::geo;

/** How stones are arranged in each course. */
enum class Bond : uint8_t
{
    Running,    // stretchers, each course offset by half a stone
    Stack,      // stretchers, no offset
    English,    // a course of stretchers, then a course of headers
    Flemish,    // stretcher and header alternating in every course
    Header,     // headers only, offset by half a header
    GardenWall, // three courses of stretchers, then one of headers
};

const char* bondName(Bond b);
bool parseBond(std::string_view name, Bond* out);
std::vector<std::string> bondNames();

/** What a stone is in the bond. */
enum class Role : uint8_t
{
    Stretcher, // long side on the face
    Header,    // end on the face, running into the wall
    Bat,       // half stretcher, at wall ends and corners
    Closer,    // narrow piece that keeps the bond at corners, or a stone trimmed against another
    Filler,    // cut to fit the length of the course
};

/** Which face of the wall a stone belongs to. Through stones show on both. */
enum class Face : uint8_t
{
    Outer,
    Inner,
    Through,
};

/** The nominal stone and the joint between stones, cm. Defaults are a UK brick. */
struct Units
{
    double length = 21.5;
    double height = 6.5;
    double depth = 10.25;
    double joint = 1.0;
};

struct WallParams
{
    Bond bond = Bond::Running;
    Units stone;
    double height = 200.0;     // cm
    double thickness = 21.5;   // cm
    double recess = 0.6;       // how far the mortar sits behind the stone faces, cm
    double cornerAngle = 30.0; // deg: turns sharper than this are corners, gentler ones curve the wall
    double jitter = 1.0;       // 0 to 1: how much of the joint stones may wander and tilt into
    double protrude = 0.3;     // cm: stones pushed out of the face by up to this
    uint32_t seed = 1;
};

/** One stone: a box in its own frame. */
struct Stone
{
    glm::dvec3 center{0.0};
    /** Columns: along the face, up, into the wall. Always a rotation. */
    glm::dmat3 frame{1.0};
    glm::dvec3 size{0.0};
    int32_t wall = 0;
    int32_t course = 0;
    int32_t run = 0;
    Role role = Role::Stretcher;
    Face face = Face::Outer;
    float random = 0.0f;
};

/** A wall's base line. Y is up; the wall stands on the lowest point. */
struct Footprint
{
    std::vector<glm::dvec3> points;
    bool closed = false;
};

struct Layout
{
    std::vector<Stone> stones;
    int courses = 0;
    double pitch = 0.0;  // course height including the bed joint
    int trimmed = 0;     // stones cut back where they met another
    int dropped = 0;     // stones that had nothing left after trimming
};

/** Lays every course of a wall along a footprint. Deterministic for the same inputs. */
Layout layoutWall(const Footprint& footprint, const WallParams& p, int wall = 0);

/** The mortar body the stones sit in: the wall's volume, recessed behind the faces and below the top. */
geo::Mesh coreMesh(const Footprint& footprint, const WallParams& p);

glm::dmat4 stoneMatrix(const Stone& s);

/** Boxes intersecting by more than tolerance (cm), by the separating axis test. */
bool overlaps(const Stone& a, const Stone& b, double tolerance = 0.01);

/** Pairs of stones intersecting by more than tolerance (cm). What masonryInfo reports; 0 for a clean layout. */
size_t countOverlaps(std::span<const Stone> stones, double tolerance = 0.01);

/** How detailed a stone mesh is. Every level fills the same box, so they swap as LODs. */
enum class Detail : uint8_t
{
    Worn,    // chamfered, corners knocked off by wear
    Clean,   // chamfered
    Box,     // six quads
};

struct StoneShape
{
    glm::dvec3 size{21.5, 6.5, 10.25};
    double bevel = 0.5;   // cm
    double wear = 0.5;    // 0 to 1
    uint32_t seed = 0;
    Detail detail = Detail::Worn;
    double tile = 100.0;  // cm of stone per UV unit
};

/** A closed stone mesh centred on the origin, in the stone's frame, with uv0 and hard edges. */
geo::Mesh stoneMesh(const StoneShape& s);

/** The stone at every detail, LOD0 first: worn, clean chamfer, plain box. All fit the same box. */
std::array<geo::Mesh, 3> stoneLods(StoneShape s);

/** Footprints from a mesh's loose edges (edges with no face), chained into lines and loops. */
std::vector<Footprint> footprintsFromEdges(const geo::Mesh& m);

/** A mesh holding footprints as loose edges, what masonryWall stores on the mesh it makes. */
geo::Mesh edgesFromFootprints(std::span<const Footprint> footprints);

/** Stones plus the meshes they reference, ready for instances or a merged mesh. */
struct Built
{
    std::vector<Stone> stones;
    std::vector<int> reference;           // per stone, into meshes
    std::vector<geo::Mesh> meshes;        // one per distinct size and variant
    std::vector<std::array<geo::Mesh, 3>> lods; // per mesh, when BuildOptions::lods
    std::vector<int> variant;             // per stone
    geo::Mesh core;
    int courses = 0;
    int trimmed = 0;
    int dropped = 0;
};

struct BuildOptions
{
    WallParams wall;
    double bevel = 0.5;
    double wear = 0.5;
    int variants = 6;
    Detail detail = Detail::Worn;
    double tile = 100.0;
    bool lods = false; // also make every mesh's LOD chain
};

/** Every footprint laid and its stone meshes made, the same work the Masonry Wall node does. */
Built buildWalls(std::span<const Footprint> footprints, const BuildOptions& o);

/** Material slots: every stone mesh is on the first, the core on the second. Unreal finds or makes materials by these names. */
inline constexpr const char* kStoneSlot = "MasonryStone";
inline constexpr const char* kMortarSlot = "MasonryMortar";

/** Convex pieces covering the wall's full volume, one per footprint segment per height band: its UCX collision. */
std::vector<geo::Mesh> collisionHulls(const Footprint& footprint, const WallParams& p, int bands = 1);

/** The given stones at one LOD (0 to 2) merged into one mesh, positioned relative to origin. */
geo::Mesh mergeStones(const Built& b, std::span<const int> stones, int lod, const glm::dvec3& origin);

/** Stones grouped by the square ground cell (size cm) their centre is in, for streaming and culling. */
struct Chunks
{
    std::vector<int> of;            // per stone, into cells
    std::vector<glm::ivec2> cells;  // x and z cell index, sorted
};
Chunks chunkStones(std::span<const Stone> stones, double size);

/** splitmix64 hash: stable random numbers keyed by seed and indices. */
uint64_t hash64(uint64_t x);
double unitRandom(uint64_t key); // [0, 1)
} // namespace masonry
