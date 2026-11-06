// SPDX-License-Identifier: Apache-2.0
#include "groomlab/portable_math.h"
#include "groomlab/subdivision.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/process_caches.h"

namespace groomlab {

int subdivisionLevels(const std::string& param, int matchRender) {
    int levels = matchRender;
    if (param == "off" || param == "0") {
        levels = 0;
    } else if (param.size() == 1 && param[0] >= '1' && param[0] <= '9') {
        levels = param[0] - '0';
    }
    return std::clamp(levels, 0, kMaxSubdivisionLevels);
}

namespace {

// A polygon mesh's faces: face f's corners are verts[offsets[f]] .. verts[offsets[f + 1] - 1].
struct Faces {
    std::vector<std::int32_t> offsets{0};
    std::vector<std::int32_t> verts;
    std::size_t count() const { return offsets.size() - 1; }
};

enum class Rule : std::uint8_t { Smooth, Crease, Corner };

// A UV as a value the rules mix.
struct UV2 {
    float u = 0.0f, v = 0.0f;
    UV2 operator+(const UV2& o) const { return {u + o.u, v + o.v}; }
    UV2 operator*(float s) const { return {u * s, v * s}; }
};

}  // namespace

// One level's refinement of a mesh (its vertices, or a UV layout's values) into the next: the
// parent's faces and edges and each vertex's rule. Child vertices: the parent's vertices first
// (their new places), then one per edge, then one per face; child faces: a quad per parent face
// corner, in corner order.
struct SubdivisionPlan::Step {
    std::int32_t vertices = 0, edges = 0;
    Faces parent;
    std::vector<std::array<std::int32_t, 2>> edgeVerts;
    std::vector<std::array<std::int32_t, 2>> edgeFaces;  // [1] < 0: a sharp edge (boundary, non-manifold)
    std::vector<std::int32_t> cornerEdge;                // per parent face corner: the edge to the next corner
    std::vector<Rule> rules;
    std::vector<std::array<std::int32_t, 2>> creaseEnds;  // a crease vertex's two crease neighbours
    std::vector<std::int32_t> ringOffsets, ring;          // each vertex's edges' other ends
    std::vector<std::int32_t> fanOffsets, fan;            // each vertex's faces

    std::int32_t faces() const { return static_cast<std::int32_t>(parent.count()); }
    std::int32_t childVertices() const { return vertices + edges + faces(); }

    static std::unique_ptr<Step> build(Faces faces, std::int32_t vertexCount);
    Faces childFaces() const;

