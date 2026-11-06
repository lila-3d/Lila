// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Viewport geometry for any host (display.h): strands, guides, instances, colors and the
// builder that keeps what each part was built for. The overlays, cards and the scalp map are in
// display_overlays.cpp.

#include "groomlab/display.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "groomlab/color_patterns.h"
#include "groomlab/curve.h"
#include "groomlab/export_view.h"
#include "groomlab/feathers.h"
#include "groomlab/follow_scalp.h"
#include "groomlab/frames.h"
#include "groomlab/hair_color.h"
#include "groomlab/parallel.h"
#include "groomlab/point_index.h"
#include "groomlab/random.h"

namespace groomlab {

static_assert(sizeof(Vec3f) == 3 * sizeof(float), "Vec3f is copied into float buffers as xyz");

std::uint64_t bufferDigest(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = hash64(bytes);
    std::size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, p + i, 8);
        h = hashCombine(h, word);
    }
    std::uint64_t tail = 0;
    if (i < bytes) std::memcpy(&tail, p + i, bytes - i);
    return hashCombine(h, tail);
}

bool DisplayGeometry::lit() const { return lines ? lines->lit && lines->vertexCount() > 0 : !normals.empty(); }

bool DisplayGeometry::empty() const { return indexCount() == 0; }

std::size_t DisplayGeometry::vertexCount() const {
    if (!lines) return positions.size() / 3;
    return ribbons ? 2 * lines->vertexCount() : lines->vertexCount();
}

std::size_t DisplayGeometry::indexCount() const {
    if (!lines) return indices.size();
    return ribbons ? 3 * lines->indexCount() : lines->indexCount();  // (six a segment, two as lines)
}

DisplayGeometry DisplayGeometry::materialized() const {
    DisplayGeometry g = *this;
    if (!lines) return g;
    g.lines.reset();
    if (ribbons) {
        if (!lines->strands) return g;
        // Lit: the lines' colors are the strands' own (per point); unlit: one a strand (the default
        // color where they carry none).
        const StrandSet& strands = *lines->strands;
        const StrandColors* own = lines->lit ? lines->colors.get() : nullptr;
        std::vector<std::array<float, 3>> fallback;
        const bool byStrand = !lines->lit && lines->colors && lines->colors->colors.size() == strands.strandCount();
        if (!lines->lit && !byStrand) fallback.assign(strands.strandCount(), kDefaultStrandColor);
        g = strandRibbons(strands, lines->scalp.get(), byStrand ? lines->colors->colors : fallback, ribbonWidthScale, lines->lit, own);
        g.ribbons = true;
        g.ribbonWidthScale = ribbonWidthScale;
        return g;
    }
    const std::size_t n = lines->vertexCount();
    g.positions.resize(n * 3);
    lines->writePositions(g.positions.data());
    g.indices.resize(lines->indexCount());
    lines->writeIndices(g.indices.data());
    if (lines->colors) {
        g.colors.resize(n * 4);
        lines->writeColors(g.colors.data());
    }
    if (lines->lit && n > 0) {
        g.normals.resize(n * 3);
        g.tangents.resize(n * 3);
        g.uvs.resize(n);
        g.uvFloats = 1;
        lines->writeFrames(g.normals.data(), g.tangents.data());
        lines->writeUvs(g.uvs.data());
    }
    return g;
}

std::uint64_t DisplayGeometry::digest() const {
    if (lines) return materialized().digest();
    std::uint64_t h = hash64(0xD15B1A7);
    auto add = [&h](const auto& v) { h = hashCombine(h, bufferDigest(v.data(), v.size() * sizeof(v[0]))); };
    add(positions);
    add(indices);
    add(colors);
    add(normals);
    add(tangents);
    add(bitangents);
    add(uvs);
    return hashCombine(h, (triangles ? 1u : 0u) + (surface ? 2u : 0u) + (cardTextures ? 4u : 0u) + (overlay ? 8u : 0u) +
                              (mapTexture ? 16u : 0u) + (scaleSurface ? 32u : 0u));
}

namespace {

// Appends `n` points (xyz) to a float buffer in one copy.
void appendPoints(std::vector<float>& out, const Vec3f* pts, std::size_t n) {
    const std::size_t at = out.size();
    out.resize(at + n * 3);
    std::memcpy(out.data() + at, pts, n * sizeof(Vec3f));
}

// Writes the line segments joining `n` consecutive vertices from `base`.
void writeSegments(std::uint32_t* out, std::uint32_t base, std::size_t n) {
    for (std::uint32_t i = 0; i + 1 < n; ++i) {
        *out++ = base + i;
        *out++ = base + i + 1;
    }
}

void appendSegments(std::vector<std::uint32_t>& indices, std::uint32_t base, std::size_t n) {
    if (n < 2) return;
    const std::size_t at = indices.size();
    indices.resize(at + (n - 1) * 2);
    writeSegments(indices.data() + at, base, n);
}

void put3(float* out, const Vec3f& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

// The scalp normal under a strand's root (up when unrooted); a feather's vane normal instead
// (Propagate's, color_patterns.h: a feather standing off the skin faces its own way).
Vec3f rootNormal(const StrandSet& s, const ScalpMesh* scalp, std::size_t i, const VaneNormals& vanes) {
    Vec3f vane;
    if (vanes.at(i, &vane)) return vane;
    const StrandRoot& root = s.roots()[i];
    const bool rooted = scalp && root.face >= 0 && static_cast<std::size_t>(root.face) < scalp->triangleCount();
    return rooted ? scalp->normal(root) : Vec3f{0, 1, 0};
}

// Shading frame at a strand point: tangent along the strand, bitangent across, and a normal
// leaning out of the scalp: the root normal with half its along-the-strand part removed, so
// combed hair shades like the surface under it and standing fur (tangent along the normal)
// still faces out, not sideways.
void strandFrame(const Vec3f* p, std::size_t n, std::size_t k, const Vec3f& up, Vec3f& tangent, Vec3f& normal, Vec3f& side) {
    tangent = (p[std::min(k + 1, n - 1)] - p[k > 0 ? k - 1 : 0]).normalized();
    side = tangent.cross(up);
    if (side.length() < 1e-4f) side = anyPerpendicular(tangent);
    side = side.normalized();
    normal = up - tangent * (0.5f * up.dot(tangent));
    normal = normal.length() > 1e-4f ? normal.normalized() : up;
}

// A frame at vertex `v` of the three lit streams (resized for it): the normal, the tangent and
// u (the bitangent across the strand is the host's cross product).
void writeFrame(DisplayGeometry& g, std::size_t v, const Vec3f& t, const Vec3f& n, float u) {
    put3(g.normals.data() + v * 3, n);
    put3(g.tangents.data() + v * 3, t);
    g.uvs[v] = u;
}

void resizeFrames(DisplayGeometry& g, std::size_t vertices) {
    g.normals.resize(vertices * 3);
    g.tangents.resize(vertices * 3);
    g.uvs.resize(vertices);
    g.uvFloats = 1;
}

// Strands per parallel task: a strand's few points are little work.
constexpr std::size_t kStrandGrain = 1024;

}  // namespace

// ---- Colors -----------------------------------------------------------------------------------

std::array<float, 3> hueColor(float h, float sat, float val) {
    h = (h - std::floor(h)) * 6.0f;
    const int i = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);
    const float p = val * (1 - sat), q = val * (1 - sat * f), t = val * (1 - sat * (1 - f));
    switch (i) {
        case 0: return {val, t, p};
        case 1: return {q, val, p};
        case 2: return {p, val, t};
        case 3: return {p, q, val};
        case 4: return {t, p, val};
        default: return {val, p, q};
    }
}

std::array<float, 3> rampColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return hueColor((1.0f - t) * (2.0f / 3.0f), 0.85f, 0.95f);
}

float guideHueSeed(std::string_view groomName) {
    if (groomName.empty()) return 0.0f;
    std::uint32_t h = 2166136261u;  // FNV-1a: the same on every platform and host
    for (const char c : groomName) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    // Mixed (MurmurHash3's finalizer): FNV alone keeps names a digit apart (groomLab1, groomLab2)
    // within a few thousandths of a turn, the same color to the eye.
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f;  // 24 bits: exact in a float, in [0, 1)
}

std::map<GroupId, std::array<float, 3>> autoGuideColors(const GuideHierarchy& h, float seed) {
    // Parents before their children, each level in the order its groups were made (ids).
    std::vector<std::pair<int, const GuideGroup*>> order;
    order.reserve(h.groups().size());
    for (const GuideGroup& g : h.groups()) order.emplace_back(h.groupDepth(g.id), &g);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second->id < b.second->id;
    });
    auto fraction = [](float v) { return v - std::floor(v); };
    auto apart = [&](float a, float b) {
        const float d = fraction(a - b);
        return std::min(d, 1.0f - d);
    };
    // A golden-ratio step a group from the seed (any two groups made one after another are
    // 0.38 apart); one that lands within kApart of its parent's or an earlier sibling's hue turns
    // by a half, then quarters, then eighths, the first turn clear of them all (else the clearest).
    constexpr float kApart = 0.12f;
    constexpr float kTurns[] = {0.0f, 0.5f, 0.25f, 0.75f, 0.125f, 0.375f, 0.625f, 0.875f};
    std::map<GroupId, float> hues;
    std::vector<float> near;
    for (const auto& item : order) {
        const GuideGroup* g = item.second;
        near.clear();
        const auto parent = hues.find(g->parent);
        if (parent != hues.end()) near.push_back(parent->second);
        for (const auto& entry : order) {
            const GuideGroup* other = entry.second;
            if (other == g) break;
            const auto sibling = hues.find(other->id);
            if (other->parent == g->parent && sibling != hues.end()) near.push_back(sibling->second);
        }
        const float base = 0.6f + seed + 0.618034f * static_cast<float>(g->id);
        float best = fraction(base), bestGap = -1.0f;
        for (const float turn : kTurns) {
            const float hue = fraction(base + turn);
            float gap = 1.0f;
            for (const float n : near) gap = std::min(gap, apart(hue, n));
            if (gap > bestGap) best = hue, bestGap = gap;
            if (gap >= kApart) break;
        }
        hues[g->id] = best;
    }
    std::map<GroupId, std::array<float, 3>> colors;
    for (const auto& [id, hue] : hues) colors[id] = hueColor(hue, kGuideSaturation, kGuideValue);
    return colors;
}

