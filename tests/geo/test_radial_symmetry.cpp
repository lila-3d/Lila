#include "TestUtil.h"

#include "geo/Knife.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"

#include <random>

using namespace lila::geo;
using lila::geo::test::boundaryEdgeCount;
using lila::geo::test::isValid;

namespace
{
/** 24 columns: every count tested (3, 6, 8) divides it, and its vertex columns lie on every wedge and mirror plane. */
Mesh sphere24()
{
    SphereOptions o;
    o.subdivAxis = 24;
    o.subdivHeight = 8;
    return makeSphere(o);
}

struct Case
{
    int count;
    bool mirror;
};
const Case kCases[] = {{3, false}, {6, false}, {8, false}, {6, true}};

std::string name(const Case& c)
{
    return (c.mirror ? "D" : "C") + std::to_string(c.count);
}

RadialSymmetry groupOf(const Case& c)
{
    return axisRadial(1, glm::dvec3(0.0), c.count, c.mirror);
}

/** Largest distance between a point's image under any element and that element applied to the point. */
double asymmetry(const Mesh& m, const SymmetryMap& map)
{
    double worst = 0.0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            for (int g = 0; g < map.order(); ++g)
                if (const Index q = map.image(g, p); q != kInvalid)
                    worst = std::max(worst, glm::length(m.position(q) - map.transform(g, m.position(p))));
    return worst;
}

Index validFaces(const Mesh& m)
{
    return m.numFaces();
}

/** The face whose centre lies furthest along +x, so selections start off every mirror plane. */
Index faceOffPlanes(const Mesh& m, const RadialSymmetry& r)
{
    Index best = kInvalid;
    double bestScore = -1.0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceSize(f) != 4)
            continue;
        glm::dvec3 c(0.0);
        for (Index p : m.facePoints(f))
            c += m.position(p) * 0.25;
        // Middle of the fundamental wedge, away from the poles.
        const double a = r.angle(c), mid = r.wedgeAngle() * 0.5;
        const double score = r.axisDistance(c) - 10.0 * std::abs(a - mid);
        if (score > bestScore)
        {
            bestScore = score;
            best = f;
        }
    }
    return best;
}
} // namespace

TEST_CASE("radial symmetry group: compose, inverse and apply agree")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        const RadialSymmetry r = groupOf(c);
        CHECK(r.order() == (c.mirror ? 2 : 1) * c.count);
        const glm::dvec3 p(13.0, -4.0, 7.5);
        for (int a = 0; a < r.order(); ++a)
        {
            CHECK(r.compose(a, r.inverse(a)) == 0);
            CHECK(glm::length(r.apply(r.inverse(a), r.apply(a, p)) - p) < 1e-9);
            for (int b = 0; b < r.order(); ++b)
                CHECK(glm::length(r.apply(r.compose(a, b), p) - r.apply(a, r.apply(b, p))) < 1e-9);
        }
        // Each element's wedge holds its image of a point in the fundamental wedge.
        const double inside = r.wedgeAngle() * 0.3;
        const glm::dvec3 q = 20.0 * (std::cos(inside) * r.reference() + std::sin(inside) * glm::cross(r.axis(), r.reference()));
        CHECK(r.wedgeOf(q) == 0);
        for (int g = 0; g < r.order(); ++g)
            CHECK(r.wedgeOf(r.apply(g, q)) == g);
    }
    // Quarter turns about a coordinate axis are exact.
    const RadialSymmetry four = axisRadial(1, glm::dvec3(0.0), 4, false);
    CHECK(four.apply(1, glm::dvec3(0.1, 0.3, 0.7)) == glm::dvec3(0.7, 0.3, -0.1));
}

TEST_CASE("radial map by position: orbits, axis points and mirror-plane points")
{
    const Mesh m = sphere24();
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        const RadialSymmetry r = groupOf(c);
        const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
        CHECK(map.isRadial());
        CHECK(map.order() == r.order());
        CHECK(map.unmatchedCount() == 0);
        CHECK(asymmetry(m, map) < 1e-9);
        Index onAxis = 0, onMirror = 0;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            if (r.axisDistance(m.position(p)) < 1e-9)
            {
                ++onAxis;
                CHECK(map.fixed(p));
                CHECK(map.wedge(p) == -1);
                for (int g = 0; g < map.order(); ++g)
                    CHECK(map.image(g, p) == p);
            }
            else if (map.fixed(p))
                ++onMirror;
            else
            {
                // Every member of an orbit is a different point and lies in its own slot's wedge.
                std::vector<Index> orbit;
                for (int g = 0; g < map.order(); ++g)
                    orbit.push_back(map.orbitPoint(p, g));
                std::ranges::sort(orbit);
                CHECK(std::ranges::adjacent_find(orbit) == orbit.end());
                CHECK(map.orbitPoint(p, map.wedge(p)) == p);
            }
        }
        CHECK(onAxis == 2);
        // Dihedral: the vertex columns on the 6 mirror planes are fixed by a reflection.
        CHECK((onMirror > 0) == c.mirror);
        const SymmetryCheck check = map.check(m);
        CHECK(check.unmatched == 0);
        CHECK(check.maxAsymmetry < 1e-9);
    }
}

