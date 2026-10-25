#include <doctest/doctest.h>

#include "deform/Deform.h"
#include "deform/Handles.h"
#include "deform/Shape.h"
#include "deform/Surface.h"
#include "geo/Primitives.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <random>

using namespace lila;
using namespace lila::deform;

namespace
{
bool near(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-9) { return glm::length(a - b) <= eps; }

std::vector<glm::dvec3> randomPoints(size_t n, double lo, double hi, unsigned seed = 11)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(lo, hi);
    std::vector<glm::dvec3> out(n);
    for (auto& p : out)
        p = {d(rng), d(rng), d(rng)};
    return out;
}

std::vector<glm::dvec3> pointsOf(const geo::Mesh& m) { return {m.positions().begin(), m.positions().end()}; }

/** A closed cube cage of half size h, triangulated, outward facing. */
geo::Mesh cubeCage(double h)
{
    geo::Mesh m = geo::makeCube({.width = 2 * h, .height = 2 * h, .depth = 2 * h});
    return m;
}

glm::dmat3 jacobian(const std::function<glm::dvec3(const glm::dvec3&)>& f, const glm::dvec3& x, double h = 1e-5)
{
    glm::dmat3 J;
    for (int k = 0; k < 3; ++k)
    {
        glm::dvec3 e(0.0);
        e[k] = h;
        J[k] = (f(x + e) - f(x - e)) / (2.0 * h);
    }
    return J;
}

double maxEdgeError(const geo::Mesh& m, std::span<const glm::dvec3> a, std::span<const glm::dvec3> b)
{
    double worst = 0.0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            geo::Index i = m.edgePoint(e, 0), j = m.edgePoint(e, 1);
            worst = std::max(worst, std::abs(glm::length(a[i] - a[j]) - glm::length(b[i] - b[j])) / glm::length(a[i] - a[j]));
        }
    return worst;
}

double meanEdgeError(const geo::Mesh& m, std::span<const glm::dvec3> a, std::span<const glm::dvec3> b)
{
    double sum = 0.0;
    size_t count = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            geo::Index i = m.edgePoint(e, 0), j = m.edgePoint(e, 1);
            sum += std::abs(glm::length(a[i] - a[j]) - glm::length(b[i] - b[j])) / glm::length(a[i] - a[j]);
            ++count;
        }
    return count ? sum / double(count) : 0.0;
}
} // namespace

TEST_CASE("kelvinlet grab matches the closed form and moves the centre exactly")
{
    const double eps = 2.0, nu = 0.3;
    const glm::dvec3 r(1.0, -0.5, 2.0), f(0.3, 1.0, -0.2);
    const double a = 1.0 / (4.0 * glm::pi<double>()), b = a / (4.0 * (1.0 - nu));
    const double re = std::sqrt(glm::dot(r, r) + eps * eps);
    glm::dvec3 expect = ((a - b) / re) * f + (b / (re * re * re)) * glm::dot(r, f) * r + (a * eps * eps / (2.0 * re * re * re)) * f;
    CHECK(near(kelvinletGrab(r, f, eps, nu), expect, 1e-15));

    for (int scales = 1; scales <= 3; ++scales)
    {
        KelvinletBrush brush{eps, nu, scales};
        CAPTURE(scales);
        CHECK(near(kelvinletGrabBrush(glm::dvec3(0.0), {1, 2, 3}, brush), {1, 2, 3}, 1e-12));
    }
    // More scales fade faster.
    glm::dvec3 far(20.0, 0.0, 0.0);
    double u1 = glm::length(kelvinletGrabBrush(far, {0, 1, 0}, {eps, nu, 1}));
    double u2 = glm::length(kelvinletGrabBrush(far, {0, 1, 0}, {eps, nu, 2}));
    double u3 = glm::length(kelvinletGrabBrush(far, {0, 1, 0}, {eps, nu, 3}));
    CHECK(u2 < u1 * 0.2);
    CHECK(u3 < u2);
}

