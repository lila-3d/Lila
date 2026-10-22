#include "math/Math.h"

#include <doctest/doctest.h>

#include <random>

using namespace lila::math;

namespace
{
constexpr double kEps = 1.0e-9;

/** Runs body once per up axis and restores Y afterwards. */
template <typename F> void forEachUpAxis(F&& body)
{
    for (UpAxis axis : {UpAxis::Y, UpAxis::Z})
    {
        setUpAxis(axis);
        CAPTURE(int(axis));
        body();
    }
    setUpAxis(UpAxis::Y);
}

std::mt19937& rng()
{
    static std::mt19937 gen(1234);
    return gen;
}

double rand(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng()); }
Vector randVector(double r = 100.0) { return {rand(-r, r), rand(-r, r), rand(-r, r)}; }
Rotator randRotator() { return {rand(-89.0, 89.0), rand(-179.0, 179.0), rand(-179.0, 179.0)}; }
Quat randQuat() { return randRotator().quaternion(); }
} // namespace

TEST_CASE("scalar helpers")
{
    CHECK(normalizeAxis(270.0) == doctest::Approx(-90.0));
    CHECK(normalizeAxis(-180.0) == doctest::Approx(180.0));
    CHECK(normalizeAxis(180.0) == doctest::Approx(180.0));
    CHECK(normalizeAxis(540.0) == doctest::Approx(180.0));
    CHECK(clampAxis(-90.0) == doctest::Approx(270.0));
    CHECK(clampAxis(720.0) == doctest::Approx(0.0));
    CHECK(clampAxis(-1.0e-15) == 0.0);
    CHECK(clampAngle(100.0, -45.0, 45.0) == doctest::Approx(45.0));
    CHECK(clampAngle(-100.0, -45.0, 45.0) == doctest::Approx(-45.0));
    CHECK(clampAngle(10.0, -45.0, 45.0) == doctest::Approx(10.0));
    CHECK(clampAngle(350.0, 300.0, 20.0) == doctest::Approx(-10.0));
    CHECK(unwindDegrees(190.0) == doctest::Approx(-170.0));
    CHECK(wrap(11.0, 0.0, 10.0) == doctest::Approx(1.0));
    CHECK(wrap(-1.0, 0.0, 10.0) == doctest::Approx(9.0));
    CHECK(mapRangeClamped(15.0, 0.0, 10.0, 0.0, 100.0) == doctest::Approx(100.0));
    CHECK(mapRangeUnclamped(15.0, 0.0, 10.0, 0.0, 100.0) == doctest::Approx(150.0));
    CHECK(gridSnap(14.0, 10.0) == doctest::Approx(10.0));
    CHECK(gridSnap(15.0, 10.0) == doctest::Approx(20.0));
    CHECK(finterpTo(0.0, 10.0, 0.1, 5.0) == doctest::Approx(5.0));
    CHECK(finterpTo(0.0, 10.0, 0.1, 0.0) == doctest::Approx(10.0));
    CHECK(finterpToConstant(0.0, 10.0, 0.5, 4.0) == doctest::Approx(2.0));
    CHECK(finterpToConstant(9.5, 10.0, 0.5, 4.0) == doctest::Approx(10.0));
}

TEST_CASE("axis basis")
{
    forEachUpAxis(
        []
        {
            const AxisBasis& b = axisBasis();
            CHECK(Vector(glm::cross(b.forward, b.up)).equals(Vector(b.right), kEps));
            CHECK(Vector::up().equals(Vector(b.up), kEps));
            CHECK(Vector::down().equals(-Vector(b.up), kEps));
            CHECK(Vector::left().equals(-Vector::right(), kEps));
            CHECK(Vector::backward().equals(-Vector::forward(), kEps));
        });
    setUpAxis(UpAxis::Y);
    CHECK(Vector::up() == Vector(0, 1, 0));
    CHECK(Vector::forward() == Vector(0, 0, 1));
    CHECK(Vector::right() == Vector(-1, 0, 0));
    setUpAxis(UpAxis::Z);
    CHECK(Vector::up() == Vector(0, 0, 1));
    CHECK(Vector::forward() == Vector(1, 0, 0));
    CHECK(Vector::right() == Vector(0, -1, 0));
    setUpAxis(UpAxis::Y);
}

