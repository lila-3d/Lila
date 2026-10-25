#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

/** Hanging cables: true catenaries between anchor points, bundles of them, and draping over surfaces. */
namespace lila::curve
{
enum class SagMode : uint8_t
{
    Slack, // length is the chord plus a fraction of it
    Depth, // the lowest point hangs this far below the chord
};

/** The closest surface point to a world point and the surface normal there (unit, facing out). */
struct SurfacePoint
{
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
};
using ClosestSurface = std::function<std::optional<SurfacePoint>(const glm::dvec3&)>;

struct CableOptions
{
    SagMode mode = SagMode::Slack;
    /** Slack: extra length as a fraction of the chord (0.1 is 10% longer than straight). */
    double slack = 0.1;
    /** Depth: cm the cable dips below the straight line between its ends, measured along gravity. */
    double sag = 20.0;
    /** Which way is down; need not be unit length. */
    glm::dvec3 gravity{0.0, -1.0, 0.0};
    /** Points along each span before fitting. */
    int samples = 32;
    /** Bezier fitting tolerance, cm. */
    double tolerance = 0.05;

    /** Cables per span; more than one makes a bundle. */
    int count = 1;
    /** Bundle: each cable's ends are moved up to this far (cm) from the anchors, across the span. */
    double spread = 2.0;
    /** Bundle: each cable's slack or sag varies by up to this fraction of it. */
    double sagVariation = 0.15;
    uint32_t seed = 1;

    /** Drape over surfaces: where the hanging cable passes into one, it settles onto it instead. Needs closest. */
    bool drape = false;
    ClosestSurface closest;
    /** Drape: how far the centreline stays off surfaces (the cable's radius), cm. */
    double clearance = 0.5;
    int iterations = 60;
};

/**
 * Points of the catenary hanging from a to b under gravity, samples + 1 of them from a to b, with the given length
 * (at least the chord). A length within a hair of the chord, or ends straight above each other, give a straight line.
 */
LILA_SCENE_API std::vector<glm::dvec3> catenary(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& gravity, double length, int samples);
/** The length whose catenary dips depth cm below the chord (along gravity) at its lowest. */
LILA_SCENE_API double lengthForSag(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& gravity, double depth);
/** The deepest dip below the chord, along gravity, of a polyline from a to b. */
LILA_SCENE_API double sagOf(const std::vector<glm::dvec3>& pts, const glm::dvec3& gravity);

struct CableResult
{
    /** One open Bezier per cable per span, span by span. */
    Splines cables;
    std::vector<std::string> warnings;
};

/** Cables between consecutive anchors (anchor i to i + 1; with closed, the last back to the first). */
LILA_SCENE_API CableResult makeCables(const std::vector<glm::dvec3>& anchors, bool closed, const CableOptions& o);
} // namespace lila::curve