TEST_CASE("kelvinlet: incompressible grab is divergence free, compressible is not")
{
    for (double nu : {0.5, 0.2})
    {
        KelvinletBrush brush{1.5, nu, 1};
        auto f = [&](const glm::dvec3& x) { return kelvinletGrabBrush(x, {0.0, 1.0, 0.0}, brush); };
        glm::dmat3 J = jacobian(f, {0.7, 0.4, -0.3});
        double div = J[0][0] + J[1][1] + J[2][2];
        CAPTURE(nu);
        if (nu == 0.5)
            CHECK(std::abs(div) < 1e-7);
        else
            CHECK(std::abs(div) > 1e-3);
    }
}

TEST_CASE("kelvinlet affine loads: twist, scale and pinch have the asked gradient at the centre")
{
    const double eps = 1.0, nu = 0.35;
    // The twist kernel is -a (1/re^3 + 3 eps^2 / (2 re^5)) q x r.
    const glm::dvec3 q(0.2, -0.4, 0.9), r(0.5, 1.5, -1.0);
    glm::dmat3 W(0.0);
    // glm is column-major: W[column][row] holds the cross product matrix [q]x.
    W[0][1] = q.z, W[0][2] = -q.y, W[1][0] = -q.z, W[1][2] = q.x, W[2][0] = q.y, W[2][1] = -q.x;
    const double a = 1.0 / (4.0 * glm::pi<double>());
    const double re = std::sqrt(glm::dot(r, r) + eps * eps);
    const double k = 1.0 / std::pow(re, 3) + 1.5 * eps * eps / std::pow(re, 5);
    CHECK(near(kelvinletAffine(r, W, eps, nu), -a * k * glm::cross(q, r), 1e-14));

    KelvinletBrush brush{eps, nu, 1};
    const glm::dvec3 axis = glm::normalize(glm::dvec3(1, 2, 2));
    const double small = 1e-4;
    glm::dmat3 J = jacobian([&](const glm::dvec3& x) { return kelvinletTwistBrush(x, axis * small, brush); }, glm::dvec3(0.0), 1e-4);
    glm::dmat3 expect(0.0);
    expect[0][1] = axis.z * small, expect[0][2] = -axis.y * small, expect[1][0] = -axis.z * small;
    expect[1][2] = axis.x * small, expect[2][0] = axis.y * small, expect[2][1] = -axis.x * small;
    // The rotation's own second-order term, (cos - 1), is about angle^2 / 2 = 5e-9.
    for (int c = 0; c < 3; ++c)
        CHECK(near(J[c], expect[c], 2e-8));
    // Large twists rotate: lengths from the centre are kept.
    glm::dvec3 p(0.3, 0.2, 0.1);
    CHECK(glm::length(p + kelvinletTwistBrush(p, axis * 2.0, brush)) == doctest::Approx(glm::length(p)));

    J = jacobian([&](const glm::dvec3& x) { return kelvinletScaleBrush(x, 0.25, brush); }, glm::dvec3(0.0));
    for (int c = 0; c < 3; ++c)
        CHECK(near(J[c], glm::dmat3(0.25)[c], 1e-8));

    glm::dmat3 G(0.0);
    G[0][0] = 0.2, G[1][1] = -0.3, G[2][2] = 0.1, G[1][0] = G[0][1] = 0.05;
    for (int scales = 1; scales <= 3; ++scales)
    {
        KelvinletBrush b{eps, nu, scales};
        J = jacobian([&](const glm::dvec3& x) { return kelvinletPinchBrush(x, G, b); }, glm::dvec3(0.0), 1e-4);
        for (int c = 0; c < 3; ++c)
            CHECK(near(J[c], G[c], 1e-6));
    }
}

TEST_CASE("kelvinlet motion splits a handle transform into its parts")
{
    glm::dmat3 R = glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 0, 1)));
    KelvinletMotion m = kelvinletMotion(R * 1.2, {1, 2, 3}, {});
    CHECK(near(m.grab, {1, 2, 3}));
    CHECK(near(m.twist, {0, 0, glm::radians(30.0)}, 1e-12));
    CHECK(m.scale == doctest::Approx(0.2));
    for (int c = 0; c < 3; ++c)
        CHECK(near(m.pinch[c], glm::dvec3(0.0), 1e-12));
    KelvinletMotion none = kelvinletMotion(glm::dmat3(1.0), glm::dvec3(0.0), {});
    CHECK(near(kelvinletDisplacement({0.4, 0.1, -2.0}, none, {}), glm::dvec3(0.0), 1e-15));
}