TEST_CASE("vector basics")
{
    const Vector a(1, 2, 3);
    const Vector b(4, 5, 6);
    CHECK((a + b) == Vector(5, 7, 9));
    CHECK((a * b) == Vector(4, 10, 18));
    CHECK((a | b) == doctest::Approx(32.0));
    CHECK((a ^ b) == Vector(-3, 6, -3));
    CHECK((Vector::xAxis() ^ Vector::yAxis()) == Vector::zAxis());
    CHECK(Vector(3, 4, 0).size() == doctest::Approx(5.0));
    CHECK(Vector(3, 4, 0).dist(Vector::zero()) == doctest::Approx(5.0));

    Vector n(0, 3, 4);
    CHECK(n.normalize());
    CHECK(n.equals(Vector(0, 0.6, 0.8), kEps));
    Vector z;
    CHECK_FALSE(z.normalize());
    CHECK(Vector::zero().getSafeNormal() == Vector::zero());
    CHECK(Vector::zero().getSafeNormal(kSmallNumber, Vector::up()) == Vector::up());

    CHECK(Vector(10, 0, 0).clampSize(1, 5).equals(Vector(5, 0, 0), kEps));
    CHECK(Vector(1, 0, 0).clampSize(2, 5).equals(Vector(2, 0, 0), kEps));
    CHECK(Vector(10, 0, 0).clampMaxSize(3).equals(Vector(3, 0, 0), kEps));
    CHECK(Vector(2, 4, 2).projection().equals(Vector(1, 2, 1), kEps));
    CHECK(Vector(1, 1, 0).projectOnTo(Vector(2, 0, 0)).equals(Vector(1, 0, 0), kEps));
    CHECK(Vector(1, 1, 0).projectOnPlane(Vector(0, 1, 0)).equals(Vector(1, 0, 0), kEps));
    CHECK(Vector(1, -1, 0).mirrorByVector(Vector(0, 1, 0)).equals(Vector(1, 1, 0), kEps));
    CHECK(Vector(-3, 2, -1).getAbs() == Vector(3, 2, 1));
    CHECK(Vector(-3, 2, -1).getMax() == 2.0);
    CHECK(Vector(-3, 2, -1).getAbsMax() == 3.0);
    CHECK(a.componentMin(Vector(0, 5, 1)) == Vector(0, 2, 1));
    CHECK(Vector(-1, 0, 2).getSignVector() == Vector(-1, 1, 1));
    CHECK(Vector(14, 16, -14).gridSnap(10) == Vector(10, 20, -10));

    // Right-handed: 90 degrees about +Z takes +X to +Y.
    CHECK(Vector::xAxis().rotateAngleAxis(90.0, Vector::zAxis()).equals(Vector::yAxis(), kEps));
}

TEST_CASE("vector ground plane and heading")
{
    forEachUpAxis(
        []
        {
            const Vector v = Vector::forward() * 3.0 + Vector::right() * 4.0 + Vector::up() * 100.0;
            CHECK(v.size2D() == doctest::Approx(5.0));
            CHECK(v.getSafeNormal2D().equals((Vector::forward() * 3.0 + Vector::right() * 4.0) / 5.0, kEps));
            CHECK(Vector::forward().headingAngle() == doctest::Approx(0.0));
            CHECK(Vector::right().headingAngle() == doctest::Approx(kPi / 2));
            CHECK(Vector::left().headingAngle() == doctest::Approx(-kPi / 2));
            CHECK(Vector::forward().cosineAngle2D(Vector::forward() + Vector::up() * 50.0) == doctest::Approx(1.0));
            CHECK(Vector::forward().cosineAngle2D(Vector::right()) == doctest::Approx(0.0));
        });
}

