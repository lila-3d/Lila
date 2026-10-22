#include "geo/Primitives.h"
#include "uv/LiveSolver.h"
#include "uv/Uv.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <chrono>
#include <cmath>
#include <functional>

using namespace lila;
using geo::Index;

namespace
{
double ms(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/** A quad grid chart (rows x cols quads, two triangles each) over pos(s, t), uvs on a uvSize rectangle. */
uv::Chart gridChart(int rows, int cols, const std::function<glm::dvec3(double, double)>& pos, glm::dvec2 uvSize)
{
    uv::Chart ch;
    for (int i = 0; i <= rows; ++i)
        for (int j = 0; j <= cols; ++j)
        {
            double s = double(j) / cols, t = double(i) / rows;
            ch.pos.push_back(pos(s, t));
            ch.uv.push_back({s * uvSize.x, t * uvSize.y});
            ch.pinned.push_back(0);
            ch.vertexPoint.push_back(Index(ch.vertexPoint.size()));
        }
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
        {
            Index v00 = Index(i * (cols + 1) + j), v10 = v00 + 1, v01 = v00 + Index(cols + 1), v11 = v01 + 1;
            ch.tris.push_back({v00, v10, v11});
            ch.tris.push_back({v00, v11, v01});
        }
    return ch;
}

uv::Chart cylinder(int rows = 8, int cols = 32, glm::dvec2 uvSize = {1.0, 1.0})
{
    return gridChart(rows, cols, [](double s, double t) {
        double a = s * glm::two_pi<double>();
        return glm::dvec3(50.0 * std::cos(a), 100.0 * t, 50.0 * std::sin(a));
    }, uvSize);
}

uv::Chart frustum()
{
    return gridChart(8, 32, [](double s, double t) {
        double a = s * glm::two_pi<double>() * 0.999, r = 50.0 - 30.0 * t;
        return glm::dvec3(r * std::cos(a), 80.0 * t, r * std::sin(a));
    }, {1.0, 1.0});
}

/** A doubly curved patch (part of a sphere) that cannot flatten without distortion. */
uv::Chart cap()
{
    return gridChart(16, 16, [](double s, double t) {
        glm::dvec3 d = glm::normalize(glm::dvec3((s - 0.5) * 1.6, 1.0, (t - 0.5) * 1.6));
        return d * 100.0;
    }, {1.0, 1.0});
}

uv::Chart flatGrid(int n = 10)
{
    return gridChart(n, n, [](double s, double t) { return glm::dvec3(s * 100.0, 0.0, t * 100.0); }, {1.0, 1.0});
}

struct Spread
{
    double compression = 0.0; // area weighted mean of max(0, 1 - smallest singular value)
    double stretch = 0.0;      // area weighted mean of max(0, largest - 1)
};

/** Singular values of each triangle's map from the rest shape (3D at the island's rest scale) to uv. */
Spread spread(const uv::LiveIsland& is)
{
    const uv::Chart& ch = is.chart();
    double s = is.restScale(), total = 0.0;
    Spread out;
    for (const auto& t : ch.tris)
    {
        glm::dvec3 e1 = (ch.pos[t[1]] - ch.pos[t[0]]) * s, e2 = (ch.pos[t[2]] - ch.pos[t[0]]) * s;
        double l1 = glm::length(e1);
        glm::dvec3 x = e1 / l1;
        double px = glm::dot(e2, x), py = glm::length(e2 - px * x);
        // Rest edges as columns, uv edges as columns: J = U P^-1.
        glm::dmat2 P(glm::dvec2(l1, 0.0), glm::dvec2(px, py));
        glm::dmat2 U(ch.uv[t[1]] - ch.uv[t[0]], ch.uv[t[2]] - ch.uv[t[0]]);
        glm::dmat2 J = U * glm::inverse(P);
        glm::dmat2 JtJ = glm::transpose(J) * J;
        double tr = JtJ[0][0] + JtJ[1][1], det = glm::determinant(JtJ);
        double disc = std::sqrt(std::max(0.0, tr * tr * 0.25 - det));
        double s1 = std::sqrt(tr * 0.5 + disc), s2 = std::sqrt(std::max(0.0, tr * 0.5 - disc));
        double a = 0.5 * l1 * py;
        out.compression += a * std::max(0.0, 1.0 - s2);
        out.stretch += a * std::max(0.0, s1 - 1.0);
        total += a;
    }
    out.compression /= total;
    out.stretch /= total;
    return out;
}

double extent(const uv::Chart& ch)
{
    glm::dvec2 d = ch.uvMax() - ch.uvMin();
    return std::max(d.x, d.y);
}
} // namespace

TEST_CASE("live ARAP flattens a cut cylinder to an isometry")
{
    uv::LiveIsland is(cylinder());
    double area0 = is.chart().areaUv();
    int n = is.solve(20000);
    MESSAGE("cylinder: " << n << " iterations, distortion " << uv::chartDistortion(is.chart()));
    CHECK(is.settled());
    CHECK(uv::chartDistortion(is.chart()) < 1e-3);
    CHECK(is.folds() == 0);
    glm::dvec2 d = is.chart().uvMax() - is.chart().uvMin();
    // Unrolled, the side is 2 pi r by h: aspect pi.
    CHECK(d.x / d.y == doctest::Approx(glm::pi<double>()).epsilon(0.02));
    CHECK(is.chart().areaUv() == doctest::Approx(area0).epsilon(0.01));
}

TEST_CASE("live ARAP flattens a cut cone frustum to an isometry")
{
    uv::LiveIsland is(frustum());
    int n = is.solve(20000);
    MESSAGE("frustum: " << n << " iterations, distortion " << uv::chartDistortion(is.chart()));
    CHECK(uv::chartDistortion(is.chart()) < 2e-3);
    CHECK(is.folds() == 0);
}

TEST_CASE("balance trades compression against stretch")
{
    uv::LiveOptions o;
    o.balance = 0.8;
    uv::LiveIsland stretchy(cap(), o);
    stretchy.solve(5000);
    o.balance = -0.8;
    uv::LiveIsland squashy(cap(), o);
    squashy.solve(5000);
    o.balance = 0.0;
    uv::LiveIsland even(cap(), o);
    even.solve(5000);
    Spread a = spread(stretchy), b = spread(squashy), c = spread(even);
    MESSAGE("balance +0.8: compression " << a.compression << " stretch " << a.stretch);
    MESSAGE("balance  0.0: compression " << c.compression << " stretch " << c.stretch);
    MESSAGE("balance -0.8: compression " << b.compression << " stretch " << b.stretch);
    CHECK(a.compression < b.compression);
    CHECK(a.stretch > b.stretch);
    CHECK(a.compression < c.compression);
    CHECK(b.stretch < c.stretch);
    CHECK(stretchy.folds() == 0);
    CHECK(squashy.folds() == 0);
}

TEST_CASE("pinned vertices stay exactly where they were pinned")
{
    uv::LiveIsland is(cylinder());
    Index a = 0, b = Index(is.vertexCount() - 1);
    glm::dvec2 pa = is.chart().uv[a] + glm::dvec2(-0.2, 0.1), pb = is.chart().uv[b] + glm::dvec2(0.5, 0.3);
    is.setUv(a, pa);
    is.setUv(b, pb);
    is.setPinned(a, true);
    is.setPinned(b, true);
    is.solve(3000);
    CHECK(is.chart().uv[a] == pa);
    CHECK(is.chart().uv[b] == pb);
    CHECK(is.folds() == 0);
    // Flatten from scratch keeps them too.
    REQUIRE(is.flatten());
    CHECK(glm::length(is.chart().uv[a] - pa) < 1e-9);
    CHECK(glm::length(is.chart().uv[b] - pb) < 1e-9);
}

TEST_CASE("no folds after a sequence of drags")
{
    uv::LiveIsland is(flatGrid(10));
    is.setPinned(0, true);
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return double(seed >> 8) / double(1u << 24);
    };
    size_t worst = 0;
    for (int round = 0; round < 40; ++round)
    {
        Index v = Index(rnd() * is.vertexCount()) % is.vertexCount();
        if (is.pinned(v))
            continue;
        glm::dvec2 to = is.chart().uv[v] + glm::dvec2(rnd() - 0.5, rnd() - 0.5) * 1.5;
        is.setTarget(v, to, 1.0);
        is.wake();
        for (int k = 0; k < 15; ++k)
        {
            is.step();
            worst = std::max(worst, is.folds());
        }
        is.clearTargets();
        is.wake();
        for (int k = 0; k < 5; ++k)
        {
            is.step();
            worst = std::max(worst, is.folds());
        }
    }
    CHECK(worst == 0);
}

