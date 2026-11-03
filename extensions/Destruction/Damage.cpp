// Damage without a simulation (Damage Pieces: cracked, missing chunks, chipped edges, partial collapse) and secondary
// debris at break events (Debris From Events), both settling on lila_physics when they need to come to rest.

#include "Pieces.h"
#include "PieceTransform.h"

#include <RigidScene.h>

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Simulation.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>

using namespace lila;

namespace destruction
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using T = gn::SocketType;
namespace ph = lila::physics;

/** Seeded uniform [0, 1) from a key, the same on every machine. */
double rand01(uint64_t seed, uint64_t key)
{
    uint64_t z = seed * 0x9E3779B97F4A7C15ull + key * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return double(z >> 11) * (1.0 / 9007199254740992.0);
}

glm::dvec3 randDir(uint64_t seed, uint64_t key)
{
    const double z = rand01(seed, key * 2) * 2.0 - 1.0, a = rand01(seed, key * 2 + 1) * 6.283185307179586;
    const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(a), z, r * std::sin(a)};
}

gn::SocketDecl input(std::string id, T t, gn::json def, std::string doc, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    gn::SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

gn::SocketDecl geometrySocket(std::string id, std::string doc) { return {.id = std::move(id), .type = T::Geometry, .doc = std::move(doc)}; }

gn::json vector3(double x, double y, double z) { return gn::json::array({x, y, z}); }

/** A piece as Damage Pieces sees it: its reference mesh, where it sits, and its bounds there. */
struct PieceInfo
{
    std::shared_ptr<const geo::Mesh> mesh;
    geo::Bounds local;
    glm::dvec3 center{0.0}; // world
    double size = 0.0;      // world bounds diagonal
};

std::vector<PieceInfo> pieceInfo(const gn::Instances& inst)
{
    std::map<int, std::shared_ptr<const geo::Mesh>> meshes;
    std::vector<PieceInfo> out(inst.size());
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        auto [it, fresh] = meshes.try_emplace(r);
        if (fresh && r >= 0 && size_t(r) < inst.references.size() && inst.references[size_t(r)])
            it->second = gn::realize(*inst.references[size_t(r)]).mesh;
        PieceInfo& p = out[k];
        p.mesh = it->second;
        if (!p.mesh)
            continue;
        p.local = p.mesh->bounds();
        geo::Bounds w;
        for (geo::Index i = 0; i < p.mesh->pointCapacity(); ++i)
            if (p.mesh->pointValid(i))
                w.extend(glm::dvec3(inst.transforms[k] * glm::dvec4(p.mesh->position(i), 1.0)));
        p.center = w.valid() ? w.center() : glm::dvec3(inst.transforms[k][3]);
        p.size = w.valid() ? glm::length(w.size()) : 0.0;
    }
    return out;
}

/** How many separate outside directions a piece shows: 1 on a face, 2 on an edge, 3 or more on a corner. */
int exposedSides(const geo::Mesh& m)
{
    const geo::Attribute* inside = m.attributes(geo::Domain::Face).find("inside");
    std::vector<std::pair<glm::dvec3, double>> clusters;
    double total = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (inside && inside->type() == geo::AttrType::Int && inside->values<int32_t>()[f] != 0))
            continue;
        const double area = m.faceArea(f);
        if (area <= 0.0)
            continue;
        const glm::dvec3 n = m.faceNormal(f);
        total += area;
        auto it = std::find_if(clusters.begin(), clusters.end(), [&](const auto& c) { return glm::dot(glm::normalize(c.first), n) > 0.85; });
        if (it == clusters.end())
            clusters.push_back({n * area, area});
        else
        {
            it->first += n * area;
            it->second += area;
        }
    }
    return int(std::count_if(clusters.begin(), clusters.end(), [&](const auto& c) { return c.second > total * 0.05; }));
}

/** The rows of an attribute set, in order. */
geo::AttributeSet gatherRows(const geo::AttributeSet& in, const std::vector<size_t>& rows)
{
    geo::AttributeSet out;
    out.mergeLayout(in);
    out.resize(rows.size());
    for (size_t r = 0; r < rows.size(); ++r)
        out.copyFrom(in, r, rows[r]);
    return out;
}