TEST_CASE("rotator semantics match unreal")
{
    forEachUpAxis(
        []
        {
            CHECK(Rotator(0, 90, 0).vector().equals(Vector::right(), kEps));
            CHECK(Rotator(90, 0, 0).vector().equals(Vector::up(), kEps));
            CHECK(Rotator(0, 0, 90).getUpVector().equals(Vector::right(), kEps));
            CHECK(Rotator(0, 0, 90).getRightVector().equals(Vector::down(), kEps));
            CHECK(Rotator(90, 0, 0).getUpVector().equals(Vector::backward(), kEps));
            CHECK(Rotator(0, 90, 0).getRightVector().equals(Vector::backward(), kEps));
            CHECK(Rotator().getForwardVector().equals(Vector::forward(), kEps));

            for (int i = 0; i < 50; ++i)
            {
                const Rotator r = randRotator();
                const double cp = std::cos(degreesToRadians(r.pitch)), sp = std::sin(degreesToRadians(r.pitch));
                const double cy = std::cos(degreesToRadians(r.yaw)), sy = std::sin(degreesToRadians(r.yaw));
                // Unreal's forward for a rotator.
                CHECK(toUnreal(r.vector()).equals(Vector(cp * cy, cp * sy, sp), kEps));

                const Vector f = r.getForwardVector(), rt = r.getRightVector(), u = r.getUpVector();
                CHECK((f ^ u).equals(rt, kEps));
                CHECK(std::abs(f | rt) < kEps);
            }
        });
}

TEST_CASE("rotator quat round trip")
{
    forEachUpAxis(
        []
        {
            for (int i = 0; i < 200; ++i)
            {
                const Rotator r = randRotator();
                const Quat q = r.quaternion();
                CHECK(q.isNormalized());
                CHECK(Rotator(q).equals(r, 1.0e-6));
                const Vector v = randVector();
                CHECK(q.rotateVector(v).equals(r.rotateVector(v), 1.0e-9));
                CHECK(r.unrotateVector(r.rotateVector(v)).equals(v, 1.0e-9));
                CHECK(r.getInverse().quaternion().equals(q.inverse(), 1.0e-9));
            }
            // Gimbal lock still describes the same orientation.
            const Rotator up(90, 30, 20);
            const Rotator back = Rotator(up.quaternion());
            CHECK(back.quaternion().equals(up.quaternion(), 1.0e-9));
            CHECK(back.pitch == doctest::Approx(90.0));
        });
}

TEST_CASE("rotator normalize, equals, compose")
{
    Rotator r(370, -190, 540);
    r.normalize();
    CHECK(r.equals(Rotator(10, 170, 180), kEps));
    CHECK(Rotator(-10, -90, 0).getDenormalized().equals(Rotator(350, 270, 0), kEps));
    CHECK(Rotator(0, 359.99999, 0).equals(Rotator(0, 0, 0), 1.0e-3));
    CHECK(Rotator(0, 360, 0).isNearlyZero());

    forEachUpAxis(
        []
        {
            const Rotator a = randRotator(), b = randRotator();
            const Rotator c = composeRotators(a, b);
            const Vector v = randVector();
            CHECK(c.rotateVector(v).equals(b.rotateVector(a.rotateVector(v)), 1.0e-9));
            CHECK(composeRotators(Rotator(0, 30, 0), Rotator(0, 45, 0)).equals(Rotator(0, 75, 0), 1.0e-9));
        });
}