TEST_CASE("warm start from a settled solve is idempotent")
{
    uv::LiveIsland first(cylinder());
    first.solve(20000);
    REQUIRE(first.settled());
    uv::LiveIsland again(uv::Chart(first.chart()));
    int n = again.solve(20000);
    double moved = 0.0;
    for (Index v = 0; v < again.vertexCount(); ++v)
        moved = std::max(moved, glm::length(again.chart().uv[v] - first.chart().uv[v]));
    MESSAGE("re-solve: " << n << " iterations, moved " << moved);
    CHECK(n <= 3);
    CHECK(moved < 1e-4 * extent(first.chart()));
}

TEST_CASE("the global matrix is factorised only when pins, stiffness or the target set change")
{
    uv::LiveIsland is(cylinder());
    is.wake();
    for (int i = 0; i < 5; ++i)
        is.step();
    CHECK(is.factorizations() == 1);
    is.setTarget(5, {0.3, 0.3}, 1.0);
    is.step();
    CHECK(is.factorizations() == 2);
    for (int i = 0; i < 5; ++i)
    {
        is.setTarget(5, {0.3 + 0.01 * i, 0.3}, 1.0); // moving a drag only changes the right-hand side
        is.step();
    }
    CHECK(is.factorizations() == 2);
    is.setPinned(7, true);
    is.step();
    CHECK(is.factorizations() == 3);
    is.setStiffness(9, 4.0);
    is.step();
    CHECK(is.factorizations() == 4);
    is.clearTargets();
    is.step();
    CHECK(is.factorizations() == 5);
}