void densityColors(const StrandSet& s, std::vector<std::array<float, 3>>& out) {
    constexpr std::size_t kNeighbors = 8;
    const std::size_t count = s.strandCount();
    if (count <= kNeighbors) return;
    std::vector<Vec3f> roots(count);
    for (std::size_t i = 0; i < count; ++i) roots[i] = s.strandPoints(i)[0];
    const PointIndex index(roots);
    std::vector<float> density(count);
    parallelFor(0, count, [&](std::size_t i) {
        std::size_t near[kNeighbors + 1];
        float d2[kNeighbors + 1];
        const std::size_t found = index.kNearest(roots[i], kNeighbors + 1, near, d2);
        density[i] = found > 1 ? static_cast<float>(found - 1) / (3.14159265f * std::max(d2[found - 1], 1e-12f)) : 0.0f;
    });
    std::vector<float> sorted = density;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count / 2), sorted.end());
    const float median = std::max(sorted[count / 2], 1e-12f);
    parallelFor(0, count, [&](std::size_t i) {
        const float stops = pm::log2(std::max(density[i], 1e-12f) / median);  // -2..2 shown
        out[i] = rampColor(0.5f + stops / 4.0f);
    }, kStrandGrain);
}

std::vector<std::array<float, 3>> strandColors(const StrandSet& s, const ScalpMesh* scalp, int mode, const std::string& source,
                                               float lo, float hi) {
    std::vector<std::array<float, 3>> out(s.strandCount(), kDefaultStrandColor);
    const std::vector<float>* channel = s.strandChannel(source);
    if (mode == kDensityColors) {
        densityColors(s, out);
        return out;
    }
    if (mode < kColorSetColors || mode > kClumpColors) return out;
    const bool colorSet = scalp && scalp->hasColorSet(source);
    parallelFor(0, s.strandCount(), [&](std::size_t i) {
        switch (mode) {
            case kColorSetColors: {
                const StrandRoot& root = s.roots()[i];
                if (colorSet && root.face >= 0 && static_cast<std::size_t>(root.face) < scalp->triangleCount()) {
                    const ScalpMesh::Color c = scalp->colorAt(source, root);
                    out[i] = {c[0], c[1], c[2]};
                }
                break;
            }
            case kAttributeColors:
                if (channel) out[i] = rampColor(hi != lo ? ((*channel)[i] - lo) / (hi - lo) : 0.5f);
                break;
            case kRandomColors:
                out[i] = hueColor(unitFloat(hash64(s.ids()[i])), 0.6f, 0.95f);
                break;
            case kClumpColors:
                if (channel) {
                    const auto group = static_cast<std::int64_t>(std::floor((*channel)[i]));
                    out[i] = hueColor(unitFloat(hash64(static_cast<std::uint64_t>(group) ^ 0xC1A5)), 0.7f, 0.95f);
                }
                break;
            default:
                break;
        }
    }, kStrandGrain);
    return out;
}

bool strandChannelRange(const StrandSet& s, const std::string& channel, float& lo, float& hi) {
    const std::vector<float>* values = s.strandChannel(channel);
    if (!values) return false;
    float low = std::numeric_limits<float>::infinity(), high = -low;
    for (const float v : *values) {
        if (!std::isfinite(v)) continue;
        low = std::min(low, v);
        high = std::max(high, v);
    }
    if (low > high) return false;
    lo = low;
    hi = high;
    return true;
}

void writeStrandPointColors(const StrandSet& s, const std::vector<std::array<float, 3>>& colors, float* rgba) {
    parallelFor(0, s.strandCount(), [&](std::size_t i) {
        float* at = rgba + s.strandBegin(i) * 4;
        const std::array<float, 3>& c = colors[i];
        for (std::size_t k = 0; k < s.strandPointCount(i); ++k, at += 4) {
            at[0] = c[0];
            at[1] = c[1];
            at[2] = c[2];
            at[3] = 1.0f;
        }
    }, kStrandGrain);
}

std::vector<float> strandPointColors(const StrandSet& s, const std::vector<std::array<float, 3>>& colors) {
    std::vector<float> rgba(s.pointCount() * 4);
    writeStrandPointColors(s, colors, rgba.data());
    return rgba;
}

float StrandChannelMeans::mean(const StrandSet* strands, std::uint64_t version, const std::string& channel, bool* found) {
    if (version != version_) {
        version_ = version;
        means_.clear();
    }
    auto it = means_.find(channel);
    if (it == means_.end()) {
        const std::vector<float>* values = strands ? strands->strandChannel(channel) : nullptr;
        double sum = 0.0;
        if (values && !values->empty()) {
            for (const float v : *values) sum += v;
            sum /= static_cast<double>(values->size());
        }
        it = means_.emplace(channel, std::make_pair(values && !values->empty(), static_cast<float>(sum))).first;
    }
    *found = it->second.first;
    return it->second.second;
}

std::array<float, 3> strandsLookColor(const StrandSet* strands, std::uint64_t version, const std::array<float, 3>& displayColor,
                                      StrandChannelMeans& means) {
    bool cr = false, cg = false, cb = false;
    const std::array<float, 3> explicitColor{means.mean(strands, version, "colorR", &cr), means.mean(strands, version, "colorG", &cg),
                                             means.mean(strands, version, "colorB", &cb)};
    if (cr && cg && cb) return explicitColor;
    bool melanin = false, redness = false;
    const float m = means.mean(strands, version, "melanin", &melanin);
    const float r = means.mean(strands, version, "melaninRedness", &redness);
    if (melanin) return melaninColor(m, redness ? r : 0.2f, {1.0f, 1.0f, 1.0f});
    return displayColor;
}

std::array<float, 3> StrandColors::at(std::size_t strand, std::size_t point) const {
    if (!channels[0]) return colors[strand];
    const std::size_t i = perPoint ? point : strand;
    const std::array<float, 3> c{(*channels[0])[i], (*channels[1])[i], (*channels[2])[i]};
    return appearance ? hairAppearanceColor(c) : c;
}

float StrandColors::alpha(std::size_t strand, std::size_t point) const {
    const float iridescent = iridescence ? (*iridescence)[point] : 0.0f;
    return wetness ? packWetLook(iridescent, (*wetness)[wetnessPerPoint ? point : strand]) : 1.0f + iridescent;
}

void StrandColors::write(float* rgba, std::size_t first, std::size_t end) const {
    if (!strands || first >= end) return;
    const StrandSet& s = *strands;
    const std::size_t base = s.strandBegin(first);
    parallelFor(first, end, [&](std::size_t i) {
        const std::size_t begin = s.strandBegin(i), n = s.strandPointCount(i);
        float* out = rgba + (begin - base) * 4;
        for (std::size_t k = 0; k < n; ++k, out += 4) {
            const std::array<float, 3> c = at(i, begin + k);
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            out[3] = alpha(i, begin + k);
        }
    }, kStrandGrain);
}

void StrandColors::write(float* rgba) const {
    if (strands) write(rgba, 0, strands->strandCount());
}

std::vector<float> StrandColors::rgba() const {
    std::vector<float> out(vertexCount() * 4);
    write(out.data());
    return out;
}

namespace {

// Whether any value differs from the first (stops at the first that does).
bool varies(const std::vector<float>& values) {
    return std::any_of(values.begin(), values.end(), [&](float v) { return v != values.front(); });
}

// bufferDigest of chunks in parallel, then the chunks' digests in order: the same for the same
// floats, at 500k strands' per-point colors (100 MB) a few ms rather than tens.
std::uint64_t parallelDigest(const float* values, std::size_t count) {
    constexpr std::size_t kChunk = std::size_t{1} << 18;
    const std::size_t chunks = (count + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> digests(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk;
        digests[c] = bufferDigest(values + begin, std::min(kChunk, count - begin) * sizeof(float));
    }, 1);
    return bufferDigest(digests.data(), digests.size() * sizeof(std::uint64_t));
}

std::uint64_t parallelDigest(const std::vector<float>& values) { return parallelDigest(values.data(), values.size()); }

}  // namespace

