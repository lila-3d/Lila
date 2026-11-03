#include "bake/TexelRaster.h"

#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>

namespace lila::bake
{
namespace
{
using detail::edgeFunction;
using detail::mix3;

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t h = hash(a ^ hash(b ^ hash(c ^ hash(d))));
    return float(h >> 8) * (1.0f / 16777216.0f);
}

/** The cage direction n blended toward the triangle's geometric normal by skew. */
glm::vec3 skewed(const glm::vec3& n, const glm::vec3* tri, float skew)
{
    const glm::vec3 g = glm::cross(tri[1] - tri[0], tri[2] - tri[0]);
    const float gl = glm::length(g);
    if (gl <= 0.0f)
        return n;
    const glm::vec3 d = glm::mix(n, g / gl, skew);
    const float dl = glm::length(d);
    return dl > 1e-6f ? d / dl : n;
}
} // namespace

TexelRaster::TexelRaster(const LowInput& low, const RasterSettings& settings, int material) : mLow(low), mSettings(settings)
{
    LILA_ZONE();

    mGrid = std::max(1, int(std::sqrt(double(std::max(1, settings.supersampling))) + 1e-6));
    mSettings.bandRows = std::max(1, settings.bandRows);
    mBandCount = size_t((mSettings.height + mSettings.bandRows - 1) / mSettings.bandRows);
    const MeshSoup& s = low.soup;
    if (s.uvs.size() != size_t(s.triangleCount()) * 3)
    {
        mSkipped = s.triangleCount();
        mTriUdim.assign(s.triangleCount(), 0);
        return;
    }
    const double H = mSettings.height;
    mTriUdim.assign(s.triangleCount(), 0);
    for (uint32_t t = 0; t < s.triangleCount(); ++t)
    {
        if (material >= 0 && s.materials[t] != uint32_t(material))
            continue;
        const glm::vec2* uv = &s.uvs[size_t(t) * 3];
        const glm::vec2 c = (uv[0] + uv[1] + uv[2]) / 3.0f;
        const int tu = int(std::floor(c.x)), tv = int(std::floor(c.y));
        const double area = edgeFunction(glm::dvec2(uv[0]), glm::dvec2(uv[1]), glm::dvec2(uv[2]));
        if (tu < 0 || tu > 9 || tv < 0 || std::abs(area) < 1e-14)
        {
            ++mSkipped;
            continue;
        }
        const int udim = udimOf(tu, tv);
        mTriUdim[t] = udim;
        auto it = std::ranges::find_if(mTiles, [&](const Tile& tile) { return tile.udim == udim; });
        if (it == mTiles.end())
        {
            mTiles.push_back(Tile{udim, tu, tv, std::vector<std::vector<uint32_t>>(mBandCount)});
            it = mTiles.end() - 1;
        }
        double minY = H, maxY = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            const double y = (1.0 - (double(uv[k].y) - tv)) * H;
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        const int y0 = std::clamp(int(std::floor(minY)), 0, mSettings.height - 1);
        const int y1 = std::clamp(int(std::ceil(maxY)), 0, mSettings.height);
        for (int b = y0 / mSettings.bandRows; b <= std::min(int(mBandCount) - 1, std::max(y0, y1 - 1) / mSettings.bandRows); ++b)
            it->bands[size_t(b)].push_back(t);
    }
    std::ranges::sort(mTiles, {}, &Tile::udim);
    for (const Tile& t : mTiles)
        mUdims.push_back(t.udim);
}

const std::vector<uint32_t>& TexelRaster::bandTriangles(int udim, size_t band) const
{
    static const std::vector<uint32_t> none;
    auto it = std::ranges::find_if(mTiles, [&](const Tile& t) { return t.udim == udim; });
    return it == mTiles.end() || band >= mBandCount ? none : it->bands[band];
}

