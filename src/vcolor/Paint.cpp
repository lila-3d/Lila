#include "vcolor/Paint.h"

#include "brush/Symmetry.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Normals.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::vcolor
{
namespace
{
constexpr std::string_view kBlendNames[] = {"replace", "add", "subtract", "multiply", "lighten", "darken", "blur", "sharpen"};
constexpr std::string_view kBakeNames[] = {"ao", "cavity", "curvature", "height"};

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float toSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

std::vector<std::vector<Index>> noAdjacency;

/** Neighbour points over edges, CSR. */
void buildAdjacency(const geo::Mesh& m, std::vector<uint32_t>& offsets, std::vector<Index>& adj)
{
    const Index pc = m.pointCapacity();
    offsets.assign(size_t(pc) + 1, 0);
    tbb::parallel_for(Index(0), pc, [&](Index p) {
        uint32_t n = 0;
        if (m.pointValid(p))
            m.forEachPointEdge(p, [&](Index) { ++n; });
        offsets[p + 1] = n;
    });
    for (Index p = 0; p < pc; ++p)
        offsets[p + 1] += offsets[p];
    adj.resize(offsets[pc]);
    tbb::parallel_for(Index(0), pc, [&](Index p) {
        if (!m.pointValid(p))
            return;
        uint32_t at = offsets[p];
        m.forEachPointEdge(p, [&](Index e) { adj[at++] = m.edgeOtherPoint(e, p); });
    });
}

std::span<glm::vec4> ensureSet(geo::Mesh& mesh, std::string_view set)
{
    std::span<glm::vec4> v = geo::colors::values(mesh, set);
    return v.empty() ? geo::colors::add(mesh, set) : v;
}

/** Radical inverse base 2, for Hammersley points. */
float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

std::vector<float> smoothValues(const geo::Mesh& m, std::vector<float> v, int iterations)
{
    if (iterations <= 0)
        return v;
    std::vector<uint32_t> off;
    std::vector<Index> adj;
    buildAdjacency(m, off, adj);
    std::vector<float> next(v.size());
    for (int it = 0; it < iterations; ++it)
    {
        tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
            if (off[p + 1] == off[p])
            {
                next[p] = v[p];
                return;
            }
            float sum = 0.0f;
            for (uint32_t k = off[p]; k < off[p + 1]; ++k)
                sum += v[adj[k]];
            next[p] = 0.5f * v[p] + 0.5f * sum / float(off[p + 1] - off[p]);
        });
        v.swap(next);
    }
    return v;
}
} // namespace

std::string_view blendName(Blend b) { return b < Blend::Count ? kBlendNames[size_t(b)] : std::string_view(); }

std::optional<Blend> blendFromName(std::string_view name)
{
    for (size_t i = 0; i < size_t(Blend::Count); ++i)
        if (kBlendNames[i] == name)
            return Blend(i);
    return std::nullopt;
}

uint8_t channelsFromString(std::string_view s)
{
    uint8_t out = 0;
    for (char c : s)
        switch (c | 0x20)
        {
        case 'r': out |= kRed; break;
        case 'g': out |= kGreen; break;
        case 'b': out |= kBlue; break;
        case 'a': out |= kAlpha; break;
        default: break;
        }
    return out;
}

std::string channelsToString(uint8_t channels)
{
    std::string out;
    for (int k = 0; k < 4; ++k)
        if (channels & (1 << k))
            out += "rgba"[k];
    return out;
}

glm::vec4 blend(Blend mode, const glm::vec4& base, const glm::vec4& brush, float w, uint8_t channels)
{
    glm::vec4 target;
    switch (mode)
    {
    case Blend::Replace: target = brush; break;
    case Blend::Add: target = base + brush; break;
    case Blend::Subtract: target = glm::max(base - brush, 0.0f); break;
    case Blend::Multiply: target = base * brush; break;
    case Blend::Lighten: target = glm::max(base, brush); break;
    case Blend::Darken: target = glm::min(base, brush); break;
    case Blend::Blur: target = brush; break;
    case Blend::Sharpen: target = glm::max(base + (base - brush), 0.0f); break;
    default: target = base; break;
    }
    glm::vec4 out = base;
    w = std::clamp(w, 0.0f, 1.0f);
    for (int k = 0; k < 4; ++k)
        if (channels & (1 << k))
            out[k] = base[k] + (target[k] - base[k]) * w;
    return out;
}

