#pragma once

#include <lila/Mesh.h>

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * Settlement layout: streets, blocks, lots and building envelopes on a plane or any ground mesh. Layout happens in the
 * XZ plane (Vec2 x is world X, y is world Z); heights come from the Ground. Lengths are cm, areas in profiles are m2.
 */
namespace settlement
{
namespace geo = lila::geo;
using Vec2 = glm::dvec2;
using json = nlohmann::json;

// ---------------------------------------------------------------------------------------------------------------------
// Polygons

/** A simple polygon in XZ, counter-clockwise (positive signedArea). tag per side: side k runs from pts[k] to pts[k + 1]. */
struct Polygon
{
    std::vector<Vec2> pts;
    std::vector<int> tag;

    size_t size() const { return pts.size(); }
};

double signedArea(std::span<const Vec2> pts);
double area(const Polygon& p);
Vec2 centroid(const Polygon& p);
bool contains(std::span<const Vec2> pts, Vec2 q);
/** Keeps the part of p where dot(q - origin, normal) >= 0. Sides along the cut get cutTag. */
Polygon clipHalfPlane(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag = -1);
/**
 * Moves each side inward by its own distance. Concave corners are mitred; when that folds the polygon over itself the
 * result is the intersection of the inset half-planes instead, which is exact for convex polygons. Empty when nothing
 * is left.
 */
Polygon inset(const Polygon& p, std::span<const double> distances);
bool selfIntersects(std::span<const Vec2> pts);
/** Closed polygon from points, made counter-clockwise, duplicate and collinear points dropped. */
Polygon makePolygon(std::vector<Vec2> pts, int tag = -1);

// ---------------------------------------------------------------------------------------------------------------------
// Ground

/** What streets and buildings stand on: a plane, or a mesh baked to a height grid by casting down onto it. */
class Ground
{
public:
    static Ground plane(double height = 0.0);
    /** Heights over the mesh's XZ bounds at about cell cm spacing (the grid is capped at 1024 a side). */
    static Ground fromMesh(const geo::Mesh& mesh, double cell = 100.0);

    double height(Vec2 p) const;
    /** Rise over run at p, from central differences over one cell. */
    double grade(Vec2 p) const;
    /** Grade along the segment a-b, the steepest of a few samples. */
    double gradeAlong(Vec2 a, Vec2 b) const;
    /** False off the ground mesh, or where it has holes. Always true on a plane. */
    bool solid(Vec2 p) const;
    bool isPlane() const { return mHeights.empty(); }

private:
    double mPlane = 0.0;
    Vec2 mOrigin{0.0};
    double mCell = 100.0;
    int mW = 0, mH = 0;
    std::vector<float> mHeights; // NaN where the mesh has no surface
};

// ---------------------------------------------------------------------------------------------------------------------
// Street graph

enum class StreetClass : uint8_t
{
    Arterial,  // main roads between the centre, gates and attractors
    Collector, // streets that grow off them
    Local,
    Alley,
    Boundary, // the settlement's outline: closes blocks, is not a street
    Water,    // a river or canal: closes blocks, only arterials bridge it
};

const char* className(StreetClass c);
bool parseClass(std::string_view name, StreetClass* out);

struct StreetNode
{
    Vec2 p{0.0};
    bool alive = true;
};

struct StreetEdge
{
    int a = -1, b = -1;
    StreetClass cls = StreetClass::Local;
    int era = 0;
    bool alive = true;
    double width = 0.0; // overrides the class's width when above 0 (rivers)
};

/** Planar street network: inserting a street splits every street it crosses and snaps its ends to what is near. */
class StreetGraph
{
public:
    StreetGraph(double snap = 150.0, double cell = 2000.0);

    std::vector<StreetNode> nodes;
    std::vector<StreetEdge> edges;

    double snap() const { return mSnap; }
    /** Adds the segment p-q, split where it crosses or touches existing edges. Returns the edges made, in order p to q. */
    std::vector<int> addStreet(Vec2 p, Vec2 q, StreetClass cls, int era = 0, double width = 0.0);
    std::vector<int> addPolyline(std::span<const Vec2> pts, StreetClass cls, int era = 0, bool closed = false, double width = 0.0);