std::shared_ptr<const StrandColors> strandLookColors(const StrandsPtr& strands, bool appearance) {
    if (!strands || strands->strandCount() == 0) return nullptr;
    const StrandSet& s = *strands;
    auto out = std::make_shared<StrandColors>();
    out->strands = strands;
    // The iridescence rides along in the alpha: with any, the colors are kept even when they do not vary.
    const std::vector<float>* iridescence = s.pointChannel(kIridescenceChannel);
    if (iridescence && iridescence->size() != s.pointCount()) iridescence = nullptr;
    out->iridescence = iridescence;
    // So does Wet's wetness, packed with it (hair_color.h packWetLook).
    const std::vector<float>* wetness = wetnessChannel(s, &out->wetnessPerPoint);
    out->wetness = wetness;
    std::uint64_t iridescenceKey = iridescence ? parallelDigest(*iridescence) : 0;
    if (wetness) iridescenceKey = hashCombine(iridescenceKey ^ (out->wetnessPerPoint ? 0x3E7ull : 0x3E8ull), parallelDigest(*wetness));
    const bool kept = iridescence || wetness;
    // HairColor's RGB, per point (a tip blend) before per strand, as the render takes them.
    for (const bool perPoint : {true, false}) {
        std::array<const std::vector<float>*, 3> rgb{};
        const std::size_t want = perPoint ? s.pointCount() : s.strandCount();
        bool found = true;
        for (int k = 0; k < 3; ++k) {
            rgb[k] = perPoint ? s.pointChannel(kColorChannels[k]) : s.strandChannel(kColorChannels[k]);
            found = found && rgb[k] && rgb[k]->size() == want;
        }
        if (!found) continue;
        if (!varies(*rgb[0]) && !varies(*rgb[1]) && !varies(*rgb[2]) && !kept) return nullptr;
        out->channels = rgb;
        out->perPoint = perPoint;
        out->appearance = appearance;
        out->key = hashCombine(hashCombine(hashCombine(hashCombine((perPoint ? 2u : 1u) + (appearance ? 8u : 0u), parallelDigest(*rgb[0])),
                                                       parallelDigest(*rgb[1])),
                                           parallelDigest(*rgb[2])),
                               iridescenceKey);
        if (out->key == 0) out->key = 1;
        return out;
    }
    // Else the pigment's color per strand (strandsLookColor's default redness without one).
    const std::vector<float>* melanin = s.strandChannel(kMelaninChannel);
    if (!melanin || melanin->size() != s.strandCount()) return nullptr;
    const std::vector<float>* redness = s.strandChannel(kMelaninRednessChannel);
    if (redness && redness->size() != s.strandCount()) redness = nullptr;
    if (!varies(*melanin) && !(redness && varies(*redness)) && !kept) return nullptr;
    out->colors.resize(s.strandCount());
    parallelFor(0, s.strandCount(), [&](std::size_t i) {
        out->colors[i] = melaninColor((*melanin)[i], redness ? (*redness)[i] : 0.2f, {1.0f, 1.0f, 1.0f});
        if (appearance) out->colors[i] = hairAppearanceColor(out->colors[i]);
    }, kStrandGrain);
    static_assert(sizeof(std::array<float, 3>) == 3 * sizeof(float), "per-strand colors are hashed as floats");
    out->key = hashCombine(hashCombine(appearance ? 11u : 3u, parallelDigest(out->colors.front().data(), out->colors.size() * 3)),
                           iridescenceKey);
    if (out->key == 0) out->key = 1;
    return out;
}

std::shared_ptr<const StrandColors> wetLookColors(const StrandsPtr& strands, const std::array<float, 3>& color) {
    if (!strands || strands->strandCount() == 0) return nullptr;
    bool perPoint = true;
    const std::vector<float>* wetness = wetnessChannel(*strands, &perPoint);
    if (!wetness) return nullptr;
    auto out = std::make_shared<StrandColors>();
    out->strands = strands;
    out->colors.assign(strands->strandCount(), color);
    out->wetness = wetness;
    out->wetnessPerPoint = perPoint;
    std::uint64_t key = hashCombine(perPoint ? 6u : 7u, parallelDigest(*wetness));
    for (const float c : color) key = hashFloat(key, c);
    out->key = key | 1u;
    return out;
}

HairHighlight hairHighlight(const HairLook& look) {
    return {{0.93f * look.specular, 0.87f * look.specular, 0.8f * look.specular},
            4.0f + 120.0f * (1.0f - look.roughness) * (1.0f - look.roughness)};
}

std::vector<std::uint8_t> rootToTipRamp(unsigned width) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * 4);
    for (unsigned x = 0; x < width; ++x) {
        const float u = width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 0.0f;
        const float v = 0.55f + 0.45f * std::sqrt(u);
        const auto b = static_cast<std::uint8_t>(std::lround(255.0f * v));
        pixels[x * 4 + 0] = pixels[x * 4 + 1] = pixels[x * 4 + 2] = b;
        pixels[x * 4 + 3] = 255;
    }
    return pixels;
}

std::map<std::string, std::array<float, 3>> parseGuideGroupColors(const std::string& colorText) {
    std::map<std::string, std::array<float, 3>> groupColors;
    std::size_t at = 0;
    while (at < colorText.size()) {
        const std::size_t end = std::min(colorText.find(' ', at), colorText.size());
        const std::string word = colorText.substr(at, end - at);
        at = end + 1;
        const std::size_t colon = word.rfind(':');
        if (colon == std::string::npos || word.size() != colon + 8 || word[colon + 1] != '#') continue;
        std::array<float, 3> rgb{};
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            char* stop = nullptr;
            const std::string hex = word.substr(colon + 2 + 2 * static_cast<std::size_t>(k), 2);
            const long v = std::strtol(hex.c_str(), &stop, 16);
            ok = stop && *stop == '\0';
            const float srgb = static_cast<float>(v) / 255.0f;
            rgb[static_cast<std::size_t>(k)] = srgb <= 0.04045f ? srgb / 12.92f : pm::pow((srgb + 0.055f) / 1.055f, 2.4f);
        }
        if (ok) groupColors[word.substr(0, colon)] = rgb;
    }
    return groupColors;
}

std::string guideColorHex(const std::array<float, 3>& linear) {
    static const char* const kDigits = "0123456789abcdef";
    std::string text = "#";
    for (const float c : linear) {
        const float v = std::clamp(c, 0.0f, 1.0f);
        const float srgb = v <= 0.0031308f ? 12.92f * v : 1.055f * pm::pow(v, 1.0f / 2.4f) - 0.055f;
        const int byte = std::clamp(static_cast<int>(std::lround(srgb * 255.0f)), 0, 255);
        text += kDigits[byte >> 4];
        text += kDigits[byte & 15];
    }
    return text;
}

std::set<std::string> parseHiddenGuideGroups(const std::string& text) {
    std::set<std::string> names;
    std::string word;
    for (const char c : text + " ") {
        if (c == ' ' || c == ',') {
            if (!word.empty()) names.insert(word);
            word.clear();
        } else {
            word += c;
        }
    }
    return names;
}

// ---- Strands ----------------------------------------------------------------------------------

void appendPolyline(DisplayGeometry& batch, const Vec3f* pts, std::size_t n) {
    if (n < 2) return;
    const auto base = static_cast<std::uint32_t>(batch.positions.size() / 3);
    appendPoints(batch.positions, pts, n);
    appendSegments(batch.indices, base, n);
}

void appendStrandLines(DisplayGeometry& batch, const StrandSet& s) {
    const auto& pts = s.positions();
    const auto base = static_cast<std::uint32_t>(batch.positions.size() / 3);
    appendPoints(batch.positions, pts.data(), pts.size());
    const std::size_t count = s.strandCount();
    // Each strand's segments at an offset of its own (the segments before it), written in parallel.
    std::vector<std::size_t> offsets(count + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = s.strandPointCount(i);
        offsets[i + 1] = offsets[i] + (n < 2 ? 0 : 2 * (n - 1));
    }
    const std::size_t at = batch.indices.size();
    batch.indices.resize(at + offsets[count]);
    std::uint32_t* out = batch.indices.data() + at;
    parallelFor(0, count, [&](std::size_t i) {
        writeSegments(out + offsets[i], base + static_cast<std::uint32_t>(s.strandBegin(i)), s.strandPointCount(i));
    }, kStrandGrain);
}

namespace {

// Strand i's shading frames (lit lines) into the streams given (null: not written), its first
// point at vertex `at`.
void writeLineFrames(const StrandSet& s, const ScalpMesh* scalp, const VaneNormals& vanes, std::size_t i, std::size_t at,
                     float* normals, float* tangents) {
    const std::size_t n = s.strandPointCount(i);
    const Vec3f* p = s.strandPoints(i);
    const Vec3f up = rootNormal(s, scalp, i, vanes);
    for (std::size_t k = 0; k < n; ++k) {
        Vec3f t, nrm, side;
        if (n > 1) {
            strandFrame(p, n, k, up, t, nrm, side);
        } else {
            t = up, nrm = anyPerpendicular(up).normalized();
        }
        const std::size_t v = (at + k) * 3;
        if (normals) put3(normals + v, nrm);
        if (tangents) put3(tangents + v, t);
    }
}

// Strand i's u (root to tip), `floats` a vertex, its first point at vertex `at`: with two, u
// plus twice the strand's number (lineIdOf) and the point's width second (`widths`, the set's per
// point; null: kNoLineWidth), any more 0.5.
void writeLineUvs(const StrandSet& s, const float* widths, std::size_t i, std::size_t at, float* uvs, std::size_t floats) {
    const std::size_t n = s.strandPointCount(i), first = s.strandBegin(i);
    const float number = floats > 1 ? 2.0f * static_cast<float>(lineIdOf(s.ids()[i])) : 0.0f;
    float* out = uvs + at * floats;
    for (std::size_t k = 0; k < n; ++k, out += floats) {
        out[0] = (n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 0.0f) + number;
        if (floats > 1) out[1] = widths ? widths[first + k] : kNoLineWidth;
        for (std::size_t f = 2; f < floats; ++f) out[f] = 0.5f;
    }
}

// The set's widths when it carries one a point (else null: none).
const float* pointWidths(const StrandSet& s) {
    return s.widths().size() == s.pointCount() && s.pointCount() > 0 ? s.widths().data() : nullptr;
}

// Points per parallel task when copying a whole stream.
constexpr std::size_t kCopyGrain = 1 << 16;

}  // namespace

