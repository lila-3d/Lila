#include "gn/NodesCommon.h"
#include "gn/Texture.h"

#include <array>
#include <bit>
#include <cfloat>

namespace lila::gn
{
namespace tex
{
namespace
{
uint32_t rot(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

void finalMix(uint32_t& a, uint32_t& b, uint32_t& c)
{
    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
}

void mix(uint32_t& a, uint32_t& b, uint32_t& c)
{
    a -= c;
    a ^= rot(c, 4);
    c += b;
    b -= a;
    b ^= rot(a, 6);
    a += c;
    c -= b;
    c ^= rot(b, 8);
    b += a;
    a -= c;
    a ^= rot(c, 16);
    c += b;
    b -= a;
    b ^= rot(a, 19);
    a += c;
    c -= b;
    c ^= rot(b, 4);
    b += a;
}

uint32_t bits(double v) { return std::bit_cast<uint32_t>(float(v)); }
double unit(uint32_t h) { return double(float(h) / float(0xFFFFFFFFu)); }
} // namespace

// Jenkins lookup3 over float bits, the hashes Blender's procedural textures use, so cells jitter the same way.
uint32_t hashU(uint32_t kx)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (1 << 2) + 13;
    a += kx;
    finalMix(a, b, c);
    return c;
}

uint32_t hashU(uint32_t kx, uint32_t ky)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (2 << 2) + 13;
    b += ky;
    a += kx;
    finalMix(a, b, c);
    return c;
}

uint32_t hashU(uint32_t kx, uint32_t ky, uint32_t kz)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (3 << 2) + 13;
    c += kz;
    b += ky;
    a += kx;
    finalMix(a, b, c);
    return c;
}

uint32_t hashU(uint32_t kx, uint32_t ky, uint32_t kz, uint32_t kw)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (4 << 2) + 13;
    a += kx;
    b += ky;
    c += kz;
    mix(a, b, c);
    a += kw;
    finalMix(a, b, c);
    return c;
}

double hashToFloat(double x) { return unit(hashU(bits(x))); }
double hashToFloat(double x, double y) { return unit(hashU(bits(x), bits(y))); }
double hashToFloat(double x, double y, double z) { return unit(hashU(bits(x), bits(y), bits(z))); }
double hashToFloat(double x, double y, double z, double w) { return unit(hashU(bits(x), bits(y), bits(z), bits(w))); }