TEST_CASE("brushes")
{
    uv::LiveIsland is(flatGrid(10));
    const uv::Chart& ch = is.chart();
    uv::LiveDab d;
    d.center = {0.5, 0.5};
    d.radius = 0.25;
    d.strength = 1.0;

    d.brush = uv::LiveBrush::Pin;
    CHECK(is.applyDab(d));
    size_t pinned = 0;
    for (Index v = 0; v < is.vertexCount(); ++v)
        pinned += is.pinned(v);
    CHECK(pinned > 0);
    CHECK(!is.pinned(0));
    d.brush = uv::LiveBrush::Unpin;
    is.applyDab(d);
    pinned = 0;
    for (Index v = 0; v < is.vertexCount(); ++v)
        pinned += is.pinned(v);
    CHECK(pinned == 0);

    d.brush = uv::LiveBrush::Stiffen;
    is.applyDab(d);
    Index mid = 5 * 11 + 5;
    CHECK(is.stiffness()[mid] > 1.0f);
    CHECK(is.stiffness()[0] == 1.0f);

    glm::dvec2 before = ch.uv[mid];
    d.brush = uv::LiveBrush::Grab;
    d.first = true;
    d.delta = {0.0, 0.0};
    is.applyDab(d);
    d.first = false;
    d.delta = {0.05, 0.0};
    is.applyDab(d);
    CHECK(ch.uv[mid].x == doctest::Approx(before.x + 0.05).epsilon(1e-6));
    CHECK(is.hasTargets());
    is.clearTargets();

    d.brush = uv::LiveBrush::Pinch;
    glm::dvec2 off = ch.uv[mid + 1];
    is.applyDab(d);
    CHECK(glm::length(ch.uv[mid + 1] - d.center) < glm::length(off - d.center));

    // Straighten along y = 0.5 pulls a bumped row back onto it.
    is.setUv(mid + 1, ch.uv[mid + 1] + glm::dvec2(0.0, 0.03));
    double dev = std::abs(ch.uv[mid + 1].y - 0.5);
    d.brush = uv::LiveBrush::Straighten;
    d.first = true;
    d.lineFrom = {0.2, 0.5};
    d.center = {0.8, 0.5};
    d.radius = 0.5;
    is.applyDab(d);
    CHECK(std::abs(ch.uv[mid + 1].y - 0.5) < dev);

    // Smooth Boundary flattens a dent in the border.
    Index edge = 5; // bottom row, middle
    is.setUv(edge, ch.uv[edge] + glm::dvec2(0.0, 0.04));
    double dent = std::abs(ch.uv[edge].y - ch.uv[edge - 1].y);
    d.brush = uv::LiveBrush::SmoothBoundary;
    d.first = false;
    d.center = ch.uv[edge];
    d.radius = 0.15;
    is.applyDab(d);
    CHECK(std::abs(ch.uv[edge].y - ch.uv[edge - 1].y) < dent);

    d.brush = uv::LiveBrush::Relax;
    d.center = {0.5, 0.5};
    d.radius = 0.4;
    is.applyDab(d);
    d.brush = uv::LiveBrush::Flatten;
    is.applyDab(d);
    CHECK(is.folds() == 0);

    // On the surface: a dab in 3D reaches the uvs of the vertices near it.
    uv::LiveDab s;
    s.surface = true;
    s.brush = uv::LiveBrush::Pin;
    s.center3 = {50.0, 0.0, 50.0};
    s.radius = 12.0;
    s.strength = 1.0;
    CHECK(is.applyDab(s));
    CHECK(is.pinned(mid));
    CHECK(!is.pinned(0));
}

