// SPDX-License-Identifier: Apache-2.0
#include "groomlab/flow.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "groomlab/frames.h"
#include "groomlab/parallel.h"
#include "groomlab/random.h"
#include "groomlab/subdivision.h"

namespace groomlab {

using json = nlohmann::json;


void surfaceFrame(const ScalpMesh& scalp, const StrandRoot& root, Vec3f& x, Vec3f& y, Vec3f& z) {
    z = scalp.normal(root).normalized();
    const ScalpMesh::Triangle& t = scalp.triangles()[static_cast<std::size_t>(root.face)];
    const Vec3f edge = scalp.points()[static_cast<std::size_t>(t[1])] - scalp.points()[static_cast<std::size_t>(t[0])];
    x = edge - z * edge.dot(z);
    if (x.length() < 1e-8f) x = anyPerpendicular(z);
    x = x.normalized();
    y = z.cross(x);
}

float flowSinkAngle(const std::string& kind) {
    if (kind == "whorl") return 70.0f;
    if (kind == "whorlcw") return -70.0f;
    if (kind == "converge") return 180.0f;
    if (kind == "part") return 0.0f;
    throw std::invalid_argument("flow sink: unknown kind '" + kind + "' (whorl, whorlcw, converge, part)");
}

FlowPin FlowSet::makePin(const ScalpMesh& scalp, const SurfacePoint& at, const Vec3f& direction) {
    if (at.root.face < 0 || static_cast<std::size_t>(at.root.face) >= scalp.triangleCount()) {
        throw std::invalid_argument("flow pin: not a point on the scalp");
    }
    Vec3f x, y, z;
    surfaceFrame(scalp, at.root, x, y, z);
    const Vec3f tangent = direction - z * direction.dot(z);
    if (tangent.length() < 1e-8f) throw std::invalid_argument("flow pin: the direction is along the surface normal");
    const Vec3f d = tangent.normalized();
    return FlowPin{at.root, Vec3f(d.dot(x), d.dot(y), 0.0f)};
}

FlowSet FlowSet::transferred(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit) const {
    return transferred(ScalpMapping(from, to, fit));
}

FlowSet FlowSet::transferred(const ScalpMapping& mapping) const {
    const ScalpMesh& from = mapping.from();
    const ScalpMesh& to = mapping.to();
    if (to.triangleCount() == 0) throw std::invalid_argument("transfer: the new scalp has no faces");
    if (!mapping.byNormals()) {
        FlowSet out;
        for (const FlowPin& p : pins) {
            if (!from.contains(p.root)) continue;
            Vec3f x, y, z;
            surfaceFrame(from, p.root, x, y, z);
            const MappedPoint m = mapping.map(p.root);
            if (m.target.root.face < 0) continue;
            try {
                FlowPin moved = makePin(to, m.target, m.rotation * (x * p.local.x + y * p.local.y + z * p.local.z));
                moved.line = p.line;
                moved.seq = p.seq;
                out.pins.push_back(moved);
            } catch (const std::invalid_argument&) {
                // Along the new surface's normal: no direction left there.
            }
        }
        for (const FlowSink& s : sinks) {
            if (!from.contains(s.root)) continue;
            const MappedPoint m = mapping.map(s.root);
            if (m.target.root.face < 0) continue;
            FlowSink moved = s;
            moved.root = m.target.root;
            moved.radius = s.radius * m.scale;
            out.sinks.push_back(moved);
        }
        return out;
    }
    const ScalpFit& fit = mapping.fit();
    auto rooted = [&](const StrandRoot& r) { return r.face >= 0 && static_cast<std::size_t>(r.face) < from.triangleCount(); };
    FlowSet out;
    for (const FlowPin& p : pins) {
        if (!rooted(p.root)) continue;
        Vec3f x, y, z;
        surfaceFrame(from, p.root, x, y, z);
        const Vec3f direction = x * p.local.x + y * p.local.y + z * p.local.z;
        try {
            FlowPin moved = makePin(to, to.closestPoint(fit.apply(from.position(p.root))), direction);
            moved.line = p.line;
            moved.seq = p.seq;
            out.pins.push_back(moved);
        } catch (const std::invalid_argument&) {
            // Along the new surface's normal: no direction left there.
        }
    }
    for (const FlowSink& s : sinks) {
        if (!rooted(s.root)) continue;
        FlowSink moved = s;
        moved.root = to.closestPoint(fit.apply(from.position(s.root))).root;
        moved.radius = s.radius * fit.scale;
        out.sinks.push_back(moved);
    }
    return out;
}

namespace {

// The pins and sinks with each root taken by `map` from `from` to `to` (the same place of the
// same surface), each pin's direction turned from from's frame there into to's by the least
// rotation between the two normals (the rotation back is its inverse). Roots off `from` stay off.
template <class Map>
FlowSet rebound(const FlowSet& set, const ScalpMesh& from, const ScalpMesh& to, Map map) {
    FlowSet out = set;
    for (FlowPin& p : out.pins) {
        const StrandRoot root = map(p.root);
        if (!from.contains(p.root) || !to.contains(root)) {
            p.root = root;  // stale: drawn by neither
            continue;
        }
        Vec3f xa, ya, za, xb, yb, zb;
        surfaceFrame(from, p.root, xa, ya, za);
        surfaceFrame(to, root, xb, yb, zb);
        Vec3f d = xa * p.local.x + ya * p.local.y + za * p.local.z;
        const float c = za.dot(zb);
        if (c > -0.999f) {  // Rodrigues' formula, axis za x zb
            const Vec3f v = za.cross(zb);
            d = d * c + v.cross(d) + v * (v.dot(d) / (1.0f + c));
        }
        p.root = root;
        p.local = Vec3f(d.dot(xb), d.dot(yb), d.dot(zb));
    }
    for (FlowSink& s : out.sinks) s.root = map(s.root);
    return out;
}

}  // namespace

FlowSet FlowSet::onRefined(const SubdivisionPlan& plan, const ScalpMesh& cage, const ScalpMesh& refined) const {
    return rebound(*this, cage, refined, [&](const StrandRoot& r) { return plan.refinedRoot(r); });
}

FlowSet FlowSet::onCage(const SubdivisionPlan& plan, const ScalpMesh& refined, const ScalpMesh& cage) const {
    return rebound(*this, refined, cage, [&](const StrandRoot& r) { return plan.cageRoot(r); });
}

std::string FlowSet::toJson() const {
    json list = json::array();
    for (const FlowPin& p : pins) {
        json pin = {{"face", p.root.face}, {"u", p.root.u}, {"v", p.root.v}, {"dir", {p.local.x, p.local.y, p.local.z}}};
        if (p.line >= 0) {
            pin["line"] = p.line;
            pin["seq"] = p.seq;
        }
        list.push_back(pin);
    }
    json out = {{"version", 1}, {"pins", list}};
    if (!sinks.empty()) {
        json sinkList = json::array();
        for (const FlowSink& s : sinks) {
            sinkList.push_back({{"face", s.root.face}, {"u", s.root.u}, {"v", s.root.v}, {"angle", s.angle},
                                {"radius", s.radius}});
        }
        out["sinks"] = sinkList;
    }
    return out.dump();
}

FlowSet FlowSet::fromJson(const std::string& text) {
    FlowSet set;
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return set;
    try {
        const json root = json::parse(text);
        for (const json& p : root.value("pins", json::array())) {
            FlowPin pin;
            pin.root.face = p.at("face").get<std::int32_t>();
            pin.root.u = p.at("u").get<float>();
            pin.root.v = p.at("v").get<float>();
            const json& d = p.at("dir");
            pin.local = Vec3f(d.at(0).get<float>(), d.at(1).get<float>(), d.at(2).get<float>());
            pin.line = p.value("line", -1);
            pin.seq = p.value("seq", 0);
            set.pins.push_back(pin);
        }
        for (const json& s : root.value("sinks", json::array())) {
            FlowSink sink;
            sink.root.face = s.at("face").get<std::int32_t>();
            sink.root.u = s.at("u").get<float>();
            sink.root.v = s.at("v").get<float>();
            sink.angle = s.value("angle", 70.0f);
            sink.radius = s.value("radius", 1.0f);
            set.sinks.push_back(sink);
        }
    } catch (const std::exception& e) {
        throw std::invalid_argument(std::string("flow: ") + e.what());
    }
    return set;
}

std::uint64_t FlowSet::contentHash() const {
    std::uint64_t h = hash64(pins.size() + 0xF10);
    for (const FlowPin& p : pins) {
        h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.root.face)));
        h = hashFloat(hashFloat(h, p.root.u), p.root.v);
        h = hashFloat(hashFloat(hashFloat(h, p.local.x), p.local.y), p.local.z);
        h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.line) * 100003 + p.seq));
    }
    for (const FlowSink& s : sinks) {
        h = hashCombine(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(s.root.face)) + 0x51);
        h = hashFloat(hashFloat(h, s.root.u), s.root.v);
        h = hashFloat(hashFloat(h, s.angle), s.radius);
    }
    return h;
}