    // The Catmull-Clark rules over values per parent vertex (in) into values per child vertex.
    template <class T>
    std::vector<T> smooth(const std::vector<T>& in) const;
    // Linear: vertices keep their values, edge and face points take their ends' and corners' mean.
    std::vector<float> linear(const std::vector<float>& in, int channels) const;
};

std::unique_ptr<SubdivisionPlan::Step> SubdivisionPlan::Step::build(Faces faces, std::int32_t vertexCount) {
    auto s = std::make_unique<Step>();
    s->vertices = vertexCount;
    s->parent = std::move(faces);
    const Faces& f = s->parent;
    const std::size_t corners = f.verts.size();
    const auto faceCount = static_cast<std::int32_t>(f.count());

    // Edges: the corners sorted by their edge's two ends.
    std::vector<std::int32_t> faceOf(corners), nextOf(corners);
    for (std::int32_t face = 0; face < faceCount; ++face) {
        const std::int32_t b = f.offsets[face], e = f.offsets[face + 1];
        for (std::int32_t c = b; c < e; ++c) {
            faceOf[c] = face;
            nextOf[c] = c + 1 < e ? c + 1 : b;
        }
    }
    std::vector<std::pair<std::uint64_t, std::int32_t>> keyed(corners);
    parallelFor(0, corners, [&](std::size_t c) {
        const auto a = static_cast<std::uint32_t>(f.verts[c]), b = static_cast<std::uint32_t>(f.verts[nextOf[c]]);
        const std::uint64_t lo = std::min(a, b), hi = std::max(a, b);
        keyed[c] = {(lo << 32) | hi, static_cast<std::int32_t>(c)};
    }, 4096);
    std::sort(keyed.begin(), keyed.end());
    s->cornerEdge.assign(corners, -1);
    for (std::size_t i = 0; i < corners;) {
        std::size_t j = i + 1;
        while (j < corners && keyed[j].first == keyed[i].first) ++j;
        const auto edge = static_cast<std::int32_t>(s->edgeVerts.size());
        const std::int32_t c0 = keyed[i].second;
        s->edgeVerts.push_back({f.verts[c0], f.verts[nextOf[c0]]});
        // Two faces: smooth. One (a boundary), more (non-manifold) or an edge from a vertex to
        // itself: sharp.
        const bool smooth = j - i == 2 && f.verts[c0] != f.verts[nextOf[c0]];
        s->edgeFaces.push_back({faceOf[c0], smooth ? faceOf[keyed[i + 1].second] : -1});
        for (std::size_t k = i; k < j; ++k) s->cornerEdge[keyed[k].second] = edge;
        i = j;
    }
    s->edges = static_cast<std::int32_t>(s->edgeVerts.size());

    // Each vertex's ring (its edges' other ends) and fan (its faces).
    const auto n = static_cast<std::size_t>(vertexCount);
    s->ringOffsets.assign(n + 1, 0);
    s->fanOffsets.assign(n + 1, 0);
    std::vector<std::int32_t> sharp(n, 0);
    for (std::int32_t e = 0; e < s->edges; ++e) {
        const auto [a, b] = s->edgeVerts[e];
        if (a == b) continue;
        ++s->ringOffsets[a + 1];
        ++s->ringOffsets[b + 1];
        if (s->edgeFaces[e][1] < 0) {
            ++sharp[a];
            ++sharp[b];
        }
    }
    for (std::size_t c = 0; c < corners; ++c) ++s->fanOffsets[f.verts[c] + 1];
    for (std::size_t v = 0; v < n; ++v) {
        s->ringOffsets[v + 1] += s->ringOffsets[v];
        s->fanOffsets[v + 1] += s->fanOffsets[v];
    }
    s->ring.resize(s->ringOffsets[n]);
    s->fan.resize(s->fanOffsets[n]);
    s->creaseEnds.assign(n, {-1, -1});
    {
        std::vector<std::int32_t> at(s->ringOffsets.begin(), s->ringOffsets.end() - 1);
        std::vector<std::int32_t> creases(n, 0);
        for (std::int32_t e = 0; e < s->edges; ++e) {
            const auto [a, b] = s->edgeVerts[e];
            if (a == b) continue;
            s->ring[at[a]++] = b;
            s->ring[at[b]++] = a;
            if (s->edgeFaces[e][1] < 0) {
                if (creases[a] < 2) s->creaseEnds[a][creases[a]] = b;
                if (creases[b] < 2) s->creaseEnds[b][creases[b]] = a;
                ++creases[a];
                ++creases[b];
            }
        }
        std::vector<std::int32_t> fat(s->fanOffsets.begin(), s->fanOffsets.end() - 1);
        for (std::size_t c = 0; c < corners; ++c) s->fan[fat[f.verts[c]]++] = faceOf[c];
    }

    // The rules ("edge and corner" boundaries): two crease edges make a crease vertex, unless it
    // has one face (a corner); more than two, or no face at all, make a corner; none or one (a
    // dart) the smooth rule.
    s->rules.assign(n, Rule::Smooth);
    for (std::size_t v = 0; v < n; ++v) {
        const std::int32_t fanSize = s->fanOffsets[v + 1] - s->fanOffsets[v];
        if (fanSize == 0 || sharp[v] > 2) {
            s->rules[v] = Rule::Corner;
        } else if (sharp[v] == 2) {
            s->rules[v] = fanSize == 1 ? Rule::Corner : Rule::Crease;
        }
    }
    return s;
}

Faces SubdivisionPlan::Step::childFaces() const {
    Faces out;
    const std::size_t corners = parent.verts.size();
    out.offsets.resize(corners + 1);
    out.verts.resize(corners * 4);
    for (std::size_t q = 0; q <= corners; ++q) out.offsets[q] = static_cast<std::int32_t>(q * 4);
    const std::int32_t edgeBase = vertices, faceBase = vertices + edges;
    for (std::int32_t face = 0; face < faces(); ++face) {
        const std::int32_t b = parent.offsets[face], e = parent.offsets[face + 1];
        for (std::int32_t c = b; c < e; ++c) {
            const std::int32_t prev = c > b ? c - 1 : e - 1;
            std::int32_t* quad = &out.verts[static_cast<std::size_t>(c) * 4];
            quad[0] = parent.verts[c];
            quad[1] = edgeBase + cornerEdge[c];
            quad[2] = faceBase + face;
            quad[3] = edgeBase + cornerEdge[prev];
        }
    }
    return out;
}

template <class T>
std::vector<T> SubdivisionPlan::Step::smooth(const std::vector<T>& in) const {
    std::vector<T> out(static_cast<std::size_t>(childVertices()));
    T* facePoints = out.data() + vertices + edges;
    T* edgePoints = out.data() + vertices;
    parallelFor(0, static_cast<std::size_t>(faces()), [&](std::size_t face) {
        const std::int32_t b = parent.offsets[face], e = parent.offsets[face + 1];
        T sum{};
        for (std::int32_t c = b; c < e; ++c) sum = sum + in[parent.verts[c]];
        facePoints[face] = sum * (1.0f / static_cast<float>(e - b));
    }, 1024);
    parallelFor(0, static_cast<std::size_t>(edges), [&](std::size_t e) {
        const auto [a, b] = edgeVerts[e];
        const auto [f0, f1] = edgeFaces[e];
        edgePoints[e] = f1 < 0 ? (in[a] + in[b]) * 0.5f : (in[a] + in[b] + facePoints[f0] + facePoints[f1]) * 0.25f;
    }, 1024);
    parallelFor(0, static_cast<std::size_t>(vertices), [&](std::size_t v) {
        const T& p = in[v];
        switch (rules[v]) {
            case Rule::Corner:
                out[v] = p;
                break;
            case Rule::Crease:
                out[v] = (in[creaseEnds[v][0]] + in[creaseEnds[v][1]] + p * 6.0f) * 0.125f;
                break;
            case Rule::Smooth: {
                // (F + R' + (n - 2) P) / n: F the mean of the faces' points, R' of the ring's
                // vertices (the edge midpoints' 2R = P + R'), n the valence.
                const std::int32_t rb = ringOffsets[v], re = ringOffsets[v + 1];
                const std::int32_t fb = fanOffsets[v], fe = fanOffsets[v + 1];
                const float valence = static_cast<float>(re - rb);
                if (re == rb) {
                    out[v] = p;
                    break;
                }
                T ringSum{}, faceSum{};
                for (std::int32_t k = rb; k < re; ++k) ringSum = ringSum + in[ring[k]];
                for (std::int32_t k = fb; k < fe; ++k) faceSum = faceSum + facePoints[fan[k]];
                out[v] = (faceSum * (1.0f / static_cast<float>(fe - fb)) + ringSum * (1.0f / valence) + p * (valence - 2.0f)) *
                         (1.0f / valence);
                break;
            }
        }
    }, 1024);
    return out;
}

std::vector<float> SubdivisionPlan::Step::linear(const std::vector<float>& in, int channels) const {
    const auto ch = static_cast<std::size_t>(channels);
    std::vector<float> out(static_cast<std::size_t>(childVertices()) * ch, 0.0f);
    std::copy(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(vertices) * ch), out.begin());
    float* edgeValues = out.data() + static_cast<std::size_t>(vertices) * ch;
    float* faceValues = out.data() + static_cast<std::size_t>(vertices + edges) * ch;
    parallelFor(0, static_cast<std::size_t>(edges), [&](std::size_t e) {
        const auto [a, b] = edgeVerts[e];
        for (std::size_t k = 0; k < ch; ++k) edgeValues[e * ch + k] = 0.5f * (in[a * ch + k] + in[b * ch + k]);
    }, 2048);
    parallelFor(0, static_cast<std::size_t>(faces()), [&](std::size_t face) {
        const std::int32_t b = parent.offsets[face], e = parent.offsets[face + 1];
        const float w = 1.0f / static_cast<float>(e - b);
        for (std::int32_t c = b; c < e; ++c) {
            for (std::size_t k = 0; k < ch; ++k) faceValues[face * ch + k] += w * in[static_cast<std::size_t>(parent.verts[c]) * ch + k];
        }
    }, 2048);
    return out;
}