/** The instances kept, in order, with their attributes; keep[k] says which. */
std::shared_ptr<gn::Instances> subset(const gn::Instances& in, const std::vector<uint8_t>& keep)
{
    auto out = std::make_shared<gn::Instances>();
    out->references = in.references;
    std::vector<size_t> rows;
    for (size_t k = 0; k < in.size(); ++k)
        if (keep[k])
        {
            out->add(in.transforms[k], in.reference[k]);
            rows.push_back(k);
        }
    out->attrs = gatherRows(in.attrs, rows);
    return out;
}

/** Links whose pieces both survive, renumbered to the kept pieces; ground anchors (pieceB -1) stay with their piece. */
GeometryPtr keptLinks(const GeometryPtr& links, const std::vector<int>& newIndex, const std::vector<uint8_t>* cut = nullptr)
{
    if (!links->curves)
        return links;
    const gn::CurveSet& c = *links->curves;
    std::span<const int32_t> pa = c.curveAttrs.get<int32_t>("pieceA"), pb = c.curveAttrs.get<int32_t>("pieceB");
    auto idx = [&](std::span<const int32_t> s, size_t k) { return k < s.size() ? s[k] : -1; };
    std::vector<size_t> rows;
    for (size_t k = 0; k < c.numCurves(); ++k)
    {
        const int a = idx(pa, k), b = idx(pb, k);
        const bool aOk = a >= 0 && size_t(a) < newIndex.size() && newIndex[size_t(a)] >= 0;
        const bool bOk = b < 0 || (size_t(b) < newIndex.size() && newIndex[size_t(b)] >= 0);
        const bool severed = cut && ((a >= 0 && size_t(a) < cut->size() && (*cut)[size_t(a)]) || (b >= 0 && size_t(b) < cut->size() && (*cut)[size_t(b)]));
        if (aOk && bOk && !severed)
            rows.push_back(k);
    }
    gn::CurveSet out;
    for (size_t k : rows)
    {
        out.addCurve(c.curvePoints(k), false);
    }
    out.syncSizes();
    out.curveAttrs = gatherRows(c.curveAttrs, rows);
    std::span<int32_t> oa = out.curveAttrs.getOrAdd<int32_t>("pieceA"), ob = out.curveAttrs.getOrAdd<int32_t>("pieceB");
    for (size_t r = 0; r < rows.size(); ++r)
    {
        oa[r] = newIndex[size_t(oa[r])];
        if (ob[r] >= 0)
            ob[r] = newIndex[size_t(ob[r])];
    }
    Geometry g = *links;
    g.curves = std::make_shared<const gn::CurveSet>(std::move(out));
    return gn::makeGeometry(std::move(g));
}

/** Steps a world in fixed steps until every piece rests or `seconds` pass. */
void settle(RigidWorld& world, const SceneDesc& scene, double seconds)
{
    constexpr double dt = 1.0 / 120.0;
    const int64_t steps = int64_t(seconds / dt);
    for (int64_t k = 1; k <= steps; ++k)
    {
        world.advance(scene, k, dt);
        if (k >= 60 && k % 12 == 0 && world.settled(2.0, 0.1))
            break;
    }
}

/** The world's piece transforms, keeping each piece's own scale. */
std::vector<glm::dmat4> posesOf(const RigidWorld& world, const gn::Instances& inst)
{
    std::vector<uint8_t> bytes;
    world.save(bytes, false);
    const StateView v(bytes);
    std::vector<glm::dmat4> out(inst.transforms.begin(), inst.transforms.end());
    if (v.valid() && v.header().bodies == inst.size())
        for (size_t k = 0; k < inst.size(); ++k)
            out[k] = compose(v.position(k), v.rotation(k), decompose(inst.transforms[k]).scale);
    return out;
}

/** The damaged region: the share `amount` of pieces nearest the impact, with seeded noise so its edge is ragged. */
std::vector<uint8_t> nearImpact(const std::vector<PieceInfo>& info, const glm::dvec3& impact, double amount, uint64_t seed)
{
    const size_t n = info.size();
    const size_t take = size_t(std::llround(std::clamp(amount, 0.0, 1.0) * double(n)));
    double reach = 1e-6;
    for (const PieceInfo& p : info)
        reach = std::max(reach, glm::length(p.center - impact));
    std::vector<double> score(n);
    for (size_t k = 0; k < n; ++k)
        score[k] = glm::length(info[k].center - impact) / reach + rand01(seed, k) * 0.35;
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return score[a] < score[b]; });
    std::vector<uint8_t> out(n, 0);
    for (size_t k = 0; k < take; ++k)
        out[order[k]] = 1;
    return out;
}