FlowField::FlowField(const FlowSet& set, const ScalpMesh& scalp) {
    hash_ = hashCombine(set.contentHash(), scalp.surfaceHash());  // color sets play no part
    for (const FlowPin& p : set.pins) {
        if (p.root.face < 0 || static_cast<std::size_t>(p.root.face) >= scalp.triangleCount()) continue;  // stale
        Vec3f x, y, z;
        surfaceFrame(scalp, p.root, x, y, z);
        const Vec3f d = x * p.local.x + y * p.local.y + z * p.local.z;
        if (d.length() < 1e-8f) continue;
        positions_.push_back(scalp.position(p.root));
        directions_.push_back(d.normalized());
        lines_.push_back(p.line);
        seqs_.push_back(p.seq);
    }
    index_ = PointIndex(positions_);
    for (const FlowSink& s : set.sinks) {
        if (s.root.face < 0 || static_cast<std::size_t>(s.root.face) >= scalp.triangleCount()) continue;  // stale
        if (!(s.radius > 0.0f)) continue;
        sinkPositions_.push_back(scalp.position(s.root));
        sinkNormals_.push_back(scalp.normal(s.root).normalized());
        sinkAngles_.push_back(s.angle * 0.0174532925f);
        sinkRadii_.push_back(s.radius);
    }
    if (!empty()) {
        // Baked at every vertex: what strands read while they are traced along the skin.
        const std::vector<Vec3f>& points = scalp.points();
        vertexDirections_.resize(points.size());
        std::vector<Vec3f> normals(points.size());
        for (const ScalpMesh::Triangle& t : scalp.triangles()) {
            const Vec3f n = (points[t[1]] - points[t[0]]).cross(points[t[2]] - points[t[0]]);
            for (const std::int32_t v : t) normals[static_cast<std::size_t>(v)] += n;
        }
        parallelFor(0, points.size(), [&](std::size_t v) {
            const Vec3f n = normals[v].length() > 0.0f ? normals[v].normalized() : Vec3f{0, 1, 0};
            vertexDirections_[v] = at(points[v], n);
        }, 1024);
    }
}