TEST_CASE("quat")
{
    const Quat q(Vector::zAxis(), kPi / 2);
    CHECK(q.rotateVector(Vector::xAxis()).equals(Vector::yAxis(), kEps));
    CHECK(q.unrotateVector(Vector::yAxis()).equals(Vector::xAxis(), kEps));
    CHECK(q.getAngle() == doctest::Approx(kPi / 2));
    CHECK(q.getRotationAxis().equals(Vector::zAxis(), kEps));
    CHECK(q.getAxisX().equals(Vector::yAxis(), kEps));

    const Quat a = randQuat(), b = randQuat();
    const Vector v = randVector();
    CHECK((a * b).rotateVector(v).equals(a.rotateVector(b.rotateVector(v)), 1.0e-9));
    CHECK((a * a.inverse()).isIdentity(1.0e-12));
    CHECK(a.equals(-a));
    CHECK(Quat::slerp(a, b, 0.0).equals(a, 1.0e-9));
    CHECK(Quat::slerp(a, b, 1.0).equals(b, 1.0e-9));
    const Quat half = Quat::slerp(Quat::identity(), q, 0.5);
    CHECK(half.getAngle() == doctest::Approx(kPi / 4));
    CHECK(Quat::identity().angularDistance(q) == doctest::Approx(kPi / 2));
    CHECK(Quat::identity().angularDistance(-q) == doctest::Approx(kPi / 2));

    const Vector from(1, 2, 3), to(-3, 0.5, 2);
    CHECK(Quat::findBetweenVectors(from, to).rotateVector(from.getSafeNormal()).equals(to.getSafeNormal(), 1.0e-9));
    CHECK(Quat::findBetweenVectors(Vector::xAxis(), -Vector::xAxis()).rotateVector(Vector::xAxis())
              .equals(-Vector::xAxis(), 1.0e-9));

    forEachUpAxis(
        []
        {
            const Rotator r = randRotator();
            const Quat rq = r.quaternion();
            CHECK(rq.getForwardVector().equals(r.getForwardVector(), 1.0e-9));
            CHECK(rq.getRightVector().equals(r.getRightVector(), 1.0e-9));
            CHECK(rq.getUpVector().equals(r.getUpVector(), 1.0e-9));
        });
}

TEST_CASE("transform")
{
    forEachUpAxis(
        []
        {
            for (int i = 0; i < 50; ++i)
            {
                const Transform a(randQuat(), randVector(), Vector(rand(0.5, 2.0)));
                const Transform b(randQuat(), randVector(), Vector(rand(0.5, 2.0)));
                const Vector p = randVector();

                // A * B applies A first.
                CHECK((a * b).transformPosition(p).equals(b.transformPosition(a.transformPosition(p)), 1.0e-8));
                CHECK(a.inverse().transformPosition(a.transformPosition(p)).equals(p, 1.0e-8));
                CHECK(a.inverseTransformPosition(a.transformPosition(p)).equals(p, 1.0e-8));
                CHECK(a.inverseTransformVector(a.transformVector(p)).equals(p, 1.0e-8));
                CHECK(a.getRelativeTransform(b).operator*(b).equals(a, 1.0e-8));
                CHECK(a.getRelativeTransformReverse(b).equals(b.getRelativeTransform(a), 1.0e-12));
                CHECK(a.toMatrix().transformPosition(p).equals(a.transformPosition(p), 1.0e-8));
                CHECK((a.toMatrix() * b.toMatrix()).equals((a * b).toMatrix(), 1.0e-8));
                CHECK(Transform(a.toMatrix()).equals(a, 1.0e-8));
            }

            // Non-uniform scale is exact for the forward direction.
            const Transform ns(randQuat(), randVector(), Vector(1.0, 2.0, 3.0));
            const Vector p = randVector();
            CHECK(ns.inverseTransformPosition(ns.transformPosition(p)).equals(p, 1.0e-8));

            // Negative scale survives a round trip through a matrix.
            const Transform neg(randQuat(), randVector(), Vector(-1.0, 2.0, 3.0));
            CHECK(Transform(neg.toMatrix()).toMatrix().equals(neg.toMatrix(), 1.0e-8));
            const Transform parent(randQuat(), randVector(), Vector(1.5));
            CHECK((neg * parent).transformPosition(p).equals(parent.transformPosition(neg.transformPosition(p)), 1.0e-8));
        });

    const Transform t0(Rotator(0, 0, 0), Vector(0, 0, 0), Vector(1));
    const Transform t1(Rotator(0, 90, 0), Vector(10, 20, 30), Vector(3));
    const Transform mid = Transform::blend(t0, t1, 0.5);
    CHECK(mid.translation.equals(Vector(5, 10, 15), kEps));
    CHECK(mid.scale3d.equals(Vector(2), kEps));
    CHECK(mid.rotator().equals(Rotator(0, 45, 0), 1.0e-9));
}

