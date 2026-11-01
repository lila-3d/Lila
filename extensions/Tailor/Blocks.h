#pragma once

// Parametric starter garments ("blocks"). Each generator returns panels placed just outside the body
// with their seams wired; after that they are ordinary panels.
//
// Placement convention: a panel's wrap cylinder axis lies behind it, at origin - (right x up) * wrapRadius,
// so a panel wraps around the body rather than away from it.

#include "TailorCore.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tailor
{

/** Body measurements in metres. Origin at the feet, +Y up, +Z forward, the body centred on the Y axis. */
struct Body
{
    float height = 1.8f;
    float chest = 0.96f;           // circumferences
    float waist = 0.82f;
    float hip = 0.98f;
    float shoulderWidth = 0.46f;   // shoulder point to shoulder point
    float armLength = 0.62f;       // shoulder point to wrist
    float inseam = 0.82f;          // floor to crotch
    float neck = 0.38f;
    float head = 0.57f;
    float armAngle = 0.785f;       // radians below horizontal: 0 is a T-pose, the default is an A-pose

    float neckY() const { return height * 0.84f; }
    float shoulderY() const { return height * 0.815f; }
    float chestY() const { return height * 0.72f; }
    float waistY() const { return height * 0.6f; }
    float hipY() const { return height * 0.52f; }
    float crotchY() const { return inseam; }
    float chestRadius() const;     // radius of a cylinder with the chest circumference
};

/** A named point on a garment (a buckle, a clasp), in pattern space on one panel and in world space. */
struct BlockAnchor
{
    std::string name;
    int panel = -1;
    Vec2 pattern{0.f, 0.f};
    Vec3 position{0.f, 0.f, 0.f};
};

// Lengths in metres, ease as a fraction of the body measurement.

/** Shirt-like tops: tee, tunic, robe. `length` runs from the shoulder point down. */
struct TopParams
{
    float length = 0.7f;
    float ease = 0.12f;
    float sleeveLength = 0.22f;    // 0 = sleeveless
    float sleeveEase = 0.3f;
    float sleeveFlare = 0.f;       // extra cuff width as a fraction
    float hemFlare = 0.f;          // extra hem width per side
    float slit = 0.f;              // side slit height from the hem
    float neckDepth = 0.08f;       // front neck depth; the V depth when the front is open
    bool openFront = false;
    float overlap = 0.f;           // open front: how far each front panel crosses the centre line
    int sleeveStyle = 0;           // SleeveStyle
    float dropShoulder = 0.f;      // shoulder line carried past the shoulder point; drop-shoulder style defaults it
};

/** The sleeve families of a pattern maker's chart. Each sets cap height, cap ease, wrist width, cuff and default length. */
enum class SleeveStyle : int
{
    SetIn,       // T-shirt: plain set-in, a little cap ease
    Cuffed,      // shirt: tapered into a cuff
    Bishop,      // full lower sleeve gathered into a cuff
    Bell,        // flares from the elbow
    Puff,        // short, gathered at cap and band
    Cap,         // a few cm over the shoulder
    Flutter,     // short, flared hem
    LegOfMutton, // big gathered cap, narrow wrist
    Poet,        // full, gathered at cap and cuff
    DropShoulder, // shoulder carried onto the arm, low flat cap
    Batwing,     // cut in one with the body, deep underarm
    Count
};
const char* sleeveStyleName(SleeveStyle s);

struct TeeParams : TopParams {};

struct TunicParams : TopParams
{
    TunicParams()
    {
        length = 0.95f;
        ease = 0.18f;
        sleeveLength = 0.55f;
        sleeveEase = 0.35f;
        sleeveFlare = 0.15f;
        hemFlare = 0.08f;
        slit = 0.2f;
        neckDepth = 0.1f;
    }
};

struct RobeParams : TopParams
{
    RobeParams()
    {
        length = 1.35f;
        ease = 0.25f;
        sleeveLength = 0.6f;
        sleeveEase = 0.6f;
        sleeveFlare = 0.6f;
        hemFlare = 0.18f;
        neckDepth = 0.35f;
        openFront = true;
        overlap = 0.08f;
    }
};

// Tops cut for one sleeve style each: the templates a pattern maker would start from.

struct DropShoulderTopParams : TopParams
{
    DropShoulderTopParams()
    {
        length = 0.68f;
        ease = 0.22f;
        sleeveLength = 0.3f;
        sleeveEase = 0.45f;
        sleeveStyle = int(SleeveStyle::DropShoulder);
        dropShoulder = 0.09f;
        neckDepth = 0.07f;
    }
};

struct ShirtParams : TopParams
{
    ShirtParams()
    {
        length = 0.78f;
        ease = 0.14f;
        sleeveLength = 0.62f;
        sleeveEase = 0.3f;
        sleeveStyle = int(SleeveStyle::Cuffed);
        neckDepth = 0.06f;
        openFront = true;
        overlap = 0.02f;
    }
};

struct PeasantBlouseParams : TopParams
{
    PeasantBlouseParams()
    {
        length = 0.66f;
        ease = 0.25f;
        sleeveLength = 0.6f;
        sleeveEase = 0.4f;
        sleeveStyle = int(SleeveStyle::Bishop);
        neckDepth = 0.12f;
    }
};

struct PuffBlouseParams : TopParams
{
    PuffBlouseParams()
    {
        length = 0.6f;
        ease = 0.12f;
        sleeveLength = 0.18f;
        sleeveEase = 0.3f;
        sleeveStyle = int(SleeveStyle::Puff);
        neckDepth = 0.1f;
    }
};

struct PoetShirtParams : TopParams
{
    PoetShirtParams()
    {
        length = 0.8f;
        ease = 0.22f;
        sleeveLength = 0.62f;
        sleeveEase = 0.4f;
        sleeveStyle = int(SleeveStyle::Poet);
        neckDepth = 0.14f;
    }
};

struct BellSleeveDressParams : TopParams
{
    BellSleeveDressParams()
    {
        length = 1.15f;
        ease = 0.14f;
        sleeveLength = 0.6f;
        sleeveEase = 0.3f;
        sleeveStyle = int(SleeveStyle::Bell);
        hemFlare = 0.12f;
        neckDepth = 0.09f;
    }
};

struct FlutterTopParams : TopParams
{
    FlutterTopParams()
    {
        length = 0.62f;
        ease = 0.12f;
        sleeveLength = 0.16f;
        sleeveEase = 0.3f;
        sleeveStyle = int(SleeveStyle::Flutter);
        neckDepth = 0.12f;
    }
};

struct CapSleeveTopParams : TopParams
{
    CapSleeveTopParams()
    {
        length = 0.6f;
        ease = 0.08f;
        sleeveLength = 0.07f;
        sleeveEase = 0.25f;
        sleeveStyle = int(SleeveStyle::Cap);
        neckDepth = 0.08f;
    }
};

struct BatwingTopParams : TopParams
{
    BatwingTopParams()
    {
        length = 0.65f;
        ease = 0.25f;
        sleeveLength = 0.5f;
        sleeveEase = 0.4f;
        sleeveStyle = int(SleeveStyle::Batwing);
        neckDepth = 0.1f;
    }
};

struct LegOfMuttonParams : TopParams
{
    LegOfMuttonParams()
    {
        length = 0.62f;
        ease = 0.1f;
        sleeveLength = 0.6f;
        sleeveEase = 0.3f;
        sleeveStyle = int(SleeveStyle::LegOfMutton);
        neckDepth = 0.08f;
    }
};

/** `length` runs from the waist down. */
struct TrousersParams
{
    float length = 1.04f;
    float ease = 0.1f;
    float hemCircumference = 0.42f;
    float backRise = 0.03f;
};

struct SkirtParams
{
    float length = 0.6f;
    float ease = 0.06f;
    float flare = 0.12f;           // extra hem width per side
};

/** Two side panels with a centre seam over the crown. `depth` is front to back, `height` neck to crown. */
struct HoodParams
{
    float depth = 0.32f;
    float height = 0.4f;
};

/** Cloaks and capes hang from the shoulders. `length` runs from the neck point down. */
struct DrapeParams
{
    float length = 1.3f;
    float flare = 0.45f;           // extra hem width per side beyond the shoulders
    float frontCoverage = 1.f;     // 1 = fronts meet at the centre line, 0 = shoulder straps only
    float overlap = 0.04f;
    bool closedSides = false;      // sew the sides to the hem; open sides let A-pose arms through
    bool hood = true;
    float hoodHeight = 0.4f;
    float clearance = 0.f;         // minimum distance from the body axis to the panels; 0 = auto
};

struct CloakParams : DrapeParams {};

struct CapeParams : DrapeParams
{
    CapeParams()
    {
        length = 0.7f;
        flare = 0.25f;
        frontCoverage = 0.3f;
        hood = false;
    }
};

/** Poncho and tabard: front and back panels joined at the shoulders. */
struct OverShoulderParams
{
    float width = 1.1f;
    float length = 0.65f;          // from the shoulder down
    float neckWidth = 0.26f;
    float sideTie = 0.f;           // length of a seam joining the sides at the waist; 0 = open
};

struct PonchoParams : OverShoulderParams {};

struct TabardParams : OverShoulderParams
{
    TabardParams()
    {
        width = 0.5f;
        length = 0.95f;
        neckWidth = 0.24f;
        sideTie = 0.08f;
    }
};

/** A strap around the waist, ends sewn at the front. Placed loose at `fitRadius` and cinched by fabric shrink. */
struct BeltParams
{
    float height = 0.f;            // belt centre relative to the waist line; negative sits on the hips
    float width = 0.05f;
    float ease = 0.04f;
    float fitRadius = 0.f;         // placement radius; 0 = just outside the waist
    bool buckle = true;            // report a "buckle" anchor at the front
};

struct SackParams
{
    float width = 0.5f;
    float height = 0.7f;
};

/** `vertical` hangs it from the top edge (banner), otherwise it flies from the left edge (flag). */
struct BannerParams
{
    float width = 0.6f;
    float height = 1.2f;
    bool vertical = true;
    float tail = 0.2f;             // swallowtail notch depth; 0 = straight
};

struct TentParams
{
    float radius = 1.1f;
    float height = 1.7f;
    int sides = 6;
    bool door = true;              // leave one seam open
};

/** The promo set: tunic (layer 0), belt (layer 1), hooded cloak (layer 2). */
struct OutfitParams
{
    float tunicLength = 0.95f;
    float cloakLength = 1.3f;
    float beltEase = 0.04f;
    bool hood = true;
};

Garment tee(const Body& body, const TeeParams& p = {});
Garment tunic(const Body& body, const TunicParams& p = {});
Garment robe(const Body& body, const RobeParams& p = {});
/** Any top: tee, tunic, robe and the sleeve-style templates all draft through it. */
Garment top(const Body& body, const TopParams& p);
Garment top(const Body& body, const TopParams& p);
Garment trousers(const Body& body, const TrousersParams& p = {});
Garment skirt(const Body& body, const SkirtParams& p = {});
Garment hood(const Body& body, const HoodParams& p = {});
Garment cloak(const Body& body, const CloakParams& p = {});
Garment cape(const Body& body, const CapeParams& p = {});
Garment drape(const Body& body, const DrapeParams& p);
Garment poncho(const Body& body, const PonchoParams& p = {});
Garment tabard(const Body& body, const TabardParams& p = {});
Garment overShoulder(const Body& body, const OverShoulderParams& p);
Garment belt(const Body& body, const BeltParams& p = {}, std::vector<BlockAnchor>* anchors = nullptr);
Garment sack(const Body& body, const SackParams& p = {});
Garment banner(const Body& body, const BannerParams& p = {});
Garment tent(const Body& body, const TentParams& p = {});
Garment cloakTunicBelt(const Body& body, const OutfitParams& p = {}, std::vector<BlockAnchor>* anchors = nullptr);

/** Appends `src` to `dst`, offsetting panel, seam and fabric indices; returns the first new panel index. */
int appendGarment(Garment& dst, const Garment& src, int layer);

// Registry: lets commands and UI enumerate blocks and their parameters. Bools are 0/1, ints are rounded.

using BlockParamValues = std::map<std::string, float>;

struct BlockParam
{
    enum class Kind : uint8_t { Float, Int, Bool };
    std::string name;
    float defaultValue = 0.f;
    float min = 0.f;
    float max = 1.f;
    Kind kind = Kind::Float;
};

struct Block
{
    std::string id;
    std::string label;
    std::vector<BlockParam> params;
    std::function<Garment(const Body&, const BlockParamValues&, std::vector<BlockAnchor>*)> build;
};

const std::vector<Block>& blocks();
const Block* findBlock(const std::string& id);
std::vector<std::string> blockNames();

/** A smooth stand-in body with `body`'s measurements: feet at y = 0, facing +Z, metres, closed and wound outward.
 *  Meshed at `cell` spacing; the blocks are placed around exactly this shape. */
void mannequin(const Body& body, std::vector<Vec3>& points, std::vector<std::array<int, 3>>& tris, float cell = 0.012f);

/** Measures a standing body mesh (metres, +Y up, facing +Z): height from its bounds; chest, waist, hip, neck and head
 *  girths as a tape would read them around horizontal sections; other lengths scaled by height. `offset` gets the
 *  centre of its feet, where garments built for the result go. */
Body measureBody(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, Vec3* offset = nullptr);

/** Something a pattern maker would flag, with what to do about it. */
struct PatternIssue
{
    enum Level
    {
        Info,
        Warning,
        Error
    };
    Level level = Warning;
    int panel = -1; // the panel it is about, or -1
    int seam = -1;  // the seam it is about, or -1
    std::string message;
    std::string tip;
};

/**
 * Checks a pattern: outlines, panel sizes against the cloth resolution, grain, seams that don't match (with sleeve cap
 * ease judged as a pattern maker would), dropped-shoulder sleeves on an undropped shoulder, and, given the body, chest ease.
 */
std::vector<PatternIssue> checkPattern(const Garment& g, const Body* body = nullptr);

/** Builds a block by id. Unknown names in `values` are ignored, values are clamped to the parameter range. */
bool buildBlock(const std::string& id, Garment& out, std::string* error = nullptr, const Body& body = {},
                const BlockParamValues& values = {}, std::vector<BlockAnchor>* anchors = nullptr);

} // namespace tailor