TEST_CASE("Green coordinates reproduce the point at rest and similarity maps of the cage")
{
    geo::Mesh cage = cubeCage(1.0);
    std::vector<glm::dvec3> rest = pointsOf(cage);
    std::vector<uint32_t> tris = trianglePoints(cage);
    const size_t nv = rest.size(), nf = tris.size() / 3;
    glm::dmat4 sim = glm::translate(glm::dmat4(1.0), glm::dvec3(3, -1, 2)) * glm::rotate(glm::dmat4(1.0), 0.7, glm::normalize(glm::dvec3(1, 1, 0))) *
                     glm::scale(glm::dmat4(1.0), glm::dvec3(1.7));
    std::vector<glm::dvec3> moved(nv);
    for (size_t i = 0; i < nv; ++i)
        moved[i] = glm::dvec3(sim * glm::dvec4(rest[i], 1.0));
    std::vector<glm::dvec3> faceDelta = greenFaceTerms(rest, moved, tris);
    for (const glm::dvec3& x : randomPoints(40, -0.9, 0.9))
    {
        std::vector<double> phi(nv), psi(nf);
        greenCoordinates(rest, tris, x, phi, psi);
        glm::dvec3 back(0.0);
        for (size_t i = 0; i < nv; ++i)
            back += phi[i] * rest[i];
        for (size_t t = 0; t < nf; ++t)
        {
            glm::dvec3 n = glm::normalize(glm::cross(rest[tris[t * 3 + 1]] - rest[tris[t * 3]], rest[tris[t * 3 + 2]] - rest[tris[t * 3]]));
            back += psi[t] * n;
        }
        CHECK(near(back, x, 1e-8));
        glm::dvec3 u(0.0);
        for (size_t i = 0; i < nv; ++i)
            u += phi[i] * (moved[i] - rest[i]);
        for (size_t t = 0; t < nf; ++t)
            u += psi[t] * faceDelta[t];
        CHECK(near(x + u, glm::dvec3(sim * glm::dvec4(x, 1.0)), 1e-7));
    }
}