TEST_CASE("matrix")
{
    const Matrix m(Vector(0, 1, 0), Vector(-1, 0, 0), Vector(0, 0, 1), Vector(10, 0, 0));
    CHECK(m.transformPosition(Vector(1, 0, 0)).equals(Vector(10, 1, 0), kEps));
    CHECK(m.transformVector(Vector(1, 0, 0)).equals(Vector(0, 1, 0), kEps));
    CHECK(m.getOrigin() == Vector(10, 0, 0));
    CHECK(m(3, 0) == 10.0);
    CHECK(m.inverse().transformPosition(Vector(10, 1, 0)).equals(Vector(1, 0, 0), kEps));
    CHECK(m.inverseTransformPosition(Vector(10, 1, 0)).equals(Vector(1, 0, 0), kEps));
    CHECK((m * m.inverse()).equals(Matrix::identity(), kEps));
    CHECK(m.toQuat().equals(Quat(Vector::zAxis(), kPi / 2), kEps));
    CHECK(m.transpose().transpose() == m);
    CHECK(Matrix::makeScale(Vector(2, 3, 4)).getScaleVector().equals(Vector(2, 3, 4), kEps));
    CHECK(Matrix(Vector(0), Vector(0), Vector(0), Vector(0)).inverse() == Matrix::identity());

    const Matrix t = Matrix::makeTranslation(Vector(1, 2, 3));
    const Matrix s = Matrix::makeScale(Vector(2));
    // Scale then translate.
    CHECK((s * t).transformPosition(Vector(1, 1, 1)).equals(Vector(3, 4, 5), kEps));
    CHECK((t * s).transformPosition(Vector(1, 1, 1)).equals(Vector(4, 6, 8), kEps));

    forEachUpAxis(
        []
        {
            const Rotator r = randRotator();
            CHECK(Matrix::makeFromRotator(r).rotator().equals(r, 1.0e-6));
        });
}