namespace {

// The cage's faces from its triangles: each host polygon (ScalpMesh::polygons) as one face, its
// corners in order round its border (the edges its triangles do not share, followed head to
// tail), each corner's data read from a triangle corner at that vertex. A polygon whose triangles
// make no single border (a vertex met twice, triangles not joined) stays as its triangles.
// Without polygons every triangle is a face. `triangleCorners`: per triangle corner, its face
// corner (an index into faces.verts).
void cageFaces(const ScalpMesh& m, Faces& faces, std::vector<std::int32_t>& source, std::vector<std::int32_t>& triangleCorners) {
    const auto& tris = m.triangles();
    triangleCorners.assign(tris.size() * 3, -1);
    auto addTriangle = [&](std::size_t t) {
        for (int k = 0; k < 3; ++k) {
            triangleCorners[t * 3 + k] = static_cast<std::int32_t>(faces.verts.size());
            faces.verts.push_back(tris[t][k]);
            source.push_back(static_cast<std::int32_t>(t * 3 + k));
        }
        faces.offsets.push_back(static_cast<std::int32_t>(faces.verts.size()));
    };
    if (m.polygons().empty()) {
        for (std::size_t t = 0; t < tris.size(); ++t) addTriangle(t);
        return;
    }
    struct Half {
        std::int32_t from, to, corner;
    };
    std::vector<Half> halves, border;
    std::size_t first = 0;
    for (const std::int32_t count : m.polygons()) {
        const std::size_t end = first + static_cast<std::size_t>(count);
        if (count == 1) {
            addTriangle(first);
        } else if (count > 1) {
            halves.clear();
            for (std::size_t t = first; t < end; ++t) {
                for (int k = 0; k < 3; ++k) halves.push_back({tris[t][k], tris[t][(k + 1) % 3], static_cast<std::int32_t>(t * 3 + k)});
            }
            border.clear();
            for (const Half& h : halves) {
                bool shared = false;
                for (const Half& o : halves) shared = shared || (o.from == h.to && o.to == h.from);
                if (!shared) border.push_back(h);
            }
            // Follow the border from its first edge; it must close after every border edge, once.
            std::vector<std::int32_t> loop, corners;
            bool ok = border.size() == static_cast<std::size_t>(count) + 2;
            std::int32_t at = ok ? border.front().from : 0;
            const Half* h = ok ? &border.front() : nullptr;
            while (ok && loop.size() < border.size()) {
                if (std::find(loop.begin(), loop.end(), h->from) != loop.end()) {
                    ok = false;
                    break;
                }
                loop.push_back(h->from);
                corners.push_back(h->corner);
                at = h->to;
                const Half* next = nullptr;
                for (const Half& o : border) {
                    if (o.from == at) next = next ? nullptr : &o;  // two ways on: not one border
                }
                if (!next) ok = false;
                h = next;
            }
            ok = ok && at == loop.front();
            // Every triangle's corners on the border (as a polygon's triangles are).
            for (std::size_t t = first; ok && t < end; ++t) {
                for (int k = 0; k < 3 && ok; ++k) {
                    const auto it = std::find(loop.begin(), loop.end(), tris[t][k]);
                    ok = it != loop.end();
                    if (ok) triangleCorners[t * 3 + k] = static_cast<std::int32_t>(faces.verts.size()) + static_cast<std::int32_t>(it - loop.begin());
                }
            }
            if (ok) {
                faces.verts.insert(faces.verts.end(), loop.begin(), loop.end());
                source.insert(source.end(), corners.begin(), corners.end());
                faces.offsets.push_back(static_cast<std::int32_t>(faces.verts.size()));
            } else {
                for (std::size_t t = first; t < end; ++t) addTriangle(t);
            }
        }
        first = end;
    }
}

// The UV layout as a mesh of its own: a value per distinct (vertex, UV) pair, so faces sharing a
// vertex and its UV share the value and a seam (the same vertex, other UVs) splits it.
Faces uvFaces(const Faces& faces, const std::vector<std::int32_t>& source, const std::vector<ScalpMesh::UV>& cornerUVs,
              std::vector<UV2>& values) {
    struct Key {
        std::int32_t vertex;
        std::uint32_t u, v;
        bool operator==(const Key& o) const { return vertex == o.vertex && u == o.u && v == o.v; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(hashCombine(hashCombine(static_cast<std::uint64_t>(k.vertex), k.u), k.v));
        }
    };
    std::unordered_map<Key, std::int32_t, KeyHash> ids;
    ids.reserve(faces.verts.size());
    Faces out;
    out.offsets = faces.offsets;
    out.verts.resize(faces.verts.size());
    for (std::size_t c = 0; c < faces.verts.size(); ++c) {
        const ScalpMesh::UV& uv = cornerUVs[static_cast<std::size_t>(source[c])];
        Key key{faces.verts[c], 0, 0};
        std::memcpy(&key.u, &uv[0], 4);
        std::memcpy(&key.v, &uv[1], 4);
        const auto [it, added] = ids.emplace(key, static_cast<std::int32_t>(values.size()));
        if (added) values.push_back({uv[0], uv[1]});
        out.verts[c] = it->second;
    }
    return out;
}

}  // namespace