TEST_CASE("Somigliana coordinates reproduce linear elastic fields and translations")
{
    geo::Mesh cage = cubeCage(1.0);
    std::vector<glm::dvec3> rest = pointsOf(cage);
    std::vector<uint32_t> tris = trianglePoints(cage);
    const size_t nv = rest.size(), nf = tris.size() / 3;
    const double nu = 0.3, lambda = 2.0 * nu / (1.0 - 2.0 * nu);
    // A symmetric linear field u = A y is an elastic solution, so the identity holds with traction sigma(A) n.
    glm::dmat3 A(0.0);
    A[0][0] = 0.01, A[1][1] = -0.02, A[2][2] = 0.015, A[1][0] = A[0][1] = 0.005, A[2][0] = A[0][2] = -0.004;
    glm::dmat3 sigma = lambda * (A[0][0] + A[1][1] + A[2][2]) * glm::dmat3(1.0) + 2.0 * A;
    for (const glm::dvec3& x : randomPoints(12, -0.6, 0.6, 5))
    {
        std::vector<double> C(nv * 9), D(nf * 9);
        somiglianaCoordinates(rest, tris, x, nu, C, D);
        auto apply = [](const double* m, const glm::dvec3& v) {
            return glm::dvec3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z);
        };
        glm::dvec3 shift(0.0), u(0.0);
        for (size_t i = 0; i < nv; ++i)
        {
            shift += apply(&C[i * 9], {1.0, -2.0, 0.5});
            u += apply(&C[i * 9], A * rest[i]);
        }
        CHECK(near(shift, {1.0, -2.0, 0.5}, 1e-12));
        for (size_t t = 0; t < nf; ++t)
        {
            glm::dvec3 n = glm::normalize(glm::cross(rest[tris[t * 3 + 1]] - rest[tris[t * 3]], rest[tris[t * 3 + 2]] - rest[tris[t * 3]]));
            u += apply(&D[t * 9], sigma * n);
        }
        CAPTURE(x);
        CHECK(glm::length(u - A * x) < 2e-3 * glm::length(A * x) + 1e-5);
    }
    // Tractions vanish at rest and under rigid motion.
    glm::dmat3 rot = glm::dmat3(glm::rotate(glm::dmat4(1.0), 1.1, glm::dvec3(0, 1, 0)));
    std::vector<glm::dvec3> moved(nv);
    for (size_t i = 0; i < nv; ++i)
        moved[i] = rot * rest[i] + glm::dvec3(4, 0, 1);
    RigidFit fit = rigidFit(rest, moved);
    glm::dmat3 fitR = fit.rotation;
    glm::dvec3 fitT = fit.to - fit.rotation * fit.from;
    for (int c = 0; c < 3; ++c)
        CHECK(near(fitR[c], rot[c], 1e-12));
    CHECK(near(fitT, {4, 0, 1}, 1e-12));
    for (const glm::dvec3& t : somiglianaTractions(rest, moved, tris, fitR, fitT, {}))
        CHECK(near(t, glm::dvec3(0.0), 1e-12));

    // A cage squashed linearly carries its inside exactly at bulge 0, and bulges it out above.
    glm::dmat3 squash(1.0);
    squash[1][1] = 0.6;
    for (size_t i = 0; i < nv; ++i)
        moved[i] = squash * rest[i];
    for (double bulge : {0.0, 1.0})
    {
        std::vector<glm::dvec3> tr = somiglianaTractions(rest, moved, tris, glm::dmat3(1.0), glm::dvec3(0.0), {nu, bulge});
        glm::dvec3 x(0.5, 0.2, 0.0);
        std::vector<double> C(nv * 9), D(nf * 9);
        somiglianaCoordinates(rest, tris, x, nu, C, D);
        glm::dvec3 u(0.0);
        for (size_t i = 0; i < nv; ++i)
            for (int r = 0; r < 3; ++r)
                u[r] += C[i * 9 + r * 3] * (moved[i] - rest[i]).x + C[i * 9 + r * 3 + 1] * (moved[i] - rest[i]).y + C[i * 9 + r * 3 + 2] * (moved[i] - rest[i]).z;
        for (size_t t = 0; t < nf; ++t)
            for (int r = 0; r < 3; ++r)
                u[r] += D[t * 9 + r * 3] * tr[t].x + D[t * 9 + r * 3 + 1] * tr[t].y + D[t * 9 + r * 3 + 2] * tr[t].z;
        if (bulge == 0.0)
            CHECK(near(x + u, squash * x, 1e-3));
        else
            CHECK((x + u).x > x.x + 0.01);
    }
}