void addLineFrames(DisplayGeometry& batch, const StrandSet& s, const ScalpMesh* scalp) {
    const std::size_t first = batch.uvs.size();
    resizeFrames(batch, first + s.pointCount());
    const VaneNormals vanes(s);
    parallelFor(0, s.strandCount(), [&](std::size_t i) {
        const std::size_t at = first + s.strandBegin(i);
        writeLineFrames(s, scalp, vanes, i, at, batch.normals.data(), batch.tangents.data());
        writeLineUvs(s, nullptr, i, at, batch.uvs.data(), 1);
    }, kStrandGrain);
}

StrandLines::StrandLines(StrandsPtr strandSet, MeshPtr scalpMesh, bool litLines, std::shared_ptr<const StrandColors> pointColors)
    : strands(std::move(strandSet)), scalp(std::move(scalpMesh)), lit(litLines), colors(std::move(pointColors)) {
    if (!strands) return;
    // The segment count and the layout's hash in one pass over the offsets, in chunks side by
    // side, the chunks' digests combined in index order (the same key for the same counts).
    const std::vector<std::size_t>& offsets = strands->offsets();
    const std::size_t count = strands->strandCount();
    constexpr std::size_t kChunk = std::size_t{1} << 16;
    const std::size_t chunks = (offsets.size() + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> digests(chunks);
    std::vector<std::size_t> segments(chunks, 0);
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk, end = std::min(offsets.size(), begin + kChunk);
        digests[c] = bufferDigest(offsets.data() + begin, (end - begin) * sizeof(std::size_t));
        std::size_t sum = 0;
        for (std::size_t i = begin; i < std::min(end, count); ++i) {
            const std::size_t n = offsets[i + 1] - offsets[i];
            sum += n < 2 ? 0 : n - 1;
        }
        segments[c] = sum;
    }, 1);
    std::size_t total = 0;
    for (const std::size_t s : segments) total += s;
    indexCount_ = total * 2;
    layoutKey_ = hashCombine(bufferDigest(digests.data(), digests.size() * sizeof(std::uint64_t)), strands->pointCount());
    // The IDs' hash the same way (chunks side by side, combined in order): the uvs carry each
    // strand's number (lineIdOf).
    const std::vector<StrandId>& ids = strands->ids();
    const std::size_t idChunks = (ids.size() + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> idParts(idChunks);
    parallelFor(0, idChunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk, end = std::min(ids.size(), begin + kChunk);
        idParts[c] = bufferDigest(ids.data() + begin, (end - begin) * sizeof(StrandId));
    }, 1);
    const std::uint64_t idsDigest = bufferDigest(idParts.data(), idParts.size() * sizeof(std::uint64_t));
    idsKey_ = hashCombine(layoutKey_, idsDigest) | 1u;
    // The widths' hash, with the IDs'; never 0 with widths.
    if (const float* widths = pointWidths(*strands)) {
        const std::size_t points = strands->pointCount(), widthChunks = (points + kChunk - 1) / kChunk;
        std::vector<std::uint64_t> parts(widthChunks);
        parallelFor(0, widthChunks, [&](std::size_t c) {
            const std::size_t begin = c * kChunk, end = std::min(points, begin + kChunk);
            parts[c] = bufferDigest(widths + begin, (end - begin) * sizeof(float));
        }, 1);
        widthsKey_ = hashCombine(hashCombine(bufferDigest(parts.data(), parts.size() * sizeof(std::uint64_t)), points), idsDigest) | 1u;
    }
}

void StrandLines::writePositions(float* xyz) const {
    const std::size_t n = vertexCount();
    if (n == 0) return;
    const Vec3f* points = strands->positions().data();
    const std::size_t chunks = (n + kCopyGrain - 1) / kCopyGrain;
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kCopyGrain, count = std::min(kCopyGrain, n - begin);
        std::memcpy(xyz + begin * 3, points + begin, count * sizeof(Vec3f));
    }, 1);
}

void StrandLines::writePositions(float* xyz, std::size_t first, std::size_t end) const {
    if (!strands || first >= end) return;
    const std::size_t begin = strands->strandBegin(first), n = strands->offsets()[end] - begin;
    const Vec3f* points = strands->positions().data() + begin;
    if (n <= kCopyGrain) {
        std::memcpy(xyz, points, n * sizeof(Vec3f));
        return;
    }
    parallelFor(0, (n + kCopyGrain - 1) / kCopyGrain, [&](std::size_t c) {
        const std::size_t at = c * kCopyGrain, count = std::min(kCopyGrain, n - at);
        std::memcpy(xyz + at * 3, points + at, count * sizeof(Vec3f));
    }, 1);
}

void StrandLines::writeFrames(float* normals, float* tangents, std::size_t first, std::size_t end) const {
    if (!strands || first >= end) return;
    const StrandSet& s = *strands;
    const std::size_t base = s.strandBegin(first);
    const VaneNormals vanes(s);
    parallelFor(first, end, [&](std::size_t i) {
        writeLineFrames(s, scalp.get(), vanes, i, s.strandBegin(i) - base, normals, tangents);
    }, kStrandGrain);
}

std::uint64_t StrandLines::writeRootNormals(float* xyzw) const {
    if (!strands) return 1;
    const StrandSet& s = *strands;
    const std::size_t count = s.strandCount();
    const VaneNormals vanes(s);
    // In chunks side by side, each hashed as written, the hashes combined in order.
    constexpr std::size_t kChunk = std::size_t{1} << 14;
    const std::size_t chunks = (count + kChunk - 1) / kChunk;
    std::vector<std::uint64_t> digests(chunks);
    parallelFor(0, chunks, [&](std::size_t c) {
        const std::size_t begin = c * kChunk, end = std::min(count, begin + kChunk);
        for (std::size_t i = begin; i < end; ++i) {
            const Vec3f up = rootNormal(s, scalp.get(), i, vanes);
            float* out = xyzw + i * 4;
            out[0] = up.x;
            out[1] = up.y;
            out[2] = up.z;
            out[3] = 0.0f;
        }
        digests[c] = bufferDigest(xyzw + begin * 4, (end - begin) * 4 * sizeof(float));
    }, 1);
    return hashCombine(bufferDigest(digests.data(), digests.size() * sizeof(std::uint64_t)), count) | 1u;
}

std::vector<std::pair<std::size_t, std::size_t>> strandRuns(const std::vector<std::uint32_t>& changed, std::size_t gap) {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    for (const std::uint32_t s : changed) {
        if (!runs.empty() && s < runs.back().second + gap) {
            runs.back().second = static_cast<std::size_t>(s) + 1;
        } else {
            runs.emplace_back(s, static_cast<std::size_t>(s) + 1);
        }
    }
    return runs;
}

void StrandLines::writeIndices(std::uint32_t* pairs) const {
    if (!strands) return;
    const StrandSet& s = *strands;
    const std::size_t count = s.strandCount();
    // Each strand's segments at an offset of its own (the segments before it), written in parallel.
    std::vector<std::size_t> offsets(count + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = s.strandPointCount(i);
        offsets[i + 1] = offsets[i] + (n < 2 ? 0 : 2 * (n - 1));
    }
    parallelFor(0, count, [&](std::size_t i) {
        writeSegments(pairs + offsets[i], static_cast<std::uint32_t>(s.strandBegin(i)), s.strandPointCount(i));
    }, kStrandGrain);
}

void StrandLines::writeFrames(float* normals, float* tangents) const {
    if (!strands) return;
    const StrandSet& s = *strands;
    const VaneNormals vanes(s);
    parallelFor(0, s.strandCount(), [&](std::size_t i) {
        writeLineFrames(s, scalp.get(), vanes, i, s.strandBegin(i), normals, tangents);
    }, kStrandGrain);
}

void StrandLines::writeUvs(float* uvs, std::size_t floatsPerVertex) const {
    if (!strands) return;
    writeUvs(uvs, floatsPerVertex, 0, strands->strandCount());
}

void StrandLines::writeUvs(float* uvs, std::size_t floatsPerVertex, std::size_t first, std::size_t end) const {
    if (!strands || first >= end) return;
    const StrandSet& s = *strands;
    const std::size_t floats = std::max<std::size_t>(1, floatsPerVertex), base = s.strandBegin(first);
    const float* widths = pointWidths(s);
    parallelFor(first, end, [&](std::size_t i) { writeLineUvs(s, widths, i, s.strandBegin(i) - base, uvs, floats); }, kStrandGrain);
}

float linePixelScale(float projection11, float viewportHeight) { return 0.5f * projection11 * viewportHeight; }

float lineCoverage(float width, float widthScale, float pixelScale, float clipW, float floor) {
    if (width < 0.0f || clipW <= 0.0f) return 1.0f;
    return std::clamp(width * widthScale * pixelScale / clipW, floor, 1.0f);
}

float lineCoverageAtDensity(float coverage, float density) {
    if (!(density > 0.0f) || density >= 1.0f || coverage >= 1.0f) return coverage;
    return 1.0f - pm::pow(1.0f - coverage, 1.0f / density);
}

std::uint32_t lineIdOf(StrandId id) { return static_cast<std::uint32_t>(hash64(id) % kLineIdCount); }

std::array<std::uint32_t, 3> linePcg3d(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    z = z * 1664525u + 1013904223u;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16u;
    y ^= y >> 16u;
    z ^= z >> 16u;
    x += y * z;
    y += z * x;
    z += x * y;
    return {x, y, z};
}