SubdivisionPlan::~SubdivisionPlan() = default;

std::shared_ptr<const SubdivisionPlan> SubdivisionPlan::build(const ScalpMesh& cage, int levels) {
    if (levels < 1 || levels > kMaxSubdivisionLevels) throw std::invalid_argument("SubdivisionPlan: levels must be 1 to 3");
    const std::vector<int> partLevels = pieceSubdivisionLevels(cage, levels);
    if (!partLevels.empty()) return buildParts(cage, levels, partLevels);
    std::shared_ptr<SubdivisionPlan> plan(new SubdivisionPlan());
    plan->levels_ = levels;
    plan->cagePoints_ = cage.pointCount();
    Faces faces;
    cageFaces(cage, faces, plan->cageCornerSource_, plan->cageTriangleCorners_);
    plan->cageFaces_ = faces.count();
    plan->cageFaceOffsets_ = faces.offsets;
    // Each face's triangles (cageRoot finds a point's among them).
    {
        const std::size_t triangles = cage.triangleCount();
        std::vector<std::int32_t> faceOf(triangles);
        plan->faceTriangleOffsets_.assign(faces.count() + 1, 0);
        for (std::size_t t = 0; t < triangles; ++t) {
            const std::int32_t corner = plan->cageTriangleCorners_[t * 3];
            const auto it = std::upper_bound(faces.offsets.begin(), faces.offsets.end(), corner);
            faceOf[t] = static_cast<std::int32_t>(it - faces.offsets.begin()) - 1;
            ++plan->faceTriangleOffsets_[static_cast<std::size_t>(faceOf[t]) + 1];
        }
        for (std::size_t f = 0; f < faces.count(); ++f) plan->faceTriangleOffsets_[f + 1] += plan->faceTriangleOffsets_[f];
        plan->faceTriangles_.resize(triangles);
        std::vector<std::int32_t> at(plan->faceTriangleOffsets_.begin(), plan->faceTriangleOffsets_.end() - 1);
        for (std::size_t t = 0; t < triangles; ++t) plan->faceTriangles_[at[faceOf[t]]++] = static_cast<std::int32_t>(t);
    }

    std::vector<UV2> uvValues;
    Faces uvLayout;
    if (cage.hasUVs()) uvLayout = uvFaces(faces, plan->cageCornerSource_, cage.cornerUVs(), uvValues);

    auto vertexCount = static_cast<std::int32_t>(cage.pointCount());
    for (int l = 0; l < levels; ++l) {
        plan->vertexSteps_.push_back(Step::build(l == 0 ? faces : plan->vertexSteps_.back()->childFaces(), vertexCount));
        vertexCount = plan->vertexSteps_.back()->childVertices();
    }
    plan->points_ = static_cast<std::size_t>(vertexCount);

    // The last level's quads, two triangles each.
    const Faces quads = plan->vertexSteps_.back()->childFaces();
    plan->triangles_.resize(quads.count() * 2);
    parallelFor(0, quads.count(), [&](std::size_t q) {
        const std::int32_t* v = &quads.verts[q * 4];
        plan->triangles_[2 * q] = {v[0], v[1], v[2]};
        plan->triangles_[2 * q + 1] = {v[0], v[2], v[3]};
    }, 4096);

    // UVs never move with the cage: refined once, here.
    if (cage.hasUVs()) {
        auto uvCount = static_cast<std::int32_t>(uvValues.size());
        for (int l = 0; l < levels; ++l) {
            plan->uvSteps_.push_back(Step::build(l == 0 ? uvLayout : plan->uvSteps_.back()->childFaces(), uvCount));
            uvValues = plan->uvSteps_.back()->smooth(uvValues);
            uvCount = plan->uvSteps_.back()->childVertices();
        }
        const Faces uvQuads = plan->uvSteps_.back()->childFaces();
        plan->cornerUVs_.resize(plan->triangles_.size() * 3);
        parallelFor(0, uvQuads.count(), [&](std::size_t q) {
            const std::int32_t* v = &uvQuads.verts[q * 4];
            constexpr int kCorners[6] = {0, 1, 2, 0, 2, 3};
            for (int k = 0; k < 6; ++k) {
                const UV2& uv = uvValues[v[kCorners[k]]];
                plan->cornerUVs_[q * 6 + k] = {uv.u, uv.v};
            }
        }, 4096);
    }
    return plan;
}

std::vector<int> pieceSubdivisionLevels(const ScalpMesh& cage, int levels) {
    std::vector<int> out;
    bool own = false;
    for (const ScalpPiece& p : cage.pieces()) {
        const int l = p.subdivision >= 0 ? std::min(p.subdivision, kMaxSubdivisionLevels) : levels;
        own = own || l != levels;
        out.push_back(l);
    }
    if (!own) out.clear();
    return out;
}

