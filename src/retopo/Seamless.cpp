#include "retopo/Internal.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
constexpr double kQuarter = glm::half_pi<double>();

double wrapQuarter(double a) { return std::remainder(a, kQuarter); }

/** Signed angle from a to b about n. */
double angleAbout(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& n)
{
    return std::atan2(glm::dot(n, glm::cross(a, b)), glm::dot(a, b));
}

/** b turned by k quarter turns about n. */
glm::dvec3 turn(glm::dvec3 b, const glm::dvec3& n, int k)
{
    for (int i = 0; i < ((k % 4) + 4) % 4; ++i)
        b = glm::cross(n, b);
    return b;
}

/** Coordinates turned by k quarter turns. */
glm::dvec2 turn2(glm::dvec2 c, int k)
{
    for (int i = 0; i < ((k % 4) + 4) % 4; ++i)
        c = {-c.y, c.x};
    return c;
}

int mod4(int k) { return ((k % 4) + 4) % 4; }

/** The surface with each singular triangle split at its centre, so singularities sit on vertices; one field direction per triangle. */
struct ParamMesh
{
    std::vector<glm::dvec3> pos;
    std::vector<double> scale;
    std::vector<std::array<Index, 3>> tris;
    std::vector<glm::dvec3> normal; // per triangle
    std::vector<glm::dvec3> dir;    // per triangle, unit, in its plane
    Index surfaceVertices = 0;

    // Edges: sorted endpoints and up to two triangles, with the side each triangle has it on.
    std::vector<std::array<Index, 2>> edges;
    std::vector<std::array<Index, 2>> edgeTris;
    std::vector<uint8_t> edgeTriCount;
    std::vector<std::array<Index, 3>> triEdges;
    std::vector<uint8_t> boundary; // per vertex
};

ParamMesh buildParamMesh(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale)
{
    ParamMesh m;
    m.pos = s.pos;
    m.scale = scale;
    m.surfaceVertices = s.vertexCount();
    std::vector<uint8_t> singular(s.tris.size(), 0);
    for (const FieldSingularity& f : fieldSingularities(s, q))
        singular[f.tri] = 1;

    // The given vertices' directions, matched by quarter turns to the first and averaged in the plane.
    auto meanDir = [&](std::initializer_list<Index> verts, const glm::dvec3& n) {
        glm::dvec3 ref(0.0), sum(0.0);
        for (Index v : verts)
        {
            glm::dvec3 d = q[v] - n * glm::dot(n, q[v]);
            if (glm::length(d) < 1e-12)
                continue;
            d = glm::normalize(d);
            if (glm::length(ref) == 0.0)
                ref = d;
            sum += turn(d, n, int(std::lround(angleAbout(d, ref, n) / kQuarter)));
        }
        return glm::length(sum) > 1e-12 ? glm::normalize(sum) : anyTangent(n);
    };
    for (size_t t = 0; t < s.tris.size(); ++t)
    {
        const auto& tri = s.tris[t];
        glm::dvec3 n = s.triNormal[t];
        if (!singular[t])
        {
            m.tris.push_back(tri);
            m.normal.push_back(n);
            m.dir.push_back(meanDir({tri[0], tri[1], tri[2]}, n));
            continue;
        }
        // Each third takes its own two corners' directions, so the field turns round the new centre vertex.
        Index c = Index(m.pos.size());
        m.pos.push_back((s.pos[tri[0]] + s.pos[tri[1]] + s.pos[tri[2]]) / 3.0);
        m.scale.push_back((scale[tri[0]] + scale[tri[1]] + scale[tri[2]]) / 3.0);
        for (int k = 0; k < 3; ++k)
        {
            m.tris.push_back({tri[k], tri[(k + 1) % 3], c});
            m.normal.push_back(n);
            m.dir.push_back(meanDir({tri[k], tri[(k + 1) % 3]}, n));
        }
    }

    std::unordered_map<uint64_t, Index> edgeOf;
    m.triEdges.resize(m.tris.size());
    for (size_t t = 0; t < m.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index a = m.tris[t][k], b = m.tris[t][(k + 1) % 3];
            auto [it, fresh] = edgeOf.try_emplace(edgeKey(a, b), Index(m.edges.size()));
            if (fresh)
            {
                m.edges.push_back({std::min(a, b), std::max(a, b)});
                m.edgeTris.push_back({Index(t), kInvalid});
                m.edgeTriCount.push_back(1);
            }
            else
            {
                Index e = it->second;
                if (m.edgeTriCount[e] < 2)
                    m.edgeTris[e][1] = Index(t);
                m.edgeTriCount[e] = uint8_t(std::min(3, m.edgeTriCount[e] + 1));
            }
            m.triEdges[t][k] = it->second;
        }
    m.boundary.assign(m.pos.size(), 0);
    for (size_t e = 0; e < m.edges.size(); ++e)
        if (m.edgeTriCount[e] != 2)
            m.boundary[m.edges[e][0]] = m.boundary[m.edges[e][1]] = 1;
    return m;
}