namespace {
// One stretch's hash: strand, the stretch's index along u at 2^level stretches a unit of u, the level.
float lineStretchHash(std::uint32_t strand, float u, float level) {
    const float cell = std::floor(u * pm::exp2(level));
    return static_cast<float>(linePcg3d(strand, static_cast<std::uint32_t>(std::max(cell, 0.0f)),
                                        static_cast<std::uint32_t>(static_cast<int>(level) + 64))[0]) *
           (1.0f / 4294967296.0f);
}
}  // namespace

float lineHashThreshold(std::uint32_t strand, float u, float uPerPixel, int samples) {
    // Stretches a pixel long (about four with one sample a pixel): 1 / uPerPixel a unit of u,
    // between the powers of two below and above, their hashes blended by where between them it is.
    const float level = pm::log2(1.0f / std::max(uPerPixel, 1e-7f)) - (samples <= 1 ? 2.0f : 0.0f);
    const float lo = std::floor(level), f = level - lo;
    const float x = (1.0f - f) * lineStretchHash(strand, u, lo) + f * lineStretchHash(strand, u, lo + 1.0f);
    // The blend of two uniform numbers is not uniform: its distribution's inverse (Wyman and McGuire).
    const float a = std::min(f, 1.0f - f);
    float t = x;
    if (a > 1e-4f) {
        if (x < a) t = x * x / (2.0f * a * (1.0f - a));
        else if (x < 1.0f - a) t = (x - 0.5f * a) / (1.0f - a);
        else t = 1.0f - (1.0f - x) * (1.0f - x) / (2.0f * a * (1.0f - a));
    }
    return std::clamp(t, 1e-6f, 1.0f);
}

float lineKeepShare(float coverage, float own) {
    const float c = std::clamp(coverage, 0.0f, 1.0f);
    const float x = std::clamp((own - kLineHashedBelow) / (kLineBlendedAbove - kLineHashedBelow), 0.0f, 1.0f);
    return c + (1.0f - c) * x * x * (3.0f - 2.0f * x);
}

std::uint32_t lineSampleMask(float coverage, float threshold, std::uint32_t strand, int samples) {
    const int n = std::clamp(samples, 1, 32);
    const int k = static_cast<int>(std::clamp(std::floor(coverage * static_cast<float>(n) + 1.0f - threshold), 0.0f, static_cast<float>(n)));
    const std::uint32_t full = n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1u;
    if (k <= 0) return 0u;
    if (k >= n) return full;
    const std::uint32_t run = (1u << k) - 1u;
    const std::uint32_t r = linePcg3d(strand, 0x9E37u, 0x51u)[1] % static_cast<std::uint32_t>(n);
    return r == 0u ? run : ((run << r) | (run >> (static_cast<std::uint32_t>(n) - r))) & full;
}

Vec3f ribbonKeyLight() { return Vec3f{0.4f, 0.8f, 0.45f}.normalized(); }

DisplayGeometry strandRibbons(const StrandSet& s, const ScalpMesh* scalp, const std::vector<std::array<float, 3>>& colors,
                              float widthScale, bool lit, const StrandColors* litColors) {
    DisplayGeometry batch;
    batch.triangles = true;
    const Vec3f light = ribbonKeyLight();
    const std::size_t count = s.strandCount();
    // Strands of two points or more, each at offsets of its own (two vertices a point, six
    // indices a segment), written in parallel.
    std::vector<std::size_t> vertexAt(count + 1, 0), indexAt(count + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = s.strandPointCount(i);
        vertexAt[i + 1] = vertexAt[i] + (n < 2 ? 0 : 2 * n);
        indexAt[i + 1] = indexAt[i] + (n < 2 ? 0 : 6 * (n - 1));
    }
    const std::size_t vertices = vertexAt[count];
    batch.positions.resize(vertices * 3);
    batch.indices.resize(indexAt[count]);
    if (lit) resizeFrames(batch, vertices);
    if (!lit || litColors) batch.colors.resize(vertices * 4);
    const std::vector<float>& widths = s.widths();
    const VaneNormals vanes(s);
    parallelFor(0, count, [&](std::size_t i) {
        const std::size_t n = s.strandPointCount(i);
        if (n < 2) return;
        const Vec3f* p = s.strandPoints(i);
        const Vec3f up = rootNormal(s, scalp, i, vanes);
        const std::size_t first = vertexAt[i];
        for (std::size_t k = 0; k < n; ++k) {
            Vec3f tangent, normal, side;
            strandFrame(p, n, k, up, tangent, normal, side);
            const std::size_t w = s.strandBegin(i) + k;
            const float half = 0.5f * widthScale * (w < widths.size() ? widths[w] : kRibbonDefaultWidth);
            const std::size_t v = first + 2 * k;
            put3(batch.positions.data() + v * 3, p[k] - side * half);
            put3(batch.positions.data() + v * 3 + 3, p[k] + side * half);
            if (lit) {
                // u plus twice the strand's number (lineIdOf), as the lines' uvs: a host's shading
                // varies each strand by it (lineUvU takes u back).
                const float u = static_cast<float>(k) / static_cast<float>(n - 1) + 2.0f * static_cast<float>(lineIdOf(s.ids()[i]));
                for (std::size_t e = 0; e < 2; ++e) writeFrame(batch, v + e, tangent, normal, u);
                if (litColors) {  // the point's own color on both edges, the shading the host's
                    const std::array<float, 3> c = litColors->at(i, w);
                    const float a = litColors->alpha(i, w);
                    const float rgba[8] = {c[0], c[1], c[2], a, c[0], c[1], c[2], a};
                    std::memcpy(batch.colors.data() + v * 4, rgba, sizeof rgba);
                }
                continue;
            }
            const float shade = 0.35f + 0.65f * std::abs(side.cross(tangent).normalized().dot(light));
            const auto& c = colors[i];
            const float rgba[8] = {c[0] * shade, c[1] * shade, c[2] * shade, 1.0f, c[0] * shade, c[1] * shade, c[2] * shade, 1.0f};
            std::memcpy(batch.colors.data() + v * 4, rgba, sizeof rgba);
        }
        std::uint32_t* out = batch.indices.data() + indexAt[i];
        const auto base = static_cast<std::uint32_t>(first);
        for (std::uint32_t k = 0; k + 1 < n; ++k) {
            const std::uint32_t a = base + 2 * k, b = a + 1, c = a + 2, d = a + 3;
            const std::uint32_t quad[6] = {a, b, c, b, d, c};
            std::memcpy(out, quad, sizeof quad);
            out += 6;
        }
    }, kStrandGrain);
    return batch;
}

void appendRootDots(DisplayGeometry& batch, const StrandSet& strands, const ScalpMesh* scalp) {
    const std::size_t count = strands.strandCount();
    if (count == 0) return;
    const float area = scalp && scalp->totalArea() > 0.0f ? scalp->totalArea() : 1.0f;
    const float radius = 0.3f * std::sqrt(area / static_cast<float>(count));
    constexpr int kSides = 6;
    batch.triangles = true;
    const std::size_t firstVertex = batch.positions.size() / 3, firstIndex = batch.indices.size();
    batch.positions.resize((firstVertex + count * (kSides + 1)) * 3);
    batch.indices.resize(firstIndex + count * kSides * 3);
    parallelFor(0, count, [&](std::size_t s) {
        const Vec3f* pts = strands.strandPoints(s);
        const Vec3f root = pts[0];
        Vec3f up = scalp && scalp->contains(strands.roots()[s]) ? scalp->normal(strands.roots()[s])
                   : strands.strandPointCount(s) > 1         ? (pts[1] - pts[0]).normalized()
                                                             : Vec3f{0.0f, 1.0f, 0.0f};
        if (up.length() < 0.5f) up = {0.0f, 1.0f, 0.0f};
        const Vec3f side = (std::abs(up.x) < 0.9f ? Vec3f{1.0f, 0.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f}).cross(up).normalized();
        const Vec3f other = up.cross(side);
        const Vec3f center = root + up * (0.2f * radius);  // just above the surface, clear of z-fighting
        const std::size_t v = firstVertex + s * (kSides + 1);
        const auto base = static_cast<std::uint32_t>(v);
        float* pos = batch.positions.data() + v * 3;
        std::uint32_t* idx = batch.indices.data() + firstIndex + s * kSides * 3;
        put3(pos, center);
        for (int k = 0; k < kSides; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / kSides;
            const Vec3f p = center + (side * pm::cos(a) + other * pm::sin(a)) * radius;
            put3(pos + 3 + 3 * k, p);
            idx[3 * k] = base;
            idx[3 * k + 1] = base + 1 + static_cast<std::uint32_t>(k);
            idx[3 * k + 2] = base + 1 + static_cast<std::uint32_t>((k + 1) % kSides);
        }
    }, kStrandGrain);
}

float overlayPullDistance(const StrandSet* strands) {
    if (!strands) return 0.0f;
    // Each strand's length in parallel (summed in its own order), then the longest.
    const std::size_t count = strands->strandCount();
    std::vector<float> lengths(count);
    parallelFor(0, count, [&](std::size_t i) {
        const Vec3f* p = strands->strandPoints(i);
        const std::size_t n = strands->strandPointCount(i);
        float length = 0.0f;
        for (std::size_t k = 1; k < n; ++k) length += (p[k] - p[k - 1]).length();
        lengths[i] = length;
    }, kStrandGrain);
    float longest = 0.0f;
    for (const float length : lengths) longest = std::max(longest, length);
    return longest * 1.05f;
}

// ---- Guides -----------------------------------------------------------------------------------