TEST_CASE("live islands split along seams, and wedges split along a slit")
{
    geo::PlaneOptions po;
    po.subdivX = 4;
    po.subdivZ = 4;
    geo::Mesh m = geo::makePlane(po);
    if (m.cornerAttrs().get<glm::vec2>("uv0").empty())
        uv::project(m, {}, "uv0", {});
    REQUIRE(uv::liveIslands(m, {}, "uv0").size() == 1);
    std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    // Edges on the line x = 0 (the middle), all of them, then only the two from one border.
    std::vector<Index> line;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            if (std::abs(a.x) < 1e-6 && std::abs(b.x) < 1e-6)
                line.push_back(e);
        }
    REQUIRE(line.size() == 4);
    for (Index e : line)
        seam[e] = true;
    CHECK(uv::liveIslands(m, {}, "uv0").size() == 2);
    std::fill(seam.begin(), seam.end(), false);
    std::sort(line.begin(), line.end(), [&](Index a, Index b) {
        return std::min(m.position(m.edgePoint(a, 0)).z, m.position(m.edgePoint(a, 1)).z) <
               std::min(m.position(m.edgePoint(b, 0)).z, m.position(m.edgePoint(b, 1)).z);
    });
    seam[line[0]] = seam[line[1]] = true;
    std::vector<uv::LiveIsland> slit = uv::liveIslands(m, {}, "uv0");
    REQUIRE(slit.size() == 1);
    // A slit of two edges from the border splits the two points along it that are not its tip.
    CHECK(slit[0].vertexCount() == Index(m.numPoints() + 2));
}

TEST_CASE("threaded solver publishes frames and goes idle")
{
    uv::LiveSolver solver;
    std::vector<uv::LiveIsland> islands;
    islands.emplace_back(cylinder());
    islands.emplace_back(frustum());
    solver.load(std::move(islands));
    solver.edit([](std::vector<uv::LiveIsland>& is) {
        for (auto& i : is)
            i.wake();
    });
    REQUIRE(solver.waitIdle(30000));
    auto f = solver.frame();
    CHECK(f->idle);
    CHECK(f->epoch == 1);
    REQUIRE(f->islands.size() == 2);
    CHECK(f->islands[0]->settled);
    uint64_t gen = solver.generation();
    // Settled: nothing runs, nothing is published.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(solver.generation() == gen);
    // sync runs on the worker and the frame reflects it.
    solver.sync([](std::vector<uv::LiveIsland>& is) { is[0].setPinned(0, true); });
    CHECK(solver.frame()->islands[0]->pinned[0] == 1);
    CHECK(solver.frame()->islands[1] == f->islands[1]); // unchanged islands are shared, not copied
}

TEST_CASE("benchmark: live iterations on a 50k triangle island")
{
    // A bumpy 158 x 158 quad patch: 49928 triangles.
    const int n = 158;
    auto t0 = std::chrono::steady_clock::now();
    uv::LiveIsland is(gridChart(n, n, [](double s, double t) {
        return glm::dvec3(s * 200.0, 12.0 * std::sin(s * 9.0) * std::cos(t * 7.0), t * 200.0);
    }, {1.0, 1.0}));
    double build = ms(t0);
    REQUIRE(is.chart().tris.size() == size_t(2 * n * n));
    is.wake();
    t0 = std::chrono::steady_clock::now();
    is.step();
    double first = ms(t0);
    const int iters = 30;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        is.step();
    double per = ms(t0) / iters;
    MESSAGE("50k-triangle island: setup " << build << " ms, first iteration (pattern + factorisation) " << first << " ms, then " << per
                                          << " ms per iteration (" << 1000.0 / per << " it/s)");
    CHECK(is.folds() == 0);
    CHECK(is.factorizations() == 1);
}