namespace
{
template <int N> using Vn = glm::vec<N, double>;

/** Two's-complement int arithmetic that wraps like Cycles' (overflow is defined on uint32_t). */
int32_t wrapMul(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }
int32_t wrapAdd(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }

/** Cycles' hash_pcgNd_i: Blender's Voronoi hashes integer cells this way from 2D up (1D hashes the float). */
template <int N> std::array<int32_t, N> pcg(std::array<int32_t, N> v)
{
    for (int32_t& x : v)
        x = wrapAdd(wrapMul(x, 1664525), 1013904223);
    auto round = [&] {
        if constexpr (N == 2)
        {
            v[0] = wrapAdd(v[0], wrapMul(v[1], 1664525));
            v[1] = wrapAdd(v[1], wrapMul(v[0], 1664525));
        }
        else if constexpr (N == 3)
        {
            v[0] = wrapAdd(v[0], wrapMul(v[1], v[2]));
            v[1] = wrapAdd(v[1], wrapMul(v[2], v[0]));
            v[2] = wrapAdd(v[2], wrapMul(v[0], v[1]));
        }
        else
        {
            v[0] = wrapAdd(v[0], wrapMul(v[1], v[3]));
            v[1] = wrapAdd(v[1], wrapMul(v[2], v[0]));
            v[2] = wrapAdd(v[2], wrapMul(v[0], v[1]));
            v[3] = wrapAdd(v[3], wrapMul(v[1], v[2]));
        }
    };
    round();
    for (int32_t& x : v)
        x ^= x >> 16;
    round();
    for (int32_t& x : v)
        x &= 0x7FFFFFFF;
    return v;
}

template <int M> glm::vec<M, double> pcgUnit(std::array<int32_t, M> k)
{
    std::array<int32_t, M> h = pcg<M>(k);
    glm::vec<M, double> out;
    for (int i = 0; i < M; ++i)
        out[i] = double(float(h[i]) * (1.0f / float(0x7FFFFFFFu)));
    return out;
}

template <int N> std::array<int32_t, N> cellInts(const Vn<N>& k)
{
    std::array<int32_t, N> c;
    for (int i = 0; i < N; ++i)
        c[i] = int32_t(k[i]);
    return c;
}

/** Cell jitter, as Blender's Voronoi computes it. */
template <int N> Vn<N> jitter(const Vn<N>& k)
{
    if constexpr (N == 1)
        return Vn<1>(hashToFloat(k.x));
    else
        return pcgUnit<N>(cellInts<N>(k));
}

/** Cell colour, as Blender's Voronoi computes it. */
template <int N> glm::dvec3 cellColor(const Vn<N>& k)
{
    if constexpr (N == 1)
        return {hashToFloat(k.x), hashToFloat(k.x, 1.0), hashToFloat(k.x, 2.0)};
    else if constexpr (N == 2)
        return pcgUnit<3>({int32_t(k.x), int32_t(k.y), 0});
    else if constexpr (N == 3)
        return pcgUnit<3>(cellInts<3>(k));
    else
        return glm::dvec3(pcgUnit<4>(cellInts<4>(k)));
}

template <int N> double distance(const Vn<N>& a, const Vn<N>& b, const VoronoiParams& p)
{
    Vn<N> d = glm::abs(a - b);
    if constexpr (N == 1)
        return d.x;
    else
    {
        switch (p.metric)
        {
        case VoronoiMetric::Manhattan:
        {
            double s = 0.0;
            for (int i = 0; i < N; ++i)
                s += d[i];
            return s;
        }
        case VoronoiMetric::Chebyshev:
        {
            double m = 0.0;
            for (int i = 0; i < N; ++i)
                m = std::max(m, d[i]);
            return m;
        }
        case VoronoiMetric::Minkowski:
        {
            double s = 0.0;
            for (int i = 0; i < N; ++i)
                s += std::pow(d[i], p.exponent);
            return std::pow(s, 1.0 / p.exponent);
        }
        default: return glm::length(a - b);
        }
    }
}

/** Offset k of the (2r+1)^N neighbourhood, x fastest, matching Blender's nested loops. */
template <int N> Vn<N> neighbour(int k, int r)
{
    Vn<N> o;
    int side = 2 * r + 1;
    for (int i = 0; i < N; ++i)
    {
        o[i] = double(k % side - r);
        k /= side;
    }
    return o;
}

constexpr int ipow(int b, int e) { return e == 0 ? 1 : b * ipow(b, e - 1); }

struct Octave
{
    double distance = 0.0;
    glm::dvec3 color{0.0};
    glm::dvec4 position{0.0};
};

template <int N> glm::dvec4 toPos(const Vn<N>& v)
{
    glm::dvec4 out(0.0);
    for (int i = 0; i < N; ++i)
        out[i] = v[i];
    return out;
}

template <int N> Octave f1(const VoronoiParams& p, const Vn<N>& coord)
{
    Vn<N> cell = glm::floor(coord), local = coord - cell;
    double best = DBL_MAX;
    Vn<N> bestOffset(0.0), bestPos(0.0);
    for (int k = 0; k < ipow(3, N); ++k)
    {
        Vn<N> off = neighbour<N>(k, 1);
        Vn<N> pt = off + jitter<N>(cell + off) * p.randomness;
        double d = distance<N>(pt, local, p);
        if (d < best)
        {
            best = d;
            bestOffset = off;
            bestPos = pt;
        }
    }
    return {best, cellColor<N>(cell + bestOffset), toPos<N>(bestPos + cell)};
}

double smoothstep01(double x)
{
    if (x < 0.0)
        return 0.0;
    if (x >= 1.0)
        return 1.0;
    return x * x * (3.0 - 2.0 * x);
}

template <int N> Octave smoothF1(const VoronoiParams& p, const Vn<N>& coord)
{
    Vn<N> cell = glm::floor(coord), local = coord - cell;
    double smoothness = std::max(p.smoothness, DBL_MIN);
    double dist = 0.0, h = -1.0;
    glm::dvec3 color(0.0);
    Vn<N> pos(0.0);
    for (int k = 0; k < ipow(5, N); ++k)
    {
        Vn<N> off = neighbour<N>(k, 2);
        Vn<N> pt = off + jitter<N>(cell + off) * p.randomness;
        double d = distance<N>(pt, local, p);
        h = h == -1.0 ? 1.0 : smoothstep01(0.5 + 0.5 * (dist - d) / smoothness);
        double correction = p.smoothness * h * (1.0 - h);
        dist = glm::mix(dist, d, h) - correction;
        correction /= 1.0 + 3.0 * p.smoothness;
        color = glm::mix(color, cellColor<N>(cell + off), h) - correction;
        pos = glm::mix(pos, pt, h) - correction;
    }
    return {dist, color, toPos<N>(cell + pos)};
}

template <int N> Octave f2(const VoronoiParams& p, const Vn<N>& coord)
{
    Vn<N> cell = glm::floor(coord), local = coord - cell;
    double d1 = DBL_MAX, d2 = DBL_MAX;
    Vn<N> off1(0.0), pos1(0.0), off2(0.0), pos2(0.0);
    for (int k = 0; k < ipow(3, N); ++k)
    {
        Vn<N> off = neighbour<N>(k, 1);
        Vn<N> pt = off + jitter<N>(cell + off) * p.randomness;
        double d = distance<N>(pt, local, p);
        if (d < d1)
        {
            d2 = d1;
            d1 = d;
            off2 = off1;
            off1 = off;
            pos2 = pos1;
            pos1 = pt;
        }
        else if (d < d2)
        {
            d2 = d;
            off2 = off;
            pos2 = pt;
        }
    }
    return {d2, cellColor<N>(cell + off2), toPos<N>(pos2 + cell)};
}

template <int N> double distanceToEdge(const VoronoiParams& p, const Vn<N>& coord)
{
    Vn<N> cell = glm::floor(coord), local = coord - cell;
    if constexpr (N == 1)
    {
        double mid = jitter<1>(cell).x * p.randomness;
        double left = -1.0 + jitter<1>(cell - Vn<1>(1.0)).x * p.randomness;
        double right = 1.0 + jitter<1>(cell + Vn<1>(1.0)).x * p.randomness;
        return std::min(std::abs((mid + left) / 2.0 - local.x), std::abs((mid + right) / 2.0 - local.x));
    }
    else
    {
        Vn<N> toClosest(0.0);
        double best = DBL_MAX;
        for (int k = 0; k < ipow(3, N); ++k)
        {
            Vn<N> off = neighbour<N>(k, 1);
            Vn<N> v = off + jitter<N>(cell + off) * p.randomness - local;
            double d = glm::dot(v, v);
            if (d < best)
            {
                best = d;
                toClosest = v;
            }
        }
        best = DBL_MAX;
        for (int k = 0; k < ipow(3, N); ++k)
        {
            Vn<N> off = neighbour<N>(k, 1);
            Vn<N> v = off + jitter<N>(cell + off) * p.randomness - local;
            Vn<N> perp = v - toClosest;
            if (glm::dot(perp, perp) > 0.0001)
                best = std::min(best, glm::dot((toClosest + v) / 2.0, glm::normalize(perp)));
        }
        return best;
    }
}

template <int N> double nSphereRadius(const VoronoiParams& p, const Vn<N>& coord)
{
    Vn<N> cell = glm::floor(coord), local = coord - cell;
    Vn<N> closest(0.0), closestOffset(0.0);
    double best = DBL_MAX;
    for (int k = 0; k < ipow(3, N); ++k)
    {
        Vn<N> off = neighbour<N>(k, 1);
        Vn<N> pt = off + jitter<N>(cell + off) * p.randomness;
        double d = glm::length(pt - local);
        if (d < best)
        {
            best = d;
            closest = pt;
            closestOffset = off;
        }
    }
    best = DBL_MAX;
    Vn<N> second(0.0);
    int centre = (ipow(3, N) - 1) / 2;
    for (int k = 0; k < ipow(3, N); ++k)
    {
        if (k == centre)
            continue;
        Vn<N> off = neighbour<N>(k, 1) + closestOffset;
        Vn<N> pt = off + jitter<N>(cell + off) * p.randomness;
        double d = glm::length(closest - pt);
        if (d < best)
        {
            best = d;
            second = pt;
        }
    }
    return glm::length(second - closest) / 2.0;
}

template <int N> double maxDistance(const VoronoiParams& p)
{
    if (p.feature == VoronoiFeature::DistanceToEdge)
        return 0.5 + 0.5 * p.randomness;
    double f = p.feature == VoronoiFeature::F2 ? 2.0 : 1.0;
    if constexpr (N == 1)
        return (0.5 + 0.5 * p.randomness) * f;
    else
    {
        return distance<N>(Vn<N>(0.0), Vn<N>(0.5 + 0.5 * p.randomness), p) * f;
    }
}

template <int N> VoronoiResult fractal(VoronoiParams p, const Vn<N>& coord)
{
    VoronoiResult r;
    double maxDist = maxDistance<N>(p);
    bool zeroInput = p.detail == 0.0 || p.roughness == 0.0;
    int last = int(std::ceil(p.detail));
    if (p.feature == VoronoiFeature::NSphereRadius)
    {
        r.radius = nSphereRadius<N>(p, coord);
        return r;
    }
    if (p.feature == VoronoiFeature::DistanceToEdge)
    {
        double amplitude = 1.0, maxAmp = maxDist, scale = 1.0, dist = 8.0;
        for (int i = 0; i <= last; ++i)
        {
            double od = distanceToEdge<N>(p, coord * scale);
            if (zeroInput)
            {
                dist = od;
                break;
            }
            if (double(i) <= p.detail)
            {
                maxAmp = glm::mix(maxAmp, maxDist / scale, amplitude);
                dist = glm::mix(dist, std::min(dist, od / scale), amplitude);
                scale *= p.lacunarity;
                amplitude *= p.roughness;
            }
            else if (double rest = p.detail - std::floor(p.detail); rest != 0.0)
            {
                double lerpAmp = glm::mix(maxAmp, maxDist / scale, amplitude);
                maxAmp = glm::mix(maxAmp, lerpAmp, rest);
                double lerpDist = glm::mix(dist, std::min(dist, od / scale), amplitude);
                dist = glm::mix(dist, std::min(dist, lerpDist), rest);
            }
        }
        r.distance = p.normalize ? dist / maxAmp : dist;
        return r;
    }

    double amplitude = 1.0, maxAmp = 0.0, scale = 1.0;
    Octave out;
    for (int i = 0; i <= last; ++i)
    {
        Octave o = p.feature == VoronoiFeature::F2 ? f2<N>(p, coord * scale)
                   : p.feature == VoronoiFeature::SmoothF1 && p.smoothness != 0.0 ? smoothF1<N>(p, coord * scale)
                                                                                    : f1<N>(p, coord * scale);
        if (zeroInput)
        {
            maxAmp = 1.0;
            out = o;
            break;
        }
        if (double(i) <= p.detail)
        {
            maxAmp += amplitude;
            out.distance += o.distance * amplitude;
            out.color += o.color * amplitude;
            out.position = glm::mix(out.position, o.position / scale, amplitude);
            scale *= p.lacunarity;
            amplitude *= p.roughness;
        }
        else if (double rest = p.detail - std::floor(p.detail); rest != 0.0)
        {
            maxAmp = glm::mix(maxAmp, maxAmp + amplitude, rest);
            out.distance = glm::mix(out.distance, out.distance + o.distance * amplitude, rest);
            out.color = glm::mix(out.color, out.color + o.color * amplitude, rest);
            out.position = glm::mix(out.position, glm::mix(out.position, o.position / scale, amplitude), rest);
        }
    }
    if (p.normalize)
    {
        out.distance /= maxAmp * maxDist;
        out.color /= maxAmp;
    }
    r.distance = out.distance;
    r.color = out.color;
    glm::dvec4 pos = p.scale != 0.0 ? out.position / p.scale : glm::dvec4(0.0);
    r.position = glm::dvec3(pos);
    r.w = N == 1 ? pos.x : pos.w;
    if constexpr (N == 1)
        r.position = glm::dvec3(0.0);
    return r;
}
} // namespace

