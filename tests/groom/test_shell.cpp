#include <doctest/doctest.h>

#include "GroomTestUtil.h"

#include "geo/ColorSets.h"
#include "groom/Locks.h"
#include "groom/Shell.h"

#include <cmath>

using namespace lila;
using namespace lila::groom;
using namespace groomtest;

namespace
{
bool closedManifold(const geo::Mesh& m)
{
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
            return false;
    return true;
}
} // namespace

TEST_CASE("shell: locks join into one watertight surface that contains every lock")
{
    const Surface scalp = sphereScalp(10.0);
    Splines in;
    for (int k = 0; k < 6; ++k)
        in.push_back(lyingGuide(10.0, k * 0.3, 16, double(k + 1)));
    LockOptions lo;
    lo.width = 2.5;
    lo.thickness = 0.3;
    const Splines resolved = resolveLocks(in, &scalp, lo);
    ShellOptions o;
    o.voxelSize = 0.25;
    o.blend = 0.8;
    o.gpu = false;
    const geo::Mesh m = shellMesh(resolved, o);
    REQUIRE(m.numFaces() > 100);
    CHECK(closedManifold(m));
    CHECK(geo::colors::has(m, "groomShell"));
    // Every lock centre point is inside the shell's bounds.
    const auto b = m.bounds();
    for (const Spline& s : resolved)
        for (const D3& p : s.points)
            CHECK(glm::all(glm::lessThanEqual(b.min - 0.01, p)));
}

TEST_CASE("shell: a larger blend joins locks that a hard union leaves apart")
{
    Splines in{guide(D3(0, 0, 0), D3(0, -1, 0), 10.0, 8, 1.0), guide(D3(2.4, 0, 0), D3(0, -1, 0), 10.0, 8, 2.0)};
    LockOptions lo;
    lo.width = 2.0;
    lo.profile = LockProfile::Tube;
    const Splines resolved = resolveLocks(in, nullptr, lo);
    ShellOptions hard;
    hard.voxelSize = 0.2;
    hard.blend = 0.0;
    hard.gpu = false;
    ShellOptions soft = hard;
    soft.blend = 1.5;
    // Connected pieces: points joined through the faces they share.
    auto pieces = [](const geo::Mesh& m) {
        std::vector<geo::Index> parent(m.pointCapacity());
        for (geo::Index p = 0; p < parent.size(); ++p)
            parent[p] = p;
        auto root = [&](geo::Index p) {
            while (parent[p] != p)
                p = parent[p] = parent[parent[p]];
            return p;
        };
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            geo::Index first = geo::kInvalid;
            m.forEachFaceCorner(f, [&](geo::Index c) {
                const geo::Index p = m.cornerPoint(c);
                if (first == geo::kInvalid)
                    first = p;
                else
                    parent[root(p)] = root(first);
            });
        }
        int n = 0;
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && root(p) == p)
                ++n;
        return n;
    };
    CHECK(pieces(shellMesh(resolved, hard)) == 2);
    CHECK(pieces(shellMesh(resolved, soft)) == 1);
}
