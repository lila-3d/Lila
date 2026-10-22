#include "math/Transform.h"

namespace lila::math
{
// Matrix -------------------------------------------------------------------------------------

Matrix::Matrix(const Vector& xAxis, const Vector& yAxis, const Vector& zAxis, const Vector& origin)
{
    m[0] = glm::dvec4(xAxis.toGlm(), 0.0);
    m[1] = glm::dvec4(yAxis.toGlm(), 0.0);
    m[2] = glm::dvec4(zAxis.toGlm(), 0.0);
    m[3] = glm::dvec4(origin.toGlm(), 1.0);
}

Matrix::Matrix(const Transform& t)
{
    const glm::dmat3 r = t.rotation.toGlmMat3();
    m[0] = glm::dvec4(r[0] * t.scale3d.x, 0.0);
    m[1] = glm::dvec4(r[1] * t.scale3d.y, 0.0);
    m[2] = glm::dvec4(r[2] * t.scale3d.z, 0.0);
    m[3] = glm::dvec4(t.translation.toGlm(), 1.0);
}

Matrix Matrix::makeTranslation(const Vector& t)
{
    Matrix r;
    r.m[3] = glm::dvec4(t.toGlm(), 1.0);
    return r;
}

Matrix Matrix::makeScale(const Vector& s)
{
    Matrix r;
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}

Matrix Matrix::makeFromQuat(const Quat& q) { return Matrix(glm::dmat4(q.toGlmMat3())); }

Matrix Matrix::makeFromRotationTranslation(const Rotator& r, const Vector& origin)
{
    Matrix result = makeFromRotator(r);
    result.setOrigin(origin);
    return result;
}

bool Matrix::equals(const Matrix& b, double tolerance) const
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (std::abs(m[i][j] - b.m[i][j]) > tolerance)
                return false;
    return true;
}

Vector Matrix::transformPosition(const Vector& p) const { return Vector(glm::dvec3(m * glm::dvec4(p.toGlm(), 1.0))); }
Vector Matrix::transformVector(const Vector& v) const { return Vector(glm::dvec3(m * glm::dvec4(v.toGlm(), 0.0))); }
Vector Matrix::inverseTransformPosition(const Vector& p) const { return inverse().transformPosition(p); }
Vector Matrix::inverseTransformVector(const Vector& v) const { return inverse().transformVector(v); }

Matrix Matrix::inverse() const
{
    if (glm::determinant(m) == 0.0)
        return identity();
    return Matrix(glm::inverse(m));
}

void Matrix::setOrigin(const Vector& o)
{
    m[3].x = o.x;
    m[3].y = o.y;
    m[3].z = o.z;
}

Matrix Matrix::concatTranslation(const Vector& t) const
{
    Matrix r = *this;
    r.m[3] += glm::dvec4(t.toGlm(), 0.0);
    return r;
}

void Matrix::setAxis(Axis axis, const Vector& v)
{
    const int i = int(axis);
    m[i].x = v.x;
    m[i].y = v.y;
    m[i].z = v.z;
}

Vector Matrix::getScaleVector(double tolerance) const
{
    Vector s;
    for (int i = 0; i < 3; ++i)
    {
        const double sq = glm::dot(glm::dvec3(m[i]), glm::dvec3(m[i]));
        s[i] = sq > tolerance ? std::sqrt(sq) : 0.0;
    }
    return s;
}

void Matrix::removeScaling(double tolerance)
{
    for (int i = 0; i < 3; ++i)
    {
        const glm::dvec3 axis(m[i]);
        const double sq = glm::dot(axis, axis);
        if (sq > tolerance)
            m[i] = glm::dvec4(axis / std::sqrt(sq), m[i].w);
    }
}

Matrix Matrix::getMatrixWithoutScale(double tolerance) const
{
    Matrix r = *this;
    r.removeScaling(tolerance);
    return r;
}

Matrix Matrix::applyScale(double scale) const
{
    Matrix r = *this;
    for (int i = 0; i < 3; ++i)
        r.m[i] *= scale;
    return r;
}

