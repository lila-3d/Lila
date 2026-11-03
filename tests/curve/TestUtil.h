#pragma once

#include <doctest/doctest.h>

#include "curve/Planar.h"
#include "curve/Spline.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace curvetest
{
inline nlohmann::json run(const char* cmd, nlohmann::json call = nlohmann::json::object()) { return lila::ops::execute(cmd, call); }

inline nlohmann::json pts(std::initializer_list<glm::dvec3> list)
{
    nlohmann::json a = nlohmann::json::array();
    for (const glm::dvec3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

inline lila::scene::MeshPtr meshOf(const std::string& name)
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    g.evaluate();
    lila::scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<lila::scene::MeshPtr>(lila::scene::MeshNode::kOutMesh);
}

inline lila::scene::NodeId idOf(const std::string& name)
{
    lila::scene::Node* n = lila::scene::Graph::current().find(name);
    REQUIRE(n);
    return n->id();
}

/** World-space curveData of a curve object. */
inline nlohmann::json curveJson(const std::string& name, bool world = true)
{
    return run("curveData", {{"args", {name}}, {"worldSpace", world}});
}

/** Creates a poly curve through the points and returns its name. */
inline std::string polyCurve(std::initializer_list<glm::dvec3> list, const char* name)
{
    return run("curve", {{"type", "poly"}, {"points", pts(list)}, {"name", name}});
}

/** Edge ids of a mesh object whose end points pass the filter. */
inline nlohmann::json edgesWhere(const std::string& mesh, const std::function<bool(const glm::dvec3& a, const glm::dvec3& b)>& keep)
{
    lila::scene::MeshPtr m = meshOf(mesh);
    nlohmann::json out = nlohmann::json::array();
    for (lila::geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && keep(m->position(m->edgePoint(e, 0)), m->position(m->edgePoint(e, 1))))
            out.push_back(e);
    return out;
}

/** Poly spline without normalize(), as the curve maths sees raw input. */
inline lila::curve::Spline poly(std::vector<glm::dvec3> pts, bool closed = false)
{
    lila::curve::Spline s;
    s.kind = lila::curve::Kind::Poly;
    s.closed = closed;
    s.points = std::move(pts);
    return s;
}

inline lila::curve::Spline polyline(std::vector<glm::dvec3> pts, bool closed = false)
{
    lila::curve::Spline s = poly(std::move(pts), closed);
    s.normalize();
    return s;
}

inline lila::curve::Spline polyLoop(std::vector<glm::dvec3> pts) { return polyline(std::move(pts), true); }

/** Signed area of a spline tessellated finely and projected to XY. */
inline double area(const lila::curve::Spline& s)
{
    lila::curve::TessOptions o;
    o.chordError = 0.0005;
    lila::curve::Polyline pl = lila::curve::tessellate(s, o);
    std::vector<glm::dvec2> p;
    for (const glm::dvec3& q : pl.points)
        p.push_back(glm::dvec2(q.x, q.y));
    return lila::curve::signedArea(p);
}

inline int boundaryEdges(const lila::geo::Mesh& m)
{
    int n = 0;
    for (lila::geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++n;
    return n;
}

inline double distToSegment(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 d = b - a;
    const double t = glm::dot(d, d) > 0.0 ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
    return glm::distance(a + d * t, p);
}

inline void checkNear(const glm::dvec3& a, const glm::dvec3& b)
{
    for (int k = 0; k < 3; ++k)
        CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-9));
}
} // namespace curvetest