void damagePieces(ExecContext& c)
{
    GeometryPtr pieces = c.geometry("pieces");
    GeometryPtr links = c.geometry("links");
    c.out("pieces", pieces);
    c.out("links", links);
    c.out("rubble", gn::makeGeometry({}));
    if (!pieces->instances || pieces->instances->size() == 0)
        return;
    const gn::Instances& inst = *pieces->instances;
    const std::string mode = c.prop("mode");
    const double amount = std::clamp(c.f("amount"), 0.0, 1.0);
    const uint64_t seed = uint64_t(c.i("seed"));
    const std::vector<PieceInfo> info = pieceInfo(inst);
    const size_t n = inst.size();

    if (mode == "cracked")
    {
        const double gap = std::max(0.0, c.f("gap")), shift = std::max(0.0, c.f("shift"));
        const double tilt = glm::radians(std::max(0.0, c.f("tilt")));
        auto moved = std::make_shared<gn::Instances>(inst);
        for (size_t k = 0; k < n; ++k)
        {
            const PieceInfo& p = info[k];
            if (!p.mesh || !p.local.valid())
                continue;
            const glm::dvec3 ext = p.local.size();
            const double thinnest = std::max(1e-3, std::min({ext.x, ext.y, ext.z}));
            const double s = std::clamp(1.0 - gap / thinnest, 0.5, 1.0);
            const glm::dvec3 mid = p.local.center();
            glm::dmat4 m = inst.transforms[k] * glm::translate(glm::dmat4(1.0), mid) * glm::scale(glm::dmat4(1.0), glm::dvec3(s)) *
                           glm::translate(glm::dmat4(1.0), -mid);
            // Only `amount` of the pieces slip out of place; the rest just show their cracks.
            if (rand01(seed, k * 3 + 7) < amount)
            {
                const glm::dvec3 offset = randDir(seed, k * 5 + 1) * (shift * rand01(seed, k * 5 + 2));
                const glm::dmat4 turn = glm::rotate(glm::dmat4(1.0), tilt * (rand01(seed, k * 5 + 3) * 2.0 - 1.0), randDir(seed, k * 5 + 4));
                m = glm::translate(glm::dmat4(1.0), offset + p.center) * turn * glm::translate(glm::dmat4(1.0), -p.center) * m;
            }
            moved->transforms[k] = m;
        }
        Geometry g = *pieces;
        g.instances = moved;
        c.out("pieces", gn::makeGeometry(std::move(g)));
        return;
    }

    std::vector<uint8_t> chosen;
    if (mode == "chipped")
    {
        chosen.assign(n, 0);
        std::vector<std::pair<double, size_t>> candidates;
        double largest = 1e-9;
        for (const PieceInfo& p : info)
            largest = std::max(largest, p.size);
        for (size_t k = 0; k < n; ++k)
        {
            if (!info[k].mesh)
                continue;
            const int sides = exposedSides(*info[k].mesh);
            if (sides < 2)
                continue;
            // Corners first, then the smallest edge pieces, with noise so the chips don't follow the cell order.
            candidates.push_back({(sides >= 3 ? 0.0 : 1.0) + info[k].size / largest + rand01(seed, k) * 0.5, k});
        }
        std::stable_sort(candidates.begin(), candidates.end());
        const size_t take = size_t(std::llround(amount * double(candidates.size())));
        for (size_t k = 0; k < take; ++k)
            chosen[candidates[k].second] = 1;
    }
    else
        chosen = nearImpact(info, c.v("impact"), amount, seed);

    if (mode == "collapsed")
    {
        Settings s;
        s.substeps = 1;
        s.gravity = c.v("gravity");
        s.ground = c.b("ground");
        s.groundHeight = c.f("groundHeight");
        s.strengthScale = std::max(0.0, c.f("strength"));
        const GeometryPtr held = keptLinks(links, [&] {
            std::vector<int> same(n);
            std::iota(same.begin(), same.end(), 0);
            return same;
        }(), &chosen);
        const SceneDesc scene = describeGeometry(pieces, held, c.geometry("colliders"), gn::makeGeometry({}), s,
                                                 Material{std::max(1e-3, c.f("density")), 0.6, 0.05}, true);
        RigidWorld world(scene);
        settle(world, scene, std::max(0.5, c.f("seconds")));
        auto moved = std::make_shared<gn::Instances>(inst);
        moved->transforms = posesOf(world, inst);
        Geometry g = *pieces;
        g.instances = moved;
        c.out("pieces", gn::makeGeometry(std::move(g)));
        c.out("links", held);
        return;
    }

    // missing and chipped: the chosen pieces leave as rubble, still where they were.
    std::vector<uint8_t> keep(n);
    std::vector<int> newIndex(n, -1);
    int next = 0;
    for (size_t k = 0; k < n; ++k)
    {
        keep[k] = chosen[k] ? 0 : 1;
        if (keep[k])
            newIndex[k] = next++;
    }
    std::vector<uint8_t> gone(n);
    for (size_t k = 0; k < n; ++k)
        gone[k] = keep[k] ? 0 : 1;
    Geometry kept = *pieces;
    kept.instances = subset(inst, keep);
    Geometry rubble;
    rubble.instances = subset(inst, gone);
    c.out("pieces", gn::makeGeometry(std::move(kept)));
    c.out("links", keptLinks(links, newIndex));
    c.out("rubble", gn::makeGeometry(std::move(rubble)));
}