glm::vec4 srgbToLinear(const glm::vec4& c) { return {toLinear(c.r), toLinear(c.g), toLinear(c.b), c.a}; }
glm::vec4 linearToSrgb(const glm::vec4& c)
{
    return {toSrgb(std::max(c.r, 0.0f)), toSrgb(std::max(c.g, 0.0f)), toSrgb(std::max(c.b, 0.0f)), c.a};
}

// ------------------------------------------------------------------------------------------------ PaintMesh

PaintMesh::PaintMesh(std::shared_ptr<const geo::Mesh> mesh, std::string set) : mMesh(std::move(mesh)), mSet(std::move(set))
{
    LILA_ZONE();

    const geo::Mesh& m = *mMesh;
    std::span<const glm::vec4> v = geo::colors::values(m, mSet);
    mColors.assign(m.cornerCapacity(), glm::vec4(1.0f));
    std::copy_n(v.begin(), std::min(v.size(), mColors.size()), mColors.begin());
    mNormals = geo::pointNormals(m);

    const Index pc = m.pointCapacity();
    mCornerOffsets.assign(size_t(pc) + 1, 0);
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            ++mCornerOffsets[m.cornerPoint(c) + 1];
    for (Index p = 0; p < pc; ++p)
        mCornerOffsets[p + 1] += mCornerOffsets[p];
    mCorners.resize(mCornerOffsets[pc]);
    {
        std::vector<uint32_t> at(mCornerOffsets.begin(), mCornerOffsets.end() - 1);
        for (Index c = 0; c < m.cornerCapacity(); ++c)
            if (m.cornerValid(c))
                mCorners[at[m.cornerPoint(c)]++] = c;
    }
    buildAdjacency(m, mAdjOffsets, mAdj);
    mBvh.build(m);

    mStamp.assign(pc, 0);
    mWeight.assign(pc, 0.0f);
    mStartIndex.assign(m.cornerCapacity(), 0);
    mChangedFlag.assign(m.cornerCapacity(), 0);
}

void PaintMesh::rebase(std::shared_ptr<const geo::Mesh> mesh)
{
    mMesh = std::move(mesh);
    std::span<const glm::vec4> v = geo::colors::values(*mMesh, mSet);
    std::copy_n(v.begin(), std::min(v.size(), mColors.size()), mColors.begin());
}

bool PaintMesh::raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const
{
    return mBvh.raycast(origin, dir, hit);
}

glm::vec4 PaintMesh::sample(const geo::BvhHit& hit) const
{
    if (hit.triangle == geo::kInvalid)
        return glm::vec4(1.0f);
    return mBvh.interpolate<glm::vec4>(mColors, hit.triangle, hit.bary);
}

void PaintMesh::beginStroke(const BrushSettings& s)
{
    mSettings = s;
    mFalloff = brush::FalloffTable(s.falloff);
    mStroking = true;
    if (++mStroke == 0)
    {
        std::fill(mStamp.begin(), mStamp.end(), 0u);
        mStroke = 1;
    }
    mTouched.clear();
    mStartCorners.clear();
    mStart.clear();
}

