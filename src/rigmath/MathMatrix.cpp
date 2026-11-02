#include "rigmath/Builder.h"

#include <glm/gtc/matrix_transform.hpp>

namespace lila::rigmath
{
namespace matrixfns
{
using V = math::Vector;
using T = math::Transform;

T toT(const M4& m) { return T(math::Matrix(m)); }
M4 fromT(const T& t) { return t.toMatrixWithScale().toGlm(); }
M4 inverseOr(const M4& m) { return std::abs(glm::determinant(m)) < 1e-12 ? M4(1.0) : glm::inverse(m); }
M3 rotationPart(const M4& m) { return math::Matrix(m).rotationPart(); }

/** Control Rig's Aim Math: turns m so primaryAxis aims at its target, then twists secondaryAxis toward its own. */
M4 aim(const M4& m, const D3& pAxis, const D3& pTarget, bool pIsPoint, const D3& sAxis, const D3& sTarget, bool sIsPoint, double weight)
{
    const D3 pos(m[3]);
    const D3 scale(glm::length(D3(m[0])), glm::length(D3(m[1])), glm::length(D3(m[2])));
    const Quat start = quatOf(m);
    Quat q = start;
    const D3 pDir = pIsPoint ? pTarget - pos : pTarget;
    if (glm::length(pDir) > math::kSmallNumber && glm::length(pAxis) > math::kSmallNumber)
    {
        const D3 aimDir = glm::normalize(pDir);
        const V cur = q.rotateVector(V(glm::normalize(pAxis)));
        q = Quat::findBetweenNormals(cur, V(aimDir)) * q;
        const D3 sDir = sIsPoint ? sTarget - pos : sTarget;
        const D3 curS = q.rotateVector(V(sAxis)).toGlm();
        const D3 a = curS - aimDir * glm::dot(curS, aimDir), b = sDir - aimDir * glm::dot(sDir, aimDir);
        if (glm::length(a) > 1e-9 && glm::length(b) > 1e-9)
        {
            const double angle = std::atan2(glm::dot(glm::cross(a, b), aimDir), glm::dot(a, b));
            q = Quat(V(aimDir), angle) * q;
        }
    }
    q = Quat::slerp(start, q, std::clamp(weight, 0.0, 1.0));
    M4 out(q.toGlmMat3());
    for (int k = 0; k < 3; ++k)
        out[k] *= scale[k];
    out[3] = glm::dvec4(pos, 1.0);
    return out;
}
} // namespace matrixfns

void addMatrix(Library& lib)
{
    using namespace matrixfns;

    over(lib.fn("makeTransform", "matrix", "A transform matrix: scale, then rotation, then translation to location."), "location rotation scale=1,1,1",
         "result", [](D3 t, Rot r, D3 s) { return M4(glm::translate(M4(1.0), t) * M4(r.mat()) * glm::scale(M4(1.0), s)); });
    over(lib.fn("breakTransform", "matrix", "A transform matrix's location, rotation (in rotateOrder) and scale; shear is dropped."), "inTransform",
         "location rotation scale", [](M4 m) {
             const T t = toT(m);
             return std::tuple{t.translation.toGlm(), Rot::of(t.rotation), t.scale3d.toGlm()};
         });
    over(lib.fn("composeTransforms", "matrix", "Transform A, then B, as Unreal composes FTransforms (shear from non-uniform scale is dropped)."), "a b",
         "result", [](M4 a, M4 b) { return fromT(toT(a) * toT(b)); });
    over(lib.fn("invertTransform", "matrix", "The exact inverse; identity for a singular matrix."), "t", "result", [](M4 m) { return inverseOr(m); });
    over(lib.fn("makeRelativeTransform", "matrix", "A expressed in relativeTo's space: result composed with relativeTo gives A."), "a relativeTo", "result",
         [](M4 a, M4 r) { return M4(inverseOr(r) * a); });
    over(lib.fn("tlerp", "interpolation", "Transform A to B by alpha: location and scale lerp, rotation slerps."), "a b alpha=0.5", "result",
         [](M4 a, M4 b, double t) { return fromT(T::blend(toT(a), toT(b), t)); });
    over(lib.fn("tinterpTo", "interpolation", "One step of easing a transform toward target; a speed of 0 or less snaps."),
         "current target deltaTime=0.0333333 interpSpeed=1", "result",
         [](M4 c, M4 t, double dt, double s) { return fromT(math::tinterpTo(toT(c), toT(t), dt, s)); });
    over(lib.fn("transformLocation", "matrix", "A point carried by the transform."), "t location", "result",
         [](M4 m, D3 p) { return D3(m * glm::dvec4(p, 1.0)); });
    over(lib.fn("transformDirection", "matrix", "A direction carried by the transform's rotation and scale, ignoring its translation."), "t direction",
         "result", [](M4 m, D3 d) { return D3(m * glm::dvec4(d, 0.0)); });
    over(lib.fn("inverseTransformLocation", "matrix", "A point brought into the transform's local space."), "t location", "result",
         [](M4 m, D3 p) { return D3(inverseOr(m) * glm::dvec4(p, 1.0)); });
    over(lib.fn("inverseTransformDirection", "matrix", "A direction brought into the transform's local space."), "t direction", "result",
         [](M4 m, D3 d) { return D3(inverseOr(m) * glm::dvec4(d, 0.0)); });
    over(lib.fn("transformRotation", "matrix", "A rotation followed by the transform's rotation."), "t rotation", "result",
         [](M4 m, Rot r) { return Rot::of(rotationPart(m) * r.mat()); });
    over(lib.fn("inverseTransformRotation", "matrix", "A rotation brought into the transform's local space."), "t rotation", "result",
         [](M4 m, Rot r) { return Rot::of(glm::transpose(rotationPart(m)) * r.mat()); });
    over(lib.fn("makeMatrix", "matrix", "A matrix from its X, Y and Z axes and origin."), "xAxis=1,0,0 yAxis=0,1,0 zAxis=0,0,1 origin", "result",
         [](D3 x, D3 y, D3 z, D3 o) { return M4(glm::dvec4(x, 0.0), glm::dvec4(y, 0.0), glm::dvec4(z, 0.0), glm::dvec4(o, 1.0)); });
    over(lib.fn("breakMatrix", "matrix", "A matrix's X, Y and Z axes (scaled) and origin."), "inMatrix", "xAxis yAxis zAxis origin",
         [](M4 m) { return std::tuple{D3(m[0]), D3(m[1]), D3(m[2]), D3(m[3])}; });
    over(lib.fn("matrixGetTransposed", "matrix", "The transposed matrix."), "m", "result", [](M4 m) { return glm::transpose(m); });
    over(lib.fn("matrixGetDeterminant", "matrix", "The determinant."), "m", "result", [](M4 m) { return glm::determinant(m); });
    over(lib.fn("matrixRemoveScaling", "matrix", "The matrix with its axes scaled to length 1, origin kept."), "m", "result", [](M4 m) {
        for (int k = 0; k < 3; ++k)
        {
            const double l = glm::length(D3(m[k]));
            if (l > math::kSmallNumber)
                m[k] /= l;
        }
        return m;
    });
    over(lib.fn("aimMath", "matrix",
                "Control Rig's Aim Math: the matrix turned so primaryAxis points at primaryTarget, then twisted so secondaryAxis leans toward "
                "secondaryTarget. A target is a point, or a direction when its kind says so. weight blends from the input rotation."),
         "inputMatrix primaryAxis=1,0,0 primaryTarget=1,0,0 primaryKind=location|direction secondaryAxis=0,1,0 secondaryTarget=0,1,0 "
         "secondaryKind=direction|location weight=1",
         "result", [](M4 m, D3 pa, D3 pt, Choice pk, D3 sa, D3 st, Choice sk, double w) { return aim(m, pa, pt, pk.index == 0, sa, st, sk.index == 1, w); });
}
} // namespace lila::rigmath