TEST_CASE("unreal conversion")
{
    forEachUpAxis(
        []
        {
            CHECK(toUnreal(Vector::up()).equals(Vector(0, 0, 1), kEps));
            CHECK(toUnreal(Vector::forward()).equals(Vector(1, 0, 0), kEps));
            CHECK(toUnreal(Vector::right()).equals(Vector(0, 1, 0), kEps));
            CHECK(fromUnreal(Vector(0, 0, 1)).equals(Vector::up(), kEps));

            for (int i = 0; i < 50; ++i)
            {
                const Vector p = randVector();
                CHECK(fromUnreal(toUnreal(p)).equals(p, kEps));

                // Unreal's quat rotation is the same algebra, so the converted quat must act the same in Unreal space.
                const Quat q = randQuat();
                CHECK(toUnreal(q).rotateVector(toUnreal(p)).equals(toUnreal(q.rotateVector(p)), 1.0e-9));
                CHECK(fromUnreal(toUnreal(q)).equals(q, kEps));

                // A rotator keeps its numbers: its Unreal-space quat rotates Unreal X onto Unreal's forward formula.
                const Rotator r = randRotator();
                glm::dvec3 f, rt, u;
                rotatorToUnrealAxes(r, f, rt, u);
                const Quat ueQ = toUnreal(r.quaternion());
                CHECK(ueQ.rotateVector(Vector(1, 0, 0)).equals(Vector(f), 1.0e-9));
                CHECK(ueQ.rotateVector(Vector(0, 1, 0)).equals(Vector(rt), 1.0e-9));
                CHECK(ueQ.rotateVector(Vector(0, 0, 1)).equals(Vector(u), 1.0e-9));

                const Transform t(randQuat(), randVector(), Vector(rand(0.5, 2), rand(0.5, 2), rand(0.5, 2)));
                CHECK(toUnreal(t).transformPosition(toUnreal(p)).equals(toUnreal(t.transformPosition(p)), 1.0e-8));
                CHECK(fromUnreal(toUnreal(t)).equals(t, 1.0e-9));

                const Matrix m = t.toMatrix();
                CHECK(toUnreal(m).transformPosition(toUnreal(p)).equals(toUnreal(m.transformPosition(p)), 1.0e-8));
                CHECK(fromUnreal(toUnreal(m)).equals(m, 1.0e-9));
                CHECK(toUnreal(m).equals(toUnreal(t).toMatrix(), 1.0e-8));
            }

            // A positive Unreal yaw (X toward Y) is a turn toward Lila's right.
            const Quat ueYaw(Vector(0, 0, 1), kPi / 2);
            CHECK(fromUnreal(ueYaw).rotateVector(Vector::forward()).equals(Vector::right(), kEps));

            const Box b(Vector(-1, -2, -3), Vector(4, 5, 6));
            const Box ub = toUnreal(b);
            CHECK(ub.getVolume() == doctest::Approx(b.getVolume()));
            CHECK(fromUnreal(ub).equals(b, kEps));
        });

    setUpAxis(UpAxis::Y);
    CHECK(toUnreal(Vector(1, 2, 3)).equals(Vector(3, -1, 2), kEps));
    CHECK(scaleToUnreal(Vector(1, 2, 3)).equals(Vector(3, 1, 2), kEps));
    setUpAxis(UpAxis::Z);
    CHECK(toUnreal(Vector(1, 2, 3)).equals(Vector(1, -2, 3), kEps));
    CHECK(scaleToUnreal(Vector(1, 2, 3)).equals(Vector(1, 2, 3), kEps));
    setUpAxis(UpAxis::Y);
}

