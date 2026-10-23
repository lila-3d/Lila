#include "retopo/Retopo.h"

#include <algorithm>
#include <cmath>

namespace lila::retopo
{
namespace
{
/** The one other border edge at p, or kInvalid when there is none or the border branches there. */
Index nextBorderEdge(const geo::Mesh& m, Index p, Index from)
{
    Index next = geo::kInvalid;
    int count = 0;
    m.forEachPointEdge(p, [&](Index e) {
        if (e != from && m.isBoundaryEdge(e))
        {
            next = e;
            ++count;
        }
    });
    return count == 1 ? next : geo::kInvalid;
}

double turnDegrees(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    glm::dvec3 u = b - a, v = c - b;
    double lu = glm::length(u), lv = glm::length(v);
    if (lu <= 0.0 || lv <= 0.0)
        return 0.0;
    return glm::degrees(std::acos(std::clamp(glm::dot(u, v) / (lu * lv), -1.0, 1.0)));
}
} // namespace

BorderRun walkBorder(const geo::Mesh& m, Index edge, double maxTurnDegrees)
{
    BorderRun run;
    if (edge >= m.edgeCapacity() || !m.edgeValid(edge) || !m.isBoundaryEdge(edge))
        return run;
    // Grows one way from `tip`, appending edges and the points they reach.
    auto grow = [&](Index tip, Index prevPoint, std::vector<Index>& edges, std::vector<Index>& points) {
        Index cur = edge;
        for (Index guard = 0; guard < m.edgeCapacity(); ++guard)
        {
            Index next = nextBorderEdge(m, tip, cur);
            if (next == geo::kInvalid)
                return false;
            Index far = m.edgeOtherPoint(next, tip);
            if (next == edge)
                return turnDegrees(m.position(prevPoint), m.position(tip), m.position(far)) <= maxTurnDegrees;
            if (turnDegrees(m.position(prevPoint), m.position(tip), m.position(far)) > maxTurnDegrees)
                return false;
            edges.push_back(next);
            points.push_back(far);
            prevPoint = tip;
            tip = far;
            cur = next;
        }
        return false;
    };
    Index a = m.edgePoint(edge, 0), b = m.edgePoint(edge, 1);
    std::vector<Index> fwdEdges, fwdPoints, backEdges, backPoints;
    if (grow(b, a, fwdEdges, fwdPoints))
    {
        // Back at the start edge: closed, and the last point reached is a.
        run.closed = true;
        fwdPoints.pop_back();
        run.edges = {edge};
        run.edges.insert(run.edges.end(), fwdEdges.begin(), fwdEdges.end());
        run.points = {a, b};
        run.points.insert(run.points.end(), fwdPoints.begin(), fwdPoints.end());
        return run;
    }
    grow(a, b, backEdges, backPoints);
    run.edges.assign(backEdges.rbegin(), backEdges.rend());
    run.edges.push_back(edge);
    run.edges.insert(run.edges.end(), fwdEdges.begin(), fwdEdges.end());
    run.points.assign(backPoints.rbegin(), backPoints.rend());
    run.points.push_back(a);
    run.points.push_back(b);
    run.points.insert(run.points.end(), fwdPoints.begin(), fwdPoints.end());
    return run;
}
} // namespace lila::retopo