Vec3f FlowField::atRoot(const ScalpMesh& scalp, const StrandRoot& root, const Vec3f& normal) const {
    if (vertexDirections_.empty() || !scalp.contains(root) || vertexDirections_.size() != scalp.pointCount()) {
        return at(scalp.contains(root) ? scalp.position(root) : Vec3f{}, normal);
    }
    const ScalpMesh::Triangle& t = scalp.triangles()[static_cast<std::size_t>(root.face)];
    const float w = 1.0f - root.u - root.v;
    const Vec3f blend = vertexDirections_[t[0]] * w + vertexDirections_[t[1]] * root.u + vertexDirections_[t[2]] * root.v;
    const Vec3f n = normal.normalized();
    const Vec3f tangent = blend - n * blend.dot(n);
    // Corners disagreeing (a whorl's middle, a part): the exact field there.
    if (tangent.length() < 0.5f) return at(scalp.position(root), normal);
    return tangent.normalized();
}

Vec3f FlowField::sinkDirection(std::size_t s, const Vec3f& position, const Vec3f& normal, float* weight) const {
    *weight = 0.0f;
    const Vec3f n = normal.normalized();
    Vec3f out = position - sinkPositions_[s];
    out = out - n * out.dot(n);  // along the surface here
    const float d = out.length(), r = sinkRadii_[s];
    if (d >= r || d < 1e-6f * r) return {};
    out = out / d;
    const float t = d / r;
    const float fade = t < 0.35f ? 1.0f : 1.0f - (t - 0.35f) / 0.65f;  // full near the center, gone at the radius
    *weight = fade * fade * (3.0f - 2.0f * fade);
    const float a = sinkAngles_[s];
    return out * pm::cos(a) + n.cross(out) * pm::sin(a);
}

Vec3f FlowField::at(const Vec3f& position, const Vec3f& normal) const {
    Vec3f dir = pinsAt(position, normal);
    if (sinkPositions_.empty()) return dir;
    // Sinks take over near their centers: their headings, weighted, replace the pins' by their sum.
    Vec3f turned;
    float total = 0.0f;
    for (std::size_t s = 0; s < sinkPositions_.size(); ++s) {
        float w = 0.0f;
        const Vec3f d = sinkDirection(s, position, normal, &w);
        if (w <= 0.0f) continue;
        turned += d * w;
        total += w;
    }
    if (total <= 0.0f) return dir;
    const Vec3f blended = dir * (1.0f - std::min(total, 1.0f)) + turned * (total > 1.0f ? 1.0f / total : 1.0f);
    return blended.length() > 1e-12f ? blended.normalized() : dir;
}

Vec3f FlowField::pinsAt(const Vec3f& position, const Vec3f& normal) const {
    if (positions_.empty()) return {};
    constexpr std::size_t kNear = 6;
    std::array<std::size_t, kNear> near{};
    std::array<float, kNear> d2{};
    const std::size_t found = index_.kNearest(position, kNear, near.data(), d2.data());
    const Vec3f n = normal.normalized();
    // Weights fall with distance relative to the nearest pin, so one arrow
    // rules close to it and neighbors blend smoothly further away. Scaled by
    // the nearest distance (the same direction, no overflow on a pin).
    const float nearest = std::max(d2[0], 1e-12f);
    Vec3f sum;
    for (std::size_t k = 0; k < found; ++k) {
        const Vec3f& d = directions_[near[k]];
        const Vec3f tangent = d - n * d.dot(n);
        const float len = tangent.length();
        if (len < 1e-6f) continue;
        const float w = nearest / (d2[k] + 0.25f * nearest);
        sum += tangent / len * (w * w);
    }
    return sum.length() > 1e-12f ? sum.normalized() : Vec3f{};
}

}  // namespace groomlab