gn::NodeDecl damageDecl()
{
    return {
        .type = "DamagePieces",
        .label = "Damage Pieces",
        .category = "Destruction",
        .doc = "Damages fractured pieces without simulating a break: cracked leaves every piece standing with a visible crack "
               "between neighbours and some slipped out of place; missing takes out the pieces around the impact; chipped takes "
               "small pieces off edges and corners; collapsed frees the pieces around the impact and lets everything settle under "
               "gravity, so what lost its support falls. The same inputs always give the same result.",
        .props = {{.id = "mode", .options = {"cracked", "missing", "chipped", "collapsed"}, .defaultValue = "cracked", .doc = "What kind of damage."}},
        .sockets = {.inputs = {geometrySocket("pieces", "Pieces, as instances (from Fracture)."), geometrySocket("links", "Links between them (from Fracture or Anchor Pieces)."),
                               geometrySocket("colliders", "Collapsed: things the pieces land on besides the ground."),
                               input("amount", T::Float, 0.3, "How much is damaged, 0 to 1.", 0.0, 1.0),
                               input("impact", T::Vector, vector3(0, 0, 0), "Where the damage centres, cm (missing, collapsed)."),
                               input("seed", T::Int, 1, "Another seed gives another result."),
                               input("gap", T::Float, 0.3, "Cracked: width of the cracks, cm.", 0.0),
                               input("shift", T::Float, 1.0, "Cracked: how far loose pieces slip, cm.", 0.0),
                               input("tilt", T::Float, 2.0, "Cracked: how far loose pieces tip, degrees.", 0.0),
                               input("gravity", T::Vector, vector3(0, -981, 0), "Collapsed: cm/s2."),
                               input("ground", T::Bool, true, "Collapsed: a floor at Ground Height."),
                               input("groundHeight", T::Float, 0.0, "Collapsed: cm."),
                               input("strength", T::Float, 1.0, "Collapsed: multiplies the links' strength; lower brings more down.", 0.0),
                               input("density", T::Float, 2400.0, "Collapsed: kg/m3.", 1.0),
                               input("seconds", T::Float, 6.0, "Collapsed: longest it settles for, s.", 0.5, 60.0)},
                    .outputs = {geometrySocket("pieces", "The damaged pieces."), geometrySocket("links", "The links still holding."),
                                geometrySocket("rubble", "Missing and chipped: the pieces taken out, where they were.")}},
        .exec = damagePieces,
    };
}

