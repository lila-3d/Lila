#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

/**
 * Blade extrudes: faces pulled out along their normal into blades, with what built each blade recorded on the mesh so
 * it can be varied later. Every point a blade made carries its root position, its fraction of the way up, the blade
 * normal and its inset direction, so the blade's shape is recomputed from its values rather than nudged, and varying it
 * again never stacks on the last variation.
 *
 * Free of the scene graph: plain geo::Mesh in and out, so the tests drive it directly.
 */
namespace blades
{
using lila::geo::Index;
using lila::geo::Mesh;

/** Point layers. */
inline constexpr const char* kBladeLayer = "bladeExtrude.blade";      // int, -1 for points no blade made
inline constexpr const char* kLevelLayer = "bladeExtrude.level";      // float, 0 at the root to 1 at the cap
inline constexpr const char* kRootLayer = "bladeExtrude.root";        // float3, where the point stands at zero height
inline constexpr const char* kNormalLayer = "bladeExtrude.normal";    // float3, the blade's growth direction
inline constexpr const char* kInsetLayer = "bladeExtrude.inset";      // float3, movement per unit of offset
inline constexpr const char* kValuesLayer = "bladeExtrude.values";    // float3, thickness, offset, local translate Z now
inline constexpr const char* kBaseLayer = "bladeExtrude.baseValues";  // float3, the values the blade was extruded with
/** Face layers. */
inline constexpr const char* kCapLayer = "bladeExtrude.cap";   // int, the blade whose cap this is, -1 otherwise
inline constexpr const char* kWallLayer = "bladeExtrude.wall"; // int, the blade whose wall this is, -1 otherwise

/** The fields a blade varies, in the order they are randomized. */
enum Field
{
    kThickness,
    kOffset,
    kTranslateZ,
    kFieldCount
};
const char* fieldName(int field); // "thickness", "offset", "localTranslateZ"
const char* fieldLabel(int field); // "Thickness", "Offset", "Local Translate Z"
/** The range the panel offers before anything is stored. */
glm::dvec2 fieldDefaultRange(int field);
int fieldIndex(const std::string& name); // -1 when unknown

struct BladeValues
{
    double v[kFieldCount] = {1.0, 0.0, 0.0};
    double& operator[](int i) { return v[i]; }
    double operator[](int i) const { return v[i]; }
};

struct ExtrudeOptions
{
    BladeValues values;
    int divisions = 1;
    /** Connected faces grow as one blade; off, every face is its own. */
    bool keepFacesTogether = false;
};

struct Blade
{
    int id = -1;
    std::vector<Index> points; // the points the blade made or moved
    std::vector<Index> caps;
    std::vector<Index> walls;
    glm::dvec3 root{0.0}; // centre of the ring it stands on, object space
    BladeValues values;
    BladeValues base;
};

/** Extrudes the faces as blades. Returns the new blade ids, in face order. */
std::vector<int> extrude(Mesh& mesh, std::span<const Index> faces, const ExtrudeOptions& options);

/** Every blade on the mesh, by id. */
std::vector<Blade> blades(const Mesh& mesh);
bool hasBlades(const Mesh& mesh);

/** Gives a blade new values and moves its points to match. */
void setValues(Mesh& mesh, int blade, const BladeValues& values);

/** The faces the blades grew from, which are their caps now. */
std::vector<Index> capFaces(const Mesh& mesh);
/** Every face a blade made or moved: caps and walls, down to the ring they stand on. */
std::vector<Index> stemFaces(const Mesh& mesh);

/** Connected groups of the faces, adjacency through shared edges. Each group sorted, groups by lowest face. */
std::vector<std::vector<Index>> faceRegions(const Mesh& mesh, std::span<const Index> faces);
/** Points used by the faces, sorted. */
std::vector<Index> facePoints(const Mesh& mesh, std::span<const Index> faces);
/** Connected groups of the points, adjacency through edges between two of them. */
std::vector<std::vector<Index>> pointGroups(const Mesh& mesh, std::span<const Index> points);
/** Centre of the points around the group that are not in it, else of the group itself. */
glm::dvec3 groupRoot(const Mesh& mesh, std::span<const Index> points);

/** Seeded from seed, or from the system when it is 0 so every run differs. */
class Rng
{
public:
    explicit Rng(uint64_t seed);
    double uniform();
    double uniform(double low, double high) { return low + (high - low) * uniform(); }

private:
    std::mt19937_64 mEngine;
};

/** A value in the range, pulled toward the high end where fraction is high (bias > 0) or low (bias < 0). */
double biased(Rng& rng, double low, double high, double fraction, double bias);
/** Each value's place between the lowest and highest, 0.5 when they all match. */
std::vector<double> fractions(std::span<const double> values);
} // namespace blades