double gradientNoise4(const glm::dvec4& p)
{
    glm::dvec4 i = glm::floor(p), f = p - i;
    auto fade = [](double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); };
    glm::dvec4 u(fade(f.x), fade(f.y), fade(f.z), fade(f.w));
    // 32 gradients: every axis pair-free direction with one zero component and +-1 elsewhere.
    auto grad = [](uint32_t h, const glm::dvec4& d) {
        h &= 31;
        int zero = int(h >> 3);
        double s0 = (h & 1) ? -1.0 : 1.0, s1 = (h & 2) ? -1.0 : 1.0, s2 = (h & 4) ? -1.0 : 1.0;
        double c[4];
        int k = 0;
        double signs[3] = {s0, s1, s2};
        for (int a = 0; a < 4; ++a)
            c[a] = a == zero ? 0.0 : signs[k++] * d[a];
        return c[0] + c[1] + c[2] + c[3];
    };
    double acc[16];
    for (int corner = 0; corner < 16; ++corner)
    {
        glm::dvec4 o(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1, (corner >> 3) & 1);
        uint32_t h = hashU(uint32_t(int64_t(i.x + o.x)), uint32_t(int64_t(i.y + o.y)), uint32_t(int64_t(i.z + o.z)), uint32_t(int64_t(i.w + o.w)));
        acc[corner] = grad(h, f - o);
    }
    for (int axis = 0, n = 16; axis < 4; ++axis, n /= 2)
        for (int k = 0; k < n / 2; ++k)
            acc[k] = acc[2 * k] + (acc[2 * k + 1] - acc[2 * k]) * u[axis];
    return acc[0] * 0.5; // largest gradient length is sqrt(3); keep roughly within -1..1
}

