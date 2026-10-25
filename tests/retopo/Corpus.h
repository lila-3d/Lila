#pragma once

#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

/** Adversarial meshes and the invariants every reshaping op must keep, shared by the triangulate, reduce, relax and remesh tests. */
namespace lila::corpus
{
using geo::Index;
using geo::Mesh;

struct Case
{
    std::string name;
    Mesh mesh;
};

inline std::vector<Index> allFaces(const Mesh& m)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

inline std::vector<Index> allEdges(const Mesh& m)
{
    std::vector<Index> out;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            out.push_back(e);
    return out;
}

/** A 2D outline as one face on a tilted plane, optionally bent out of it, extruded into a prism when depth > 0. */
inline Mesh polygon(const std::vector<glm::dvec2>& poly, double bend = 0.0, double depth = 0.0)
{
    const glm::dvec3 u = glm::normalize(glm::dvec3(1.0, 0.3, -0.2));
    const glm::dvec3 v = glm::normalize(glm::cross(glm::dvec3(0.2, 0.5, 1.0), u));
    const glm::dvec3 n = glm::cross(u, v);
    Mesh m;
    std::vector<Index> top, bottom;
    for (size_t i = 0; i < poly.size(); ++i)
        top.push_back(m.addPoint(glm::dvec3(30, -20, 8) + u * poly[i].x * 50.0 + v * poly[i].y * 50.0
                                 + n * (bend * 50.0 * std::sin(double(i) * 1.7))));
    if (depth <= 0.0)
    {
        m.addFace(top);
        return m;
    }
    for (size_t i = 0; i < poly.size(); ++i)
        bottom.push_back(m.addPoint(m.position(top[i]) - n * depth));
    m.addFace(top);
    std::vector<Index> rev(bottom.rbegin(), bottom.rend());
    m.addFace(rev);
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const size_t j = (i + 1) % poly.size();
        m.addFace({top[j], top[i], bottom[i], bottom[j]});
    }
    return m;
}

inline std::vector<glm::dvec2> star(int points, double inner)
{
    std::vector<glm::dvec2> p;
    for (int i = 0; i < 2 * points; ++i)
    {
        const double t = i * 3.14159265358979323846 / points, r = i % 2 ? inner : 1.0;
        p.push_back({r * std::cos(t), r * std::sin(t)});
    }
    return p;
}

inline std::vector<glm::dvec2> comb(int teeth)
{
    std::vector<glm::dvec2> p;
    for (int x = 0; x < teeth * 2; ++x)
        p.push_back({0.2 * x, 0.0});
    for (int k = teeth - 1; k >= 0; --k)
    {
        p.push_back({0.2 * (2 * k + 1), 1.5});
        p.push_back({0.2 * (2 * k), 1.5});
        if (k > 0)
        {
            p.push_back({0.2 * (2 * k), 0.2});
            p.push_back({0.2 * (2 * k - 1), 0.2});
        }
    }
    return p;
}

inline std::vector<glm::dvec2> spiral(int turnsTimes8)
{
    std::vector<glm::dvec2> outer, inner;
    for (int i = 0; i <= turnsTimes8; ++i)
    {
        const double t = i * 3.14159265358979323846 / 4.0, r = 0.3 + 0.08 * t;
        outer.push_back({(r + 0.12) * std::cos(t), (r + 0.12) * std::sin(t)});
        inner.push_back({r * std::cos(t), r * std::sin(t)});
    }
    std::vector<glm::dvec2> p(outer.begin(), outer.end());
    p.insert(p.end(), inner.rbegin(), inner.rend());
    return p;
}

/** Square with a square hole, joined by a bridge whose two sides are distinct points at the same positions. */
inline std::vector<glm::dvec2> squareWithHole()
{
    return {{-1, -1}, {0, -1}, {0, -0.4}, {-0.4, -0.4}, {-0.4, 0.4}, {0.4, 0.4}, {0.4, -0.4}, {0, -0.4}, {0, -1}, {1, -1}, {1, 1}, {-1, 1}};
}