/** A few seeded rock shapes about a unit across, shared by every chunk. */
std::vector<GeometryPtr> chunkShapes(uint64_t seed, int variants, const std::string& material)
{
    std::vector<GeometryPtr> out;
    for (int v = 0; v < variants; ++v)
    {
        std::vector<glm::dvec3> pts;
        const glm::dvec3 squash(1.0, 0.55 + 0.35 * rand01(seed, uint64_t(v) * 101 + 1), 0.7 + 0.3 * rand01(seed, uint64_t(v) * 101 + 2));
        for (int k = 0; k < 14; ++k)
            pts.push_back(randDir(seed + uint64_t(v) * 7919, uint64_t(k)) * squash * (0.35 + 0.15 * rand01(seed, uint64_t(v) * 101 + 10 + k)));
        geo::Mesh m = geo::convexHullMesh(pts, 32);
        if (!material.empty())
        {
            m.materialNames() = {material};
            std::span<int32_t> slot = m.faceAttrs().getOrAdd<int32_t>("material");
            std::fill(slot.begin(), slot.end(), 0);
        }
        out.push_back(gn::meshGeometry(std::move(m)));
    }
    return out;
}

/** The inside material of the first piece that has one, so chunks look like the broken material. */
std::string interiorOf(const Geometry& g)
{
    if (!g.instances)
        return {};
    for (const GeometryPtr& r : g.instances->references)
        if (r && r->mesh)
            for (const std::string& name : r->mesh->materialNames())
                if (name.ends_with("_Interior"))
                    return name;
    return {};
}