std::size_t appendGuideCurve(DisplayGeometry& batch, const std::vector<Vec3f>& pts, int subdivisions) {
    if (subdivisions < 2 || pts.size() < 3) {
        appendPolyline(batch, pts.data(), pts.size());
        return pts.size();
    }
    const std::vector<Vec3f> smooth = smoothPolyline(pts.data(), pts.size(), subdivisions);
    appendPolyline(batch, smooth.data(), smooth.size());
    return smooth.size();
}

std::vector<const HierarchyGuide*> activeTubeGuides(const GuideHierarchy& hierarchy, const std::string& hiddenGuideGroups,
                                                    const std::string& freeze) {
    const std::set<std::string> hidden = parseHiddenGuideGroups(hiddenGuideGroups);
    const bool layers = hierarchy.hasAttributes();
    std::vector<const HierarchyGuide*> out;
    for (const HierarchyGuide& g : hierarchy.guides()) {
        if (g.points.size() < 2) continue;
        if (!hidden.empty()) {
            const GuideGroup* grp = hierarchy.group(g.group);
            if (grp && hidden.count(grp->name)) continue;
        }
        float frozen = 0.0f;
        if (layers && hierarchy.effectiveAttribute(g, freeze, &frozen) && frozen >= 0.5f) continue;
        out.push_back(&g);
    }
    return out;
}

void appendTubeWire(std::vector<Vec3f>& out, const HierarchyGuide& g, float scale, std::size_t lengthwise, std::size_t ringStep) {
    const std::size_t n = g.points.size();
    if (n < 2) return;
    constexpr std::size_t kAround = 32;  // outline points per ring (of the shape's 96)
    const std::vector<SectionPoint>& outline = tubeOutline(g.tube);
    // Evenly round the outline, each corner (tubeOutlineCorners) in place of the ring point
    // nearest it, so the rings keep a blade's points and a ribbon's corners.
    std::array<std::size_t, kAround> index;
    for (std::size_t s = 0; s < kAround; ++s) index[s] = s * outline.size() / kAround;
    const std::vector<std::size_t>& corners = tubeOutlineCorners(g.tube);
    std::vector<std::size_t> cornerSlots;  // the ring positions holding a corner
    for (const std::size_t c : corners) {
        const std::size_t slot = ((c * kAround + outline.size() / 2) / outline.size()) % kAround;
        index[slot] = c;
        if (std::find(cornerSlots.begin(), cornerSlots.end(), slot) == cornerSlots.end()) cornerSlots.push_back(slot);
    }
    std::array<SectionPoint, kAround> ring;
    for (std::size_t s = 0; s < kAround; ++s) ring[s] = outline[index[s]];
    const TubeSpine spine(g.points.data(), n, g.frameUp, g.tube, scale);
    const std::size_t step = std::max<std::size_t>(ringStep, 1);
    std::array<Vec3f, kAround> at;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % step != 0 && i + 1 != n) continue;
        for (std::size_t s = 0; s < kAround; ++s) at[s] = spine.atSample(i, ring[s].u, ring[s].v);
        for (std::size_t s = 0; s < kAround; ++s) {
            out.push_back(at[s]);
            out.push_back(at[(s + 1) % kAround]);
        }
    }
    // The lines along the tube: through the corners first, then each through the ring point
    // farthest round the ring from those taken (the first of equals), so they fill in evenly.
    const std::size_t lines = std::min(lengthwise, kAround);
    std::vector<std::size_t> along;
    for (std::size_t k = 0; k < cornerSlots.size() && along.size() < lines; ++k) along.push_back(cornerSlots[k]);
    if (along.empty() && lines > 0) along.push_back(0);
    while (along.size() < lines) {
        std::size_t best = 0, bestGap = 0;
        for (std::size_t s = 0; s < kAround; ++s) {
            std::size_t gap = kAround;
            for (const std::size_t a : along) {
                const std::size_t d = s > a ? s - a : a - s;
                gap = std::min(gap, std::min(d, kAround - d));
            }
            if (gap > bestGap) best = s, bestGap = gap;
        }
        if (bestGap == 0) break;
        along.push_back(best);
    }
    for (const std::size_t slot : along) {
        const SectionPoint& p = ring[slot];
        for (std::size_t i = 0; i + 1 < n; ++i) {
            out.push_back(spine.atSample(i, p.u, p.v));
            out.push_back(spine.atSample(i + 1, p.u, p.v));
        }
    }
}

void appendTubeSurface(DisplayGeometry& batch, const HierarchyGuide& g, float scale) {
    const std::size_t n = g.points.size();
    if (n < 2) return;
    constexpr int kAlong = 24;
    const auto base = static_cast<std::uint32_t>(batch.positions.size() / 3);
    const std::vector<SectionPoint>& outline = tubeOutline(g.tube);
    // A plain round tube: 16 sides round each ring, in the nearest sample's frame.
    const bool round = !g.tube.shaped();
    const int kAround = round ? 16 : static_cast<int>(outline.size() / 2);
    if (round) {
        const std::vector<Frame> frames = rotationMinimizingFrames(g.points.data(), n, g.frameUp);
        for (int j = 0; j <= kAlong; ++j) {
            const float t = static_cast<float>(j) / kAlong;
            const float f = t * static_cast<float>(n - 1);
            const std::size_t i = std::min(static_cast<std::size_t>(f), n - 2);
            const float w = f - static_cast<float>(i);
            const Vec3f center = g.points[i] * (1.0f - w) + g.points[i + 1] * w;
            const Frame& fr = frames[w < 0.5f ? i : i + 1];
            const float r = g.tube.widthAt(t) * scale;  // the size the hierarchy shows (GuideHierarchy::scale)
            for (int s = 0; s < kAround; ++s) {
                const float a = 6.2831853f * static_cast<float>(s) / kAround;
                const Vec3f out = fr.toWorld({pm::cos(a), pm::sin(a), 0.0f});
                const Vec3f p = center + out * r;
                batch.positions.insert(batch.positions.end(), {p.x, p.y, p.z});
                batch.normals.insert(batch.normals.end(), {out.x, out.y, out.z});
            }
        }
    }
    // Any other: the tube's sections (tube.h) along the guide, its shape's outline (every other
    // point) at each ring, turned, flattened and sized as the sections say.
    const TubeSpine spine = round ? TubeSpine() : TubeSpine(g.points.data(), n, g.frameUp, g.tube, scale);
    for (int j = 0; j <= kAlong && !round; ++j) {
        const float t = static_cast<float>(j) / kAlong;
        const Vec3f center = spine.at(t, 0.0f, 0.0f);
        const Vec3f axisU = spine.at(t, 1.0f, 0.0f) - center, axisV = spine.at(t, 0.0f, 1.0f) - center;
        const float uu = std::max(axisU.dot(axisU), 1e-20f), vv = std::max(axisV.dot(axisV), 1e-20f);
        for (int s = 0; s < kAround; ++s) {
            const SectionPoint& p = outline[static_cast<std::size_t>(2 * s)];
            const SectionPoint& next = outline[static_cast<std::size_t>((2 * s + 1) % outline.size())];
            const SectionPoint& prev = outline[(static_cast<std::size_t>(2 * s) + outline.size() - 1) % outline.size()];
            // Out of the outline (counter-clockwise: its edge's right), through the section's axes.
            const float nu = next.v - prev.v, nv = prev.u - next.u;
            Vec3f out = axisU * (nu / uu) + axisV * (nv / vv);
            out = out.length() > 0.0f ? out.normalized() : axisU.normalized();
            const Vec3f at = center + axisU * p.u + axisV * p.v;
            batch.positions.insert(batch.positions.end(), {at.x, at.y, at.z});
            batch.normals.insert(batch.normals.end(), {out.x, out.y, out.z});
        }
    }
    for (int j = 0; j < kAlong; ++j) {
        for (int s = 0; s < kAround; ++s) {
            const std::uint32_t a = base + static_cast<std::uint32_t>(j * kAround + s);
            const std::uint32_t b = base + static_cast<std::uint32_t>(j * kAround + (s + 1) % kAround);
            const std::uint32_t c = a + kAround, d = b + kAround;
            batch.indices.insert(batch.indices.end(), {a, b, d, a, d, c});
        }
    }
    // A flat cap at the tip (a fan round the guide's tip).
    const Vec3f tip = g.points[n - 1];
    const Vec3f tipWay = round ? rotationMinimizingFrames(g.points.data(), n, g.frameUp)[n - 1].tangent
                               : (tip - g.points[n - 2]).normalized();
    const auto cap = static_cast<std::uint32_t>(batch.positions.size() / 3);
    batch.positions.insert(batch.positions.end(), {tip.x, tip.y, tip.z});
    batch.normals.insert(batch.normals.end(), {tipWay.x, tipWay.y, tipWay.z});
    for (int s = 0; s < kAround; ++s) {
        const std::uint32_t ring = base + static_cast<std::uint32_t>(kAlong * kAround);
        batch.indices.insert(batch.indices.end(),
                             {cap, ring + static_cast<std::uint32_t>(s), ring + static_cast<std::uint32_t>((s + 1) % kAround)});
    }
}

// ---- Instances --------------------------------------------------------------------------------

