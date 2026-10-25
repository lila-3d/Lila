#include "retopo/Corpus.h"
#include "geo/Primitives.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace lila;

namespace
{
retopo::QuadRemeshResult remesh(const geo::Mesh& m, int faces, bool seamless)
{
    retopo::QuadRemeshOptions o{.targetFaces = faces};
    o.seamless = seamless;
    if (std::getenv("LILA_NO_GAPS"))
        o.resolveGaps = false;
    if (std::getenv("LILA_NO_SOFT"))
        o.softCreaseAngle = 0.0;
    return retopo::quadRemesh(m, o);
}

double seconds(const retopo::QuadRemeshResult& r)
{
    double total = 0.0;
    for (auto& [stage, sec] : r.timings)
        total += sec;
    return total;
}

void compare(const char* name, const geo::Mesh& m, int faces, geo::Index maxIrregular)
{
    retopo::QuadRemeshResult local = remesh(m, faces, false), global = remesh(m, faces, true);
    REQUIRE(local.ok());
    REQUIRE(global.ok());
    MESSAGE(name, "\n  local:    ", local.metrics.summary(), "  ", seconds(local), " s\n  seamless: ", global.metrics.summary(), "  ",
            seconds(global), " s");
    CHECK(global.metrics.irregular <= maxIrregular);
    CHECK(global.metrics.irregular <= local.metrics.irregular);
}
} // namespace

TEST_CASE("seamless lattice only turns at the poles")
{
    compare("sphere", geo::makeSphere({.radius = 50.0, .subdivAxis = 48, .subdivHeight = 48}), 600, 8);
    compare("torus", geo::makeTorus({.subdivAxis = 64, .subdivHeight = 32}), 800, 0);
    compare("cube", geo::makeCube({.subdivX = 8, .subdivY = 8, .subdivZ = 8}), 600, 8);
    compare("cylinder", geo::makeCylinder({.radius = 30.0, .height = 120.0, .subdivAxis = 48, .subdivHeight = 8, .subdivCaps = 4, .capFill = geo::CapFill::Fan}), 600, 8);
}

TEST_CASE("zz seamless corpus timing" * doctest::skip())
{
    // One corpus case per run, by index: lila_retopo_tests -tc="zz seamless corpus timing" -ns, with LILA_CORPUS_CASE=n.
    const char* which = std::getenv("LILA_CORPUS_CASE");
    if (!which)
        return;
    std::vector<corpus::Case> cases = corpus::all();
    size_t i = size_t(std::atoi(which));
    if (i >= cases.size())
        return;
    auto start = std::chrono::steady_clock::now();
    retopo::QuadRemeshResult r = remesh(cases[i].mesh, 600, !std::getenv("LILA_CORPUS_LOCAL"));
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s: %.2f s %s %s\n", cases[i].name.c_str(), s, r.ok() ? r.metrics.summary().c_str() : r.error.c_str(), "");
}

TEST_CASE("quad remesh gives up past its timeout and says how far it got")
{
    retopo::QuadRemeshOptions o{.targetFaces = 4000};
    o.timeout = 1e-6;
    retopo::QuadRemeshResult r = retopo::quadRemesh(geo::makeSphere({.radius = 50.0, .subdivAxis = 96, .subdivHeight = 96}), o);
    REQUIRE(!r.ok());
    MESSAGE(r.error);
    CHECK(r.error.rfind("gave up after", 0) == 0);
    CHECK(r.mesh.numFaces() == 0);
}
