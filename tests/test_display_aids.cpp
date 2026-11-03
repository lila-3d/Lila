#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "ui/DisplayAids.h"
#include "ui/ViewportDisplay.h"

#include <algorithm>

using namespace lila;
using namespace lila::ui;
using namespace testutil;
namespace da = lila::ui::displayAids;

namespace
{
/** Two quads side by side sharing the edge between points 2 and 3; uv0 follows the positions, or splits along it. */
geo::Mesh twoQuads(bool seam)
{
    geo::Mesh m;
    for (int i = 0; i < 3; ++i)
    {
        m.addPoint({double(i), 0.0, 0.0});
        m.addPoint({double(i), 1.0, 0.0});
    }
    m.addFace({0, 2, 3, 1});
    m.addFace({2, 4, 5, 3});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        for (geo::Index c : m.faceCorners(f))
        {
            glm::dvec3 p = m.position(m.cornerPoint(c));
            uv[c] = glm::vec2(float(p.x) + (seam && f == 1 ? 0.5f : 0.0f), float(p.y));
        }
    return m;
}
} // namespace

TEST_CASE("display aids sort edges into soft, hard, border and non-manifold")
{
    geo::Mesh m = twoQuads(false);
    const geo::Index shared = m.findEdge(2, 3);
    da::EdgeClasses e = da::classifyEdges(m);
    CHECK(e.border.size() == 6);
    CHECK(e.soft == std::vector<geo::Index>{shared});
    CHECK(e.hard.empty());
    CHECK(e.nonManifold.empty());
    CHECK(e.nonManifoldPoints.empty());

    m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard)[shared] = true;
    e = da::classifyEdges(m);
    CHECK(e.hard == std::vector<geo::Index>{shared});
    CHECK(e.soft.empty());

    // A third face on the shared edge makes it, and both its points, non-manifold.
    m.addPoint({1.0, 0.0, 1.0});
    m.addFace({2, 3, 6});
    e = da::classifyEdges(m);
    CHECK(e.nonManifold == std::vector<geo::Index>{shared});
    CHECK(has(e.nonManifoldPoints, 2));
    CHECK(has(e.nonManifoldPoints, 3));
    CHECK(!has(e.nonManifoldPoints, 0));
}

TEST_CASE("display aids find a bowtie point where two fans of faces meet")
{
    geo::Mesh m;
    m.addPoint({0, 0, 0});
    m.addPoint({1, 1, 0});
    m.addPoint({-1, 1, 0});
    m.addPoint({1, -1, 0});
    m.addPoint({-1, -1, 0});
    m.addFace({0, 1, 2});
    m.addFace({0, 4, 3});
    da::EdgeClasses e = da::classifyEdges(m);
    CHECK(e.nonManifold.empty());
    CHECK(e.nonManifoldPoints == std::vector<geo::Index>{0});
}

TEST_CASE("UV ids are shared across faces until a seam splits them")
{
    std::vector<geo::Index> ids;
    geo::Mesh joined = twoQuads(false);
    CHECK(da::uvIds(joined, ids).size() == 6);
    const geo::Index a = joined.findCorner(0, 2), b = joined.findCorner(1, 2);
    CHECK(ids[a] == ids[b]);

    geo::Mesh split = twoQuads(true);
    CHECK(da::uvIds(split, ids).size() == 8);
    CHECK(ids[split.findCorner(0, 2)] != ids[split.findCorner(1, 2)]);

    geo::Mesh none;
    none.addPoint({0, 0, 0});
    none.addPoint({1, 0, 0});
    none.addPoint({0, 1, 0});
    none.addFace({0, 1, 2});
    CHECK(da::uvIds(none, ids).empty());
    CHECK(std::all_of(ids.begin(), ids.end(), [](geo::Index i) { return i == geo::kInvalid; }));
}

TEST_CASE("display aids thin dense meshes and keep the nearest labels")
{
    CHECK(da::strideFor(10, 100) == 1);
    CHECK(da::strideFor(100, 100) == 1);
    CHECK(da::strideFor(101, 100) == 2);
    CHECK(da::strideFor(1000, 100) == 10);
    CHECK(da::strideFor(5, 0) == 1);

    da::Nearest<int> nearest(3);
    for (int i : {5, 1, 9, 3, 7, 2})
        nearest.offer(float(i), i);
    CHECK(!nearest.wants(4.0f));
    CHECK(nearest.wants(2.5f));
    CHECK(nearest.take() == std::vector<int>{1, 2, 3});
    CHECK(nearest.take().empty());

    da::Nearest<int> none(0);
    none.offer(1.0f, 1);
    CHECK(none.take().empty());
}

TEST_CASE("display aids are per panel, two-sided lighting on by default")
{
    PrefStore& p = PrefStore::get();
    for (int panel : {0, 1})
        for (const da::Info& i : da::all())
            p.set("general", da::prefKey(panel, i.aid), i.fallback);
    CHECK(da::on(0, da::Aid::TwoSidedLighting));
    CHECK(!da::on(0, da::Aid::BackfaceTint));
    CHECK(!da::anyDrawn(0));

    da::setOn(1, da::Aid::FaceNormals, true);
    CHECK(da::on(1, da::Aid::FaceNormals));
    CHECK(!da::on(0, da::Aid::FaceNormals));
    CHECK(da::anyDrawn(1));
    CHECK(!da::anyDrawn(0));

    da::setOn(0, da::Aid::BackfaceTint, true);
    da::setOn(0, da::Aid::TwoSidedLighting, false);
    CHECK(!da::anyDrawn(0)); // the lighting aids change shading, not what draw() adds
    render::FrameState s;
    p.set("general", viewportDisplay::kMonotone, false);
    da::apply(s, 0);
    CHECK(!s.twoSidedLighting);
    CHECK(s.backfaceTint.a > 0.0f);
    CHECK(s.backfaceTint.r > s.backfaceTint.g);
    p.set("general", viewportDisplay::kMonotone, true);
    da::apply(s, 0);
    CHECK(s.backfaceTint.r == s.backfaceTint.g);
    CHECK(s.backfaceTint.g == s.backfaceTint.b);
    da::apply(s, 1);
    CHECK(s.twoSidedLighting);
    CHECK(s.backfaceTint.a == 0.0f);

    p.set("general", viewportDisplay::kMonotone, false);
    for (int panel : {0, 1})
        for (const da::Info& i : da::all())
            p.set("general", da::prefKey(panel, i.aid), i.fallback);
    CHECK(da::find("uvIds") == &da::info(da::Aid::UvIds));
    CHECK(da::find("nope") == nullptr);
}