void TexelRaster::band(int udim, size_t band, const CageParams& cage, std::vector<TexelSample>& out) const
{
    LILA_ZONE();

    auto it = std::ranges::find_if(mTiles, [&](const Tile& t) { return t.udim == udim; });
    if (it == mTiles.end() || band >= mBandCount)
        return;
    const Tile& tile = *it;
    const int W = mSettings.width, H = mSettings.height, n = mGrid, S = n * n;
    const int y0 = int(band) * mSettings.bandRows, y1 = std::min(H, y0 + mSettings.bandRows);
    std::vector<uint8_t> claimed(size_t(y1 - y0) * size_t(W) * size_t(S), 0);
    const MeshSoup& s = mLow.soup;
    const bool explicitCage = mLow.cagePositions.size() == s.positions.size();

    for (uint32_t t : tile.bands[band])
    {
        const glm::vec2* uv = &s.uvs[size_t(t) * 3];
        glm::dvec2 p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = {(double(uv[k].x) - tile.u) * W, (1.0 - (double(uv[k].y) - tile.v)) * H};
        const double area = edgeFunction(p[0], p[1], p[2]);
        if (std::abs(area) < 1e-12)
            continue;
        const double minX = std::min({p[0].x, p[1].x, p[2].x}), maxX = std::max({p[0].x, p[1].x, p[2].x});
        const double minY = std::min({p[0].y, p[1].y, p[2].y}), maxY = std::max({p[0].y, p[1].y, p[2].y});
        const int xa = std::clamp(int(std::floor(minX)), 0, W), xb = std::clamp(int(std::ceil(maxX)), 0, W);
        const int ya = std::clamp(int(std::floor(minY)), y0, y1), yb = std::clamp(int(std::ceil(maxY)), y0, y1);
        for (int y = ya; y < yb; ++y)
            for (int x = xa; x < xb; ++x)
                for (int sub = 0; sub < S; ++sub)
                {
                    const size_t slot = (size_t(y - y0) * size_t(W) + size_t(x)) * size_t(S) + size_t(sub);
                    if (claimed[slot])
                        continue;
                    const int i = sub % n, j = sub / n;
                    const double jx = n == 1 ? 0.5 : hash01(uint32_t(x), uint32_t(y), uint32_t(sub * 2), uint32_t(udim));
                    const double jy = n == 1 ? 0.5 : hash01(uint32_t(x), uint32_t(y), uint32_t(sub * 2 + 1), uint32_t(udim));
                    const glm::dvec2 sp(x + (i + jx) / n, y + (j + jy) / n);
                    const glm::dvec3 w(edgeFunction(p[1], p[2], sp) / area, edgeFunction(p[2], p[0], sp) / area, edgeFunction(p[0], p[1], sp) / area);
                    if (w.x < 0.0 || w.y < 0.0 || w.z < 0.0)
                        continue;
                    claimed[slot] = 1;

                    const size_t c0 = size_t(t) * 3;
                    TexelSample smp{};
                    const glm::vec3 P = mix3(&s.positions[c0], w);
                    const glm::vec3 N = mix3(&s.normals[c0], w);
                    const glm::vec4 T = mix3(&mLow.tangents[c0], w);
                    const glm::vec2 U = mix3(&s.uvs[c0], w);
                    glm::vec3 origin, dir;
                    float length;
                    const glm::vec3 C = explicitCage ? mix3(&mLow.cagePositions[c0], w) : glm::vec3(0.0f);
                    const float toCage = explicitCage ? glm::length(P - C) : 0.0f;
                    if (explicitCage && toCage > 1e-6f)
                    {
                        origin = C;
                        dir = (P - C) / toCage;
                        length = toCage + cage.rear;
                    }
                    else
                    {
                        glm::vec3 cn = mix3(&mLow.cageNormals[c0], w);
                        const float l = glm::length(cn);
                        cn = l > 0.0f ? cn / l : glm::vec3(0.0f, 0.0f, 1.0f);
                        const float skew = std::clamp(cage.skew + (mLow.cageSkew.size() == s.positions.size() ? mix3(&mLow.cageSkew[c0], w) : 0.0f), 0.0f, 1.0f);
                        if (skew > 0.0f)
                            cn = skewed(cn, &s.positions[c0], skew);
                        const float push = cage.frontal * cage.offset * mix3(&mLow.cageScale[c0], w);
                        origin = P + cn * push;
                        dir = -cn;
                        length = push + cage.rear;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        smp.position[k] = P[k];
                        smp.normal[k] = N[k];
                        smp.tangent[k] = T[k];
                        smp.origin[k] = origin[k];
                        smp.direction[k] = dir[k];
                    }
                    smp.tangent[3] = mLow.tangents[c0].w < 0.0f ? -1.0f : 1.0f;
                    smp.rayLength = length;
                    smp.texel = uint32_t(y) * uint32_t(W) + uint32_t(x);
                    smp.lowTriangle = t;
                    smp.sub = uint32_t(sub);
                    smp.uv[0] = U.x;
                    smp.uv[1] = U.y;
                    smp.tile = uint32_t(udim);
                    out.push_back(smp);
                }
    }
}
} // namespace lila::bake