    /** Nearest node within r, or -1. */
    int nearestNode(Vec2 p, double r) const;
    /** Nearest alive edge within r: its id, the closest point and the parameter along it. -1 if none. */
    int nearestEdge(Vec2 p, double r, Vec2* at = nullptr, double* t = nullptr, int skipNode = -1) const;
    /** The first edge the segment p-q crosses, by distance from p: edge, point, parameter along p-q. -1 if none. */
    int firstCrossing(Vec2 p, Vec2 q, Vec2* at, double* s, int skipNodeA = -1, int skipNodeB = -1) const;

    int degree(int node, bool streetsOnly = true) const;
    /** Edges at a node (alive), any class. */
    const std::vector<int>& incident(int node) const { return mIncident[size_t(node)]; }
    double length(int edge) const;
    bool isStreet(int edge) const { return edges[size_t(edge)].alive && edges[size_t(edge)].cls < StreetClass::Boundary; }
    /** Splits the edge at parameter t, returning the new node. */
    int splitEdge(int edge, double t);
    void removeEdge(int edge);

private:
    double mSnap;
    double mCell;
    std::vector<std::vector<int>> mIncident;
    std::unordered_map<int64_t, std::vector<int>> mEdgeCells;
    std::unordered_map<int64_t, std::vector<int>> mNodeCells;

    int addNode(Vec2 p);
    int nodeAt(Vec2 p);
    int addEdge(int a, int b, StreetClass cls, int era, double width = 0.0);
    int64_t key(int x, int y) const { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    template <class F> void forCells(Vec2 lo, Vec2 hi, F&& f) const;
};

/** A bounded face of the street graph: its outline (tag = edge id per side) and its block id. */
struct Face
{
    Polygon outline;
};

/** Every bounded face, dead-end spikes removed. */
std::vector<Face> planarFaces(const StreetGraph& g);

// ---------------------------------------------------------------------------------------------------------------------
// Profiles: what makes a medieval town different from Manhattan

enum class Pattern : uint8_t
{
    Organic,     // roads to the centre from gates and attractors, streets growing off them
    Grid,
    Radial,      // rings and spokes
    Linear,      // one main street through, lots and lanes off it
    OrganicGrid, // an organic core inside a ring road, a grid beyond it
};

const char* patternName(Pattern p);
bool parsePattern(std::string_view name, Pattern* out);

/** Every rule a style sets. Lengths cm, areas m2. Read and written as JSON (field names as here). */
struct Profile
{
    std::string name = "medievalTown";
    Pattern pattern = Pattern::Organic;

    double widthArterial = 900.0, widthCollector = 650.0, widthLocal = 450.0, widthAlley = 250.0;
    int gates = 4;            // arterials leave through this many points on the outline
    double segment = 2500.0;  // growing street segment length
    double turn = 18.0;       // degrees a growing street may bend per segment
    double minAngle = 35.0;   // degrees between streets at a junction
    double spacing = 2200.0;  // streets closer than this side by side are refused
    double maxGrade = 0.12;   // rise over run; steeper streets turn along the slope
    double density = 1.0;     // street length per area, relative to the pattern's own
    double falloff = 1.6;     // how fast streets and heights thin out from the centre
    double maxBlock = 6000.0; // m2: larger blocks are cut by a lane
    bool plaza = true;        // a market square at the centre

    double gridAngle = 0.0;   // degrees
    double gridWidth = 9000.0, gridDepth = 9000.0; // block size along and across the grid, cm
    int arterialEvery = 4;
    double coreRadius = 0.45; // OrganicGrid: core radius as a fraction of the settlement radius
    int rings = 4, spokes = 8; // Radial

