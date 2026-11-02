#include "rigmath/Builder.h"

#include <array>

namespace lila::rigmath
{
namespace rotationfns
{
using V = math::Vector;

D3 unit(const D3& v) { return V(v).getSafeNormal().toGlm(); }

/** Any unit vector perpendicular to a unit v. */
D3 anyPerpendicular(const D3& v)
{
    const D3 hint = std::abs(v.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
    return glm::normalize(glm::cross(v, hint));
}

/** A rotation whose local axis `primary` points along a and whose axis `secondary` leans toward b. */
M3 fromTwoAxes(int primary, const D3& a, int secondary, const D3& b)
{
    const D3 p = glm::length(a) > math::kSmallNumber ? glm::normalize(a) : M3(1.0)[primary];
    D3 s = b - p * glm::dot(b, p);
    s = glm::length(s) > 1e-9 ? glm::normalize(s) : anyPerpendicular(p);
    const int third = 3 - primary - secondary;
    M3 m;
    m[primary] = p;
    m[secondary] = s;
    m[third] = glm::cross(m[(third + 1) % 3], m[(third + 2) % 3]);
    return m;
}

/** The smallest turn taking local axis `axis` onto direction d. */
M3 fromOneAxis(int axis, const D3& d)
{
    if (glm::length(d) < math::kSmallNumber)
        return M3(1.0);
    return Quat::findBetweenNormals(V(M3(1.0)[axis]), V(glm::normalize(d))).toGlmMat3();
}

D3 normalizedDelta(const D3& a, const D3& b)
{
    return {math::normalizeAxis(a.x - b.x), math::normalizeAxis(a.y - b.y), math::normalizeAxis(a.z - b.z)};
}

D3 normalizedRot(const D3& r) { return {math::normalizeAxis(r.x), math::normalizeAxis(r.y), math::normalizeAxis(r.z)}; }

/** q split into a swing and a twist about the unit axis: q = swing * twist. */
std::pair<Quat, Quat> swingTwist(const Quat& q, const D3& axis)
{
    const D3 a = unit(axis);
    const D3 v(q.x, q.y, q.z);
    const D3 p = a * glm::dot(v, a);
    Quat twist(p.x, p.y, p.z, q.w);
    if (twist.sizeSquared() < 1e-12)
        twist = Quat();
    twist.normalize();
    return {q * twist.inverse(), twist};
}
} // namespace rotationfns

void addRotation(Library& lib)
{
    using namespace rotationfns;
    const char* axisNames = "XYZ";

    // Rotators
    over(lib.fn("makeRotator", "rotator", "A rotator from Euler angles in degrees, read in rotateOrder."), "x y z", "result",
         [](double x, double y, double z) { return Rot{D3(x, y, z)}; });
    over(lib.fn("breakRotator", "rotator", "A rotator's Euler angles in degrees."), "inRot", "x y z", [](Rot r) { return std::tuple{r.deg.x, r.deg.y, r.deg.z}; });
    over(lib.fn("composeRotators", "rotator", "Rotation A, then B."), "a b", "result", [](Rot a, Rot b) { return Rot::of(b.mat() * a.mat()); });
    over(lib.fn("invertRotator", "rotator", "The rotation that undoes A."), "a", "result", [](Rot a) { return Rot::of(glm::transpose(a.mat())); });
    over(lib.fn("normalizedDeltaRotator", "rotator", "A - B per axis, each wrapped into (-180, 180]."), "a b", "result",
         [](Rot a, Rot b) { return Rot{normalizedDelta(a.deg, b.deg)}; });
    over(lib.fn("rlerp", "interpolation", "Rotator A to B by alpha: along the shortest arc (slerp), or per Euler angle when shortestPath is off."),
         "a b alpha=0.5 shortestPath=true", "result", [](Rot a, Rot b, double t, bool shortest) {
             if (shortest)
                 return Rot::of(Quat::slerp(a.quat(), b.quat(), t));
             return Rot{a.deg + (b.deg - a.deg) * t};
         });
    over(lib.fn("rinterpTo", "interpolation", "One step of easing a rotator toward target per axis, the short way; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result", [](Rot c, Rot t, double dt, double s) {
             if (s <= 0.0)
                 return t;
             const D3 d = normalizedDelta(t.deg, c.deg);
             if (glm::length(d) < math::kKindaSmallNumber)
                 return t;
             return Rot{normalizedRot(c.deg + d * std::clamp(dt * s, 0.0, 1.0))};
         });
    over(lib.fn("rinterpToConstant", "interpolation", "One step of turning a rotator toward target at interpSpeed degrees a second per axis."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result", [](Rot c, Rot t, double dt, double s) {
             if (s <= 0.0)
                 return t;
             const double step = dt * s;
             const D3 d = glm::clamp(normalizedDelta(t.deg, c.deg), D3(-step), D3(step));
             return Rot{normalizedRot(c.deg + d)};
         });
    over(lib.fn("rotateVector", "rotator", "Vector A turned by rotation B."), "a b", "result", [](D3 a, Rot b) { return b.mat() * a; });
    over(lib.fn("unrotateVector", "rotator", "Vector A turned back by rotation B."), "a b", "result", [](D3 a, Rot b) { return glm::transpose(b.mat()) * a; });
    over(lib.fn("getAxes", "rotator", "The rotation's local X, Y and Z axes."), "a", "x y z", [](Rot a) {
        const M3 m = a.mat();
        return std::tuple{m[0], m[1], m[2]};
    });
    over(lib.fn("rotatorFromAxisAndAngle", "rotator", "angle degrees about axis (right-hand rule)."), "axis=0,1,0 angle", "result", [](D3 axis, double a) {
        return glm::length(axis) < math::kSmallNumber ? Rot{} : Rot::of(Quat(V(unit(axis)), math::degreesToRadians(a)));
    });
    for (int p = 0; p < 3; ++p)
    {
        const std::string pn(1, axisNames[p]);
        over(lib.fn("makeRotFrom" + pn, "rotator", "The smallest rotation taking the local " + pn + " axis onto the direction."),
             std::string(1, char('x' + p)) + "=" + (p == 0 ? "1,0,0" : p == 1 ? "0,1,0" : "0,0,1"), "result",
             [p](D3 d) { return Rot::of(fromOneAxis(p, d)); });
    }
    for (int p = 0; p < 3; ++p)
        for (int s = 0; s < 3; ++s)
            if (s != p)
            {
                const std::string pn(1, axisNames[p]), sn(1, axisNames[s]);
                const std::string pin = std::string(1, char('x' + p)) + "=" + (p == 0 ? "1,0,0" : p == 1 ? "0,1,0" : "0,0,1");
                const std::string sec = std::string(1, char('x' + s)) + "=" + (s == 0 ? "1,0,0" : s == 1 ? "0,1,0" : "0,0,1");
                over(lib.fn("makeRotFrom" + pn + sn, "rotator",
                            "A rotation whose local " + pn + " axis points along " + char('x' + p) + " exactly and whose " + sn + " axis leans toward " +
                                char('x' + s) + "."),
                     pin + " " + sec, "result", [p, s](D3 a, D3 b) { return Rot::of(fromTwoAxes(p, a, s, b)); });
            }
    over(lib.fn("makeRotationFromAxes", "rotator", "A rotation from local X (forward, kept), Y (right) and Z (up) axes, made orthonormal."),
         "forward=1,0,0 right=0,1,0 up=0,0,1", "result", [](D3 x, D3 y, D3 z) {
             M3 m = fromTwoAxes(0, x, 1, y);
             if (glm::dot(m[2], z) < 0.0)
                 m[1] = -m[1], m[2] = -m[2];
             return Rot::of(m);
         });
    over(lib.fn("findLookAtRotation", "rotator", "A rotation whose local X axis points from start to target, its Y axis leaning toward world up."),
         "start target=1,0,0", "result", [](D3 a, D3 b) { return Rot::of(fromTwoAxes(0, b - a, 1, math::axisBasis().up)); });
    over(lib.fn("rotatorToQuat", "quat", "A rotator as a quat."), "inRot", "result", [](Rot r) { return r.quat(); });
    over(lib.fn("quatToRotator", "quat", "A quat as a rotator in rotateOrder."), "q", "result", [](Quat q) { return Rot::of(q); });
    over(lib.fn("rotatorToMatrix", "matrix", "A rotator as a rotation matrix."), "inRot", "result", [](Rot r) { return M4(r.mat()); });

    // Quats
    over(lib.fn("makeQuat", "quat", "A quat from its components, normalised; all zero is the identity."), "x y z w=1", "result",
         [](double x, double y, double z, double w) {
             Quat q(x, y, z, w);
             return q.sizeSquared() < 1e-12 ? Quat() : q.getNormalized();
         });
    over(lib.fn("breakQuat", "quat", "A quat's components, w kept non-negative."), "q", "x y z w", [](Quat q) {
        if (q.w < 0.0)
            q = -q;
        return std::tuple{q.x, q.y, q.z, q.w};
    });
    over(lib.fn("quatFromAxisAndAngle", "quat", "angle radians about axis (right-hand rule), as Unreal's FQuat(axis, angle)."), "axis=0,1,0 angle", "result",
         [](D3 axis, double a) { return glm::length(axis) < math::kSmallNumber ? Quat() : Quat(V(unit(axis)), a); });
    over(lib.fn("quatToAxisAndAngle", "quat", "A quat's rotation axis and angle in radians."), "q", "axis angle", [](Quat q) {
        V axis;
        double angle = 0.0;
        q.toAxisAndAngle(axis, angle);
        return std::tuple{axis.toGlm(), angle};
    });
    over(lib.fn("quatInversed", "quat", "The rotation that undoes Q."), "q", "result", [](Quat q) { return q.inverse(); });
    over(lib.fn("quatSlerp", "interpolation", "Quat A to B by alpha along the shortest arc."), "a b alpha=0.5", "result",
         [](Quat a, Quat b, double t) { return Quat::slerp(a, b, t); });
    over(lib.fn("quatRotateVector", "quat", "V turned by Q."), "q v", "result", [](Quat q, D3 v) { return q.rotateVector(V(v)).toGlm(); });
    over(lib.fn("quatUnrotateVector", "quat", "V turned back by Q."), "q v", "result", [](Quat q, D3 v) { return q.unrotateVector(V(v)).toGlm(); });
    over(lib.fn("quatGetAngle", "quat", "The angle Q turns by, radians."), "q", "result", [](Quat q) {
        const double a = q.getAngle();
        return a > math::kPi ? 2.0 * math::kPi - a : a;
    });
    over(lib.fn("quatGetRotationAxis", "quat", "The axis Q turns about."), "q", "result", [](Quat q) { return q.getRotationAxis().toGlm(); });
    over(lib.fn("quatAngularDistance", "quat", "The angle in radians between two rotations, 0..pi."), "a b", "result",
         [](Quat a, Quat b) { return a.angularDistance(b); });
    over(lib.fn("quatFindBetweenVectors", "quat", "The smallest rotation taking direction start onto end."), "start=1,0,0 end=0,1,0", "result",
         [](D3 a, D3 b) { return Quat::findBetweenVectors(V(a), V(b)); });
    for (int k = 0; k < 3; ++k)
        over(lib.fn(std::string("quatGetAxis") + axisNames[k], "quat", std::string("Q's local ") + axisNames[k] + " axis."), "q", "result",
             [k](Quat q) { return q.rotateVector(V(M3(1.0)[k])).toGlm(); });
    over(lib.fn("quatToSwingTwist", "quat",
                "Splits Q into a twist about twistAxis and the swing that follows it (q = swing * twist), with the twist angle in degrees."),
         "q twistAxis=1,0,0", "swing twist twistAngle", [](Quat q, D3 axis) {
             auto [swing, twist] = swingTwist(q, axis);
             V ax;
             double angle = 0.0;
             twist.toAxisAndAngle(ax, angle);
             if (angle > math::kPi)
                 angle -= 2.0 * math::kPi;
             if (glm::dot(ax.toGlm(), unit(axis)) < 0.0)
                 angle = -angle;
             return std::tuple{swing, twist, math::radiansToDegrees(angle)};
         });
    over(lib.fn("qinterpTo", "interpolation", "One step of easing a quat toward target; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result", [](Quat c, Quat t, double dt, double s) { return math::qinterpTo(c, t, dt, s); });
    over(lib.fn("qinterpToConstant", "interpolation", "One step of turning a quat toward target at interpSpeed radians a second."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result",
         [](Quat c, Quat t, double dt, double s) { return math::qinterpToConstant(c, t, dt, s); });
}
} // namespace lila::rigmath