namespace {

// Scalp `p` of `cage` as a mesh of its own for its plan: every one of its points (in order, so
// point i of it is point firstPoint + i of the cage), its triangles, their UVs and the polygons
// within it (none when the range cuts one).
ScalpMesh partCage(const ScalpMesh& cage, const ScalpPiece& p) {
    const auto fp = static_cast<std::ptrdiff_t>(p.firstPoint);
    std::vector<Vec3f> points(cage.points().begin() + fp, cage.points().begin() + fp + static_cast<std::ptrdiff_t>(p.pointCount));
    std::vector<ScalpMesh::Triangle> triangles;
    triangles.reserve(p.triangleCount);
    const auto offset = static_cast<std::int32_t>(p.firstPoint);
    for (std::size_t t = p.firstTriangle; t < p.firstTriangle + p.triangleCount; ++t) {
        const ScalpMesh::Triangle& tri = cage.triangles()[t];
        triangles.push_back({tri[0] - offset, tri[1] - offset, tri[2] - offset});
    }
    ScalpMesh out(std::move(points), std::move(triangles));
    if (cage.hasUVs()) {
        out.setCornerUVs({cage.cornerUVs().begin() + static_cast<std::ptrdiff_t>(p.firstTriangle * 3),
                          cage.cornerUVs().begin() + static_cast<std::ptrdiff_t>((p.firstTriangle + p.triangleCount) * 3)});
    }
    if (!cage.polygons().empty()) {
        std::vector<std::int32_t> polygons;
        std::size_t at = 0, covered = 0;
        bool aligned = true;
        for (const std::int32_t n : cage.polygons()) {
            const std::size_t next = at + static_cast<std::size_t>(n);
            if (at >= p.firstTriangle && next <= p.firstTriangle + p.triangleCount) {
                polygons.push_back(n);
                covered += static_cast<std::size_t>(n);
            } else if (next > p.firstTriangle && at < p.firstTriangle + p.triangleCount) {
                aligned = false;
            }
            at = next;
        }
        if (aligned && covered == p.triangleCount) out.setPolygons(std::move(polygons));
    }
    return out;
}

}  // namespace

std::shared_ptr<const SubdivisionPlan> SubdivisionPlan::buildParts(const ScalpMesh& cage, int levels, const std::vector<int>& partLevels) {
    std::shared_ptr<SubdivisionPlan> plan(new SubdivisionPlan());
    plan->levels_ = levels;
    plan->cagePoints_ = cage.pointCount();
    const std::vector<ScalpPiece>& pieces = cage.pieces();
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const ScalpPiece& p = pieces[i];
        Part part;
        part.firstTriangle = p.firstTriangle;
        part.triangleCount = p.triangleCount;
        part.firstPoint = p.firstPoint;
        part.pointCount = p.pointCount;
        part.refinedFirstTriangle = plan->triangles_.size();
        part.refinedFirstPoint = plan->points_;
        const ScalpMesh own = partCage(cage, p);
        const std::vector<ScalpMesh::Triangle>* triangles = &own.triangles();
        const std::vector<ScalpMesh::UV>* uvs = own.hasUVs() ? &own.cornerUVs() : nullptr;
        std::size_t faces = own.polygons().empty() ? own.triangleCount() : own.polygons().size();
        part.refinedPointCount = own.pointCount();
        if (partLevels[i] > 0 && own.triangleCount() > 0) {
            part.plan = build(own, partLevels[i]);
            triangles = &part.plan->triangles_;
            uvs = part.plan->cornerUVs_.empty() ? nullptr : &part.plan->cornerUVs_;
            faces = part.plan->cageFaces_;
            part.refinedPointCount = part.plan->points_;
        }
        part.refinedTriangleCount = triangles->size();
        const auto offset = static_cast<std::int32_t>(part.refinedFirstPoint);
        for (const ScalpMesh::Triangle& t : *triangles) plan->triangles_.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
        if (cage.hasUVs() && uvs) plan->cornerUVs_.insert(plan->cornerUVs_.end(), uvs->begin(), uvs->end());
        plan->points_ += part.refinedPointCount;
        plan->cageFaces_ += faces;
        plan->parts_.push_back(std::move(part));
    }
    return plan;
}

const SubdivisionPlan::Part* SubdivisionPlan::partOfCageTriangle(std::int32_t face) const {
    if (face < 0) return nullptr;
    const auto f = static_cast<std::size_t>(face);
    for (const Part& part : parts_) {
        if (f >= part.firstTriangle && f < part.firstTriangle + part.triangleCount) return &part;
    }
    return nullptr;
}

const SubdivisionPlan::Part* SubdivisionPlan::partOfRefinedTriangle(std::int32_t face) const {
    if (face < 0) return nullptr;
    const auto f = static_cast<std::size_t>(face);
    for (const Part& part : parts_) {
        if (f >= part.refinedFirstTriangle && f < part.refinedFirstTriangle + part.refinedTriangleCount) return &part;
    }
    return nullptr;
}

std::shared_ptr<const SubdivisionPlan> SubdivisionPlan::of(const ScalpMesh& cage, int levels) {
    // A few plans, most recent first: a session's scalp and colliders at their levels.
    constexpr std::size_t kKept = 8;
    static std::mutex mutex;
    static std::list<std::pair<std::uint64_t, std::shared_ptr<const SubdivisionPlan>>> kept;
    static const bool registered = (registerProcessCache("subdivision plans", [] {
                                        std::list<std::pair<std::uint64_t, std::shared_ptr<const SubdivisionPlan>>> drop;
                                        const std::lock_guard<std::mutex> lock(mutex);
                                        drop.swap(kept);
                                    }),
                                    true);
    (void)registered;
    std::uint64_t key = hashCombine(hashCombine(hash64(0xCC5D), cage.topologyHash()), cage.polygonHash());
    key = hashCombine(hashCombine(hashCombine(key, cage.uvHash()), cage.pointCount()), static_cast<std::uint64_t>(levels));
    if (cage.piecesHash() != 0) key = hashCombine(key, cage.piecesHash());  // the scalps' own levels
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (it->first != key) continue;
            kept.splice(kept.begin(), kept, it);
            return kept.front().second;
        }
    }
    std::shared_ptr<const SubdivisionPlan> plan = build(cage, levels);
    const std::lock_guard<std::mutex> lock(mutex);
    kept.emplace_front(key, plan);
    if (kept.size() > kKept) kept.pop_back();
    return plan;
}