TEST_CASE("radial map: a count that does not fit leaves points unmatched")
{
    const Mesh m = sphere24();
    const SymmetryMap map = SymmetryMap::buildRadial(m, axisRadial(1, glm::dvec3(0.0), 5, false), 1e-6);
    CHECK(map.unmatchedCount() > 0);
    CHECK(map.unmatchedPoints(m).size() == map.unmatchedCount());
    // The poles still match: they sit on the axis whatever the count.
    CHECK(map.unmatchedCount() == m.numPoints() - 2);
}

TEST_CASE("radial map by topology survives drift")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        Mesh m = sphere24();
        const RadialSymmetry r = groupOf(c);
        const SymmetryMap exact = SymmetryMap::buildRadial(m, r, 1e-6);
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> d(-1.5, 1.5);
        for (glm::dvec3& p : m.positions())
            p += glm::dvec3(d(rng), d(rng), d(rng));
        CHECK(SymmetryMap::buildRadial(m, r, 1e-3).unmatchedCount() > 0);
        const SymmetryMap topo = SymmetryMap::buildRadialTopological(m, 5, r);
        REQUIRE(!topo.empty());
        CHECK(topo.unmatchedCount() == 0);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            for (int g = 0; g < r.order(); ++g)
                CHECK(topo.image(g, p) == exact.image(g, p));
    }
}

TEST_CASE("radial persistent layer round trip")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        Mesh m = sphere24();
        const RadialSymmetry r = groupOf(c);
        SymmetrySync sync;
        const SymmetryMap first = SymmetryMap::persistent(m, r, 1e-4, &sync);
        CHECK(sync.unmatched == 0);
        CHECK(sync.byPosition == m.numPoints());
        CHECK(m.pointAttrs().has(radialLayerName(r)));
        CHECK(isSymmetryLayer(radialLayerName(r)));

        // Far past any matching tolerance: the stored orbits still hold.
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> d(-5.0, 5.0);
        for (glm::dvec3& p : m.positions())
            p += glm::dvec3(d(rng), d(rng), d(rng));
        const SymmetryMap again = SymmetryMap::persistent(m, r, 1e-4, &sync);
        CHECK(sync.stored == m.numPoints());
        CHECK(sync.byPosition + sync.topological == 0);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            for (int g = 0; g < r.order(); ++g)
                CHECK(again.image(g, p) == first.image(g, p));

        // Delete one whole orbit's faces and renumber: the rest keep their orbits.
        const Index f = faceOffPlanes(m, r);
        deleteFaces(m, std::vector<Index>{f}, &again);
        m.compact();
        const SymmetryMap after = SymmetryMap::persistent(m, r, 1e-4, &sync);
        CHECK(sync.unmatched == 0);
        CHECK(sync.byPosition + sync.topological == 0);
        CHECK(validFaces(m) == validFaces(sphere24()) - Index(r.order()));

        // Enforcing from a wedge makes every orbit exact again.
        after.symmetrizeWedge(m, 0);
        CHECK(asymmetry(m, after) < 1e-9);
        CHECK(after.check(m).maxAsymmetry < 1e-9);
    }
}

TEST_CASE("radial persistent: new points from an op are matched by topology")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        Mesh m = sphere24();
        const RadialSymmetry r = groupOf(c);
        SymmetryMap map = SymmetryMap::persistent(m, r, 1e-4);
        // Drift so position matching cannot help, then extrude: the new points must still find their orbits.
        for (glm::dvec3& p : m.positions())
            p *= 1.0 + 0.01 * p.y;
        const Index firstNew = m.pointCapacity();
        ExtrudeOptions o;
        o.offset = 4.0;
        o.symmetry = &map;
        extrudeFaces(m, std::vector<Index>{faceOffPlanes(m, r)}, o);
        clearSymmetryKeys(m, firstNew);
        SymmetrySync sync;
        const SymmetryMap after = SymmetryMap::persistent(m, r, 1e-4, &sync);
        CHECK(sync.unmatched == 0);
        CHECK(sync.topological == m.pointCapacity() - firstNew);
    }
}