TEST_CASE("kismet rotators")
{
    forEachUpAxis(
        []
        {
            const Vector dir = Vector::forward() + Vector::right();
            const Rotator look = findLookAtRotation(Vector(1, 1, 1), Vector(1, 1, 1) + dir * 5.0);
            CHECK(look.equals(Rotator(0, 45, 0), 1.0e-9));
            CHECK(findLookAtRotation(Vector::zero(), Vector::up()).pitch == doctest::Approx(90.0));

            for (int i = 0; i < 30; ++i)
            {
                const Rotator r = randRotator();
                const Vector f = r.getForwardVector(), rt = r.getRightVector(), u = r.getUpVector();
                CHECK(makeRotFromXY(f, rt + f * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotFromXZ(f, u + f * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotFromYX(rt, f + rt * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotFromYZ(rt, u + rt * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotFromZX(u, f + u * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotFromZY(u, rt + u * 0.3).equals(r, 1.0e-6));
                CHECK(makeRotationFromAxes(f, rt, u).equals(r, 1.0e-6));
                CHECK(makeRotFromX(f * 3.0).vector().equals(f, 1.0e-9));
                CHECK(makeRotFromY(rt).getRightVector().equals(rt, 1.0e-9));
                CHECK(makeRotFromZ(u).getUpVector().equals(u, 1.0e-9));
            }
            CHECK(makeRotFromZ(Vector::up()).isNearlyZero(1.0e-9));
            CHECK(makeRotFromY(Vector::up()).getRightVector().equals(Vector::up(), 1.0e-9));
            CHECK(makeRotFromZ(Vector::forward()).getUpVector().equals(Vector::forward(), 1.0e-9));
        });
}

TEST_CASE("interp")
{
    CHECK(vinterpTo(Vector(0), Vector(10, 0, 0), 0.1, 5.0).equals(Vector(5, 0, 0), kEps));
    CHECK(vinterpToConstant(Vector(0), Vector(10, 0, 0), 0.5, 4.0).equals(Vector(2, 0, 0), kEps));
    CHECK(vinterpToConstant(Vector(9, 0, 0), Vector(10, 0, 0), 0.5, 4.0).equals(Vector(10, 0, 0), kEps));
    CHECK(rinterpTo(Rotator(0, 170, 0), Rotator(0, -170, 0), 0.1, 5.0).equals(Rotator(0, 180, 0), kEps));
    CHECK(rinterpToConstant(Rotator(0, 0, 0), Rotator(0, 90, 0), 0.5, 20.0).equals(Rotator(0, 10, 0), kEps));
    CHECK(rlerp(Rotator(0, 0, 0), Rotator(0, 90, 0), 0.5, true).equals(Rotator(0, 45, 0), 1.0e-9));
    CHECK(rlerp(Rotator(0, 0, 0), Rotator(0, 270, 0), 0.5, false).equals(Rotator(0, 135, 0), 1.0e-9));
    CHECK(rlerp(Rotator(0, 0, 0), Rotator(0, 270, 0), 0.5, true).equals(Rotator(0, -45, 0), 1.0e-9));
    const Quat target(Vector::zAxis(), 1.0);
    CHECK(qinterpToConstant(Quat::identity(), target, 0.5, 1.0).getAngle() == doctest::Approx(0.5));
    CHECK(qinterpTo(Quat::identity(), target, 0.1, 5.0).getAngle() == doctest::Approx(0.5));
}

TEST_CASE("box")
{
    Box b;
    CHECK_FALSE(b.isValid);
    b += Vector(1, 2, 3);
    b += Vector(-1, 0, 5);
    CHECK(b.isValid);
    CHECK(b.min == Vector(-1, 0, 3));
    CHECK(b.max == Vector(1, 2, 5));
    CHECK(b.getCenter() == Vector(0, 1, 4));
    CHECK(b.getExtent() == Vector(1, 1, 1));
    CHECK(b.getVolume() == doctest::Approx(8.0));
    CHECK(b.isInside(Vector(0, 1, 4)));
    CHECK_FALSE(b.isInside(Vector(1, 1, 4)));
    CHECK(b.isInsideOrOn(Vector(1, 1, 4)));
    const Box other(Vector(1, 1, 1), Vector(3, 3, 3));
    CHECK(b.intersect(other));
    CHECK(b.overlap(other).equals(Box(Vector(1, 1, 3), Vector(1, 2, 3)), kEps));
    CHECK_FALSE(b.intersect(Box(Vector(5), Vector(6))));
    CHECK_FALSE(b.overlap(Box(Vector(5), Vector(6))).isValid);
    CHECK(b.getClosestPointTo(Vector(5, 1, 4)) == Vector(1, 1, 4));
    CHECK(b.computeSquaredDistanceToPoint(Vector(5, 1, 4)) == doctest::Approx(16.0));
    const Box moved = b.transformBy(Transform(Vector(10, 0, 0)));
    CHECK(moved.equals(b.shiftBy(Vector(10, 0, 0)), kEps));
    CHECK(b.expandBy(1).getSize() == Vector(4, 4, 4));
    const Vector pts[] = {Vector(0), Vector(1, 2, 3)};
    CHECK(Box(pts).equals(Box(Vector(0), Vector(1, 2, 3)), kEps));
}

TEST_CASE("plane")
{
    const Plane p(Vector(0, 5, 0), Vector(0, 2, 0));
    CHECK(p.getNormal() == Vector(0, 1, 0));
    CHECK(p.w == doctest::Approx(5.0));
    CHECK(p.planeDot(Vector(3, 7, 1)) == doctest::Approx(2.0));
    CHECK(p.projectPoint(Vector(3, 7, 1)).equals(Vector(3, 5, 1), kEps));
    CHECK(Vector(3, 7, 1).mirrorByPlane(p).equals(Vector(3, 3, 1), kEps));
    CHECK(linePlaneIntersection(Vector(0, 0, 0), Vector(0, 10, 0), p).equals(Vector(0, 5, 0), kEps));
    const Plane tri(Vector(0, 0, 0), Vector(1, 0, 0), Vector(0, 1, 0));
    CHECK(tri.getNormal().equals(Vector(0, 0, 1), kEps));
    const Plane moved = p.transformBy(Matrix::makeTranslation(Vector(0, 3, 0)));
    CHECK(moved.equals(Plane(0, 1, 0, 8), kEps));
    CHECK(p.flip().planeDot(Vector(3, 7, 1)) == doctest::Approx(-2.0));
}

TEST_CASE("color")
{
    CHECK(srgbToLinear(linearToSRGB(0.25f)) == doctest::Approx(0.25f));
    CHECK(LinearColor(Color(255, 0, 0)).equals(LinearColor::red()));
    CHECK(LinearColor(Color(128, 128, 128)).r == doctest::Approx(0.2158605f).epsilon(1e-4));
    for (int i = 0; i < 256; ++i)
    {
        const Color c(uint8_t(i), uint8_t(255 - i), uint8_t(i / 2), uint8_t(i));
        CHECK(LinearColor(c).toColor(true) == c);
    }
    CHECK(LinearColor(0.5f, 0.5f, 0.5f).toColor(false) == Color(128, 128, 128));
    CHECK(LinearColor(0.5f, 0.5f, 0.5f).quantize() == Color(127, 127, 127));
    CHECK(Color(0x12, 0xAB, 0xFF, 0x80).toHex() == "12ABFF80");
    CHECK(Color::fromHex("#12abff") == Color(0x12, 0xAB, 0xFF));
    CHECK(Color::fromHex("f00") == Color(255, 0, 0));
    CHECK_FALSE(Color::fromHex("xyz").has_value());
    CHECK_FALSE(Color::fromHex("12345").has_value());
    CHECK(Color(1, 2, 3, 4).toPackedARGB() == 0x04010203u);

    const LinearColor hsv = LinearColor(0.2f, 0.4f, 0.8f).linearRGBToHSV();
    CHECK(hsv.r == doctest::Approx(220.0f));
    CHECK(hsv.b == doctest::Approx(0.8f));
    CHECK(hsv.hsvToLinearRGB().equals(LinearColor(0.2f, 0.4f, 0.8f), 1.0e-5f));
    CHECK(LinearColor::fromHSV(120.0f, 1.0f, 1.0f).equals(LinearColor::green(), 1.0e-6f));
    CHECK(LinearColor(1, 0, 0).desaturate(1.0f).equals(LinearColor(0.2126f, 0.2126f, 0.2126f), 1.0e-6f));
}

TEST_CASE("vector2 and int types")
{
    CHECK(Vector2(1, 0).getRotated(90).equals(Vector2(0, 1), kEps));
    CHECK((Vector2(1, 0) ^ Vector2(0, 1)) == 1.0);
    CHECK(Vector2(3, 4).size() == doctest::Approx(5.0));
    CHECK(Vector4(1, 2, 3, 4).dot4(Vector4(1, 1, 1, 1)) == doctest::Approx(10.0));
    CHECK(Vector4(3, 0, 4, 9).getSafeNormal().equals(Vector4(0.6, 0, 0.8, 0), kEps));
    CHECK((IntPoint(1, 2) + IntPoint(3, 4)) == IntPoint(4, 6));
    CHECK(IntVector(Vector(1.9, -1.9, 0.5)) == IntVector(1, -1, 0));
}