std::vector<Vec3f> SubdivisionPlan::refinePoints(const std::vector<Vec3f>& cagePoints) const {
    if (cagePoints.size() != cagePoints_) throw std::invalid_argument("SubdivisionPlan: the points are not the cage's");
    if (!parts_.empty()) {
        std::vector<Vec3f> out;
        out.reserve(points_);
        for (const Part& part : parts_) {
            const auto first = cagePoints.begin() + static_cast<std::ptrdiff_t>(part.firstPoint);
            std::vector<Vec3f> own(first, first + static_cast<std::ptrdiff_t>(part.pointCount));
            if (part.plan) own = part.plan->refinePoints(own);
            out.insert(out.end(), own.begin(), own.end());
        }
        return out;
    }
    std::vector<Vec3f> points = vertexSteps_.front()->smooth(cagePoints);
    for (std::size_t l = 1; l < vertexSteps_.size(); ++l) points = vertexSteps_[l]->smooth(points);
    return points;
}

std::vector<float> SubdivisionPlan::refineVertexValues(const std::vector<float>& values, int channels) const {
    if (channels < 1 || values.size() != cagePoints_ * static_cast<std::size_t>(channels)) {
        throw std::invalid_argument("SubdivisionPlan: the values are not the cage's vertices'");
    }
    if (!parts_.empty()) {
        const auto per = static_cast<std::size_t>(channels);
        std::vector<float> out;
        out.reserve(points_ * per);
        for (const Part& part : parts_) {
            const auto first = values.begin() + static_cast<std::ptrdiff_t>(part.firstPoint * per);
            std::vector<float> own(first, first + static_cast<std::ptrdiff_t>(part.pointCount * per));
            if (part.plan) own = part.plan->refineVertexValues(own, channels);
            out.insert(out.end(), own.begin(), own.end());
        }
        return out;
    }
    std::vector<float> out = vertexSteps_.front()->linear(values, channels);
    for (std::size_t l = 1; l < vertexSteps_.size(); ++l) out = vertexSteps_[l]->linear(out, channels);
    return out;
}

std::vector<ScalpMesh::Color> SubdivisionPlan::refineCorners(const std::vector<ScalpMesh::Color>& triangleCorners) const {
    // Per face corner, level by level: child quad c (of parent corner c, face f) takes the
    // corner's value, the mean with the next corner, the face's mean, the mean with the previous.
    using Color = ScalpMesh::Color;
    if (!parts_.empty()) {
        std::vector<Color> out;
        out.reserve(triangles_.size() * 3);
        for (const Part& part : parts_) {
            const auto first = triangleCorners.begin() + static_cast<std::ptrdiff_t>(part.firstTriangle * 3);
            std::vector<Color> own(first, first + static_cast<std::ptrdiff_t>(part.triangleCount * 3));
            if (part.plan) own = part.plan->refineCorners(own);
            out.insert(out.end(), own.begin(), own.end());
        }
        return out;
    }
    std::vector<Color> corners(cageCornerSource_.size());
    for (std::size_t c = 0; c < corners.size(); ++c) corners[c] = triangleCorners[static_cast<std::size_t>(cageCornerSource_[c])];
    auto mix = [](const Color& a, const Color& b, float wa, float wb) {
        return Color{a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb, a[2] * wa + b[2] * wb, a[3] * wa + b[3] * wb};
    };
    for (const auto& step : vertexSteps_) {
        const Faces& parent = step->parent;
        std::vector<Color> child(parent.verts.size() * 4);
        parallelFor(0, parent.count(), [&](std::size_t face) {
            const std::int32_t b = parent.offsets[face], e = parent.offsets[face + 1];
            Color mean{0, 0, 0, 0};
            const float w = 1.0f / static_cast<float>(e - b);
            for (std::int32_t c = b; c < e; ++c) mean = mix(mean, corners[c], 1.0f, w);
            for (std::int32_t c = b; c < e; ++c) {
                const std::int32_t next = c + 1 < e ? c + 1 : b, prev = c > b ? c - 1 : e - 1;
                Color* q = &child[static_cast<std::size_t>(c) * 4];
                q[0] = corners[c];
                q[1] = mix(corners[c], corners[next], 0.5f, 0.5f);
                q[2] = mean;
                q[3] = mix(corners[prev], corners[c], 0.5f, 0.5f);
            }
        }, 1024);
        corners = std::move(child);
    }
    std::vector<Color> out(triangles_.size() * 3);
    parallelFor(0, corners.size() / 4, [&](std::size_t q) {
        constexpr int kCorners[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; ++k) out[q * 6 + k] = corners[q * 4 + kCorners[k]];
    }, 4096);
    return out;
}

ScalpMesh SubdivisionPlan::refine(const ScalpMesh& cage) const {
    if (cage.pointCount() != cagePoints_) throw std::invalid_argument("SubdivisionPlan: not this plan's cage");
    ScalpMesh out(refinePoints(cage.points()), triangles_);
    if (!cornerUVs_.empty()) out.setCornerUVs(cornerUVs_);
    for (const std::string& name : cage.colorSetNames()) {
        const auto* colors = cage.cornerColors(name);
        if (colors && colors->size() == cage.triangleCount() * 3) out.setCornerColors(name, refineCorners(*colors));
    }
    if (cage.hasRest()) out.setRestPoints(refinePoints(cage.restPoints()));
    out.setSubdivided(levels_, cageFaces_, cage.renderSubdivisionFrom());
    return out;
}

// ---- Cage points on the refined mesh (refinedRoot, cageRoot) ----------------------------------