std::pair<GeometryPtr, GeometryPtr> makeDebris(ExecContext& c)
{
    GeometryPtr events = c.geometry("events");
    GeometryPtr colliders = c.geometry("colliders");
    const uint64_t seed = uint64_t(c.i("seed"));
    const double amount = std::max(0.0, c.f("amount"));
    const double size = std::max(0.1, c.f("size"));
    const double variation = std::clamp(c.f("sizeVariation"), 0.0, 1.0);
    const double spread = std::max(0.0, c.f("spread"));
    const int dustPer = int(std::clamp<int64_t>(c.i("dust"), 0, 10000));

    struct Event
    {
        glm::dvec3 at;
        double impulse;
        int frame;
    };
    std::vector<Event> found;
    for (const std::shared_ptr<const geo::Mesh>& m : {events->points, events->mesh})
    {
        if (!m)
            continue;
        const geo::Attribute* imp = m->pointAttrs().find("impulse");
        const geo::Attribute* fr = m->pointAttrs().find("frame");
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                found.push_back({m->position(p), imp && imp->type() == geo::AttrType::Float ? double(imp->values<float>()[p]) : 1.0,
                                 fr && fr->type() == geo::AttrType::Int ? fr->values<int32_t>()[p] : 0});
    }
    if (found.empty())
        return {gn::makeGeometry({}), gn::makeGeometry({})};
    double meanImpulse = 0.0;
    for (const Event& e : found)
        meanImpulse += std::abs(e.impulse);
    meanImpulse = std::max(meanImpulse / double(found.size()), 1e-9);

    std::string material = c.s("material");
    if (material.empty())
        material = interiorOf(*colliders);
    const int variants = 6;
    auto inst = std::make_shared<gn::Instances>();
    inst->references = chunkShapes(seed, variants, material);
    std::vector<glm::dvec3> velocity, spin;
    geo::Mesh dust;
    std::vector<float> dustSize;
    std::vector<glm::vec3> dustVelocity;
    std::vector<int32_t> dustFrame;
    uint64_t key = 0;
    for (size_t e = 0; e < found.size(); ++e)
    {
        const Event& ev = found[e];
        // More chunks from harder breaks.
        const double weight = std::clamp(std::sqrt(std::abs(ev.impulse) / meanImpulse), 0.25, 3.0);
        const int count = int(std::floor(amount * weight + rand01(seed, e * 977 + 3)));
        for (int k = 0; k < count; ++k, ++key)
        {
            const double scale = size * (1.0 + variation * (rand01(seed, key * 11 + 1) * 2.0 - 1.0));
            const glm::dvec3 at = ev.at + randDir(seed, key * 11 + 2) * (size * 0.5 * rand01(seed, key * 11 + 3));
            const glm::dmat4 turn = glm::rotate(glm::dmat4(1.0), rand01(seed, key * 11 + 4) * 6.283185307179586, randDir(seed, key * 11 + 5));
            inst->add(glm::translate(glm::dmat4(1.0), at) * turn * glm::scale(glm::dmat4(1.0), glm::dvec3(scale)),
                      int(rand01(seed, key * 11 + 6) * variants) % variants);
            glm::dvec3 dir = randDir(seed, key * 11 + 7);
            dir.y = std::abs(dir.y) * 0.5 + 0.5;
            velocity.push_back(glm::normalize(dir) * spread * (0.3 + 0.7 * rand01(seed, key * 11 + 8)) * std::min(weight, 2.0));
            spin.push_back(randDir(seed, key * 11 + 9) * 6.0 * rand01(seed, key * 11 + 10));
        }
        for (int k = 0; k < dustPer; ++k)
        {
            const uint64_t d = (uint64_t(e) << 20) + uint64_t(k);
            dust.addPoint(ev.at + randDir(seed + 17, d) * (size * rand01(seed + 17, d + 1)));
            dustSize.push_back(float(size * (0.5 + rand01(seed + 19, d))));
            glm::dvec3 up = randDir(seed + 23, d);
            up.y = std::abs(up.y);
            dustVelocity.push_back(glm::vec3(up * spread * 0.4 * rand01(seed + 29, d)));
            dustFrame.push_back(ev.frame);
        }
    }
    if (dust.numPoints() > 0)
    {
        std::span<float> ds = dust.pointAttrs().getOrAdd<float>("size");
        std::span<glm::vec3> dv = dust.pointAttrs().getOrAdd<glm::vec3>("velocity");
        std::span<int32_t> df = dust.pointAttrs().getOrAdd<int32_t>("frame");
        std::copy(dustSize.begin(), dustSize.end(), ds.begin());
        std::copy(dustVelocity.begin(), dustVelocity.end(), dv.begin());
        std::copy(dustFrame.begin(), dustFrame.end(), df.begin());
    }
    GeometryPtr dustOut = dust.numPoints() > 0 ? gn::pointGeometry(std::move(dust)) : gn::makeGeometry({});

    inst->attrs.resize(inst->size());
    if (!c.b("settle") || inst->size() == 0)
    {
        std::span<glm::vec3> v = inst->attrs.getOrAdd<glm::vec3>("velocity");
        std::span<glm::vec3> w = inst->attrs.getOrAdd<glm::vec3>("angularVelocity");
        for (size_t k = 0; k < inst->size(); ++k)
        {
            v[k] = glm::vec3(velocity[k]);
            w[k] = glm::vec3(spin[k]);
        }
        Geometry g;
        g.instances = inst;
        return {gn::makeGeometry(std::move(g)), dustOut};
    }

    // Settle: chunks fly off and come to rest on the ground and on whatever pieces reached Colliders, which hold still.
    Geometry flying;
    {
        auto withVelocity = std::make_shared<gn::Instances>(*inst);
        std::span<glm::vec3> v = withVelocity->attrs.getOrAdd<glm::vec3>("velocity");
        std::span<glm::vec3> w = withVelocity->attrs.getOrAdd<glm::vec3>("angularVelocity");
        for (size_t k = 0; k < withVelocity->size(); ++k)
        {
            v[k] = glm::vec3(velocity[k]);
            w[k] = glm::vec3(spin[k]);
        }
        flying.instances = withVelocity;
    }
    Geometry still;
    if (colliders->instances)
    {
        // Pieces become fixed meshes: each realized where it is, joined into one collider.
        Geometry onlyPieces;
        onlyPieces.instances = colliders->instances;
        still.mesh = gn::realize(onlyPieces).mesh;
    }
    else
        still.mesh = colliders->mesh;
    Settings s;
    s.substeps = 1;
    s.gravity = c.v("gravity");
    s.ground = c.b("ground");
    s.groundHeight = c.f("groundHeight");
    const GeometryPtr flyingPtr = gn::makeGeometry(std::move(flying));
    const SceneDesc scene = describeGeometry(flyingPtr, gn::makeGeometry({}), gn::makeGeometry(std::move(still)), gn::makeGeometry({}), s,
                                             Material{std::max(1e-3, c.f("density")), 0.7, 0.05}, true);
    RigidWorld world(scene);
    settle(world, scene, std::max(0.5, c.f("seconds")));
    inst->transforms = posesOf(world, *flyingPtr->instances);
    Geometry g;
    g.instances = inst;
    return {gn::makeGeometry(std::move(g)), dustOut};
}