TEST_CASE("shear, cast, Taubin smoothing, noise and tension")
{
    CHECK(near(shear({1, 0.5, 2}, {0.5, -1.0, -1.0, 1.0}), {1.25, 0.5, 1.5}));
    CHECK(near(shear({1, 3, 2}, {0.5, 0.0, -1.0, 1.0}), {1.5, 3, 2}));

    std::vector<glm::dvec3> pts = randomPoints(50, -2.0, 2.0);
    std::vector<glm::dvec3> sphere = pts;
    cast(sphere, CastShape::Sphere, 1.0, 3.0);
    for (const auto& p : sphere)
        CHECK(glm::length(p) == doctest::Approx(3.0));
    std::vector<glm::dvec3> box = pts;
    cast(box, CastShape::Box, 1.0, 1.0);
    for (const auto& p : box)
        CHECK(std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) == doctest::Approx(1.0));
    std::vector<glm::dvec3> half = pts;
    cast(half, CastShape::Cylinder, 0.0, 0.0);
    CHECK(near(half[3], pts[3]));

    // Taubin keeps a sphere's size where plain Laplacian smoothing shrinks it.
    geo::Mesh ball = geo::makeSphere({.radius = 10.0, .subdivAxis = 24, .subdivHeight = 16});
    Adjacency adj = pointAdjacency(ball);
    std::vector<glm::dvec3> lap = pointsOf(ball), tau = pointsOf(ball);
    smooth(lap, adj, 10, 0.5, false);
    smoothTaubin(tau, adj, 10, 0.5, 0.1, false);
    auto meanRadius = [](const std::vector<glm::dvec3>& v) {
        double s = 0.0;
        for (const auto& p : v)
            s += glm::length(p);
        return s / double(v.size());
    };
    CHECK(meanRadius(lap) < 9.5);
    CHECK(std::abs(meanRadius(tau) - 10.0) < 0.3);

    CHECK(noise3({1.3, 2.7, -0.4}, 7) == noise3({1.3, 2.7, -0.4}, 7));
    CHECK(noise3({1.3, 2.7, -0.4}, 7) != noise3({1.3, 2.7, -0.4}, 8));
    CHECK(noise3({2, 3, 4}, 1) == doctest::Approx(0.0));
    for (const auto& p : pts)
        CHECK(std::abs(fractalNoise(p, 3, 4)) <= 1.5);

    std::vector<glm::dvec3> rest = pointsOf(ball);
    for (float t : tension(ball, rest, rest))
        CHECK(t == 0.0f);
    std::vector<glm::dvec3> grown = rest;
    for (auto& p : grown)
        p *= 1.1;
    for (float t : tension(ball, rest, grown))
        CHECK(t == doctest::Approx(0.1).epsilon(1e-6));
}

TEST_CASE("handle solver: identity at rest, rigid moves are exact, ARAP keeps lengths better than Laplacian")
{
    geo::Mesh tube = geo::makeCylinder({.radius = 5.0, .height = 60.0, .subdivAxis = 16, .subdivHeight = 24});
    std::vector<glm::dvec3> rest = pointsOf(tube);
    const size_t n = rest.size();
    std::vector<int> group(n, -1);
    for (size_t p = 0; p < n; ++p)
        if (rest[p].y < -29.0)
            group[p] = 0;
        else if (rest[p].y > 29.0)
            group[p] = 1;
    for (auto mode : {HandleSolver::Mode::Laplacian, HandleSolver::Mode::Arap})
    {
        CAPTURE(int(mode));
        HandleSolver s;
        REQUIRE(s.prepare(tube, rest, group, mode));
        std::vector<glm::dvec3> out = s.solve(rest, {}, 5);
        for (size_t p = 0; p < n; ++p)
            CHECK(near(out[p], rest[p], 1e-7));
        glm::dmat4 shift = glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 0));
        std::vector<glm::dmat4> both = {shift, shift};
        out = s.solve(rest, both, 3);
        for (size_t p = 0; p < n; ++p)
            CHECK(near(out[p], rest[p] + glm::dvec3(4, 0, 0), 1e-6));
        if (mode == HandleSolver::Mode::Arap)
        {
            // Both ends under one rigid motion: the whole tube follows it.
            glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(5, 2, -1)) * glm::rotate(glm::dmat4(1.0), 0.8, glm::normalize(glm::dvec3(1, 0, 1)));
            both = {m, m};
            out = s.solve(rest, both, 2);
            CHECK(maxEdgeError(tube, rest, out) < 1e-9);
            // One end alone moving rigidly carries everything too.
            std::vector<int> top(n, -1);
            for (size_t p = 0; p < n; ++p)
                if (rest[p].y > 29.0)
                    top[p] = 0;
            HandleSolver single;
            REQUIRE(single.prepare(tube, rest, top, mode));
            std::vector<glm::dmat4> one = {m};
            out = single.solve(rest, one, 2);
            for (size_t p = 0; p < n; ++p)
                CHECK(near(out[p], glm::dvec3(m * glm::dvec4(rest[p], 1.0)), 1e-8));
        }
    }
    // Bend into a quarter circle of the tube's length: the top turns 90 degrees, the bottom stays.
    const double r = 60.0 / glm::half_pi<double>();
    glm::dmat4 bend = glm::translate(glm::dmat4(1.0), glm::dvec3(r, r - 30.0, 0)) * glm::rotate(glm::dmat4(1.0), glm::half_pi<double>(), glm::dvec3(0, 0, -1)) *
                      glm::translate(glm::dmat4(1.0), glm::dvec3(0, -30, 0));
    std::vector<glm::dmat4> ends = {glm::dmat4(1.0), bend};
    HandleSolver lap, arap;
    REQUIRE(lap.prepare(tube, rest, group, HandleSolver::Mode::Laplacian));
    REQUIRE(arap.prepare(tube, rest, group, HandleSolver::Mode::Arap));
    double lapErr = meanEdgeError(tube, rest, lap.solve(rest, ends, 0));
    double arapErr = meanEdgeError(tube, rest, arap.solve(rest, ends, 20));
    CAPTURE(lapErr);
    CAPTURE(arapErr);
    // A bent tube must stretch outside and squash inside, so neither reaches 0; ARAP spreads it more evenly.
    CHECK(arapErr < lapErr * 0.75);
}

