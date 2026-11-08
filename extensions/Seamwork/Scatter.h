#pragma once

#include "Seams.h"

#include <functional>
#include <random>
#include <string>
#include <vector>

namespace seamwork
{
/** Which side of a join things spread onto: the lower or ground surface (b), the wall (a), or both. */
enum class Side : uint8_t
{
    Ground,
    Wall,
    Both,
};
const std::vector<std::string>& sideNames();

/** How things are picked and placed along joins. Lengths are cm. */
struct ScatterOptions
{
    /** Clumps per metre of join. */
    double density = 2.0;
    int clump = 1;
    double clumpRadius = 10.0;
    /** How far from the join things spread, falling off linearly. */
    double spread = 0.0;
    Side side = Side::Ground;
    /** Lean into the crease: up along the bisector of both normals. Off stands things on the surface they land on. */
    bool wedge = true;
    /** Fraction of each thing's height pushed below where it lands. */
    double sink = 0.1;
    double scaleMin = 0.8;
    double scaleMax = 1.2;
    /** Random tilt, degrees. */
    double tilt = 10.0;
    uint32_t seed = 0;
    /** Kinds taken, bit per Kind (1 ground, 2 mesh, 4 crease). */
    int kinds = 7;
    double angleMin = 0.0, angleMax = 180.0;
    double exposureMin = 0.0, exposureMax = 1.0;
    /** Joins deeper than this take nothing (a rock buried past it needs no cover); 0 for no limit. */
    double depthMax = 0.0;
    /** Stretch along the join (snow drifts), 1 for none. */
    double stretch = 1.0;
    glm::dvec3 up{0.0, 1.0, 0.0};
};

/** One asset: its size, so placement can sink and wedge it. Base centre at the origin, up +Y. */
struct AssetShape
{
    double height = 10.0;
    double radius = 5.0;
};

struct Placement
{
    glm::dmat4 world{1.0};
    /** Index into the assets. */
    int asset = 0;
    /** Where on the join it came from, and the point its base landed on. */
    glm::dvec3 seamPoint{0.0};
    glm::dvec3 base{0.0};
    size_t curve = 0;
};

/** Where a point lands when dropped along -dir onto the surface it spread over; false keeps it where it is. */
using DropFn = std::function<bool(const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& normal)>;

/** Whether a point passes the angle, exposure, depth and kind filters. */
bool accepts(const SeamPoint& p, const ScatterOptions& o);

/** Placements along the joins; the same inputs give the same placements at any thread count. */
std::vector<Placement> distribute(const Result& r, const std::vector<AssetShape>& assets, const ScatterOptions& o, const DropFn& drop = {});
/** distribute over points already sampled (Seam Points' output). */
std::vector<Placement> distributePoints(const std::vector<SeamPoint>& pts, const std::vector<AssetShape>& assets, const ScatterOptions& o,
                                        const DropFn& drop = {});

/** Named settings and the built-in asset they use: rubble, gravel, grassTufts, mossClumps, roots, snowDrift, leafLitter. */
struct Preset
{
    std::string name;
    std::string doc;
    ScatterOptions options;
    /** Builds variant k of the asset (base centre at the origin, up +Y), cm. */
    std::function<Mesh(int variant, uint32_t seed)> asset;
    int variants = 3;
};
const std::vector<Preset>& presets();
const Preset* preset(const std::string& name);

// Proximity rules on any scatter.

enum class RuleKind : uint8_t
{
    Repel,
    Shrink,
    Tilt,
    CullInside,
    CullOutside,
};
const std::vector<std::string>& ruleNames();

struct ProximityRule
{
    RuleKind kind = RuleKind::Repel;
    /** The object the rule reads: its mesh, tree and world. */
    Object target;
    double radius = 100.0;
    /** Repel: fraction of the remaining distance pushed; shrink: smallest scale; tilt: degrees at the surface. */
    double strength = 1.0;
};

struct RuleOutcome
{
    glm::dmat4 world{1.0};
    bool culled = false;
};

/** Applies the rules in order to one instance placed at world (its base at the matrix origin). */
RuleOutcome applyRules(const glm::dmat4& world, const std::vector<ProximityRule>& rules, const glm::dvec3& up);
/** Inside a closed mesh, by ray parity along up and two other axes (majority). */
bool insideClosed(const Object& o, const glm::dvec3& p);
} // namespace seamwork
