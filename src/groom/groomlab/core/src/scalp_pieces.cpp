// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_pieces.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <set>

#include "groomlab/scalp_maps.h"
#include "groomlab/subdivision.h"
#include "groomlab/process_caches.h"

namespace groomlab {

bool validScalpName(const std::string& name) { return name != kMainScalpName && validMapName(name); }

std::string scalpPieceExternal(const std::string& name) { return "scalp:" + name; }

MeshPtr combineScalps(const MeshPtr& main, const NamedScalps& extras) {
    if (!main) return main;
    std::vector<std::pair<std::string, const ScalpMesh*>> parts{{kMainScalpName, main.get()}};
    std::set<std::string> taken{kMainScalpName};
    for (const auto& [name, mesh] : extras) {
        if (!mesh || !validScalpName(name) || !taken.insert(name).second) continue;
        parts.emplace_back(name, mesh.get());
    }
    if (parts.size() == 1) return main;

    bool uvs = false, rest = false, polygons = false;
    std::set<std::string> colorSets;
    std::size_t points = 0, triangles = 0;
    for (const auto& [name, m] : parts) {
        uvs = uvs || m->hasUVs();
        rest = rest || m->hasRest();
        polygons = polygons || !m->polygons().empty();
        for (const std::string& set : m->colorSetNames()) colorSets.insert(set);
        points += m->pointCount();
        triangles += m->triangleCount();
    }
    std::vector<Vec3f> allPoints, restPoints;
    std::vector<ScalpMesh::Triangle> allTriangles;
    std::vector<ScalpMesh::UV> allUVs;
    std::vector<std::int32_t> allPolygons;
    std::map<std::string, std::vector<ScalpMesh::Color>> colors;
    allPoints.reserve(points);
    allTriangles.reserve(triangles);
    if (rest) restPoints.reserve(points);
    if (uvs) allUVs.reserve(triangles * 3);
    std::vector<ScalpPiece> pieces;
    int renderLevels = 0;
    for (const auto& [name, m] : parts) {
        ScalpPiece piece;
        piece.name = name;
        piece.firstTriangle = allTriangles.size();
        piece.triangleCount = m->triangleCount();
        piece.firstPoint = allPoints.size();
        piece.pointCount = m->pointCount();
        piece.renderSubdivision = m->renderSubdivision();
        piece.renderSubdivisionFrom = m->renderSubdivisionFrom();
        renderLevels = std::max(renderLevels, m->renderSubdivision());
        const auto offset = static_cast<std::int32_t>(piece.firstPoint);
        allPoints.insert(allPoints.end(), m->points().begin(), m->points().end());
        if (rest) {
            const std::vector<Vec3f>& r = m->hasRest() ? m->restPoints() : m->points();
            restPoints.insert(restPoints.end(), r.begin(), r.end());
        }
        for (const ScalpMesh::Triangle& t : m->triangles()) allTriangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
        if (uvs) {
            if (m->hasUVs()) {
                allUVs.insert(allUVs.end(), m->cornerUVs().begin(), m->cornerUVs().end());
            } else {
                allUVs.resize(allUVs.size() + m->triangleCount() * 3, ScalpMesh::UV{0.0f, 0.0f});
            }
        }
        for (const std::string& set : colorSets) {
            std::vector<ScalpMesh::Color>& out = colors[set];
            const std::vector<ScalpMesh::Color>* own = m->cornerColors(set);
            if (own && own->size() == m->triangleCount() * 3) {
                out.insert(out.end(), own->begin(), own->end());
            } else {
                out.resize(out.size() + m->triangleCount() * 3, ScalpMesh::Color{0.0f, 0.0f, 0.0f, 1.0f});
            }
        }
        if (polygons) {
            if (!m->polygons().empty()) {
                allPolygons.insert(allPolygons.end(), m->polygons().begin(), m->polygons().end());
            } else {
                allPolygons.resize(allPolygons.size() + m->triangleCount(), 1);
            }
        }
        pieces.push_back(std::move(piece));
    }
    auto out = std::make_shared<ScalpMesh>(std::move(allPoints), std::move(allTriangles));
    if (uvs) out->setCornerUVs(std::move(allUVs));
    for (auto& [set, values] : colors) out->setCornerColors(set, std::move(values));
    if (rest) out->setRestPoints(std::move(restPoints));
    if (polygons) out->setPolygons(std::move(allPolygons));
    if (renderLevels > 0) out->setRenderSubdivision(renderLevels, "the most of the groom's scalps");
    out->setPieces(std::move(pieces));
    return out;
}

MeshPtr withScalpLevels(const MeshPtr& joined, const std::map<std::string, int>& levels, int most) {
    if (!joined || joined->pieces().empty()) return joined;
    std::vector<ScalpPiece> pieces = joined->pieces();
    bool changed = false;
    for (ScalpPiece& p : pieces) {
        const auto it = levels.find(p.name);
        const int own = it == levels.end() || it->second == most ? -1 : std::clamp(it->second, 0, kMaxSubdivisionLevels);
        changed = changed || own != p.subdivision;
        p.subdivision = own;
    }
    if (!changed) return joined;
    auto out = std::make_shared<ScalpMesh>(*joined);
    out->setPieces(std::move(pieces));
    return out;
}

ScalpMesh sliceScalp(const ScalpMesh& mesh, std::size_t first, std::size_t count) {
    first = std::min(first, mesh.triangleCount());
    count = std::min(count, mesh.triangleCount() - first);
    // The points the triangles use, in their order on the mesh.
    std::vector<std::int32_t> remap(mesh.pointCount(), -1);
    const std::vector<ScalpMesh::Triangle>& tris = mesh.triangles();
    for (std::size_t t = first; t < first + count; ++t) {
        for (const std::int32_t v : tris[t]) remap[static_cast<std::size_t>(v)] = 0;
    }
    std::vector<Vec3f> points, rest;
    for (std::size_t v = 0; v < remap.size(); ++v) {
        if (remap[v] < 0) continue;
        remap[v] = static_cast<std::int32_t>(points.size());
        points.push_back(mesh.points()[v]);
        if (mesh.hasRest()) rest.push_back(mesh.restPoints()[v]);
    }
    std::vector<ScalpMesh::Triangle> triangles;
    triangles.reserve(count);
    for (std::size_t t = first; t < first + count; ++t) {
        triangles.push_back({remap[static_cast<std::size_t>(tris[t][0])], remap[static_cast<std::size_t>(tris[t][1])],
                             remap[static_cast<std::size_t>(tris[t][2])]});
    }
    ScalpMesh out(std::move(points), std::move(triangles));
    if (mesh.hasUVs()) {
        out.setCornerUVs({mesh.cornerUVs().begin() + static_cast<std::ptrdiff_t>(first * 3),
                          mesh.cornerUVs().begin() + static_cast<std::ptrdiff_t>((first + count) * 3)});
    }
    for (const std::string& set : mesh.colorSetNames()) {
        const std::vector<ScalpMesh::Color>& c = *mesh.cornerColors(set);
        if (c.size() < (first + count) * 3) continue;
        out.setCornerColors(set, {c.begin() + static_cast<std::ptrdiff_t>(first * 3), c.begin() + static_cast<std::ptrdiff_t>((first + count) * 3)});
    }
    if (!rest.empty()) out.setRestPoints(std::move(rest));
    if (!mesh.polygons().empty()) {
        std::vector<std::int32_t> polygons;
        std::size_t at = 0, covered = 0;
        bool aligned = true;
        for (const std::int32_t n : mesh.polygons()) {
            const std::size_t next = at + static_cast<std::size_t>(n);
            if (at >= first && next <= first + count) {
                polygons.push_back(n);
                covered += static_cast<std::size_t>(n);
            } else if (next > first && at < first + count) {
                aligned = false;  // a polygon the range cuts
            }
            at = next;
        }
        if (aligned && covered == count) out.setPolygons(std::move(polygons));
    }
    if (mesh.renderSubdivision() > 0 || !mesh.renderSubdivisionFrom().empty()) {
        out.setRenderSubdivision(mesh.renderSubdivision(), mesh.renderSubdivisionFrom());
    }
    return out;
}

namespace {

// The table of pieces `whole` is cut by: its cage's (a refined mesh), else its own; null for none.
const ScalpMesh* pieceTable(const ScalpMesh& whole) {
    const int levels = whole.subdivisionLevel();
    const ScalpMesh* cage = levels > 0 ? whole.cage().get() : &whole;
    return cage && !cage->pieces().empty() ? cage : nullptr;
}

std::map<std::string, MeshPtr> cutPieces(const MeshPtr& whole) {
    std::map<std::string, MeshPtr> out;
    const int levels = whole->subdivisionLevel();
    const ScalpMesh* cage = pieceTable(*whole);
    const std::vector<ScalpPiece>& pieces = cage->pieces();
    std::size_t refinedAt = 0;
    for (const ScalpPiece& p : pieces) {
        auto cagePiece = std::make_shared<ScalpMesh>(sliceScalp(*cage, p.firstTriangle, p.triangleCount));
        cagePiece->setRenderSubdivision(p.renderSubdivision, p.renderSubdivisionFrom);
        std::shared_ptr<ScalpMesh> piece = cagePiece;
        std::size_t offset = p.firstTriangle;
        // The scalp's own level (ScalpPiece::subdivision), else the whole's.
        const int own = p.subdivision >= 0 ? std::min(p.subdivision, kMaxSubdivisionLevels) : levels;
        if (levels > 0 && own > 0 && p.triangleCount > 0) {
            // Its triangles of the refined mesh: each cage face's children follow the face before's
            // (each scalp's refinement after the one before's), so a scalp's are its own
            // refinement's, in its order.
            const auto plan = SubdivisionPlan::of(*cagePiece, own);
            const std::size_t n = plan->triangles().size();
            if (refinedAt + n > whole->triangleCount()) return {};
            piece = std::make_shared<ScalpMesh>(sliceScalp(*whole, refinedAt, n));
            piece->setRenderSubdivision(0);
            piece->setSubdivided(own, plan->cageFaceCount(), whole->renderSubdivisionFrom());
            piece->setCage(cagePiece);
            offset = refinedAt;
            refinedAt += n;
        } else if (levels > 0) {
            // A scalp left as it is on a refined whole: its triangles there are its own.
            if (refinedAt + p.triangleCount > whole->triangleCount()) return {};
            piece = std::make_shared<ScalpMesh>(sliceScalp(*whole, refinedAt, p.triangleCount));
            piece->setRenderSubdivision(p.renderSubdivision, p.renderSubdivisionFrom);
            offset = refinedAt;
            refinedAt += p.triangleCount;
        }
        piece->setWhole(whole, static_cast<std::int32_t>(offset));
        out[p.name] = std::move(piece);
    }
    if (levels > 0 && refinedAt != whole->triangleCount()) return {};
    return out;
}

}  // namespace

std::map<std::string, MeshPtr> scalpPieces(const MeshPtr& whole) {
    if (!whole || !pieceTable(*whole)) return {};
    // Cut once per mesh: every evaluation registers the pieces (setGroomExternals), and a frame
    // of a deforming scalp is a new mesh. The last few wholes' (pieces hold their whole weakly).
    struct Kept {
        std::weak_ptr<const ScalpMesh> whole;
        std::map<std::string, MeshPtr> pieces;
    };
    constexpr std::size_t kKept = 3;  // the scalp, its rest pose (following), a rest pose's (pinned instances)
    static std::mutex mutex;
    static std::list<Kept> kept;
    static const bool registered = (registerProcessCache("scalp pieces", [] {
                                        std::list<Kept> drop;
                                        const std::lock_guard<std::mutex> lock(mutex);
                                        drop.swap(kept);
                                    }),
                                    true);
    (void)registered;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto it = kept.begin(); it != kept.end(); ++it) {
            if (it->whole.lock() != whole) continue;
            kept.splice(kept.begin(), kept, it);
            return kept.front().pieces;
        }
    }
    std::map<std::string, MeshPtr> pieces = cutPieces(whole);
    const std::lock_guard<std::mutex> lock(mutex);
    kept.push_front({whole, pieces});
    while (kept.size() > kKept) kept.pop_back();
    return pieces;
}

const ScalpMesh* wholeScalp(const ScalpMesh* mesh) {
    if (!mesh || !mesh->isPiece()) return mesh;
    const MeshPtr whole = mesh->whole();  // held by the evaluation's "scalp" external
    return whole ? whole.get() : mesh;
}

MeshPtr wholeScalp(const MeshPtr& mesh) {
    if (!mesh || !mesh->isPiece()) return mesh;
    MeshPtr whole = mesh->whole();
    return whole ? whole : mesh;
}

StrandRoot toPieceRoot(const ScalpMesh& mesh, StrandRoot root) {
    if (!mesh.isPiece() || root.face < 0) return root;
    root.face -= mesh.wholeFaceOffset();
    if (root.face < 0 || static_cast<std::size_t>(root.face) >= mesh.triangleCount()) root.face = -1;
    return root;
}

}  // namespace groomlab
