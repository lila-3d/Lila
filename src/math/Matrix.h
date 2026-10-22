#pragma once

#include "math/Quat.h"

#include <glm/mat4x4.hpp>

namespace lila::math
{
struct Transform;

enum class Axis : uint8_t
{
    X,
    Y,
    Z,
};

/**
 * 4x4 matrix with Unreal's layout and conventions: m[row][col], rows 0-2 are the X/Y/Z axes,
 * row 3 is the origin, points are row vectors, and A * B applies A first, then B.
 * That layout is exactly glm's column-major storage, so m holds the glm matrix as is:
 * m[i][j] here is glm's column i, row j.
 */
struct Matrix
{
    glm::dmat4 m{1.0};

    Matrix() = default;
    explicit Matrix(const glm::dmat4& inM) : m(inM) {}
    /** Rows: X axis, Y axis, Z axis, origin. */
    Matrix(const Vector& xAxis, const Vector& yAxis, const Vector& zAxis, const Vector& origin);
    explicit Matrix(const Transform& t);
    const glm::dmat4& toGlm() const { return m; }

    static Matrix identity() { return {}; }
    static Matrix makeTranslation(const Vector& t);
    static Matrix makeScale(const Vector& s);
    static Matrix makeFromQuat(const Quat& q);
    static Matrix makeFromRotator(const Rotator& r) { return makeFromQuat(r.quaternion()); }
    static Matrix makeFromRotationTranslation(const Rotator& r, const Vector& origin);

    double& operator()(int row, int col) { return m[row][col]; }
    double operator()(int row, int col) const { return m[row][col]; }
    Matrix operator*(const Matrix& b) const { return Matrix(b.m * m); }
    Matrix operator*(double s) const { return Matrix(m * s); }
    Matrix operator+(const Matrix& b) const { return Matrix(m + b.m); }
    Matrix& operator*=(const Matrix& b) { return *this = *this * b; }
    bool operator==(const Matrix& b) const { return m == b.m; }
    bool equals(const Matrix& b, double tolerance = kKindaSmallNumber) const;

    Vector4 transformVector4(const Vector4& v) const { return m * v.toGlm(); }
    /** Affine: w = 1, no perspective divide. */
    Vector transformPosition(const Vector& p) const;
    /** w = 0: ignores the origin. */
    Vector transformVector(const Vector& v) const;
    Vector inverseTransformPosition(const Vector& p) const;
    Vector inverseTransformVector(const Vector& v) const;

    /** Identity when singular. */
    Matrix inverse() const;
    Matrix transpose() const { return Matrix(glm::transpose(m)); }
    double determinant() const { return glm::determinant(m); }
    /** Determinant of the upper 3x3. */
    double rotDeterminant() const { return glm::determinant(glm::dmat3(m)); }

    Vector getOrigin() const { return Vector(glm::dvec3(m[3])); }
    void setOrigin(const Vector& o);
    Matrix concatTranslation(const Vector& t) const;
    Vector getScaledAxis(Axis axis) const { return Vector(glm::dvec3(m[int(axis)])); }
    Vector getUnitAxis(Axis axis) const { return getScaledAxis(axis).getSafeNormal(); }
    void setAxis(Axis axis, const Vector& v);
    Vector getScaleVector(double tolerance = kSmallNumber) const;
    /** Normalizes the axes in place. */
    void removeScaling(double tolerance = kSmallNumber);
    Matrix getMatrixWithoutScale(double tolerance = kSmallNumber) const;
    /** Scales the axes (rows 0-2), keeping the origin. */
    Matrix applyScale(double scale) const;

    Rotator rotator() const;
    Quat toQuat() const;
    /** Upper 3x3 with axes normalized, a negative determinant folded into X. */
    glm::dmat3 rotationPart() const;
};
} // namespace lila::math
