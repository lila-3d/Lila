#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace terrain
{
/** A road's cross-section and what stands along it, cm, symmetric about the centreline. */
struct RoadProfile
{
    int lanes = 2;
    double laneWidth = 350.0;
    /** Paved strip beyond the outer lanes, each side. */
    double shoulder = 100.0;
    /** 0 is no kerb. */
    double kerbHeight = 0.0;
    double kerbWidth = 15.0;
    /** Behind the kerb, each side. */
    double sidewalk = 0.0;
    /** Level unpaved strip at the formation's edge, each side. */
    double verge = 100.0;
    /** V ditch beyond the verge, in cuttings only; 0 is none. */
    double ditchWidth = 0.0;
    double ditchDepth = 0.0;
    /** asphalt, concrete, gravel, dirt, ballast: what the surface masks and meshes are named for. */
    std::string surface = "asphalt";
    /** Guardrails where the fill at the edge drops more than this, cm, and on bridges; 0 is none. */
    double guardrailFill = 300.0;
    /** Delineators (runway: edge lights) every this many cm on both edges; 0 is none. */
    double markerSpacing = 5000.0;
    /** Poles (lights, catenary masts) every this many cm on the right edge; 0 is none. */
    double poleSpacing = 0.0;
    /** Corner radius where another road joins, cm; 0 leaves sharp corners. */
    double kerbReturn = 800.0;
    /** Width of the worn dirt band outside the formation, cm. */
    double dirtEdge = 150.0;

    double carriageway() const { return double(std::max(1, lanes)) * laneWidth; }
    double pavedHalf() const { return carriageway() * 0.5 + shoulder; }
    double kerbHalf() const { return pavedHalf() + (kerbHeight > 0.0 ? kerbWidth : 0.0); }
    /** Half the levelled width: paved, kerb, sidewalk and verge. */
    double formationHalf() const { return kerbHalf() + sidewalk + verge; }

    nlohmann::json toJson() const;
    /** Fields present in j override this one's. */
    void read(const nlohmann::json& j);
};

/** How a road is laid on the ground. Lengths and heights cm, grades and banks percent, angles degrees. */
struct RoadDesign
{
    /** km/h: sets the curve radius and banking. */
    double designSpeed = 60.0;
    double maxGrade = 8.0;
    /** Smallest radius of a crest or sag; the road is smooth over bumps shorter than this allows. */
    double minVerticalRadius = 100000.0;
    /** Largest superelevation (cross fall into a curve). */
    double maxBank = 6.0;
    /** Fastest change of bank, as the edge's rise against the centreline, percent. */
    double bankRunoff = 0.6;
    /** Smallest horizontal curve radius; 0 takes it from the design speed and maxBank. */
    double minCurveRadius = 0.0;
    double cutAngle = 45.0;
    double fillAngle = 33.7;
    /** Cut or fill slopes taller than this become retaining walls; 0 never builds walls. */
    double wallHeight = 600.0;
    /** Fill deeper than this at the centre becomes a bridge span; 0 never bridges. */
    double bridgeHeight = 800.0;
    /** Cut deeper than this becomes a tunnel span; 0 never tunnels. */
    double tunnelDepth = 2000.0;
    /** Bore radius; 0 fits the formation. */
    double tunnelRadius = 0.0;
    double deckDepth = 150.0;
    double pillarSpacing = 3000.0;
    /** How far the grade line may stray from the smoothed ground before it bends. */
    double fitTolerance = 50.0;

    /** The radius the design speed and maxBank need, cm. */
    double speedRadius() const;

    nlohmann::json toJson() const;
    void read(const nlohmann::json& j);
};

const std::vector<std::string>& roadKindNames();
const std::vector<std::string>& roadProfileNames();
/** Throws std::invalid_argument for an unknown name. */
RoadProfile roadProfilePreset(const std::string& name);
RoadDesign roadDesignPreset(const std::string& kind);
std::string defaultRoadProfile(const std::string& kind);

/** A height the road must pass through: at the centreline point nearest at. */
struct RoadKey
{
    glm::dvec2 at{0.0};
    double height = 0.0;
};

/**
 * One road in a Terrain Roads node, in the heightfield's own space: a kind (road, path, track, rail, runway) with a profile
 * and design from presets plus overrides, along path or the curve an Object Info node named object follows.
 */
struct Road
{
    std::string id;
    std::string kind = "road";
    std::string profileName;
    RoadProfile profile;
    RoadDesign design;
    std::vector<glm::dvec2> path;
    std::string object;
    std::vector<RoadKey> keys;
    /** Round the path's corners into curves of at least the design radius. */
    bool smooth = true;
    /** Extra masks written over the paved surface (paved, for a city). */
    std::vector<std::string> masks;
    /**
     * A roundabout's ring: path is the closed loop (first point not repeated), laid level at one height with ringCrossFall
     * (percent) falling away from the central island, which is raised and masked. Roads laid after it that end on it get
     * splitter islands.
     */
    bool ring = false;
    double ringCrossFall = 2.0;

    nlohmann::json toJson() const;
    /** Presets for kind and profileName, then every field j gives. Throws std::invalid_argument on unknown names. */
    static Road fromJson(const nlohmann::json& j);
};

/** Masks every road writes. */
inline constexpr const char* kRoadSurfaceMask = "roadSurface";
inline constexpr const char* kRoadVergeMask = "roadVerge";
inline constexpr const char* kRoadDirtMask = "roadDirtEdge";
inline constexpr const char* kRoadWearMask = "roadWear";
inline constexpr const char* kErosionProtectMask = "erosionProtect";
/** Raised kerbed islands: a roundabout's centre and the splitters on the roads entering it. */
inline constexpr const char* kRoadIslandMask = "roadIsland";

struct RoadResult
{
    /** Stations along the alignment: x, surface height, z. */
    std::vector<glm::dvec3> centreline;
    std::vector<glm::dvec2> tangents;
    std::vector<double> along;
    std::vector<double> ground;
    /** Cross fall as a fraction, positive lowering the left edge (into a left-hand curve). */
    std::vector<double> bank;
    /** Grade between this station and the next, fraction. */
    std::vector<double> grade;
    /** Signed horizontal curvature, 1/cm, positive turning left. */
    std::vector<double> curvature;
    /** 0 on ground, 1 bridge, 2 tunnel, 3 inside another road's junction. */
    std::vector<uint8_t> span;

    double length = 0.0;
    /** Percent. */
    double maxGrade = 0.0;
    double maxBank = 0.0;
    /** Smallest crest or sag radius built, cm (infinite when the profile is one straight grade). */
    double minVerticalRadius = 0.0;
    double minCurveRadius = 0.0;
    double formationHalf = 0.0;
    double pavedHalf = 0.0;
    double kerbHalf = 0.0;

    struct Span
    {
        size_t from = 0, to = 0;
        std::string kind;
    };
    std::vector<Span> spans;

    struct WallPoint
    {
        glm::dvec2 at;
        double top = 0.0, bottom = 0.0;
    };
    /** Retaining walls along the formation edge, side +1 left, -1 right. */
    struct Wall
    {
        int side = 1;
        bool cut = true;
        std::vector<WallPoint> points;
    };
    std::vector<Wall> walls;

    struct Pillar
    {
        glm::dvec3 top;
        double ground = 0.0;
    };
    struct Bridge
    {
        std::vector<glm::dvec3> deck;
        std::vector<Pillar> pillars;
        double length = 0.0;
        double maxHeight = 0.0;
    };
    std::vector<Bridge> bridges;

    struct Portal
    {
        glm::dvec3 at;
        /** Into the tunnel. */
        glm::dvec2 direction;
    };
    /** Tunnel spans as data a volume pass carves: the bore's axis and radius, its floor (the road surface) and portals. */
    struct Tunnel
    {
        std::vector<glm::dvec3> axis;
        std::vector<glm::dvec3> floor;
        double radius = 0.0;
        double length = 0.0;
        Portal portals[2];
    };
    std::vector<Tunnel> tunnels;
    std::vector<glm::dvec2> culverts;

    struct PropRun
    {
        std::string kind;
        int side = 1;
        std::vector<glm::dvec3> points;
    };
    /** Guardrails as runs; markers and poles as runs of placements along an edge. */
    std::vector<PropRun> guardrails;
    struct Prop
    {
        std::string kind;
        int side = 1;
        glm::dvec3 at;
        glm::dvec2 direction;
    };
    std::vector<Prop> props;

    struct Junction
    {
        std::string other;
        std::string type;
        glm::dvec3 at;
    };
    std::vector<Junction> junctions;
    /** Rounded corners where this road meets another: the corner point and the arc (both on the other road's plane). */
    struct KerbReturn
    {
        std::string other;
        glm::dvec3 corner;
        std::vector<glm::dvec3> arc;
    };
    std::vector<KerbReturn> kerbReturns;
    /** Raised islands: central (a ring's centre) or splitter (between the entry and exit lanes where this road meets a ring). */
    struct Island
    {
        std::string kind;
        std::string other;
        std::vector<glm::dvec2> outline;
        double height = 0.0;
    };
    std::vector<Island> islands;
    /** A closed loop: the last station is the first. */
    bool ring = false;

    /** cm3 and cm2 in the heightfield's units. */
    double cut = 0.0, fill = 0.0, area = 0.0;
    std::vector<std::string> warnings;

    bool empty() const { return centreline.size() < 2; }
    /** The design surface at a point: height on the banked plane of the nearest station, lateral offset (left +). */
    struct Surface
    {
        double height = 0.0;
        double lateral = 0.0;
        double overshoot = 0.0;
        size_t station = 0;
    };
    Surface surfaceAt(glm::dvec2 p) const;
};

/** One road for applyRoads: its settings and its line (path, or the followed curve) in the heightfield's space. */
struct RoadInput
{
    Road road;
    std::vector<glm::dvec2> line;
};

/**
 * Lays the roads into the heightfield in order: horizontal curves, a longitudinal profile held to maxGrade with vertical
 * curves of at least minVerticalRadius through the height keys, banking, cut and fill at their talus angles, retaining
 * walls, bridge and tunnel spans, junctions with the roads before it (keys, matching cross fall, kerb returns), and the
 * road masks. Returns each road's result by id.
 */
std::map<std::string, RoadResult> applyRoads(Heightfield& h, const std::vector<RoadInput>& roads);

/**
 * The longitudinal profile alone: heights at stations s over ground g, through keys (station index, height), grades within
 * maxGrade (a fraction) joined by vertical curves of at least minRadius, ground smoothed over window first and followed to
 * within tolerance. achievedRadius gets the tightest curve built.
 */
std::vector<double> solveRoadProfile(const std::vector<double>& s, const std::vector<double>& g, const std::vector<std::pair<size_t, double>>& keys,
                                     double maxGrade, double minRadius, double window, double tolerance, double* achievedRadius,
                                     std::vector<std::string>* warnings);

/** What a Terrain Roads node (by name) of a modifier did on its last evaluation, by road id. */
void publishRoadResults(uint64_t modifier, const std::string& node, std::map<std::string, RoadResult> results);
std::map<std::string, RoadResult> roadResults(uint64_t modifier, const std::string& node);

/** Registers the Terrain Roads node, the road commands and their menu items. */
bool registerRoads(lila::ext::Context& ctx);
} // namespace terrain
