#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <string>
#include <vector>

namespace lila::scene::anim
{
enum class Tangent : uint8_t
{
    Auto, // Maya's auto: spline, flattened at extremes and clamped so it never overshoots its neighbours
    Spline,
    Linear,
    Flat,
    Step,
    Clamped, // spline, but flat where a neighbour has the same value
    Fixed, // uses the stored angle
};

enum class Infinity : uint8_t
{
    Constant,
    Linear,
    Cycle,
    CycleRelative, // cycle with offset
    Oscillate,
};

LILA_SCENE_API const char* tangentName(Tangent t);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API Tangent tangentFromName(std::string_view name);
LILA_SCENE_API const char* infinityName(Infinity i);
LILA_SCENE_API Infinity infinityFromName(std::string_view name);

struct Key
{
    double time = 0.0; // seconds
    double value = 0.0;
    Tangent in = Tangent::Auto;
    Tangent out = Tangent::Auto;
    double inAngle = 0.0; // degrees in (seconds, value) space; used by Fixed
    double outAngle = 0.0;
    double inWeight = 1.0; // handle length relative to a third of the segment; only on weighted curves
    double outWeight = 1.0;
    /** A registered Interpolator (AnimInterpolator.h) for the segment after this key; empty uses the tangents. */
    std::string interpolator;
    /** A breakdown key (Maya's): editors mark it apart from the others. */
    bool breakdown = false;

    bool operator==(const Key&) const = default;
};

/**
 * One animation curve. Segments are cubic Beziers between keys; unweighted curves keep the handles at a third of
 * each segment, which is exactly Hermite interpolation.
 */
struct LILA_SCENE_API Curve
{
    std::vector<Key> keys; // sorted by time
    Infinity pre = Infinity::Constant;
    Infinity post = Infinity::Constant;
    bool weighted = false;
    /** The last key mirrors the first and post infinity cycles; see looped(). */
    bool loop = false;

    double evaluate(double t) const;
    /** Slopes (value per second) actually used on either side of key i, after the tangent type is applied. */
    double inSlope(size_t i) const;
    double outSlope(size_t i) const;

    /** Index of the key at t (within eps), or -1. */
    int find(double t, double eps = 1e-6) const;
    /** Adds or replaces the key at its time, keeping keys sorted. Returns its index. */
    size_t insert(const Key& k);
    void sort();

    /**
     * The curve as evaluation sees it with loop on: the last key takes the first key's value and tangents, the seam's
     * spline tangents see across the loop (as fixed angles), and post infinity cycles. Unchanged when loop is off.
     */
    Curve looped() const;

    /** Keys only; infinity and weighting live in their own attributes. */
    json keysToJson() const;
    /** Throws std::invalid_argument on malformed input. */
    static std::vector<Key> keysFromJson(const json& j);

    bool operator==(const Curve&) const = default;

private:
    double autoSlope(size_t i, bool clampOvershoot) const;
    double evalSegment(size_t i, double t) const;
};

/**
 * animCurveTL / TA / TU. The keys are a JSON string attribute so files and undo need nothing special; the parsed
 * curve is cached on the node and rebuilt only when the string changes.
 */
class LILA_SCENE_API CurveNode : public Node
{
public:
    enum Attr
    {
        kKeys,
        kPreInfinity,
        kPostInfinity,
        kWeighted,
        kOutput,
        kLoop,
        kMute,
        kMuteTime,
        kCount
    };

    void compute(const EvalContext& ctx) override;

private:
    std::string mCachedText;
    Curve mCached;
};

/** Reads a curve node's keys and settings. Malformed keys read as none. */
LILA_SCENE_API Curve curveOf(const Node& n);
/** Writes c into a curve node through the graph (undoable). */
LILA_SCENE_API void setCurve(Graph& g, NodeId curveNode, const Curve& c);

LILA_SCENE_API bool isCurve(const Node& n);

LILA_SCENE_API void registerTypes();
} // namespace lila::scene::anim