namespace {

struct P2 {
    double x = 0.0, y = 0.0;
};

// Corner i of a regular n-gon on the unit circle: a cage face laid out flat.
P2 layoutCorner(std::int32_t i, std::int32_t n) {
    const double a = 6.283185307179586 * static_cast<double>(i % n) / static_cast<double>(n);
    return {pm::cos(a), pm::sin(a)};
}

P2 mid(const P2& a, const P2& b) { return {0.5 * (a.x + b.x), 0.5 * (a.y + b.y)}; }

// Barycentrics of p in the 2D triangle (a, b, c): the weights of b and c (a's is 1 - both).
void barycentric(const P2& p, const P2& a, const P2& b, const P2& c, double& wb, double& wc) {
    const double e1x = b.x - a.x, e1y = b.y - a.y, e2x = c.x - a.x, e2y = c.y - a.y;
    const double px = p.x - a.x, py = p.y - a.y;
    const double det = e1x * e2y - e2x * e1y;
    if (std::abs(det) < 1e-300) {
        wb = wc = 0.0;
        return;
    }
    wb = (px * e2y - e2x * py) / det;
    wc = (e1x * py - px * e1y) / det;
}

float unit(double x) { return static_cast<float>(std::clamp(x, 0.0, 1.0)); }

}  // namespace

StrandRoot SubdivisionPlan::refinedRoot(const StrandRoot& onCage) const {
    if (!parts_.empty()) {  // on its scalp's own refinement
        const Part* part = partOfCageTriangle(onCage.face);
        if (!part) return {};
        StrandRoot local = onCage;
        local.face -= static_cast<std::int32_t>(part->firstTriangle);
        StrandRoot out = part->plan ? part->plan->refinedRoot(local) : local;
        if (out.face >= 0) out.face += static_cast<std::int32_t>(part->refinedFirstTriangle);
        return out;
    }
    const std::size_t cageTriangles = cageTriangleCorners_.size() / 3;
    if (onCage.face < 0 || static_cast<std::size_t>(onCage.face) >= cageTriangles) return {};
    const std::int32_t* corners = &cageTriangleCorners_[static_cast<std::size_t>(onCage.face) * 3];
    const auto it = std::upper_bound(cageFaceOffsets_.begin(), cageFaceOffsets_.end(), corners[0]);
    const auto face = static_cast<std::size_t>(it - cageFaceOffsets_.begin()) - 1;
    const std::int32_t first = cageFaceOffsets_[face], n = cageFaceOffsets_[face + 1] - first;
    // The point in the face's layout.
    const double w[3] = {1.0 - static_cast<double>(onCage.u) - static_cast<double>(onCage.v), static_cast<double>(onCage.u),
                         static_cast<double>(onCage.v)};
    P2 p;
    for (int k = 0; k < 3; ++k) {
        const P2 c = layoutCorner(corners[k] - first, n);
        p.x += w[k] * c.x;
        p.y += w[k] * c.y;
    }
    // Its corner's quad (the corner nearest by angle), and the half of it on its side of the
    // diagonal from the corner to the center: (corner, next middle, center) or (corner, center,
    // previous middle), the quad's (s, t) being (0, 0), (1, 0), (1, 1), (0, 1) round it.
    const double turn = 6.283185307179586 / static_cast<double>(n);
    double angle = pm::atan2(p.y, p.x);
    if (angle < 0.0) angle += 6.283185307179586;
    const auto k = static_cast<std::int32_t>(std::floor(angle / turn + 0.5)) % n;
    const P2 corner = layoutCorner(k, n), next = mid(corner, layoutCorner(k + 1, n)), prev = mid(layoutCorner(k + n - 1, n), corner);
    double s = 0.0, t = 0.0;
    double wb = 0.0, wc = 0.0;
    if (corner.x * p.y - corner.y * p.x >= 0.0) {  // toward the next corner
        barycentric(p, corner, next, P2{}, wb, wc);
        s = wb + wc;
        t = wc;
    } else {
        barycentric(p, corner, P2{}, prev, wb, wc);
        s = wb;
        t = wb + wc;
    }
    s = std::clamp(s, 0.0, 1.0);
    t = std::clamp(t, 0.0, 1.0);
    // Down the levels: the quarter it is in, and its place there.
    auto quad = static_cast<std::size_t>(first + k);
    for (int l = 1; l < levels_; ++l) {
        std::size_t child = 0;
        double cs = 0.0, ct = 0.0;
        if (s < 0.5 && t < 0.5) {
            child = 0;
            cs = 2.0 * s;
            ct = 2.0 * t;
        } else if (t < 0.5) {
            child = 1;
            cs = 2.0 * t;
            ct = 2.0 * (1.0 - s);
        } else if (s >= 0.5) {
            child = 2;
            cs = 2.0 * (1.0 - s);
            ct = 2.0 * (1.0 - t);
        } else {
            child = 3;
            cs = 2.0 * (1.0 - t);
            ct = 2.0 * s;
        }
        quad = quad * 4 + child;
        s = std::clamp(cs, 0.0, 1.0);
        t = std::clamp(ct, 0.0, 1.0);
    }
    // The quad's triangles: (0, 1, 2) where s >= t, else (0, 2, 3).
    if (s >= t) return StrandRoot{static_cast<std::int32_t>(quad * 2), unit(s - t), unit(t)};
    return StrandRoot{static_cast<std::int32_t>(quad * 2 + 1), unit(s), unit(t - s)};
}