/** Rectangle whose sides carry many collinear points. */
inline std::vector<glm::dvec2> collinearRect(int perSide)
{
    const glm::dvec2 c[4] = {{-1.5, -0.5}, {1.5, -0.5}, {1.5, 0.5}, {-1.5, 0.5}};
    std::vector<glm::dvec2> p;
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < perSide; ++k)
            p.push_back(glm::mix(c[s], c[(s + 1) % 4], double(k) / perSide));
    return p;
}

inline Mesh bevelledCube(int segments)
{
    Mesh m = geo::makeCube();
    geo::BevelOptions b;
    b.offset = 10.0;
    b.segments = segments;
    geo::bevelEdges(m, allEdges(m), b);
    m.compact();
    return m;
}

inline Mesh noisySphere(int subdiv, double amount, unsigned seed)
{
    geo::SphereOptions o;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    Mesh m = geo::makeSphere(o);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-amount, amount);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) * (1.0 + d(rng)));
    return m;
}

/** Plane bent into a saddle with jittered heights, so its quads are strongly non-planar. */
inline Mesh twistedGrid(int n)
{
    geo::PlaneOptions o;
    o.subdivX = n;
    o.subdivZ = n;
    Mesh m = geo::makePlane(o);
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> d(-6.0, 6.0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 q = m.position(p);
            q.y = q.x * q.z / 60.0 + d(rng);
            m.setPosition(p, q);
        }
    return m;
}

inline Mesh hemisphere()
{
    geo::SphereOptions o;
    o.subdivAxis = 24;
    o.subdivHeight = 16;
    Mesh m = geo::makeSphere(o);
    std::vector<Index> low;
    for (Index f : allFaces(m))
        if (m.faceCentroid(f).y < 0.0)
            low.push_back(f);
    geo::deleteFaces(m, low);
    m.compact();
    return m;
}

inline Mesh hardCube()
{
    Mesh m = geo::makeCube({100, 100, 100, 3, 3, 3});
    geo::setHardEdgesByAngle(m, 30.0);
    geo::markUvSeams(m);
    return m;
}

/** Two quads sharing two edges at a valence-2 point, where both would like the same diagonal. */
inline Mesh tent()
{
    Mesh m;
    const Index a = m.addPoint({-50, 0, 0}), b = m.addPoint({50, 0, 0}), p = m.addPoint({0, 0, 10});
    const Index x = m.addPoint({0, -150, 0}), y = m.addPoint({0, 150, 0});
    m.addFace({a, p, b, x});
    m.addFace({b, p, a, y});
    return m;
}

inline Mesh objRoundTrip(const Mesh& in)
{
    geo::ObjReadResult r = geo::readObj(geo::writeObj(in));
    return r.ok() && !r.objects.empty() ? std::move(r.objects.front().mesh) : Mesh();
}