void PaintMesh::gather(const Dab& d, std::vector<std::pair<Index, float>>& out) const
{
    LILA_ZONE();

    const geo::Mesh& m = *mMesh;
    geo::Bounds box;
    box.extend(d.center - glm::dvec3(d.radius));
    box.extend(d.center + glm::dvec3(d.radius));
    std::vector<Index> tris = mBvh.overlapping(box);
    const auto& corners = mBvh.triangles().corners;
    std::vector<Index> pts;
    pts.reserve(tris.size());
    for (Index t : tris)
        for (int k = 0; k < 3; ++k)
            pts.push_back(m.cornerPoint(corners[size_t(t) * 3 + k]));
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

    const bool facing = mSettings.frontFacesOnly && glm::length(d.viewDir) > 0.0;
    const bool occlusion = !mSettings.paintThrough;
    const double eps = d.radius * 1e-3;
    std::vector<float> w(pts.size(), 0.0f);
    tbb::parallel_for(size_t(0), pts.size(), [&](size_t i) {
        Index p = pts[i];
        if (!mMask.empty() && (p >= mMask.size() || !mMask[p]))
            return;
        glm::dvec3 pos = m.position(p);
        double dist = glm::length(pos - d.center);
        if (dist > d.radius)
            return;
        glm::dvec3 n(mNormals[p]);
        if (facing && glm::dot(n, d.viewDir) >= 0.0)
            return;
        if (occlusion)
        {
            glm::dvec3 toEye = d.eye - pos;
            if (glm::length(toEye) > 0.0 && mBvh.occluded(pos + n * eps + glm::normalize(toEye) * eps, toEye, 1.0))
                return;
        }
        w[i] = brush::tipWeight(d, mFalloff, pos - d.center) * float(d.opacity * d.flow);
    });
    out.clear();
    for (size_t i = 0; i < pts.size(); ++i)
        if (w[i] > 0.0f)
            out.emplace_back(pts[i], w[i]);
}

void PaintMesh::touchPoint(Index p, float w)
{
    if (mStamp[p] != mStroke)
    {
        mStamp[p] = mStroke;
        mWeight[p] = 0.0f;
        mTouched.push_back(p);
        for (Index c : pointCorners(p))
        {
            mStartIndex[c] = uint32_t(mStart.size());
            mStartCorners.push_back(c);
            mStart.push_back(mColors[c]);
        }
    }
    (void)w;
}

void PaintMesh::dab(const Dab& d)
{
    LILA_ZONE();

    if (!mStroking)
        return;
    std::vector<std::pair<Index, float>> hits;
    gather(d, hits);
    if (mSettings.radial.any())
    {
        std::vector<std::pair<Index, float>> more;
        for (const Dab& copy : brush::radialDabs(d, mSettings.radial))
            if (copy.copy != 0)
            {
                gather(copy, more);
                hits.insert(hits.end(), more.begin(), more.end());
            }
        brush::mergeHits(hits);
    }
    // A point reached from both sides keeps the stronger weight, so mirrored strokes stay symmetric.
    if (mSettings.mirror)
        brush::mirrorHits(hits, mSymmetry, mMask);
    for (auto& [p, w] : hits)
    {
        w *= mSettings.strength;
        touchPoint(p, w);
    }

    const Blend mode = mSettings.blend;
    const uint8_t ch = mSettings.channels;
    if (isFilter(mode))
    {
        // Each point moves toward (or away from) the average of itself and its neighbours, from the current colors.
        std::vector<glm::vec4> avg(hits.size());
        auto pointColor = [&](Index p) {
            glm::vec4 sum(0.0f);
            auto cs = pointCorners(p);
            for (Index c : cs)
                sum += mColors[c];
            return cs.empty() ? glm::vec4(1.0f) : sum / float(cs.size());
        };
        tbb::parallel_for(size_t(0), hits.size(), [&](size_t i) {
            Index p = hits[i].first;
            glm::vec4 sum = pointColor(p);
            auto nb = neighbours(p);
            for (Index q : nb)
                sum += pointColor(q);
            avg[i] = sum / float(nb.size() + 1);
        });
        tbb::parallel_for(size_t(0), hits.size(), [&](size_t i) {
            auto [p, w] = hits[i];
            for (Index c : pointCorners(p))
                mColors[c] = blend(mode, mColors[c], avg[i], w * 0.5f, ch);
        });
    }
    else
    {
        // From the stroke-start color at the strongest weight so far: passing over a spot again never builds up.
        const glm::vec4 color = mSettings.color;
        tbb::parallel_for(size_t(0), hits.size(), [&](size_t i) {
            auto [p, w] = hits[i];
            if (w <= mWeight[p])
                return;
            mWeight[p] = w;
            for (Index c : pointCorners(p))
                mColors[c] = blend(mode, mStart[mStartIndex[c]], color, w, ch);
        });
    }
    for (const auto& [p, w] : hits)
        for (Index c : pointCorners(p))
            if (!mChangedFlag[c])
            {
                mChangedFlag[c] = 1;
                mChanged.push_back(c);
            }
}