StrandRoot SubdivisionPlan::cageRoot(const StrandRoot& onRefined) const {
    if (!parts_.empty()) {
        const Part* part = partOfRefinedTriangle(onRefined.face);
        if (!part) return {};
        StrandRoot local = onRefined;
        local.face -= static_cast<std::int32_t>(part->refinedFirstTriangle);
        StrandRoot out = part->plan ? part->plan->cageRoot(local) : local;
        if (out.face >= 0) out.face += static_cast<std::int32_t>(part->firstTriangle);
        return out;
    }
    if (onRefined.face < 0 || static_cast<std::size_t>(onRefined.face) >= triangles_.size()) return {};
    auto quad = static_cast<std::size_t>(onRefined.face) / 2;
    const double u = onRefined.u, v = onRefined.v;
    double s = u + v, t = v;  // (0, 1, 2)
    if (onRefined.face % 2 == 1) {  // (0, 2, 3)
        s = u;
        t = u + v;
    }
    // Up the levels: each quarter's place in its quad.
    for (int l = 1; l < levels_; ++l) {
        const std::size_t child = quad % 4;
        quad /= 4;
        const double cs = s, ct = t;
        if (child == 0) {
            s = 0.5 * cs;
            t = 0.5 * ct;
        } else if (child == 1) {
            s = 1.0 - 0.5 * ct;
            t = 0.5 * cs;
        } else if (child == 2) {
            s = 1.0 - 0.5 * cs;
            t = 1.0 - 0.5 * ct;
        } else {
            s = 0.5 * ct;
            t = 1.0 - 0.5 * cs;
        }
    }
    // The level-1 quad is a cage face's corner: the point in the face's layout.
    const auto corner = static_cast<std::int32_t>(quad);
    const auto it = std::upper_bound(cageFaceOffsets_.begin(), cageFaceOffsets_.end(), corner);
    const auto face = static_cast<std::size_t>(it - cageFaceOffsets_.begin()) - 1;
    const std::int32_t first = cageFaceOffsets_[face], n = cageFaceOffsets_[face + 1] - first, k = corner - first;
    const P2 c = layoutCorner(k, n), next = mid(c, layoutCorner(k + 1, n)), prev = mid(layoutCorner(k + n - 1, n), c);
    P2 p;
    if (s >= t) {  // (corner, next middle, center): weights 1 - s, s - t, t
        p = {(1.0 - s) * c.x + (s - t) * next.x, (1.0 - s) * c.y + (s - t) * next.y};
    } else {  // (corner, center, previous middle): weights 1 - t, s, t - s
        p = {(1.0 - t) * c.x + (t - s) * prev.x, (1.0 - t) * c.y + (t - s) * prev.y};
    }
    // The face's triangle it is in (the one it is least outside of, for rounding at the seams).
    StrandRoot best;
    double bestInside = -1e300;
    for (std::int32_t i = faceTriangleOffsets_[face]; i < faceTriangleOffsets_[face + 1]; ++i) {
        const auto tri = static_cast<std::size_t>(faceTriangles_[static_cast<std::size_t>(i)]);
        const std::int32_t* corners = &cageTriangleCorners_[tri * 3];
        double wb = 0.0, wc = 0.0;
        barycentric(p, layoutCorner(corners[0] - first, n), layoutCorner(corners[1] - first, n), layoutCorner(corners[2] - first, n),
                    wb, wc);
        const double inside = std::min({1.0 - wb - wc, wb, wc});
        if (inside <= bestInside) continue;
        bestInside = inside;
        wb = std::max(wb, 0.0);
        wc = std::max(wc, 0.0);
        const double sum = wb + wc;
        if (sum > 1.0) {
            wb /= sum;
            wc /= sum;
        }
        best = StrandRoot{static_cast<std::int32_t>(tri), unit(wb), unit(wc)};
    }
    return best;
}

std::shared_ptr<const SubdivisionPlan> refinementPlan(const ScalpMesh& cage, const ScalpMesh& refined) {
    const int levels = refined.subdivisionLevel();
    if (levels < 1 || levels > kMaxSubdivisionLevels || cage.subdivisionLevel() != 0 || cage.triangleCount() == 0) return nullptr;
    std::shared_ptr<const SubdivisionPlan> plan = SubdivisionPlan::of(cage, levels);
    if (plan->cageFaceCount() != refined.cageFaceCount() || plan->pointCount() != refined.pointCount() ||
        plan->triangles().size() != refined.triangleCount()) {
        return nullptr;
    }
    return plan;
}

MeshPtr subdivideScalp(const MeshPtr& cage, int levels) { return subdivideScalp(cage, levels, nullptr, nullptr); }

MeshPtr subdivideScalp(const MeshPtr& cage, int levels, const MeshPtr& previousCage, const MeshPtr& previous) {
    levels = std::clamp(levels, 0, kMaxSubdivisionLevels);
    if (!cage || levels == 0 || cage->triangleCount() == 0) return cage;
    const std::shared_ptr<const SubdivisionPlan> plan = SubdivisionPlan::of(*cage, levels);
    if (previous && previousCage && previous->subdivisionLevel() == levels && previous->pointCount() == plan->pointCount() &&
        cage->samePointsLayout(*previousCage)) {
        if (cage->points() == previousCage->points() && previous->cage() == cage) return previous;
        auto moved = std::make_shared<ScalpMesh>(previous->withPoints(plan->refinePoints(cage->points())));
        moved->setCage(cage);
        return moved;
    }
    auto refined = std::make_shared<ScalpMesh>(plan->refine(*cage));
    refined->setCage(cage);
    return refined;
}

CageBinding::CageBinding(const ScalpMesh* scalp) : scalp_(scalp) {
    if (!scalp || scalp->subdivisionLevel() == 0 || !scalp->cage()) return;
    std::shared_ptr<const SubdivisionPlan> plan = refinementPlan(*scalp->cage(), *scalp);
    if (!plan) return;
    cage_ = scalp->cage();
    plan_ = std::move(plan);
}

Vec3f CageBinding::stablePosition(const StrandRoot& onScalp, const Vec3f& fallback) const {
    if (!scalp_ || !scalp_->contains(onScalp)) return fallback;
    if (!plan_) return scalp_->restPositionOr(onScalp, fallback);
    return stableCagePosition(plan_->cageRoot(onScalp), fallback);
}

Vec3f CageBinding::stableCagePosition(const StrandRoot& onCage, const Vec3f& fallback) const {
    const ScalpMesh* mesh = cage();
    if (!mesh || !mesh->contains(onCage)) return fallback;
    return mesh->restPositionOr(onCage, mesh->position(onCage));
}

}  // namespace groomlab