    double lotWidth = 700.0, lotDepth = 3000.0; // street frontage and depth
    double minLot = 60.0;                       // m2: smaller pieces become open ground
    double frontSetback = 0.0, sideSetback = 0.0, buildDepth = 1400.0;
    int minFloors = 2, maxFloors = 4;
    double floorHeight = 320.0;
    double foundation = 50.0; // cm below the lowest ground under a footprint
    std::string roof = "gable";
    double commercial = 0.35; // lots on main streets within this fraction of the radius are commercial
};

/** The built-in styles, by name. */
std::vector<std::string> profileNames();
/** A built-in style; false for an unknown name. */
bool builtinProfile(std::string_view name, Profile* out);
json toJson(const Profile& p);
/** Applies the fields present in j over p. Throws std::runtime_error naming an unknown field or a bad value. */
void applyJson(Profile& p, const json& j);
double streetWidth(const Profile& p, StreetClass c);

// ---------------------------------------------------------------------------------------------------------------------
// Layout

/** A river, stream or canal as a centreline. */
struct WaterLine
{
    std::vector<Vec2> pts;
    double width = 2000.0;
};

/** What the user gives: an outline, fixed streets and roads, water, a centre and attractors. */
struct Site
{
    std::vector<Vec2> outline;                 // closed; empty for a square of size
    double size = 40000.0;                     // cm, the square's side when there is no outline
    std::vector<std::vector<Vec2>> streets;    // kept as they are; generators fill around them
    std::vector<std::vector<Vec2>> roads;      // main roads arriving from outside, kept as arterials
    std::vector<WaterLine> water;              // streets other than arterials stop at its banks
    std::vector<Vec2> attractors;              // the first is the centre when hasCenter
    bool hasCenter = false;
};

struct Options
{
    Profile profile;
    uint32_t seed = 1;
    double growth = 1.0;      // 0 to 1: how far the settlement has grown
    double heightScale = 1.0;
    double widthScale = 1.0;
    double lotScale = 1.0;
};

enum class Use : uint8_t
{
    Residential,
    Commercial,
    Civic,
    Plaza,
    Open, // too small or too odd to build on
};

const char* useName(Use u);

struct Block
{
    int id = 0;
    Polygon outline; // inside the street edges; tag = class of the street on that side, -1 none
    double area = 0.0;
    Use use = Use::Residential;
};

struct Lot
{
    int id = 0;
    int block = 0;
    Polygon outline; // tag = class of the street the side faces, -1 none
    double area = 0.0;
    int frontages = 0;
    bool corner = false;
    StreetClass frontClass = StreetClass::Local;
    Use use = Use::Residential;
    float random = 0.0f;
};

/** The space a building may fill on its lot, and what it is for. Buildings themselves are made elsewhere. */
struct Envelope
{
    int lot = 0;
    Polygon footprint; // tag: 0 street side, 1 party wall (shared with a neighbour), 2 open side
    double base = 0.0;   // bottom of the foundation, world Y
    double ground = 0.0; // lowest ground under the footprint
    double top = 0.0;    // highest ground plus height
    double height = 0.0;
    int floors = 0;
    Use use = Use::Residential;
    std::string roof;
    Vec2 front{0.0, 1.0}; // facing the street
    float random = 0.0f;
};

struct Layout
{
    StreetGraph graph;
    Profile profile; // as built, with the options' scales applied
    std::vector<Block> blocks;
    std::vector<Lot> lots;
    std::vector<Envelope> envelopes;
    Polygon outline;
    Vec2 center{0.0};
    double radius = 0.0;
    int refused = 0; // grown streets turned down by the local rules
    std::vector<WaterLine> water;
    std::vector<Vec2> bridges; // where streets cross water
};

/** An edge's width: its own (rivers), else its class's. */
double edgeWidth(const Layout& l, int edge);

/** Streets, blocks, lots and envelopes for a site. Same inputs give the same layout. */
Layout build(const Site& site, const Ground& ground, const Options& o);

// Stages, exposed for tests.
void growOrganic(Layout& l, const Ground& ground, const Options& o, double radiusLimit = 0.0);
void layGrid(Layout& l, const Ground& ground, const Options& o, double innerRadius = 0.0);
void layRadial(Layout& l, const Ground& ground, const Options& o);
void cutLargeBlocks(Layout& l, const Ground& ground, const Options& o);
std::vector<Lot> subdivide(const Block& b, const Options& o, int firstId);
Envelope envelopeFor(const Lot& lot, const Layout& l, const Ground& ground, const Options& o);
/** Least-cost path over the ground between a and b inside the outline, smoothed. Empty when there is none. */
std::vector<Vec2> route(const Layout& l, const Ground& ground, const Profile& p, Vec2 a, Vec2 b);

// ---------------------------------------------------------------------------------------------------------------------
// Measures, checks, output

/** Street measures comparable with real cities (Boeing 2019): orientation entropy and order, degrees, dead ends. */
json metrics(const Layout& l);
/** Problems an agent should fix: disconnected streets, sharp junctions, steep streets, lots without frontage, crossings. */
json checks(const Layout& l, const Ground& ground, const Options& o);
/** Counts and sizes. */
json stats(const Layout& l);
/** The whole layout as data: streets, blocks, lots, envelopes (the schema building generators read). */
json toJson(const Layout& l);

struct MeshParts
{
    geo::Mesh streets;   // ribbons on the ground, face attrs street, class
    geo::Mesh lots;      // lot outlines as loose edges, edge attr lot
    geo::Mesh envelopes; // closed volumes, face attrs lot, use, floors
    geo::Mesh water;     // river ribbons, face attr water
};

struct OutputOptions
{
    double segment = 500.0; // ribbons and outlines are draped at about this spacing
    double lift = 2.0;      // cm above the ground, so streets do not z-fight it
};

MeshParts toMeshes(const Layout& l, const Ground& ground, const OutputOptions& o = {});

/** Street centrelines joined through plain junctions, as polylines with their class. */
struct StreetLine
{
    std::vector<glm::dvec3> pts;
    StreetClass cls = StreetClass::Local;
};
std::vector<StreetLine> streetLines(const Layout& l, const Ground& ground);

// ---------------------------------------------------------------------------------------------------------------------
// Siting and regions: where settlements go, and many of them on one map

/** A candidate site and why it scored: flat ground, water close but out of the flood, confluences. */
struct SiteScore
{
    Vec2 p{0.0};
    double score = 0.0;
    json reasons;
};

struct SiteOptions
{
    int count = 5;
    double spacing = 0.0;  // cm between chosen sites; 0 picks one from the area and count
    double floodMargin = 3000.0; // no centre closer than this beyond a river's bank
    double waterReach = 40000.0; // water further than this adds nothing
    double maxGrade = 0.12;
    uint32_t seed = 1;
};

/** How far q is from the nearest river bank: negative inside the water, very large when there is none. */
double bankDistance(Vec2 q, std::span<const WaterLine> water);

/** Best sites inside the outline, best first, no two closer than the spacing. */
std::vector<SiteScore> suggestSites(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const SiteOptions& o);

enum class Tier : uint8_t
{
    City,
    Town,
    Village,
};

const char* tierName(Tier t);

struct RegionOptions
{
    int cities = 1, towns = 3, villages = 8;
    double cityRadius = 120000.0, townRadius = 45000.0, villageRadius = 15000.0;
    std::string cityProfile = "europeanCity", townProfile = "medievalTown", villageProfile = "hamlet";
    uint32_t seed = 1;
};

struct RegionSettlement
{
    Tier tier = Tier::Village;
    std::string profile;
    Site site;
    double radius = 0.0;
    SiteScore score;
};

/** Settlements placed by tier (central place spacing: the city on the best site, villages between), joined by routed roads. */
struct RegionPlan
{
    std::vector<RegionSettlement> settlements;
    std::vector<std::vector<Vec2>> roads;
    std::vector<Vec2> bridges;
};

RegionPlan planRegion(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const RegionOptions& o);

uint64_t hash64(uint64_t x);
double unitRandom(uint64_t key); // [0, 1)
} // namespace settlement