ColorDelta PaintMesh::endStroke()
{
    ColorDelta d;
    mStroking = false;
    for (size_t i = 0; i < mStartCorners.size(); ++i)
    {
        Index c = mStartCorners[i];
        if (mColors[c] != mStart[i])
        {
            d.corners.push_back(c);
            d.before.push_back(mStart[i]);
            d.after.push_back(mColors[c]);
        }
    }
    mTouched.clear();
    mStartCorners.clear();
    mStart.clear();
    return d;
}

std::vector<Index> PaintMesh::takeChanged()
{
    for (Index c : mChanged)
        mChangedFlag[c] = 0;
    return std::exchange(mChanged, {});
}

void PaintMesh::apply(const ColorDelta& d, bool undo)
{
    const auto& values = undo ? d.before : d.after;
    for (size_t i = 0; i < d.corners.size(); ++i)
        if (d.corners[i] < mColors.size())
        {
            mColors[d.corners[i]] = values[i];
            if (!mChangedFlag[d.corners[i]])
            {
                mChangedFlag[d.corners[i]] = 1;
                mChanged.push_back(d.corners[i]);
            }
        }
}

// ------------------------------------------------------------------------------------------------ whole-set edits

std::vector<Index> targetPoints(const geo::Mesh& mesh, std::span<const Index> selectedPoints)
{
    std::vector<Index> out;
    if (!selectedPoints.empty())
    {
        for (Index p : selectedPoints)
            if (mesh.pointValid(p))
                out.push_back(p);
        return out;
    }
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            out.push_back(p);
    return out;
}

std::vector<Index> elementPoints(const geo::Mesh& mesh, std::span<const Index> seeds)
{
    std::vector<uint8_t> seen(mesh.pointCapacity(), 0);
    std::vector<Index> stack, out;
    for (Index s : seeds)
        if (mesh.pointValid(s) && !seen[s])
        {
            seen[s] = 1;
            stack.push_back(s);
        }
    while (!stack.empty())
    {
        Index p = stack.back();
        stack.pop_back();
        out.push_back(p);
        mesh.forEachPointEdge(p, [&](Index e) {
            Index q = mesh.edgeOtherPoint(e, p);
            if (!seen[q])
            {
                seen[q] = 1;
                stack.push_back(q);
            }
        });
    }
    std::sort(out.begin(), out.end());
    return out;
}

ColorDelta editPoints(geo::Mesh& mesh, std::string_view set, std::span<const Index> points,
                      const std::function<glm::vec4(Index point, const glm::vec4& color)>& fn)
{
    LILA_ZONE();

    std::span<glm::vec4> v = ensureSet(mesh, set);
    std::vector<glm::vec4> out(v.begin(), v.end());
    const bool half = geo::colors::isHalf(mesh, set);
    tbb::parallel_for(size_t(0), points.size(), [&](size_t i) {
        Index p = points[i];
        mesh.forEachPointCorner(p, [&](Index c) {
            out[c] = fn(p, v[c]);
            if (half)
                for (int k = 0; k < 4; ++k)
                    out[c][k] = geo::colors::roundToHalf(out[c][k]);
        });
    });
    ColorDelta d;
    for (Index c = 0; c < mesh.cornerCapacity(); ++c)
        if (mesh.cornerValid(c) && out[c] != v[c])
        {
            d.corners.push_back(c);
            d.before.push_back(v[c]);
            d.after.push_back(out[c]);
            v[c] = out[c];
        }
    return d;
}

ColorDelta setCorners(geo::Mesh& mesh, std::string_view set, std::span<const glm::vec4> perCorner)
{
    std::span<glm::vec4> v = ensureSet(mesh, set);
    const bool half = geo::colors::isHalf(mesh, set);
    ColorDelta d;
    for (Index c = 0; c < std::min<size_t>(mesh.cornerCapacity(), perCorner.size()); ++c)
    {
        if (!mesh.cornerValid(c))
            continue;
        glm::vec4 n = perCorner[c];
        if (half)
            for (int k = 0; k < 4; ++k)
                n[k] = geo::colors::roundToHalf(n[k]);
        if (n != v[c])
        {
            d.corners.push_back(c);
            d.before.push_back(v[c]);
            d.after.push_back(n);
            v[c] = n;
        }
    }
    return d;
}

