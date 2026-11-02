#include "rigmath/Builder.h"

#include <algorithm>

namespace lila::rigmath
{
namespace vectorfns
{
using V = math::Vector;

D3 safeNormal(const D3& v, double tol = math::kSmallNumber) { return V(v).getSafeNormal(tol).toGlm(); }

D3 closestOnLine(const D3& p, const D3& origin, const D3& dir)
{
    const D3 d = safeNormal(dir);
    return origin + d * glm::dot(p - origin, d);
}

D3 closestOnSegment(const D3& p, const D3& a, const D3& b)
{
    const D3 ab = b - a;
    const double l = glm::dot(ab, ab);
    if (l <= math::kSmallNumber)
        return a;
    return a + ab * std::clamp(glm::dot(p - a, ab) / l, 0.0, 1.0);
}
} // namespace vectorfns

void addVector(Library& lib)
{
    using namespace vectorfns;

    over(lib.fn("makeVector", "vector", "A vector from its components."), "x y z", "result", [](double x, double y, double z) { return D3(x, y, z); });
    over(lib.fn("breakVector", "vector", "A vector's components."), "inVec", "x y z", [](D3 v) { return std::tuple{v.x, v.y, v.z}; });
    over(lib.fn("dot", "vector", "A . B."), "a b", "result", [](D3 a, D3 b) { return glm::dot(a, b); });
    over(lib.fn("cross", "vector", "A x B (right-handed)."), "a b", "result", [](D3 a, D3 b) { return glm::cross(a, b); });
    over(lib.fn("normal", "vector", "A scaled to length 1; zero when it is shorter than tolerance."), "a tolerance=0.0001", "result",
         [](D3 a, double t) { return safeNormal(a, t * t); });
    over(lib.fn("vSize", "vector", "The length of A."), "a", "result", [](D3 a) { return glm::length(a); });
    over(lib.fn("vSizeSquared", "vector", "The squared length of A."), "a", "result", [](D3 a) { return glm::dot(a, a); });
    over(lib.fn("distance", "vector", "The distance between two points."), "v1 v2", "result", [](D3 a, D3 b) { return glm::distance(a, b); });
    over(lib.fn("distanceSquared", "vector", "The squared distance between two points."), "v1 v2", "result", [](D3 a, D3 b) {
        const D3 d = a - b;
        return glm::dot(d, d);
    });
    over(lib.fn("getDirectionUnitVector", "vector", "The unit vector from one point to another; zero when they coincide."), "from to", "result",
         [](D3 a, D3 b) { return safeNormal(b - a); });
    over(lib.fn("projectVectorOnToVector", "vector", "The part of V along target."), "v target=1,0,0", "result",
         [](D3 v, D3 t) { return V(v).projectOnTo(t).toGlm(); });
    over(lib.fn("projectVectorOnToPlane", "vector", "V with its part along the plane normal removed."), "v planeNormal=0,1,0", "result",
         [](D3 v, D3 n) { return V(v).projectOnPlane(safeNormal(n)).toGlm(); });
    over(lib.fn("projectPointOnToPlane", "vector", "The point on the plane through planeBase nearest to point."), "point planeBase planeNormal=0,1,0",
         "result", [](D3 p, D3 base, D3 n) {
             const D3 u = safeNormal(n);
             return p - u * glm::dot(p - base, u);
         });
    over(lib.fn("getReflectionVector", "vector", "Direction reflected off a surface with the given normal."), "direction surfaceNormal=0,1,0", "result",
         [](D3 d, D3 n) { return V(d).mirrorByVector(safeNormal(n)).toGlm(); });
    over(lib.fn("rotateAngleAxis", "vector", "V turned angleDeg degrees about axis (right-hand rule)."), "inVect angleDeg axis=0,1,0", "result",
         [](D3 v, double a, D3 axis) { return glm::length(axis) < math::kSmallNumber ? v : V(v).rotateAngleAxis(a, axis).toGlm(); });
    over(lib.fn("angleBetween", "vector", "The angle in degrees between two directions, 0..180; 0 when either is zero."), "a b", "result", [](D3 a, D3 b) {
        if (glm::length(a) < math::kSmallNumber || glm::length(b) < math::kSmallNumber)
            return 0.0;
        return math::radiansToDegrees(std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b)));
    });
    over(lib.fn("signedAngleBetween", "vector", "The angle in degrees turning A onto B about axis, -180..180."), "a b axis=0,1,0", "result",
         [](D3 a, D3 b, D3 axis) {
             const D3 n = safeNormal(axis);
             const D3 pa = a - n * glm::dot(a, n), pb = b - n * glm::dot(b, n);
             return math::radiansToDegrees(std::atan2(glm::dot(glm::cross(pa, pb), n), glm::dot(pa, pb)));
         });
    over(lib.fn("clampVectorSize", "vector", "A with its length held within min..max."), "a min max=1", "result",
         [](D3 a, double lo, double hi) { return V(a).clampSize(lo, hi).toGlm(); });
    over(lib.fn("getMaxElement", "vector", "The largest component."), "a", "result", [](D3 a) { return std::max({a.x, a.y, a.z}); });
    over(lib.fn("getMinElement", "vector", "The smallest component."), "a", "result", [](D3 a) { return std::min({a.x, a.y, a.z}); });
    over(lib.fn("findClosestPointOnLine", "vector", "The point on the infinite line nearest to point."), "point lineOrigin lineDirection=1,0,0", "result",
         [](D3 p, D3 o, D3 d) { return closestOnLine(p, o, d); });
    over(lib.fn("findClosestPointOnSegment", "vector", "The point on the segment nearest to point."), "point segmentStart segmentEnd=1,0,0", "result",
         [](D3 p, D3 a, D3 b) { return closestOnSegment(p, a, b); });
    over(lib.fn("getPointDistanceToLine", "vector", "The distance from point to the infinite line."), "point lineOrigin lineDirection=1,0,0", "result",
         [](D3 p, D3 o, D3 d) { return glm::distance(p, closestOnLine(p, o, d)); });
    over(lib.fn("getPointDistanceToSegment", "vector", "The distance from point to the segment."), "point segmentStart segmentEnd=1,0,0", "result",
         [](D3 p, D3 a, D3 b) { return glm::distance(p, closestOnSegment(p, a, b)); });
    over(lib.fn("vlerp", "interpolation", "Vector A + alpha * (B - A)."), "a b=1,1,1 alpha=0.5", "result", [](D3 a, D3 b, double t) { return glm::mix(a, b, t); });
    over(lib.fn("vinterpTo", "interpolation", "One step of easing a vector toward target; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result",
         [](D3 c, D3 t, double dt, double s) { return math::vinterpTo(V(c), V(t), dt, s).toGlm(); });
    over(lib.fn("vinterpToConstant", "interpolation", "One step of moving a vector toward target at interpSpeed units a second."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result",
         [](D3 c, D3 t, double dt, double s) { return math::vinterpToConstant(V(c), V(t), dt, s).toGlm(); });

    // Colour
    over(lib.fn("linearColorLerp", "color", "Colour A + alpha * (B - A), in linear RGB."), "a b=1,1,1 alpha=0.5", "result",
         [](Col a, Col b, double t) { return Col{glm::mix(a.rgb, b.rgb, t)}; });
    over(lib.fn("linearColorLerpUsingHSV", "color", "Colour A to B by alpha through HSV, the hue the short way round."), "a b=1,1,1 alpha=0.5", "result",
         [](Col a, Col b, double t) {
             math::LinearColor ha = math::LinearColor(V(a.rgb)).linearRGBToHSV(), hb = math::LinearColor(V(b.rgb)).linearRGBToHSV();
             const double dh = math::normalizeAxis(double(hb.r) - double(ha.r));
             math::LinearColor h(float(math::clampAxis(ha.r + dh * t)), float(math::lerp(ha.g, hb.g, t)), float(math::lerp(ha.b, hb.b, t)));
             return Col{h.hsvToLinearRGB().toVector().toGlm()};
         });
    over(lib.fn("rgbToHSV", "color", "Linear RGB to hue (degrees), saturation and value."), "inColor", "h s v", [](Col c) {
        math::LinearColor h = math::LinearColor(V(c.rgb)).linearRGBToHSV();
        return std::tuple{double(h.r), double(h.g), double(h.b)};
    });
    over(lib.fn("hsvToRGB", "color", "Hue (degrees), saturation and value to linear RGB."), "h s=1 v=1", "result", [](double h, double s, double v) {
        return Col{math::LinearColor(float(h), float(s), float(v)).hsvToLinearRGB().toVector().toGlm()};
    });
    over(lib.fn("linearColorDesaturated", "color", "The colour moved toward its luminance grey by desaturation (0..1)."), "inColor desaturation=1",
         "result", [](Col c, double d) { return Col{math::LinearColor(V(c.rgb)).desaturate(float(d)).toVector().toGlm()}; });
    over(lib.fn("linearColorGetLuminance", "color", "Rec. 709 luminance of a linear colour."), "inColor", "result",
         [](Col c) { return double(math::LinearColor(V(c.rgb)).getLuminance()); });
    over(lib.fn("cinterpTo", "interpolation", "One step of easing a colour toward target; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result", [](Col c, Col t, double dt, double s) {
             return Col{math::cinterpTo(math::LinearColor(V(c.rgb)), math::LinearColor(V(t.rgb)), dt, s).toVector().toGlm()};
         });
}
} // namespace lila::rigmath