/** Every mesh the ops are run on. Closed solids, open sheets, n-gons, slivers, attribute seams. */
inline std::vector<Case> all()
{
    std::vector<Case> c;
    c.push_back({"cube", geo::makeCube()});
    c.push_back({"cube4", geo::makeCube({100, 100, 100, 4, 4, 4})});
    c.push_back({"plane1", geo::makePlane()});
    c.push_back({"plane10", geo::makePlane({100, 100, 10, 10})});
    c.push_back({"sphere8x6", geo::makeSphere({50, 8, 6})});
    c.push_back({"sphere32", geo::makeSphere({50, 32, 32})});
    c.push_back({"cylinder", geo::makeCylinder({.capFill = geo::CapFill::Ngon})});
    {
        geo::CylinderOptions o;
        o.caps = false;
        o.subdivHeight = 4;
        c.push_back({"openCylinder", geo::makeCylinder(o)});
    }
    c.push_back({"cone", geo::makeCone({.capFill = geo::CapFill::Ngon})});
    c.push_back({"torus", geo::makeTorus()});
    c.push_back({"disc", geo::makeDisc()});
    c.push_back({"pipe", geo::makePipe()});
    c.push_back({"quadSphere", geo::makeQuadSphere()});
    c.push_back({"icoSphere", geo::makeIcoSphere()});
    c.push_back({"starPrism", polygon(star(7, 0.35), 0.0, 20.0)});
    c.push_back({"combPrism", polygon(comb(5), 0.0, 10.0)});
    c.push_back({"spiralFace", polygon(spiral(40))});
    c.push_back({"collinearPrism", polygon(collinearRect(6), 0.0, 15.0)});
    c.push_back({"holeFace", polygon(squareWithHole())});
    c.push_back({"bentStar", polygon(star(9, 0.5), 0.15)});
    c.push_back({"bentComb", polygon(comb(4), 0.1)});
    c.push_back({"twistedGrid", twistedGrid(6)});
    c.push_back({"slivers", geo::makePlane({1000, 0.5, 40, 1})});
    c.push_back({"hardCube", hardCube()});
    c.push_back({"objSphere", objRoundTrip(geo::makeSphere())});
    c.push_back({"bevelCube", bevelledCube(3)});
    c.push_back({"noisySphere", noisySphere(48, 0.03, 11)});
    c.push_back({"hemisphere", hemisphere()});
    c.push_back({"tent", tent()});
    for (Case& k : c)
        k.mesh.compact();
    return c;
}

// Invariants

inline double diagonal(const Mesh& m) { return std::max(1e-12, glm::length(m.bounds().size())); }

/** Faces of (near) zero area relative to the mesh's size. */
inline Index degenerateFaces(const Mesh& m, double rel = 1e-9)
{
    const double d = diagonal(m), lim = rel * d * d;
    Index n = 0;
    for (Index f : allFaces(m))
        n += m.faceArea(f) <= lim;
    return n;
}

/** Faces on exactly the same point set as an earlier face. */
inline Index duplicateFaces(const Mesh& m)
{
    std::set<std::vector<Index>> seen;
    Index n = 0;
    for (Index f : allFaces(m))
    {
        std::vector<Index> p = m.facePoints(f);
        std::sort(p.begin(), p.end());
        n += !seen.insert(p).second;
    }
    return n;
}

inline Index nonManifoldEdges(const Mesh& m)
{
    Index n = 0;
    for (Index e : allEdges(m))
        n += m.edgeFaceCount(e) > 2;
    return n;
}

/** Manifold edges whose two faces run it the same way, i.e. inconsistent winding. */
inline Index windingErrors(const Mesh& m)
{
    Index n = 0;
    for (Index e : allEdges(m))
    {
        if (m.edgeFaceCount(e) != 2)
            continue;
        const Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
        n += m.cornerPoint(c0) == m.cornerPoint(c1);
    }
    return n;
}

inline Index boundaryEdges(const Mesh& m)
{
    Index n = 0;
    for (Index e : allEdges(m))
        n += m.edgeFaceCount(e) == 1;
    return n;
}

inline double boundaryLength(const Mesh& m)
{
    double l = 0.0;
    for (Index e : allEdges(m))
        if (m.edgeFaceCount(e) == 1)
            l += m.edgeLength(e);
    return l;
}

/** Largest distance from the mesh's points and triangle centres to the other surface. */
inline double oneSided(const Mesh& from, const geo::MeshBvh& to)
{
    double worst = 0.0;
    geo::BvhClosest c;
    for (Index p = 0; p < from.pointCapacity(); ++p)
        if (from.pointValid(p) && to.closestPoint(from.position(p), c))
            worst = std::max(worst, c.distance);
    const geo::Triangulation tris = geo::triangulate(from);
    for (size_t t = 0; t < tris.corners.size(); t += 3)
    {
        glm::dvec3 centre(0.0);
        for (int k = 0; k < 3; ++k)
            centre += from.position(from.cornerPoint(tris.corners[t + k])) / 3.0;
        if (to.closestPoint(centre, c))
            worst = std::max(worst, c.distance);
    }
    return worst;
}

