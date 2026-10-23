#pragma once

#include "core/Export.h"

#include "math/Matrix.h"

namespace lila::math
{
/**
 * Rotation, translation and non-uniform scale, applied scale, then rotation, then translation.
 * A * B applies A first, then B, so child * parent gives the child in the parent's space. With
 * non-uniform scale under a rotation the result is an approximation, as in Unreal (shear drops).
 */
struct LILA_FOUNDATION_API Transform
{
    Quat rotation;
    Vector translation;
    Vector scale3d = Vector::one();

    Transform() = default;
    Transform(const Quat& r, const Vector& t = Vector::zero(), const Vector& s = Vector::one())
        : rotation(r), translation(t), scale3d(s)
    {
    }
    Transform(const Rotator& r, const Vector& t = Vector::zero(), const Vector& s = Vector::one())
        : rotation(r.quaternion()), translation(t), scale3d(s)
    {
    }
    explicit Transform(const Vector& t) : translation(t) {}
    /** Decomposes; shear is lost. */
    explicit Transform(const Matrix& m);

    static Transform identity() { return {}; }
    static Transform multiply(const Transform& a, const Transform& b) { return a * b; }
    /** Lerps translation and scale, slerps rotation. */
    static Transform blend(const Transform& a, const Transform& b, double alpha);

    Transform operator*(const Transform& b) const;
    bool operator==(const Transform&) const = default;
    bool equals(const Transform& b, double tolerance = kKindaSmallNumber) const;
    bool equalsNoScale(const Transform& b, double tolerance = kKindaSmallNumber) const;

    Vector transformPosition(const Vector& p) const { return rotation.rotateVector(p * scale3d) + translation; }
    Vector transformPositionNoScale(const Vector& p) const { return rotation.rotateVector(p) + translation; }
    Vector transformVector(const Vector& v) const { return rotation.rotateVector(v * scale3d); }
    Vector transformVectorNoScale(const Vector& v) const { return rotation.rotateVector(v); }
    Quat transformRotation(const Quat& q) const { return rotation * q; }
    Vector inverseTransformPosition(const Vector& p) const;
    Vector inverseTransformPositionNoScale(const Vector& p) const;
    Vector inverseTransformVector(const Vector& v) const;
    Vector inverseTransformVectorNoScale(const Vector& v) const { return rotation.unrotateVector(v); }
    Quat inverseTransformRotation(const Quat& q) const { return rotation.inverse() * q; }

    /** Exact for uniform scale. */
    Transform inverse() const;
    /** This expressed relative to other: result * other == this. */
    Transform getRelativeTransform(const Transform& other) const;
    /** Other expressed relative to this. */
    Transform getRelativeTransformReverse(const Transform& other) const;
    Transform blendWith(const Transform& other, double alpha) const { return blend(*this, other, alpha); }

    Matrix toMatrixWithScale() const { return Matrix(*this); }
    Matrix toMatrix() const { return toMatrixWithScale(); }
    Matrix toMatrixNoScale() const;

    Rotator rotator() const { return rotation.rotator(); }
    double getDeterminant() const { return scale3d.x * scale3d.y * scale3d.z; }
    double getMaximumAxisScale() const { return scale3d.getAbsMax(); }
    double getMinimumAxisScale() const { return scale3d.getAbsMin(); }
    bool isRotationNormalized() const { return rotation.isNormalized(); }
    void normalizeRotation() { rotation.normalize(); }
    Vector getScaledAxis(Axis axis) const;
    Vector getUnitAxis(Axis axis) const;
    bool containsNaN() const { return rotation.containsNaN() || translation.containsNaN() || scale3d.containsNaN(); }
};
} // namespace lila::math
