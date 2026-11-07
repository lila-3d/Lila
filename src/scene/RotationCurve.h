#pragma once

#include "core/Export.h"

#include "scene/AnimCurve.h"
#include "scene/Transform.h"

#include <glm/gtc/quaternion.hpp>

#include <array>
#include <string>
#include <vector>

namespace lila::scene::anim
{
enum class RotInterp : uint8_t
{
    Slerp, // shortest-arc slerp between keys, tangents ignored except step
    Smooth, // cubic in log-quaternion space, tangent types and weights honoured
};

LILA_SCENE_API const char* rotInterpName(RotInterp i);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API RotInterp rotInterpFromName(std::string_view name);

/** Quaternion of an Euler rotation in degrees. */
LILA_SCENE_API glm::dquat eulerToQuat(const glm::dvec3& degrees, RotateOrder order);
/** Rotation vector (axis times angle, radians) of a unit quaternion, angle in [0, pi]. */
LILA_SCENE_API glm::dvec3 quatLog(glm::dquat q);
/** Unit quaternion turning by the rotation vector v (radians). */
LILA_SCENE_API glm::dquat quatExp(const glm::dvec3& v);

struct RotKey
{
    double time = 0.0; // seconds
    glm::dquat q{1.0, 0.0, 0.0, 0.0};
    /** The same rotation as q in the curve's order, degrees: what the user typed, so it also carries extra turns. */
    glm::dvec3 euler{0.0};
    Tangent in = Tangent::Auto;
    Tangent out = Tangent::Auto;
    /** Fixed tangents: angular velocity in the key's own frame, degrees per second. */
    glm::dvec3 inVelocity{0.0};
    glm::dvec3 outVelocity{0.0};
    double inWeight = 1.0; // handle length relative to a third of the segment; only on weighted curves
    double outWeight = 1.0;
    bool breakdown = false;
    /** In and out handles move independently in editors. */
    bool broken = false;

    bool operator==(const RotKey&) const = default;
};

/**
 * One synchronized rotation curve. Keys hold quaternions, so interpolation takes the shortest arc (or the extra
 * turns the keys' Euler values ask for) and never gimbal locks; the output is Euler angles in `order`, chosen
 * continuous with the keys' Euler values so the rebuilt matrix equals the quaternion exactly.
 */
struct LILA_SCENE_API RotationCurve
{
    std::vector<RotKey> keys; // sorted by time
    Infinity pre = Infinity::Constant;
    Infinity post = Infinity::Constant;
    bool weighted = false;
    /** The last key mirrors the first and post infinity cycles; see looped(). */
    bool loop = false;
    RotInterp interp = RotInterp::Smooth;
    RotateOrder order = RotateOrder::XYZ;

    /** A key at time with this Euler rotation (degrees, in order). */
    RotKey makeKey(double time, const glm::dvec3& euler) const;
    glm::dquat evaluateQuat(double t) const;
    /** Euler degrees in order. */
    glm::dvec3 evaluate(double t) const;
    /** Angular velocity (radians per second, key frame) after tangent types, either side of key i. */
    glm::dvec3 inVelocity(size_t i) const;
    glm::dvec3 outVelocity(size_t i) const;
    /** Degrees per second of Euler axis `axis` of the output just before (out = false) or after key i. */
    double eulerSlope(size_t i, int axis, bool out) const;

    int find(double t, double eps = 1e-6) const;
    /** Adds the key, or replaces the rotation of the key already at its time. Returns its index. */
    size_t insert(const RotKey& k);
    void sort();
    /** Sets key i's Euler value and the quaternion with it. */
    void setEuler(size_t i, const glm::dvec3& euler);
    /** Re-expresses every key's Euler value in another order, keeping the rotations. */
    void reorder(RotateOrder to);

    /**
     * The curve as evaluation sees it with loop on: the last key takes the first key's rotation (keeping whole turns)
     * and tangents, the seam's tangents see across the loop, and post infinity cycles. Unchanged when loop is off.
     */
    RotationCurve looped() const;