/** Symmetric Hausdorff distance (sampled at points and face centres) as a fraction of the source's diagonal. */
inline double hausdorff(const Mesh& a, const Mesh& b)
{
    geo::MeshBvh ba(a), bb(b);
    return std::max(oneSided(a, bb), oneSided(b, ba)) / diagonal(a);
}

/**
 * Faces of out whose normal points against the source surface under their centre. Faces whose centre sits near a
 * sharp source edge are judged against the best of the faces around it, so bevels and corners don't false-positive.
 */
inline Index flippedFaces(const Mesh& out, const Mesh& src, double minDot = -0.2)
{
    geo::MeshBvh bvh(src);
    const std::vector<glm::vec3> smooth = geo::pointNormals(src);
    const double d = diagonal(src);
    Index n = 0;
    for (Index f : allFaces(out))
    {
        if (out.faceArea(f) <= 1e-12 * d * d)
            continue;
        const glm::dvec3 fn = out.faceNormal(f);
        geo::BvhClosest c;
        if (!bvh.closestPoint(out.faceCentroid(f), c))
            continue;
        // The smooth normal under the centre too, so crumpled source facets (sculpt noise) don't count against it.
        glm::dvec3 interp(0.0);
        for (int k = 0; k < 3; ++k)
            interp += glm::dvec3(smooth[src.cornerPoint(bvh.triangles().corners[size_t(c.triangle) * 3 + k])]) * c.bary[k];
        double best = std::max(glm::dot(fn, src.faceNormal(c.face)), glm::length(interp) > 0.0 ? glm::dot(fn, glm::normalize(interp)) : -1.0);
        if (best < minDot)
            for (Index t : bvh.overlapping({c.point - glm::dvec3(1e-3 * d), c.point + glm::dvec3(1e-3 * d)}))
                best = std::max(best, glm::dot(fn, src.faceNormal(bvh.triangles().faces[t])));
        n += best < minDot;
    }
    return n;
}

inline double signedVolume(const Mesh& m)
{
    double v = 0.0;
    for (Index f : allFaces(m))
    {
        const Index c0 = m.faceCorner(f);
        const glm::dvec3 a = m.position(m.cornerPoint(c0));
        for (Index c = m.cornerNext(c0); m.cornerNext(c) != c0; c = m.cornerNext(c))
            v += glm::dot(a, glm::cross(m.position(m.cornerPoint(c)), m.position(m.cornerNextPoint(c)))) / 6.0;
    }
    return v;
}

/** Problems an op introduced: things the output has more of than the input. Empty means clean. */
inline std::vector<std::string> introduced(const Mesh& in, const Mesh& out)
{
    std::vector<std::string> p;
    const auto more = [&](const char* what, Index a, Index b) {
        if (b > a)
            p.push_back(std::string(what) + " " + std::to_string(a) + " -> " + std::to_string(b));
    };
    more("degenerate faces", degenerateFaces(in), degenerateFaces(out));
    more("duplicate faces", duplicateFaces(in), duplicateFaces(out));
    more("non-manifold edges", nonManifoldEdges(in), nonManifoldEdges(out));
    more("winding errors", windingErrors(in), windingErrors(out));
    std::vector<std::string> v = out.validate(geo::kValidateStructure);
    for (const std::string& s : v)
        p.push_back("invalid: " + s);
    return p;
}

inline std::string join(const std::vector<std::string>& v)
{
    std::ostringstream s;
    for (const std::string& x : v)
        s << x << "; ";
    return s.str();
}
} // namespace lila::corpus