VoronoiResult voronoi(const VoronoiParams& given, const glm::dvec3& vector, double w)
{
    VoronoiParams p = given;
    p.detail = std::clamp(p.detail, 0.0, 15.0);
    p.roughness = std::clamp(p.roughness, 0.0, 1.0);
    p.randomness = std::clamp(p.randomness, 0.0, 1.0);
    p.smoothness = std::clamp(p.smoothness / 2.0, 0.0, 0.5);
    glm::dvec3 v = vector * p.scale;
    double sw = w * p.scale;
    switch (p.dimensions)
    {
    case 1: return fractal<1>(p, Vn<1>(sw));
    case 2: return fractal<2>(p, Vn<2>(v.x, v.y));
    case 4: return fractal<4>(p, Vn<4>(v.x, v.y, v.z, sw));
    default: return fractal<3>(p, v);
    }
}
} // namespace tex

namespace nodes
{
namespace
{
using D3 = glm::dvec3;

int dimensionsOf(const std::string& s) { return s == "1D" ? 1 : s == "2D" ? 2 : s == "4D" ? 4 : 3; }

tex::VoronoiFeature featureOf(const std::string& s)
{
    if (s == "f2")
        return tex::VoronoiFeature::F2;
    if (s == "smoothF1")
        return tex::VoronoiFeature::SmoothF1;
    if (s == "distanceToEdge")
        return tex::VoronoiFeature::DistanceToEdge;
    if (s == "nSphereRadius")
        return tex::VoronoiFeature::NSphereRadius;
    return tex::VoronoiFeature::F1;
}

tex::VoronoiMetric metricOf(const std::string& s)
{
    if (s == "manhattan")
        return tex::VoronoiMetric::Manhattan;
    if (s == "chebyshev")
        return tex::VoronoiMetric::Chebyshev;
    if (s == "minkowski")
        return tex::VoronoiMetric::Minkowski;
    return tex::VoronoiMetric::Euclidean;
}

Sockets voronoiSockets(const Group&, const GNode& n)
{
    const NodeDecl* d = findDecl("VoronoiTexture");
    int dims = dimensionsOf(d->propValue(n, "dimensions"));
    tex::VoronoiFeature f = featureOf(d->propValue(n, "feature"));
    bool fractalOk = f != tex::VoronoiFeature::NSphereRadius;
    Sockets s;
    if (dims != 1)
        s.inputs.push_back(fin("vector", T::Vector, nullptr, Implicit::Position, "Where to sample; defaults to the position."));
    if (dims == 1 || dims == 4)
        s.inputs.push_back(fin("w", T::Float, 0.0, Implicit::None, "The fourth coordinate (the only one in 1D). Animate it to make the cells evolve."));
    s.inputs.push_back(fin("scale", T::Float, 5.0, Implicit::None, "Cells per unit of the vector. Positions are in cm."));
    if (fractalOk)
    {
        s.inputs.push_back(fin("detail", T::Float, 0.0, Implicit::None, "Octaves of finer cells layered on top (0 to 15)."));
        s.inputs.push_back(fin("roughness", T::Float, 0.5, Implicit::None, "How much each finer octave contributes (0 to 1)."));
        s.inputs.push_back(fin("lacunarity", T::Float, 2.0, Implicit::None, "Scale step between octaves."));
    }
    if (f == tex::VoronoiFeature::SmoothF1)
        s.inputs.push_back(fin("smoothness", T::Float, 1.0, Implicit::None, "Blend width between cells (0 to 1)."));
    if (d->propValue(n, "distance") == "minkowski" && dims != 1 && f != tex::VoronoiFeature::DistanceToEdge && f != tex::VoronoiFeature::NSphereRadius)
        s.inputs.push_back(fin("exponent", T::Float, 0.5, Implicit::None, "Minkowski exponent: 1 is manhattan, 2 euclidean, large values chebyshev."));
    s.inputs.push_back(fin("randomness", T::Float, 1.0, Implicit::None, "How far cell points stray from the grid (0 to 1)."));
    if (f == tex::VoronoiFeature::NSphereRadius)
        s.outputs = {fout("radius", T::Float)};
    else if (f == tex::VoronoiFeature::DistanceToEdge)
        s.outputs = {fout("distance", T::Float)};
    else
    {
        s.outputs = {fout("distance", T::Float), fout("color", T::Color, "A random colour per cell.")};
        if (dims != 1)
            s.outputs.push_back(fout("position", T::Vector, "The nearest cell point."));
        if (dims == 1 || dims == 4)
            s.outputs.push_back(fout("w", T::Float));
    }
    return s;
}

void voronoiNode()
{
    registerDecl({
        .type = "VoronoiTexture",
        .label = "Voronoi Texture",
        .category = "Texture",
        .doc = "Worley cell noise, matching Blender's Voronoi Texture: distance to the nearest (F1), second nearest (F2) or blended "
               "(smooth F1) cell point, distance to the cell edge, or n-sphere radius, in 1D to 4D.",
        .props = {prop("dimensions", {"1D", "2D", "3D", "4D"}, "3D"),
                  prop("feature", {"f1", "f2", "smoothF1", "distanceToEdge", "nSphereRadius"}, "f1"),
                  prop("distance", {"euclidean", "manhattan", "chebyshev", "minkowski"}, "euclidean"),
                  prop("normalize", {"false", "true"}, "false", "Divide distance and colour by their largest possible value, for 0..1 output.")},
        .dynamicSockets = voronoiSockets,
        .exec =
            [](ExecContext& c) {
                tex::VoronoiParams base;
                base.dimensions = dimensionsOf(c.prop("dimensions"));
                base.feature = featureOf(c.prop("feature"));
                base.metric = base.dimensions == 1 ? tex::VoronoiMetric::Euclidean : metricOf(c.prop("distance"));
                base.normalize = c.prop("normalize") == "true";
                auto opt = [&](const char* id, double fallback) { return c.sockets.inputIndex(id) >= 0 ? c.field(id) : constantFloat(fallback); };
                FieldPtr vector = c.sockets.inputIndex("vector") >= 0 ? c.field("vector") : constantVector(D3(0.0));
                std::vector<FieldPtr> ins{vector,           opt("w", 0.0),          opt("scale", 5.0),      opt("detail", 0.0), opt("roughness", 0.5),
                                          opt("lacunarity", 2.0), opt("smoothness", 1.0), opt("exponent", 0.5), opt("randomness", 1.0)};
                std::vector<SocketType> types{T::Vector, T::Float, T::Float, T::Float, T::Float, T::Float, T::Float, T::Float, T::Float};
                enum Out
                {
                    Distance,
                    Color,
                    Position,
                    W,
                    Radius
                };
                auto make = [&](Out what) {
                    SocketType t = what == Color ? T::Color : what == Position ? T::Vector : T::Float;
                    return std::make_shared<FunctionField>(t, ins, types, [what, base](const std::vector<Column>& in, size_t n) {
                        bool single = true;
                        for (const Column& x : in)
                            single = single && x.single;
                        size_t cnt = single ? 1 : n;
                        std::vector<double> f(what == Color || what == Position ? 0 : cnt);
                        std::vector<D3> v(what == Position ? cnt : 0);
                        std::vector<glm::dvec4> col(what == Color ? cnt : 0);
                        auto body = [&](size_t k) {
                            tex::VoronoiParams p = base;
                            p.scale = in[2].at<double>(k);
                            p.detail = in[3].at<double>(k);
                            p.roughness = in[4].at<double>(k);
                            p.lacunarity = in[5].at<double>(k);
                            p.smoothness = in[6].at<double>(k);
                            p.exponent = in[7].at<double>(k);
                            p.randomness = in[8].at<double>(k);
                            tex::VoronoiResult r = tex::voronoi(p, in[0].at<D3>(k), in[1].at<double>(k));
                            switch (what)
                            {
                            case Distance: f[k] = r.distance; break;
                            case Color: col[k] = glm::dvec4(r.color, 1.0); break;
                            case Position: v[k] = r.position; break;
                            case W: f[k] = r.w; break;
                            case Radius: f[k] = r.radius; break;
                            }
                        };
                        if (cnt > 256)
                            tbb::parallel_for(size_t(0), cnt, body);
                        else
                            for (size_t k = 0; k < cnt; ++k)
                                body(k);
                        if (what == Color)
                            return Column{std::move(col), single};
                        if (what == Position)
                            return Column{std::move(v), single};
                        return Column{std::move(f), single};
                    });
                };
                c.out("distance", make(Distance));
                c.out("color", make(Color));
                c.out("position", make(Position));
                c.out("w", make(W));
                c.out("radius", make(Radius));
            },
    });
}
} // namespace

void registerTextureNodes() { voronoiNode(); }
} // namespace nodes
} // namespace lila::gn