/**
 * Debris settles over seconds of simulation, and its inputs stop changing once everything has broken and come to rest,
 * so the last few results are kept by the inputs that made them.
 */
void debrisFromEvents(ExecContext& c)
{
    uint64_t key = gn::hashGeometry(*c.geometry("events"));
    key = gn::hashGeometry(*c.geometry("colliders"), key);
    for (const char* f : {"amount", "size", "sizeVariation", "spread", "groundHeight", "density", "seconds"})
    {
        const double v = c.f(f);
        key = scene::sim::hashBytes(&v, sizeof v, key);
    }
    for (const char* i : {"dust", "seed"})
    {
        const int64_t v = c.i(i);
        key = scene::sim::hashBytes(&v, sizeof v, key);
    }
    const uint8_t flags[2] = {uint8_t(c.b("settle")), uint8_t(c.b("ground"))};
    key = scene::sim::hashBytes(flags, sizeof flags, key);
    const std::string material = c.s("material");
    key = scene::sim::hashBytes(material.data(), material.size(), key);
    const glm::dvec3 gravity = c.v("gravity");
    key = scene::sim::hashBytes(&gravity, sizeof gravity, key);

    static std::mutex mutex;
    static std::vector<std::pair<uint64_t, std::pair<GeometryPtr, GeometryPtr>>> recent;
    {
        std::lock_guard lock(mutex);
        for (const auto& [k, result] : recent)
            if (k == key)
            {
                c.out("debris", result.first);
                c.out("dust", result.second);
                return;
            }
    }
    const std::pair<GeometryPtr, GeometryPtr> result = makeDebris(c);
    {
        std::lock_guard lock(mutex);
        recent.push_back({key, result});
        if (recent.size() > 8)
            recent.erase(recent.begin());
    }
    c.out("debris", result.first);
    c.out("dust", result.second);
}

gn::NodeDecl debrisDecl()
{
    return {
        .type = "DebrisFromEvents",
        .label = "Debris From Events",
        .category = "Destruction",
        .doc = "Small chunks and dust where things broke: each event point (a Rigid Body Solver's breaks) throws off chunks, "
               "more for harder breaks, that settle on the ground and on the pieces, ready to keep as level dressing; dust comes "
               "out as points (size, velocity, frame) for Niagara. Chunks share a few rock shapes, so they stay light. Without "
               "Settle they come out flying, with velocity, for another Rigid Body Solver.",
        .sockets = {.inputs = {geometrySocket("events", "Where things broke: points, with impulse and frame attributes when they have them."),
                               geometrySocket("colliders", "What chunks land on and take their inside material from: the pieces, or a mesh."),
                               input("amount", T::Float, 4.0, "Chunks per break, on average.", 0.0, 1000.0),
                               input("size", T::Float, 8.0, "Typical chunk size, cm.", 0.1),
                               input("sizeVariation", T::Float, 0.5, "0 makes every chunk the same size; 1 from nothing to twice it.", 0.0, 1.0),
                               input("spread", T::Float, 300.0, "How fast chunks fly off, cm/s.", 0.0),
                               input("dust", T::Int, 20, "Dust points per break.", 0, 10000), input("seed", T::Int, 1, "Another seed gives another scatter."),
                               input("settle", T::Bool, true, "Let the chunks fall and come to rest."),
                               input("gravity", T::Vector, vector3(0, -981, 0), "cm/s2."), input("ground", T::Bool, true, "A floor at Ground Height."),
                               input("groundHeight", T::Float, 0.0, "cm."), input("density", T::Float, 2400.0, "kg/m3.", 1.0),
                               input("seconds", T::Float, 6.0, "Longest the chunks settle for, s.", 0.5, 60.0),
                               input("material", T::String, "", "Material of the chunks; empty takes the pieces' inside material.")},
                    .outputs = {geometrySocket("debris", "The chunks, as instances of a few shared shapes."), geometrySocket("dust", "Dust points for Niagara.")}},
        .exec = debrisFromEvents,
    };
}
} // namespace

bool registerDamageNodes(ext::Context& ctx) { return gn::registerDecl(ctx, damageDecl()) && gn::registerDecl(ctx, debrisDecl()); }
} // namespace destruction