ColorDelta fill(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, Blend mode, const glm::vec4& color, float opacity,
                uint8_t channels)
{
    if (isFilter(mode))
        return mode == Blend::Blur ? blur(mesh, set, points, 1, opacity, channels) : blur(mesh, set, points, 1, -opacity, channels);
    return editPoints(mesh, set, points, [&](Index, const glm::vec4& c) { return blend(mode, c, color, opacity, channels); });
}

ColorDelta gradient(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, GradientType type, const glm::dvec3& from,
                    const glm::dvec3& to, const glm::vec4& colorA, const glm::vec4& colorB, uint8_t channels)
{
    const glm::dvec3 axis = to - from;
    const double len2 = std::max(glm::dot(axis, axis), 1e-24);
    return editPoints(mesh, set, points, [&](Index p, const glm::vec4& c) {
        glm::dvec3 d = mesh.position(p) - from;
        double t = type == GradientType::Linear ? glm::dot(d, axis) / len2 : glm::length(d) / std::sqrt(len2);
        glm::vec4 g = glm::mix(colorA, colorB, float(std::clamp(t, 0.0, 1.0)));
        return blend(Blend::Replace, c, g, 1.0f, channels);
    });
}

ColorDelta invert(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, uint8_t channels)
{
    return editPoints(mesh, set, points, [&](Index, const glm::vec4& c) {
        glm::vec4 out = c;
        for (int k = 0; k < 4; ++k)
            if (channels & (1 << k))
                out[k] = 1.0f - std::clamp(c[k], 0.0f, 1.0f);
        return out;
    });
}

float applyLevels(float v, const Levels& l)
{
    float range = std::max(l.inWhite - l.inBlack, 1e-6f);
    float t = std::clamp((v - l.inBlack) / range, 0.0f, 1.0f);
    t = std::pow(t, 1.0f / std::max(l.gamma, 1e-3f));
    return l.outBlack + t * (l.outWhite - l.outBlack);
}

ColorDelta levels(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, const Levels& l, uint8_t channels)
{
    return editPoints(mesh, set, points, [&](Index, const glm::vec4& c) {
        glm::vec4 out = c;
        for (int k = 0; k < 4; ++k)
            if (channels & (1 << k))
                out[k] = applyLevels(c[k], l);
        return out;
    });
}

ColorDelta blur(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, int iterations, float strength, uint8_t channels)
{
    LILA_ZONE();

    ensureSet(mesh, set);
    std::vector<glm::vec4> pc = geo::colors::pointColors(mesh, set);
    std::vector<uint32_t> off;
    std::vector<Index> adj;
    buildAdjacency(mesh, off, adj);
    std::vector<uint8_t> in(mesh.pointCapacity(), 0);
    for (Index p : points)
        in[p] = 1;
    // Negative strength sharpens: pushes each point away from its neighbourhood.
    const Blend mode = strength < 0.0f ? Blend::Sharpen : Blend::Blur;
    const float w = std::min(std::abs(strength), 1.0f);
    std::vector<glm::vec4> next = pc;
    for (int it = 0; it < std::max(iterations, 1); ++it)
    {
        tbb::parallel_for(size_t(0), points.size(), [&](size_t i) {
            Index p = points[i];
            if (off[p + 1] == off[p])
                return;
            glm::vec4 sum(0.0f);
            for (uint32_t k = off[p]; k < off[p + 1]; ++k)
                sum += pc[adj[k]];
            next[p] = blend(mode, pc[p], sum / float(off[p + 1] - off[p]), w, kRgba);
        });
        pc = next;
    }
    return editPoints(mesh, set, points, [&](Index p, const glm::vec4& c) { return blend(Blend::Replace, c, pc[p], 1.0f, channels); });
}

// ------------------------------------------------------------------------------------------------ bakes

std::string_view bakeName(BakeType t) { return t < BakeType::Count ? kBakeNames[size_t(t)] : std::string_view(); }

std::optional<BakeType> bakeFromName(std::string_view name)
{
    for (size_t i = 0; i < size_t(BakeType::Count); ++i)
        if (kBakeNames[i] == name)
            return BakeType(i);
    return std::nullopt;
}