TEST_CASE("bounded biharmonic weights are bounded, partition unity and interpolate the handles")
{
    geo::Mesh sheet = geo::makePlane({.width = 100.0, .depth = 100.0, .subdivX = 20, .subdivZ = 20});
    std::vector<glm::dvec3> pts = pointsOf(sheet);
    auto nearest = [&](const glm::dvec3& at) {
        uint32_t best = 0;
        for (uint32_t p = 0; p < pts.size(); ++p)
            if (glm::length(pts[p] - at) < glm::length(pts[best] - at))
                best = p;
        return best;
    };
    std::vector<std::vector<uint32_t>> handles = {{nearest({-40, 0, -40})}, {nearest({40, 0, -40})}, {nearest({0, 0, 40})}};
    std::vector<float> w = boundedBiharmonicWeights(sheet, pts, handles);
    REQUIRE(w.size() == pts.size() * 3);
    for (size_t p = 0; p < pts.size(); ++p)
    {
        double sum = 0.0;
        for (size_t k = 0; k < 3; ++k)
        {
            CHECK(w[p * 3 + k] >= 0.0f);
            CHECK(w[p * 3 + k] <= 1.0f);
            sum += w[p * 3 + k];
        }
        CHECK(sum == doctest::Approx(1.0).epsilon(1e-5));
    }
    for (size_t k = 0; k < 3; ++k)
        CHECK(w[handles[k][0] * 3 + k] == doctest::Approx(1.0));
    uint32_t mid = nearest({0, 0, -40});
    CHECK(w[mid * 3 + 0] == doctest::Approx(w[mid * 3 + 1]).epsilon(0.05));
    CHECK(w[mid * 3 + 0] > 0.2f);
}

TEST_CASE("harmonic coordinates partition unity, are positive and symmetric in a cube")
{
    geo::Mesh cage = cubeCage(1.0);
    std::vector<glm::dvec3> rest = pointsOf(cage);
    std::vector<uint32_t> tris = trianglePoints(cage);
    std::vector<glm::dvec3> pts = randomPoints(20, -0.8, 0.8, 3);
    pts.push_back(glm::dvec3(0.0));
    std::vector<float> w = harmonicCoordinates(rest, tris, pts, 24);
    const size_t nv = rest.size();
    for (size_t p = 0; p < pts.size(); ++p)
    {
        double sum = 0.0;
        glm::dvec3 back(0.0);
        for (size_t v = 0; v < nv; ++v)
        {
            CHECK(w[p * nv + v] >= -1e-6f);
            sum += w[p * nv + v];
            back += double(w[p * nv + v]) * rest[v];
        }
        CHECK(sum == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(glm::length(back - pts[p]) < 0.15);
    }
    for (size_t v = 0; v < nv; ++v)
        CHECK(w[(pts.size() - 1) * nv + v] == doctest::Approx(1.0 / double(nv)).epsilon(0.05));
}