void appendInstances(DisplayGeometry& markers, DisplayGeometry& meshes, const std::vector<InstanceSet::Placed>& placed,
                     const std::vector<const InstancePrototype*>& prototypes) {
    meshes.triangles = true;
    meshes.surface = true;
    const std::size_t count = std::min(placed.size(), kMaxDrawnInstances);
    // Which draw as their meshes (in order, while the triangle budget lasts; the rest as markers),
    // and where each one's vertices and indices go: then every instance written in parallel, in
    // the order it would be appended.
    constexpr std::size_t kMarkerPoints = 6, kMarkerIndices = 24;
    std::vector<const InstancePrototype*> meshOf(count, nullptr);
    std::vector<std::size_t> vertexAt(count + 1, 0), indexAt(count + 1, 0), markerAt(count + 1, 0);
    std::size_t trianglesDrawn = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const InstanceSet::Placed& p = placed[i];
        const InstancePrototype* mesh = p.prototype < prototypes.size() ? prototypes[p.prototype] : nullptr;
        const std::size_t triangles = mesh ? mesh->triangles.size() / 3 : 0;
        const bool meshed = mesh && triangles > 0 && trianglesDrawn + triangles <= kInstanceTriangleBudget;
        if (meshed) {
            trianglesDrawn += triangles;
            meshOf[i] = mesh;
        }
        vertexAt[i + 1] = vertexAt[i] + (meshed ? mesh->points.size() : 0);
        indexAt[i + 1] = indexAt[i] + (meshed ? mesh->triangles.size() : 0);
        markerAt[i + 1] = markerAt[i] + (meshed ? 0 : 1);
    }
    const std::size_t meshVertexBase = meshes.positions.size() / 3, meshIndexBase = meshes.indices.size();
    const std::size_t markerVertexBase = markers.positions.size() / 3, markerIndexBase = markers.indices.size();
    meshes.positions.resize((meshVertexBase + vertexAt[count]) * 3);
    meshes.normals.resize(meshes.positions.size());
    meshes.indices.resize(meshIndexBase + indexAt[count]);
    markers.positions.resize((markerVertexBase + markerAt[count] * kMarkerPoints) * 3);
    markers.indices.resize(markerIndexBase + markerAt[count] * kMarkerIndices);
    parallelFor(0, count, [&](std::size_t i) {
        const InstanceSet::Placed& p = placed[i];
        if (const InstancePrototype* mesh = meshOf[i]) {
            const std::size_t first = meshVertexBase + vertexAt[i];
            const auto base = static_cast<std::uint32_t>(first);
            // Normals through the inverse transpose: the axes are at right angles, each scaled on
            // its own (a strand-fitted instance is longer along +Y), so each over its squared length.
            auto inverseSquared = [](const Vec3f& a) {
                const float l2 = a.dot(a);
                return l2 > 1e-30f ? a / l2 : a;
            };
            const Vec3f nx = inverseSquared(p.x), ny = inverseSquared(p.y), nz = inverseSquared(p.z);
            float* positions = meshes.positions.data() + first * 3;
            float* normals = meshes.normals.data() + first * 3;
            for (std::size_t v = 0; v < mesh->points.size(); ++v) {
                const Vec3f& q = mesh->points[v];
                const Vec3f w = p.origin + p.x * q.x + p.y * q.y + p.z * q.z;
                const Vec3f& m = mesh->normals[v];
                Vec3f n = nx * m.x + ny * m.y + nz * m.z;
                n = n.length() > 1e-12f ? n.normalized() : Vec3f{0.0f, 1.0f, 0.0f};
                positions[v * 3] = w.x, positions[v * 3 + 1] = w.y, positions[v * 3 + 2] = w.z;
                normals[v * 3] = n.x, normals[v * 3 + 1] = n.y, normals[v * 3 + 2] = n.z;
            }
            std::uint32_t* indices = meshes.indices.data() + meshIndexBase + indexAt[i];
            for (std::size_t k = 0; k < mesh->triangles.size(); ++k) indices[k] = base + mesh->triangles[k];
            return;
        }
        const Vec3f tips[kMarkerPoints] = {p.origin + p.x * 0.5f, p.origin - p.x * 0.5f, p.origin + p.y,
                                           p.origin,            p.origin + p.z * 0.5f, p.origin - p.z * 0.5f};
        const std::size_t first = markerVertexBase + markerAt[i] * kMarkerPoints;
        const auto base = static_cast<std::uint32_t>(first);
        float* positions = markers.positions.data() + first * 3;
        for (std::size_t v = 0; v < kMarkerPoints; ++v) {
            positions[v * 3] = tips[v].x, positions[v * 3 + 1] = tips[v].y, positions[v * 3 + 2] = tips[v].z;
        }
        std::uint32_t* indices = markers.indices.data() + markerIndexBase + markerAt[i] * kMarkerIndices;
        // Edges from the top (2) and bottom (3) to the four around, then the ring.
        for (const std::uint32_t pole : {2u, 3u}) {
            for (const std::uint32_t ring : {0u, 4u, 1u, 5u}) *indices++ = base + pole, *indices++ = base + ring;
        }
        const std::uint32_t ringOrder[5] = {0u, 4u, 1u, 5u, 0u};
        for (int k = 0; k < 4; ++k) *indices++ = base + ringOrder[k], *indices++ = base + ringOrder[k + 1];
    }, 64);
}

DisplayGeometry scaleGeometry(const ScaleMesh& scales) {
    DisplayGeometry g;
    g.triangles = true;
    g.scaleSurface = true;
    const std::size_t n = scales.vertexCount();
    g.positions.resize(n * 3);
    g.normals.resize(n * 3);
    g.tangents.resize(n * 3);
    g.bitangents.resize(n * 3);
    g.uvs.resize(n * 2);
    g.colors.resize(n * 4);
    parallelFor(0, n, [&](std::size_t v) {
        const Vec3f& p = scales.points[v];
        const Vec3f& m = scales.normals[v];
        const Vec3f& t = scales.tangents[v];
        for (int c = 0; c < 3; ++c) {
            g.positions[v * 3 + static_cast<std::size_t>(c)] = (&p.x)[c];
            g.normals[v * 3 + static_cast<std::size_t>(c)] = (&m.x)[c];
            g.bitangents[v * 3 + static_cast<std::size_t>(c)] = (&m.x)[c];  // the shading normal (the cards' stream)
            g.tangents[v * 3 + static_cast<std::size_t>(c)] = (&t.x)[c];
            g.colors[v * 4 + static_cast<std::size_t>(c)] = scales.colors[v][static_cast<std::size_t>(c)];
        }
        g.colors[v * 4 + 3] = 1.0f;
        // x along the scale (the ramp's root to tip: a base a little darker, under the scale before it).
        g.uvs[v * 2] = scales.uvs[v][1];
        g.uvs[v * 2 + 1] = scales.uvs[v][0];
    }, 4096);
    g.indices = scales.triangles;
    return g;
}