glm::dmat3 Matrix::rotationPart() const
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
    {
        const double len = glm::length(r[i]);
        if (len > kSmallNumber)
            r[i] /= len;
    }
    if (glm::determinant(r) < 0.0)
        r[0] = -r[0];
    return r;
}

Rotator Matrix::rotator() const { return Rotator::fromGlmMat3(rotationPart()); }
Quat Matrix::toQuat() const { return Quat::fromGlmMat3(rotationPart()); }

// Transform ----------------------------------------------------------------------------------

namespace
{
double safeReciprocal(double v) { return std::abs(v) <= kSmallNumber ? 0.0 : 1.0 / v; }
Vector safeReciprocal(const Vector& v) { return {safeReciprocal(v.x), safeReciprocal(v.y), safeReciprocal(v.z)}; }
bool hasNegativeScale(const Vector& s) { return s.x < 0.0 || s.y < 0.0 || s.z < 0.0; }
} // namespace

Transform::Transform(const Matrix& m)
{
    scale3d = m.getScaleVector();
    if (m.rotDeterminant() < 0.0)
        scale3d.x = -scale3d.x;
    rotation = m.toQuat();
    translation = m.getOrigin();
}

Transform Transform::blend(const Transform& a, const Transform& b, double alpha)
{
    return Transform(Quat::slerp(a.rotation, b.rotation, alpha), Vector::lerp(a.translation, b.translation, alpha),
                     Vector::lerp(a.scale3d, b.scale3d, alpha));
}

Transform Transform::operator*(const Transform& b) const
{
    // Negative scale cannot be carried per component through a rotation; let the matrix sort out the sign.
    if (hasNegativeScale(scale3d) || hasNegativeScale(b.scale3d))
        return Transform(toMatrixWithScale() * b.toMatrixWithScale());

    Transform r;
    r.rotation = b.rotation * rotation;
    r.scale3d = scale3d * b.scale3d;
    r.translation = b.rotation.rotateVector(b.scale3d * translation) + b.translation;
    return r;
}

bool Transform::equals(const Transform& b, double tolerance) const
{
    return rotation.equals(b.rotation, tolerance) && translation.equals(b.translation, tolerance) &&
           scale3d.equals(b.scale3d, tolerance);
}

bool Transform::equalsNoScale(const Transform& b, double tolerance) const
{
    return rotation.equals(b.rotation, tolerance) && translation.equals(b.translation, tolerance);
}

Vector Transform::inverseTransformPosition(const Vector& p) const
{
    return rotation.unrotateVector(p - translation) * safeReciprocal(scale3d);
}

Vector Transform::inverseTransformPositionNoScale(const Vector& p) const
{
    return rotation.unrotateVector(p - translation);
}

Vector Transform::inverseTransformVector(const Vector& v) const
{
    return rotation.unrotateVector(v) * safeReciprocal(scale3d);
}

Transform Transform::inverse() const
{
    const Quat invRotation = rotation.inverse();
    const Vector invScale = safeReciprocal(scale3d);
    return Transform(invRotation, invScale * invRotation.rotateVector(-translation), invScale);
}

Transform Transform::getRelativeTransform(const Transform& other) const
{
    const Quat invOther = other.rotation.inverse();
    const Vector invScale = safeReciprocal(other.scale3d);
    return Transform(invOther * rotation, invOther.rotateVector(translation - other.translation) * invScale,
                     scale3d * invScale);
}

Transform Transform::getRelativeTransformReverse(const Transform& other) const
{
    return other.getRelativeTransform(*this);
}

Matrix Transform::toMatrixNoScale() const { return Matrix(Transform(rotation, translation)); }

Vector Transform::getScaledAxis(Axis axis) const
{
    Vector unit;
    unit[int(axis)] = 1.0;
    return transformVector(unit);
}

Vector Transform::getUnitAxis(Axis axis) const
{
    Vector unit;
    unit[int(axis)] = 1.0;
    return transformVectorNoScale(unit);
}
} // namespace lila::math