TEST_CASE("geo ops follow radial symmetry")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        const RadialSymmetry r = groupOf(c);
        auto check = [&](const char* op, const Mesh& m) {
            CAPTURE(op);
            CHECK(isValid(m));
            const SymmetryMap after = SymmetryMap::buildRadial(m, r, 1e-6);
            CHECK(after.unmatchedCount() == 0);
        };
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            const Index faces = m.numFaces();
            ExtrudeOptions o;
            o.offset = 5.0;
            o.individual = true;
            o.symmetry = &map;
            const ExtrudeResult res = extrudeFaces(m, std::vector<Index>{faceOffPlanes(m, r)}, o);
            CHECK(res.faces.size() == size_t(r.order()));
            CHECK(m.numFaces() == faces + Index(4 * r.order()));
            check("extrude", m);
        }
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            InsetOptions o;
            o.thickness = 1.0;
            o.symmetry = &map;
            insetFaces(m, std::vector<Index>{faceOffPlanes(m, r)}, o);
            check("inset", m);
        }
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            const Index f = faceOffPlanes(m, r);
            BevelOptions o;
            o.offset = 1.0;
            o.symmetry = &map;
            bevelEdges(m, std::vector<Index>{m.cornerEdge(m.faceCorner(f))}, o);
            check("bevel", m);
        }
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            const Index faces = m.numFaces();
            deleteFaces(m, std::vector<Index>{faceOffPlanes(m, r)}, &map);
            CHECK(m.numFaces() == faces - Index(r.order()));
            m.compact();
            check("delete", m);
        }
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            const Index f = faceOffPlanes(m, r);
            EdgeExtrudeOptions o;
            o.offset = 2.0;
            o.symmetry = &map;
            deleteFaces(m, std::vector<Index>{f}, &map);
            m.compact();
            const SymmetryMap holes = SymmetryMap::buildRadial(m, r, 1e-6);
            std::vector<Index> border;
            for (Index e = 0; e < m.edgeCapacity() && border.empty(); ++e)
                if (m.edgeValid(e) && m.isBoundaryEdge(e))
                    border.push_back(e);
            o.symmetry = &holes;
            extrudeEdges(m, border, o);
            check("edge extrude", m);
        }
        {
            Mesh m = sphere24();
            const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
            const Index f = faceOffPlanes(m, r);
            const Index e = m.cornerEdge(m.faceCorner(f));
            const KnifePlan plan = knifeLoopPlan(m, e, 0.3, &map);
            CHECK(!plan.segments.empty());
            knifeApply(m, plan);
            check("knife loop", m);
        }
    }
}

TEST_CASE("enforceWedge carries one wedge's edit onto every slot")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        Mesh m = sphere24();
        const RadialSymmetry r = groupOf(c);
        const SymmetryMap map = SymmetryMap::buildRadial(m, r, 1e-6);
        const Index f = faceOffPlanes(m, r);
        std::vector<Index> pts = m.facePoints(f);
        const int w = map.majorityWedge(pts);
        for (Index p : pts)
            if (map.drives(p, w))
                m.setPosition(p, m.position(p) * 1.3 + glm::dvec3(0.0, 2.0, 0.0));
        const std::vector<Index> all = map.expandPoints(pts);
        CHECK(all.size() >= pts.size() * size_t(c.count));
        map.enforceWedge(m, all, w);
        CHECK(asymmetry(m, map) < 1e-9);
        // The poles stay exactly on the axis.
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (map.fixed(p) && r.axisDistance(m.position(p)) < 1e-6)
                CHECK(r.axisDistance(m.position(p)) == 0.0);
    }
}

TEST_CASE("symmetrizeRadialTopology rebuilds the kept wedge exactly")
{
    for (const Case& c : kCases)
    {
        CAPTURE(name(c));
        const RadialSymmetry r = groupOf(c);
        Mesh m = sphere24();
        const Index points = m.numPoints(), faces = m.numFaces();
        // Spoil every wedge but one: the rebuild must come out of the kept wedge alone.
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (r.wedgeOf(m.position(p)) != 0 && r.axisDistance(m.position(p)) > 1.0)
                m.setPosition(p, m.position(p) + glm::dvec3(0.0, 3.0, 0.0));
        const SymmetryMap map = symmetrizeRadialTopology(m, r, 0, 1e-4);
        CHECK(isValid(m));
        CHECK(boundaryEdgeCount(m) == 0);
        CHECK(m.numPoints() == points);
        CHECK(m.numFaces() == faces);
        CHECK(map.unmatchedCount() == 0);
        CHECK(asymmetry(m, map) < 1e-9);
        CHECK(m.pointAttrs().has(radialLayerName(r)));
    }
    // A count that cuts across the columns makes new seam points, still exactly symmetric and closed.
    Mesh m = sphere24();
    const RadialSymmetry five = axisRadial(1, glm::dvec3(0.0), 5, false);
    const SymmetryMap map = symmetrizeRadialTopology(m, five, 0, 1e-4);
    CHECK(isValid(m));
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(map.unmatchedCount() == 0);
    CHECK(asymmetry(m, map) < 1e-9);
}

TEST_CASE("mirror maps answer the generic calls as the order 2 case")
{
    CubeOptions o;
    o.subdivX = 3;
    const Mesh m = makeCube(o);
    const SymmetryMap map = SymmetryMap::build(m, axisPlane(0), 1e-6);
    CHECK(!map.isRadial());
    CHECK(map.order() == 2);
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        CHECK(map.image(0, p) == p);
        CHECK(map.image(1, p) == map.mirrorPoint(p));
        CHECK(map.fixed(p) == map.onPlane(p));
        if (map.side(p) > 0)
            CHECK(map.wedge(p) == 0);
        if (map.side(p) < 0)
            CHECK(map.wedge(p) == 1);
        CHECK(map.orbitPoint(p, 0) == (map.side(p) < 0 ? map.mirrorPoint(p) : p));
    }
    CHECK(asymmetry(m, map) < 1e-12);
}