DisplayGeometry strandMeshGeometry(const std::vector<StrandMeshSource>& meshes, const ScalpMesh* scalp,
                                   const std::array<float, 3>& color, const PrototypeLookup& prototypes) {
    DisplayGeometry g;
    g.triangles = true;
    g.scaleSurface = true;
    // A tube or ribbon mesh is laid out here from its strands; a scale or feather mesh (ToMesh's
    // other makes) is built whole and drawn as its faces are.
    std::vector<std::optional<StrandMesh>> built(meshes.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const StrandMeshSource& m = meshes[i];
        if (!m.strands) continue;
        if (resolveMeshMake(*m.strands, m.options) != StrandMeshOptions::Make::Strands) {
            built[i] = buildStrandMesh(*m.strands, scalp, m.options, prototypes);
            total += built[i]->mesh.points.size();
        } else {
            total += strandMeshPointCount(*m.strands, m.options);
        }
    }
    if (total == 0) return g;
    std::vector<Vec3f> points(total);
    g.positions.resize(total * 3);
    g.normals.resize(total * 3);
    g.tangents.resize(total * 3);
    g.bitangents.resize(total * 3);
    g.uvs.resize(total * 2);
    g.colors.resize(total * 4);
    std::size_t base = 0;
    for (std::size_t mi = 0; mi < meshes.size(); ++mi) {
        const StrandMeshSource& m = meshes[mi];
        if (!m.strands) continue;
        if (built[mi]) {
            // Its faces as they are: normals the faces' about each point, the one color.
            const RachisMesh& mesh = built[mi]->mesh;
            const std::size_t count = mesh.points.size();
            std::vector<Vec3f> normal(count);
            std::vector<float> along(count, 0.0f);  // v (root to tip) at each point: a corner's uv
            for (std::size_t c = 0; c < mesh.faceVertices.size() && c < mesh.faceUVs.size(); ++c) {
                const auto uv = static_cast<std::size_t>(mesh.faceUVs[c]);
                if (uv < mesh.v.size()) along[static_cast<std::size_t>(mesh.faceVertices[c])] = mesh.v[uv];
            }
            std::size_t corner = 0;
            for (std::size_t f = 0; f < mesh.faceCounts.size(); ++f) {
                const int n = mesh.faceCounts[f];
                if (n >= 3) {
                    const Vec3f& a = mesh.points[static_cast<std::size_t>(mesh.faceVertices[corner])];
                    const Vec3f& b = mesh.points[static_cast<std::size_t>(mesh.faceVertices[corner + 1])];
                    const Vec3f& c = mesh.points[static_cast<std::size_t>(mesh.faceVertices[corner + 2])];
                    const Vec3f face = (b - a).cross(c - a);
                    for (int k = 0; k < n; ++k) normal[static_cast<std::size_t>(mesh.faceVertices[corner + static_cast<std::size_t>(k)])] += face;
                    for (int k = 1; k + 1 < n; ++k) {
                        g.indices.insert(g.indices.end(), {static_cast<std::uint32_t>(base + static_cast<std::size_t>(mesh.faceVertices[corner])),
                                                           static_cast<std::uint32_t>(base + static_cast<std::size_t>(mesh.faceVertices[corner + static_cast<std::size_t>(k)])),
                                                           static_cast<std::uint32_t>(base + static_cast<std::size_t>(mesh.faceVertices[corner + static_cast<std::size_t>(k) + 1]))});
                    }
                }
                corner += static_cast<std::size_t>(std::max(n, 0));
            }
            for (std::size_t v = 0; v < count; ++v) {
                const Vec3f nv = normal[v].length() > 1e-20f ? normal[v].normalized() : Vec3f(0.0f, 1.0f, 0.0f);
                const Vec3f tv = anyPerpendicular(nv);
                const std::size_t at = base + v;
                for (int a = 0; a < 3; ++a) {
                    const auto i = at * 3 + static_cast<std::size_t>(a);
                    g.positions[i] = (&mesh.points[v].x)[a];
                    g.normals[i] = g.bitangents[i] = (&nv.x)[a];
                    g.tangents[i] = (&tv.x)[a];
                    g.colors[at * 4 + static_cast<std::size_t>(a)] = color[static_cast<std::size_t>(a)];
                }
                g.colors[at * 4 + 3] = 1.0f;
                g.uvs[at * 2] = along[v];
                g.uvs[at * 2 + 1] = 0.0f;
            }
            base += count;
            continue;
        }
        const StrandSet& s = *m.strands;
        const std::size_t count = strandMeshPointCount(s, m.options);
        strandMeshPoints(s, scalp, m.options, points.data() + base);
        const bool ribbon = m.options.style == StrandMeshOptions::Style::Ribbon;
        const std::size_t sides = ribbon ? 2 : static_cast<std::size_t>(std::max(3, m.options.sides));
        const std::size_t n = s.strandCount();
        std::vector<std::size_t> first(n + 1, base);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = s.strandPointCount(k);
            first[k + 1] = first[k] + (c < 2 ? 0 : c * sides + (ribbon ? 0 : 1));
        }
        // HairColor's linear RGB, per point or per strand; else the one color.
        std::array<const std::vector<float>*, 3> perPoint{}, perStrand{};
        bool pointColors = true, strandColors = true;
        for (int c = 0; c < 3; ++c) {
            perPoint[c] = s.pointChannel(kColorChannels[c]);
            perStrand[c] = s.strandChannel(kColorChannels[c]);
            pointColors = pointColors && perPoint[c] && perPoint[c]->size() == s.pointCount();
            strandColors = strandColors && perStrand[c] && perStrand[c]->size() == n;
        }
        parallelFor(0, n, [&](std::size_t k) {
            const std::size_t c = s.strandPointCount(k);
            if (c < 2) return;
            const Vec3f* p = s.strandPoints(k);
            const std::size_t begin = s.strandBegin(k);
            auto write = [&](std::size_t v, const Vec3f& normal, const Vec3f& tangent, std::size_t point, float along) {
                const Vec3f& q = points[v];
                std::array<float, 3> rgb = color;
                for (int ch = 0; ch < 3; ++ch) {
                    if (pointColors) rgb[ch] = (*perPoint[ch])[begin + point];
                    else if (strandColors) rgb[ch] = (*perStrand[ch])[k];
                }
                for (int a = 0; a < 3; ++a) {
                    const auto i = v * 3 + static_cast<std::size_t>(a);
                    g.positions[i] = (&q.x)[a];
                    g.normals[i] = g.bitangents[i] = (&normal.x)[a];  // the shading normal (the scales' stream)
                    g.tangents[i] = (&tangent.x)[a];
                    g.colors[v * 4 + static_cast<std::size_t>(a)] = rgb[static_cast<std::size_t>(a)];
                }
                g.colors[v * 4 + 3] = 1.0f;
                g.uvs[v * 2] = along;
                g.uvs[v * 2 + 1] = 0.0f;
            };
            Vec3f tangent;
            for (std::size_t i = 0; i < c; ++i) {
                tangent = (p[std::min(i + 1, c - 1)] - p[i > 0 ? i - 1 : 0]).normalized();
                const float along = static_cast<float>(i) / static_cast<float>(c - 1);
                const std::size_t ring = first[k] + i * sides;
                if (ribbon) {
                    const Vec3f across = points[ring] - points[ring + 1];
                    Vec3f normal = tangent.cross(across);
                    normal = normal.length() > 1e-12f ? normal.normalized() : anyPerpendicular(tangent);
                    write(ring, normal, tangent, i, along);
                    write(ring + 1, normal, tangent, i, along);
                } else {
                    for (std::size_t j = 0; j < sides; ++j) {
                        Vec3f out = points[ring + j] - p[i];
                        out = out.length() > 1e-12f ? out.normalized() : anyPerpendicular(tangent);
                        write(ring + j, out, tangent, i, along);
                    }
                }
            }
            if (!ribbon) write(first[k + 1] - 1, tangent, tangent, c - 1, 1.0f);  // the tip
        }, 64);
        // Triangles: two a quad between rings (outward, as buildRachisMesh's), a tube's tip a fan.
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = s.strandPointCount(k);
            if (c < 2) continue;
            const auto at = [&](std::size_t ring, std::size_t j) {
                return static_cast<std::uint32_t>(first[k] + ring * sides + (j % sides));
            };
            const std::size_t quads = ribbon ? 1 : sides;
            for (std::size_t r = 0; r + 1 < c; ++r) {
                for (std::size_t j = 0; j < quads; ++j) {
                    const std::uint32_t a = at(r, j), b = at(r + 1, j), d = at(r + 1, j + 1), e = at(r, j + 1);
                    g.indices.insert(g.indices.end(), {a, b, d, a, d, e});
                }
            }
            if (!ribbon) {
                const auto apex = static_cast<std::uint32_t>(first[k + 1] - 1);
                for (std::size_t j = 0; j < sides; ++j) g.indices.insert(g.indices.end(), {at(c - 1, j), apex, at(c - 1, j + 1)});
            }
        }
        base += count;
    }
    return g;
}

DisplayGeometry rachisGeometry(const StrandSet& strands, const std::array<float, 3>& color) {
    DisplayGeometry g;
    g.triangles = true;
    g.scaleSurface = true;
    if (!hasRachis(strands.featherLayers())) return g;
    const RachisMesh mesh = buildRachisMesh(strands.featherLayers());
    const std::size_t count = mesh.points.size();
    if (count == 0) return g;
    const std::vector<std::array<float, 3>> colors = rachisColors(strands, mesh, color);  // each feather's shaft's
    // Normals: the faces' about each point; tangents: the way v (root to tip) grows over them.
    std::vector<Vec3f> normal(count), tangent(count);
    std::vector<float> along(count, 0.0f);
    std::size_t corner = 0;
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
        const auto n = static_cast<std::size_t>(mesh.faceCounts[f]);
        const int* v = mesh.faceVertices.data() + corner;
        const int* uv = mesh.faceUVs.data() + corner;
        auto at = [&](std::size_t i) { return mesh.points[static_cast<std::size_t>(v[i % n])]; };
        auto vOf = [&](std::size_t i) { return mesh.v[static_cast<std::size_t>(uv[i % n])]; };
        Vec3f faceNormal;
        for (std::size_t i = 1; i + 1 < n; ++i) faceNormal += (at(i) - at(0)).cross(at(i + 1) - at(0));
        for (std::size_t i = 0; i < n; ++i) {
            const auto p = static_cast<std::size_t>(v[i]), q = static_cast<std::size_t>(v[(i + 1) % n]);
            const Vec3f edge = (at(i + 1) - at(i)) * (vOf(i + 1) - vOf(i));
            normal[p] += faceNormal;
            tangent[p] += edge;
            tangent[q] += edge;
            along[p] = vOf(i);
        }
        corner += n;
    }
    g.positions.resize(count * 3);
    g.normals.resize(count * 3);
    g.tangents.resize(count * 3);
    g.bitangents.resize(count * 3);
    g.uvs.resize(count * 2);
    g.colors.resize(count * 4);
    for (std::size_t p = 0; p < count; ++p) {
        const Vec3f nrm = normal[p].length() > 1e-12f ? normal[p].normalized() : Vec3f{0.0f, 1.0f, 0.0f};
        Vec3f tan = tangent[p] - nrm * tangent[p].dot(nrm);
        tan = tan.length() > 1e-12f ? tan.normalized() : anyPerpendicular(nrm);
        const std::array<float, 3>& rgb = colors[p];
        for (int a = 0; a < 3; ++a) {
            const auto i = p * 3 + static_cast<std::size_t>(a);
            g.positions[i] = (&mesh.points[p].x)[a];
            g.normals[i] = g.bitangents[i] = (&nrm.x)[a];  // the shading normal (the scales' stream)
            g.tangents[i] = (&tan.x)[a];
            g.colors[p * 4 + static_cast<std::size_t>(a)] = rgb[static_cast<std::size_t>(a)];
        }
        g.colors[p * 4 + 3] = 1.0f;
        g.uvs[p * 2] = along[p];
        g.uvs[p * 2 + 1] = 0.0f;
    }
    corner = 0;
    for (std::size_t f = 0; f < mesh.faceCount(); ++f) {  // each face a fan
        const auto n = static_cast<std::size_t>(mesh.faceCounts[f]);
        const int* v = mesh.faceVertices.data() + corner;
        for (std::size_t i = 1; i + 1 < n; ++i) {
            g.indices.insert(g.indices.end(), {static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[i]),
                                               static_cast<std::uint32_t>(v[i + 1])});
        }
        corner += n;
    }
    return g;
}

}  // namespace groomlab