    json keysToJson() const;
    /** Throws std::invalid_argument on malformed input. Keys without a quaternion get one from their Euler value. */
    static std::vector<RotKey> keysFromJson(const json& j, RotateOrder order);

    bool operator==(const RotationCurve&) const = default;

private:
    /** Rotation vector from key i to key i + 1, the same in either key's frame. */
    glm::dvec3 segment(size_t i) const;
    glm::dquat evalSegment(size_t i, double t) const;
    glm::dquat evalInside(double t) const;
    glm::dvec3 referenceEuler(double t) const;
    /** Rotation vector from the first key to the last, in the first key's frame, with every turn the keys make. */
    glm::dvec3 segmentTotal() const;
    glm::dvec3 velocity(size_t i, bool out) const;
    bool neighbour(size_t i, bool next, glm::dvec3& r, double& dt) const;

    /** Set on looped() copies: the end keys' tangents treat the other end as their neighbour. */
    bool mCyclic = false;
};

/** animCurveRQ: a rotation curve driving a whole rotate plug. Keys are a JSON string like the scalar curves'. */
class LILA_SCENE_API RotationCurveNode : public Node
{
public:
    enum Attr
    {
        kKeys,
        kPreInfinity,
        kPostInfinity,
        kWeighted,
        kLoop,
        kRotateOrder,
        kInterpolation,
        kOutput,
        kMute,
        kMuteTime,
        kModifiers,
        kCount
    };

    void compute(const EvalContext& ctx) override;

private:
    std::string mCachedText;
    RotationCurve mCached;
    std::string mModifiersText;
    std::vector<Modifier> mModifiers;
};

LILA_SCENE_API bool isRotationCurve(const Node& n);
/** Scalar or rotation curve. */
LILA_SCENE_API bool isAnyCurve(const Node& n);
/** Malformed keys read as none. */
LILA_SCENE_API RotationCurve rotationCurveOf(const Node& n);
/** The rotation half of setPreview (AnimCurve.h): the node evaluates c until clearPreviews. */
LILA_SCENE_API void setPreview(Graph& g, NodeId curve, const RotationCurve& c);
void clearRotationPreviews(Graph& g);
const RotationCurve* rotationPreviewOf(NodeId curve);

/** Writes c into an animCurveRQ through the graph (undoable). */
LILA_SCENE_API void setRotationCurve(Graph& g, NodeId curveNode, const RotationCurve& c);

/**
 * A rotation curve from Euler curves (null for an unanimated axis, which holds rest): a key at every time any of them
 * has one, posed exactly as the Euler curves pose it there. Tangent types and infinity come from the curves.
 */
LILA_SCENE_API RotationCurve rotationFromEuler(const std::array<const Curve*, 3>& xyz, const glm::dvec3& rest, RotateOrder order,
                                               RotInterp interp);
/**
 * Euler curves keyed at times (the rotation curve's key times when empty), each sample continuous with the one
 * before. Keys keep the rotation curve's tangent types where one sits; others are linear.
 */
LILA_SCENE_API std::array<Curve, 3> eulerFromRotation(const RotationCurve& rc, std::vector<double> times = {});
/**
 * Euler curves that reproduce rc within toleranceDegrees at every one of frames: a key at each of rc's keys with fixed
 * tangents matching its real slopes (exact for single-axis motion), plus keys at the frames where that is not close
 * enough. For writing to formats that only have Euler curves.
 */
LILA_SCENE_API std::array<Curve, 3> eulerKeysFor(const RotationCurve& rc, const std::vector<double>& frames, double toleranceDegrees = 0.01);

/**
 * Euler filter: rewrites the keys of three Euler curves so each key's rotation is unchanged but its angles are the
 * solution closest to the previous key's, removing flips and 360 jumps. Times where an axis has no key only unwrap
 * that axis. Returns the number of keys changed.
 */
LILA_SCENE_API int eulerFilter(std::array<Curve*, 3> xyz, const glm::dvec3& rest, RotateOrder order);

void registerRotationCurveType();
} // namespace lila::scene::anim