/** Quarter turns k with dir[t] ~ turn(dir[other], k) across edge e, unfolded about the edge. */
int matching(const ParamMesh& m, Index e, Index t, Index other)
{
    glm::dvec3 d = glm::normalize(m.pos[m.edges[e][1]] - m.pos[m.edges[e][0]]);
    double a = angleAbout(d, m.dir[t], m.normal[t]);
    double b = angleAbout(d, m.dir[other], m.normal[other]);
    return mod4(int(std::lround((a - b) / kQuarter)));
}

/**
 * Per vertex, whether the field turns round it: the quarter-wrapped jumps of the field across the fan's edges, measured
 * against those edges, plus the angle defect the fan closes over, come to a whole quarter turn or more.
 */
std::vector<uint8_t> singularVertices(const ParamMesh& m)
{
    std::vector<double> turn(m.pos.size(), glm::two_pi<double>());
    for (size_t t = 0; t < m.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index v = m.tris[t][k], next = m.tris[t][(k + 1) % 3], prev = m.tris[t][(k + 2) % 3];
            glm::dvec3 toNext = glm::normalize(m.pos[next] - m.pos[v]), toPrev = glm::normalize(m.pos[prev] - m.pos[v]);
            double gamma = angleAbout(toNext, toPrev, m.normal[t]);
            turn[v] -= gamma;
            // Leaving t across edge v-prev into the triangle that has it as its first edge from v.
            Index e = m.triEdges[t][(k + 2) % 3];
            if (m.edgeTriCount[e] != 2)
                continue;
            Index other = m.edgeTris[e][0] == Index(t) ? m.edgeTris[e][1] : m.edgeTris[e][0];
            double leaving = angleAbout(toNext, m.dir[t], m.normal[t]) - gamma;
            double entering = angleAbout(toPrev, m.dir[other], m.normal[other]);
            turn[v] += wrapQuarter(entering - leaving);
        }
    std::vector<uint8_t> out(m.pos.size(), 0);
    for (size_t v = 0; v < m.pos.size(); ++v)
        out[v] = !m.boundary[v] && std::lround(turn[v] / kQuarter) != 0;
    return out;
}
} // namespace