std::vector<float> bake(const geo::Mesh& mesh, const BakeOptions& o)
{
    LILA_ZONE();

    const Index pc = mesh.pointCapacity();
    std::vector<float> out(pc, 1.0f);
    std::vector<glm::vec3> normals = geo::pointNormals(mesh);
    geo::Bounds bounds = mesh.bounds();
    const double diag = std::max(glm::length(bounds.size()), 1e-9);

    switch (o.type)
    {
    case BakeType::AmbientOcclusion:
    {
        geo::MeshBvh bvh(mesh);
        const int rays = std::clamp(o.rays, 1, 4096);
        const double maxDist = o.maxDistance > 0.0 ? o.maxDistance : diag * 0.25;
        const double eps = diag * 1e-5;
        tbb::parallel_for(Index(0), pc, [&](Index p) {
            if (!mesh.pointValid(p) || mesh.pointEdge(p) == geo::kInvalid)
                return;
            glm::dvec3 n(normals[p]);
            if (glm::length(n) < 1e-12)
                return;
            n = glm::normalize(n);
            glm::dvec3 ref = std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            glm::dvec3 tx = glm::normalize(glm::cross(n, ref)), ty = glm::cross(n, tx);
            glm::dvec3 origin = mesh.position(p) + n * eps;
            // Cosine-weighted Hammersley directions, rotated per point so neighbours don't share banding.
            float rot = float(hash(p) & 0xFFFF) / 65536.0f;
            int hits = 0;
            for (int r = 0; r < rays; ++r)
            {
                float u = (float(r) + 0.5f) / float(rays);
                float v = std::fmod(radicalInverse(uint32_t(r)) + rot, 1.0f);
                double phi = 2.0 * 3.14159265358979 * v;
                double sr = std::sqrt(u);
                glm::dvec3 dir = tx * (sr * std::cos(phi)) + ty * (sr * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1.0 - u));
                if (bvh.occluded(origin, dir, maxDist))
                    ++hits;
            }
            out[p] = 1.0f - float(hits) / float(rays);
        });
        break;
    }
    case BakeType::Cavity:
    case BakeType::Curvature:
    {
        std::vector<uint32_t> off;
        std::vector<Index> adj;
        buildAdjacency(mesh, off, adj);
        const bool cavity = o.type == BakeType::Cavity;
        tbb::parallel_for(Index(0), pc, [&](Index p) {
            if (off[p + 1] == off[p])
                return;
            glm::dvec3 n(normals[p]);
            double s = 0.0;
            for (uint32_t k = off[p]; k < off[p + 1]; ++k)
            {
                glm::dvec3 d = mesh.position(adj[k]) - mesh.position(p);
                double l = glm::length(d);
                if (l > 1e-12)
                    s -= glm::dot(n, d) / l;
            }
            // s > 0 where the neighbours fall away below the tangent plane (convex), < 0 in creases.
            s = s / double(off[p + 1] - off[p]) * 4.0 * o.contrast;
            out[p] = cavity ? float(std::clamp(1.0 + std::min(s, 0.0), 0.0, 1.0)) : float(std::clamp(0.5 + 0.5 * s, 0.0, 1.0));
        });
        break;
    }
    case BakeType::Height:
    {
        int axis = std::clamp(o.axis, 0, 2);
        double lo = bounds.min[axis], span = std::max(bounds.max[axis] - lo, 1e-12);
        tbb::parallel_for(Index(0), pc, [&](Index p) {
            if (mesh.pointValid(p))
                out[p] = float((mesh.position(p)[axis] - lo) / span);
        });
        break;
    }
    default: break;
    }
    return smoothValues(mesh, std::move(out), o.blur);
}

ColorDelta writeChannel(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, std::span<const float> perPoint, uint8_t channels)
{
    return editPoints(mesh, set, points, [&](Index p, const glm::vec4& c) {
        glm::vec4 out = c;
        for (int k = 0; k < 4; ++k)
            if (channels & (1 << k))
                out[k] = p < perPoint.size() ? perPoint[p] : 1.0f;
        return out;
    });
}
} // namespace lila::vcolor
