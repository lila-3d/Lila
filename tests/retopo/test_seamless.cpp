#include "retopo/Corpus.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"
#include "retopo/Retopo.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

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
    if (const char* a = std::getenv("LILA_AB_ADAPT"))
        o.adaptivity = std::atof(a);
    if (const char* t = std::getenv("LILA_AB_TIMEOUT"))
        o.timeout = std::atof(t);
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
    std::printf("%s: %.2f s %s, %d defects\n", cases[i].name.c_str(), s, r.ok() ? r.metrics.summary().c_str() : r.error.c_str(), r.defects);
    if (const char* out = std::getenv("LILA_AB_OUT"))
        std::ofstream(out) << geo::writeObj(r.mesh);
    if (const char* src = std::getenv("LILA_AB_SRC"))
        std::ofstream(src) << geo::writeObj(cases[i].mesh);
}

TEST_CASE("zz remesh obj" * doctest::skip())
{
    // LILA_AB_OBJ=world-space obj, LILA_AB_MATRIX=its world matrix (16 numbers, column major), LILA_AB_FACES, LILA_AB_SYM, LILA_AB_OUT=result obj.
    const char* path = std::getenv("LILA_AB_OBJ");
    if (!path)
        return;
    geo::ObjReadResult read = geo::readObjFile(path);
    REQUIRE(read.ok());
    geo::Mesh m = read.objects.front().mesh;
    if (const char* mat = std::getenv("LILA_AB_MATRIX"))
    {
        std::istringstream in(mat);
        glm::dmat4 w(1.0);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                in >> w[c][r];
        glm::dmat4 inv = glm::inverse(w);
        for (glm::dvec3& p : m.positions())
            p = glm::dvec3(inv * glm::dvec4(p, 1.0));
    }
    retopo::QuadRemeshOptions o;
    o.targetFaces = std::getenv("LILA_AB_FACES") ? std::atoi(std::getenv("LILA_AB_FACES")) : 500;
    o.symmetryAxis = std::getenv("LILA_AB_SYM") ? std::atoi(std::getenv("LILA_AB_SYM")) : -1;
    o.resolveGaps = !std::getenv("LILA_NO_GAPS");
    o.seamless = std::getenv("LILA_SEAMLESS") != nullptr;
    o.useHardEdges = !std::getenv("LILA_NO_HARD");
    if (const char* a = std::getenv("LILA_AB_ADAPT"))
        o.adaptivity = std::atof(a);
    if (const char* a = std::getenv("LILA_AB_AROUND"))
        o.edgesAround = std::atoi(a);
    if (const char* a = std::getenv("LILA_AB_SHARP"))
        o.sharpAngle = std::atof(a);
    if (std::getenv("LILA_AB_GRID"))
        o.method = retopo::RemeshMethod::Grid;
    if (const char* t = std::getenv("LILA_AB_TOL"))
        o.countTolerance = std::atof(t);
    const auto started = std::chrono::steady_clock::now();
    retopo::QuadRemeshResult r = retopo::quadRemesh(m, o);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%s, %d defects, %s, %.2f s\n", r.ok() ? r.metrics.summary().c_str() : r.error.c_str(), r.defects, r.layout.c_str(), took);
    for (const retopo::RemeshAttempt& at : r.attempts)
        std::printf("  %s %u faces %d defects\n", at.layout.c_str(), unsigned(at.faces), at.defects);
    if (const char* out = std::getenv("LILA_AB_OUT"))
        std::ofstream(out) << geo::writeObj(r.mesh);
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