std::vector<glm::dvec3> solveSeamlessPositions(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale,
                                               std::string* error, std::vector<std::array<int, 2>>* edgeSteps)
{
    LILA_ZONE();

    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return std::vector<glm::dvec3>{};
    };
    ParamMesh m = buildParamMesh(s, q, scale);
    const Index nt = Index(m.tris.size()), nv = Index(m.pos.size()), ne = Index(m.edges.size());

    // Comb: turn each triangle's direction so it matches its parent in a spanning tree of the triangles, then the tree's
    // edges need no transition.
    std::vector<uint8_t> treeEdge(ne, 0), seen(nt, 0);
    for (Index root = 0; root < nt; ++root)
    {
        if (seen[root])
            continue;
        seen[root] = 1;
        std::vector<Index> todo = {root};
        for (size_t head = 0; head < todo.size(); ++head)
        {
            Index t = todo[head];
            for (int k = 0; k < 3; ++k)
            {
                Index e = m.triEdges[t][k];
                if (m.edgeTriCount[e] != 2)
                    continue;
                Index other = m.edgeTris[e][0] == t ? m.edgeTris[e][1] : m.edgeTris[e][0];
                if (seen[other])
                    continue;
                seen[other] = 1;
                treeEdge[e] = 1;
                m.dir[other] = turn(m.dir[other], m.normal[other], matching(m, e, t, other));
                todo.push_back(other);
            }
        }
    }

    // Cut graph: the edges the tree doesn't cross, pruned back to what joins singularities, borders and handles.
    std::vector<uint8_t> singular = singularVertices(m);
    std::vector<uint8_t> cut(ne, 0);
    std::vector<int> degree(nv, 0);
    for (Index e = 0; e < ne; ++e)
        if (m.edgeTriCount[e] == 2 && !treeEdge[e])
        {
            cut[e] = 1;
            ++degree[m.edges[e][0]];
            ++degree[m.edges[e][1]];
        }
    {
        std::vector<std::vector<Index>> vertexEdges(nv);
        for (Index e = 0; e < ne; ++e)
            if (cut[e])
            {
                vertexEdges[m.edges[e][0]].push_back(e);
                vertexEdges[m.edges[e][1]].push_back(e);
            }
        std::vector<Index> todo;
        auto prunable = [&](Index v) { return degree[v] == 1 && !singular[v] && !m.boundary[v]; };
        for (Index v = 0; v < nv; ++v)
            if (prunable(v))
                todo.push_back(v);
        while (!todo.empty())
        {
            Index v = todo.back();
            todo.pop_back();
            if (!prunable(v))
                continue;
            for (Index e : vertexEdges[v])
                if (cut[e])
                {
                    cut[e] = 0;
                    Index w = m.edges[e][0] == v ? m.edges[e][1] : m.edges[e][0];
                    --degree[v];
                    --degree[w];
                    if (prunable(w))
                        todo.push_back(w);
                    break;
                }
        }
    }

    // Wedges: the corners round a vertex not separated by a cut (or a border) share one parameter value.
    std::vector<Index> parent(size_t(nt) * 3);
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    auto cornerOf = [&](Index t, Index v) {
        for (int k = 0; k < 3; ++k)
            if (m.tris[t][k] == v)
                return t * 3 + Index(k);
        return kInvalid;
    };
    for (Index e = 0; e < ne; ++e)
        if (m.edgeTriCount[e] == 2 && !cut[e])
            for (Index v : m.edges[e])
                parent[find(cornerOf(m.edgeTris[e][0], v))] = find(cornerOf(m.edgeTris[e][1], v));
    std::vector<Index> wedgeOf(size_t(nt) * 3);
    std::unordered_map<Index, Index> wedgeOfRoot;
    for (Index c = 0; c < nt * 3; ++c)
    {
        auto [it, fresh] = wedgeOfRoot.try_emplace(find(c), Index(wedgeOfRoot.size()));
        wedgeOf[c] = it->second;
    }
    const Index nw = Index(wedgeOfRoot.size());

    // Each cut edge has an integer translation; along a chain the shared vertices tie them together.
    std::vector<Index> cutIndex(ne, kInvalid);
    Index nc = 0;
    for (Index e = 0; e < ne; ++e)
        if (cut[e])
            cutIndex[e] = nc++;

    // Unknowns: (u, v) per wedge, then (ju, jv) per cut edge.
    const Index nx = 2 * nw + 2 * nc;
    auto jVar = [&](Index c) { return 2 * nw + 2 * c; };

    // Gradient fit: across each triangle edge, the parameter changes by the edge in the triangle's frame over the spacing.
    std::vector<Eigen::Triplet<double>> hTrip;
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nx);
    double meanW = 0.0;
    for (Index t = 0; t < nt; ++t)
    {
        const auto& tri = m.tris[t];
        glm::dvec3 x = m.dir[t], y = glm::cross(m.normal[t], x);
        double h = (m.scale[tri[0]] + m.scale[tri[1]] + m.scale[tri[2]]) / 3.0;
        for (int k = 0; k < 3; ++k)
        {
            Index a = tri[k], b = tri[(k + 1) % 3], o = tri[(k + 2) % 3];
            glm::dvec3 ea = m.pos[a] - m.pos[o], eb = m.pos[b] - m.pos[o];
            double cotan = glm::dot(ea, eb) / std::max(glm::length(glm::cross(ea, eb)), 1e-300);
            double w = std::max(0.5 * cotan, 1e-3);
            meanW += w;
            glm::dvec3 d = m.pos[b] - m.pos[a];
            glm::dvec2 g(glm::dot(d, x) / h, glm::dot(d, y) / h);
            Index wa = wedgeOf[t * 3 + k], wb = wedgeOf[t * 3 + (k + 1) % 3];
            for (int c = 0; c < 2; ++c)
            {
                Index ia = 2 * wa + c, ib = 2 * wb + c;
                hTrip.emplace_back(ia, ia, w);
                hTrip.emplace_back(ib, ib, w);
                hTrip.emplace_back(ia, ib, -w);
                hTrip.emplace_back(ib, ia, -w);
                rhs[ib] += w * g[c];
                rhs[ia] -= w * g[c];
            }
        }
    }
    meanW /= std::max<Index>(1, nt * 3);
    for (Index i = 0; i < nx; ++i)
        hTrip.emplace_back(i, i, 1e-8 * meanW);

    // Transitions: across a cut, P(t side) = turn(P(other side), -k) + j.
    struct Row
    {
        std::vector<std::pair<Index, double>> terms;
        double value = 0.0;
    };
    std::vector<Row> rows;
    {
        for (Index e = 0; e < ne; ++e)
        {
            if (!cut[e])
                continue;
            Index t = m.edgeTris[e][0], o = m.edgeTris[e][1];
            int k = matching(m, e, t, o);
            Index j = jVar(cutIndex[e]);
            for (Index v : m.edges[e])
            {
                Index wa = wedgeOf[cornerOf(t, v)], wb = wedgeOf[cornerOf(o, v)];
                // Rows of turn2(., -k) as a matrix: columns are its images of (1, 0) and (0, 1).
                glm::dvec2 c0 = turn2({1.0, 0.0}, -k), c1 = turn2({0.0, 1.0}, -k);
                for (int r = 0; r < 2; ++r)
                {
                    Row row;
                    row.terms.push_back({2 * wa + r, 1.0});
                    if (c0[r] != 0.0)
                        row.terms.push_back({2 * wb, -c0[r]});
                    if (c1[r] != 0.0)
                        row.terms.push_back({2 * wb + 1, -c1[r]});
                    row.terms.push_back({j + r, -1.0});
                    rows.push_back(std::move(row));
                }
            }
        }
    }

    // Integer variables: cut translations, and singular vertices land on lattice points.
    // Feature lines and borders stay on lattice lines: along each such edge the coordinate across it is constant and
    // whole, and corners land on lattice points.
    std::vector<Index> featureInts;
    {
        std::vector<uint8_t> added(nx, 0);
        auto addInt = [&](Index var) {
            if (!added[var])
            {
                added[var] = 1;
                featureInts.push_back(var);
            }
        };
        auto surfaceEdge = [&](Index a, Index b) {
            for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
                if (s.adj[k] == b)
                    return s.adjEdge[k];
            return kInvalid;
        };
        for (Index e = 0; e < ne; ++e)
        {
            Index a = m.edges[e][0], b = m.edges[e][1];
            if (a >= m.surfaceVertices || b >= m.surfaceVertices)
                continue;
            Index se = surfaceEdge(a, b);
            if (se == kInvalid || !(s.edgeFeature[se] || s.edgeBoundary[se]))
                continue;
            for (int side = 0; side < std::min<int>(2, m.edgeTriCount[e]); ++side)
            {
                Index t = m.edgeTris[e][side];
                glm::dvec3 d = m.pos[b] - m.pos[a];
                glm::dvec3 xd = m.dir[t], yd = glm::cross(m.normal[t], xd);
                int across = std::abs(glm::dot(d, xd)) >= std::abs(glm::dot(d, yd)) ? 1 : 0;
                Index wa = wedgeOf[cornerOf(t, a)], wb = wedgeOf[cornerOf(t, b)];
                rows.push_back({{{2 * wa + across, 1.0}, {2 * wb + across, -1.0}}, 0.0});
                addInt(2 * wa + across);
                addInt(2 * wb + across);
            }
        }
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
                if (Index v = m.tris[t][k]; v < m.surfaceVertices && s.feature[v] == FeatureKind::Corner)
                {
                    addInt(2 * wedgeOf[t * 3 + k]);
                    addInt(2 * wedgeOf[t * 3 + k] + 1);
                }
    }

    // Singular vertices first: their coordinates decide the translations of the cuts that end at them.
    std::vector<Index> singularInts, cutInts;
    {
        std::vector<uint8_t> added(nw, 0);
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
                if (singular[m.tris[t][k]] && !added[wedgeOf[t * 3 + k]])
                {
                    Index w = wedgeOf[t * 3 + k];
                    added[w] = 1;
                    singularInts.insert(singularInts.end(), {2 * w, 2 * w + 1});
                }
    }
    for (Index c = 0; c < nc; ++c)
        cutInts.insert(cutInts.end(), {jVar(c), jVar(c) + 1});
    const bool anyIntegers = !singularInts.empty() || !featureInts.empty() || !cutInts.empty();

    Eigen::SparseMatrix<double> hess(nx, nx);
    hess.setFromTriplets(hTrip.begin(), hTrip.end());
    std::vector<std::pair<Index, double>> fixedVars;
    Eigen::VectorXd x;
    // Transitions and fixed integers enter as stiff penalties, which keeps the system positive definite even where
    // constraints repeat (a chain constrains its shared vertices from both edges).
    const double stiff = 1e6 * meanW;
    // Fixing an integer only adds to its diagonal, so the pattern is analysed once and each round refactors numerically.
    Eigen::SparseMatrix<double> base(nx, nx);
    Eigen::VectorXd baseRhs = rhs;
    {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(hess.nonZeros() + rows.size() * 16);
        for (int k = 0; k < hess.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(hess, k); it; ++it)
                trip.emplace_back(Index(it.row()), Index(it.col()), it.value());
        for (const Row& row : rows)
            for (auto [vi, ci] : row.terms)
            {
                for (auto [vj, cj] : row.terms)
                    trip.emplace_back(vi, vj, stiff * ci * cj);
                baseRhs[vi] += stiff * ci * row.value;
            }
        base.setFromTriplets(trip.begin(), trip.end());
        base.makeCompressed();
    }
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;
    ldlt.analyzePattern(base);
    auto solve = [&]() {
        Eigen::SparseMatrix<double> a = base;
        Eigen::VectorXd b = baseRhs;
        for (auto [var, value] : fixedVars)
        {
            a.coeffRef(var, var) += stiff;
            b[var] += stiff * value;
        }
        ldlt.factorize(a);
        if (ldlt.info() != Eigen::Success)
            return false;
        Eigen::VectorXd sol = ldlt.solve(b);
        if (ldlt.info() != Eigen::Success || !sol.allFinite())
            return false;
        x = std::move(sol);
        return true;
    };

    // Greedy rounding: fix the integers closest to a whole number first, a growing batch at a time, re-solving between.
    std::vector<uint8_t> isFixed(nx, 0);
    if (!anyIntegers)
        fixedVars.push_back({0, 0.0}); // no cuts and no singularities: pin the translation
    auto countFlipped = [&]() {
        Index flipped = 0;
        for (Index t = 0; t < nt; ++t)
        {
            auto at = [&](int k) { return glm::dvec2(x[2 * wedgeOf[t * 3 + k]], x[2 * wedgeOf[t * 3 + k] + 1]); };
            glm::dvec2 a = at(1) - at(0), b = at(2) - at(0);
            flipped += a.x * b.y - a.y * b.x <= 0.0;
        }
        return flipped;
    };
    const bool debug = std::getenv("LILA_SEAMLESS_DEBUG") != nullptr;
    if (!solve())
        return fail("seamless parametrization didn't solve");
    if (debug)
    {
        Index sing = 0;
        for (Index v = 0; v < nv; ++v)
            sing += singular[v];
        std::fprintf(stderr, "seamless: %d tris, %zu field poles, %d singular verts, %d cut edges, %zu feature ints; continuous flips %d\n",
                     int(nt), fieldSingularities(s, q).size(), int(sing), int(nc), featureInts.size(), int(countFlipped()));
    }
    for (const std::vector<Index>* stage : {&singularInts, &featureInts, &cutInts})
    {
        if (debug)
            std::fprintf(stderr, "  before stage of %zu ints: flips %d\n", stage->size(), int(countFlipped()));
        for (int round = 0; round < 64; ++round)
        {
            Budget::checkpoint();
            std::vector<std::pair<double, Index>> open;
            for (Index var : *stage)
                if (!isFixed[var])
                    open.push_back({std::abs(x[var] - std::round(x[var])), var});
            if (open.empty())
                break;
            std::sort(open.begin(), open.end());
            size_t batch = std::max<size_t>(1, open.size() / 3);
            for (size_t i = 0; i < open.size() && (i < batch || open[i].first < 0.05); ++i)
            {
                isFixed[open[i].second] = 1;
                fixedVars.push_back({open[i].second, std::round(x[open[i].second])});
            }
            if (!solve())
                return fail("seamless parametrization didn't solve after rounding");
        }
    }
    const Index flipped = countFlipped();
    if (debug)
        std::fprintf(stderr, "  final flips %d of %d\n", int(flipped), int(nt));

    // Folded triangles mean the rounding forced the lattice through itself; the local position field does better then.
    if (flipped > nt / 200)
        return fail("the seamless parametrization folds");

    // Each surface vertex's nearest lattice point, from any one of its corners.
    std::vector<glm::dvec3> o(s.vertexCount());
    std::vector<uint8_t> have(s.vertexCount(), 0);
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index v = m.tris[t][k];
            if (v >= m.surfaceVertices || have[v])
                continue;
            have[v] = 1;
            Index w = wedgeOf[t * 3 + k];
            glm::dvec3 xd = m.dir[t], yd = glm::cross(m.normal[t], xd);
            double u = x[2 * w], vv = x[2 * w + 1];
            o[v] = m.pos[v] - scale[v] * ((u - std::round(u)) * xd + (vv - std::round(vv)) * yd);
        }
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (!have[v])
            o[v] = s.pos[v];
    if (edgeSteps)
    {
        // Within one triangle all three corners share a chart, so rounding there gives the step exactly.
        edgeSteps->assign(s.edges.size(), {0, 0});
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
            {
                const Index a = m.tris[t][k], b = m.tris[t][(k + 1) % 3];
                if (a >= m.surfaceVertices || b >= m.surfaceVertices)
                    continue;
                Index se = kInvalid;
                for (Index i = s.adjStart[a]; i < s.adjStart[a + 1] && se == kInvalid; ++i)
                    if (s.adj[i] == b)
                        se = s.adjEdge[i];
                if (se == kInvalid)
                    continue;
                const Index wa = wedgeOf[t * 3 + k], wb = wedgeOf[t * 3 + (k + 1) % 3];
                std::array<int, 2> step = {int(std::lround(x[2 * wb]) - std::lround(x[2 * wa])),
                                           int(std::lround(x[2 * wb + 1]) - std::lround(x[2 * wa + 1]))};
                if (s.edges[se][0] != a)
                    step = {-step[0], -step[1]};
                (*edgeSteps)[se] = step;
            }
    }
    return o;
}
} // namespace lila::retopo::detail
