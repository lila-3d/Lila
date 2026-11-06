// SPDX-License-Identifier: Apache-2.0
#include "groomlab/frames.h"
#include "groomlab/portable_math.h"

#include <algorithm>
#include <cmath>

#if defined(_M_X64) || defined(__SSE2__)
#define GROOMLAB_FRAMES_SSE2 1
#include <emmintrin.h>
#else
#define GROOMLAB_FRAMES_SSE2 0
#endif

namespace groomlab {

namespace {

constexpr float kEpsilon = 1e-12f;

Vec3f tangentAt(const Vec3f* p, std::size_t n, std::size_t i) {
    const Vec3f d = (i == 0) ? p[1] - p[0] : (i + 1 == n) ? p[n - 1] - p[n - 2] : p[i + 1] - p[i - 1];
    return d.normalized();
}

Frame firstFrame(const Vec3f* points, std::size_t count, const Vec3f& initialNormal);

#if GROOMLAB_FRAMES_SSE2
// Four lanes of Vec3f, each operation the scalar one per lane (in Vec3f's order), so each lane
// gets the scalar code's floats bit for bit.
struct V4 {
    __m128 x, y, z;
};
inline V4 operator-(const V4& a, const V4& b) {
    return {_mm_sub_ps(a.x, b.x), _mm_sub_ps(a.y, b.y), _mm_sub_ps(a.z, b.z)};
}
inline V4 operator+(const V4& a, const V4& b) {
    return {_mm_add_ps(a.x, b.x), _mm_add_ps(a.y, b.y), _mm_add_ps(a.z, b.z)};
}
inline V4 operator*(const V4& a, __m128 s) { return {_mm_mul_ps(a.x, s), _mm_mul_ps(a.y, s), _mm_mul_ps(a.z, s)}; }
inline __m128 dot(const V4& a, const V4& b) {
    return _mm_add_ps(_mm_add_ps(_mm_mul_ps(a.x, b.x), _mm_mul_ps(a.y, b.y)), _mm_mul_ps(a.z, b.z));
}
inline V4 cross(const V4& a, const V4& b) {
    return {_mm_sub_ps(_mm_mul_ps(a.y, b.z), _mm_mul_ps(a.z, b.y)), _mm_sub_ps(_mm_mul_ps(a.z, b.x), _mm_mul_ps(a.x, b.z)),
            _mm_sub_ps(_mm_mul_ps(a.x, b.y), _mm_mul_ps(a.y, b.x))};
}
inline __m128 pick(__m128 mask, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b)); }
inline V4 pick(__m128 mask, const V4& a, const V4& b) {
    return {pick(mask, a.x, b.x), pick(mask, a.y, b.y), pick(mask, a.z, b.z)};
}
// Vec3f::normalized: v / length, or zero when the length is not above 0.
inline V4 normalized(const V4& v) {
    const __m128 len = _mm_sqrt_ps(dot(v, v));
    const V4 q{_mm_div_ps(v.x, len), _mm_div_ps(v.y, len), _mm_div_ps(v.z, len)};
    const __m128 ok = _mm_cmpgt_ps(len, _mm_setzero_ps());
    return {_mm_and_ps(ok, q.x), _mm_and_ps(ok, q.y), _mm_and_ps(ok, q.z)};
}
inline V4 lanes(const Vec3f* const p[4], std::size_t i) {
    return {_mm_setr_ps(p[0][i].x, p[1][i].x, p[2][i].x, p[3][i].x), _mm_setr_ps(p[0][i].y, p[1][i].y, p[2][i].y, p[3][i].y),
            _mm_setr_ps(p[0][i].z, p[1][i].z, p[2][i].z, p[3][i].z)};
}
inline V4 lanes(const Vec3f v[4]) {
    return {_mm_setr_ps(v[0].x, v[1].x, v[2].x, v[3].x), _mm_setr_ps(v[0].y, v[1].y, v[2].y, v[3].y),
            _mm_setr_ps(v[0].z, v[1].z, v[2].z, v[3].z)};
}
inline void unpack(const V4& v, Vec3f out[4]) {
    alignas(16) float x[4], y[4], z[4];
    _mm_store_ps(x, v.x);
    _mm_store_ps(y, v.y);
    _mm_store_ps(z, v.z);
    for (int k = 0; k < 4; ++k) out[k] = {x[k], y[k], z[k]};
}
inline V4 tangentAt4(const Vec3f* const p[4], std::size_t n, std::size_t i) {
    const V4 d = (i == 0) ? lanes(p, 1) - lanes(p, 0) : (i + 1 == n) ? lanes(p, n - 1) - lanes(p, n - 2)
                                                                      : lanes(p, i + 1) - lanes(p, i - 1);
    return normalized(d);
}
#endif

}  // namespace

namespace {

Frame firstFrame(const Vec3f* points, std::size_t count, const Vec3f& initialNormal) {
    Frame f0;
    f0.tangent = tangentAt(points, count, 0);
    Vec3f n0 = initialNormal - f0.tangent * initialNormal.dot(f0.tangent);
    f0.normal = n0.length() > 1e-6f ? n0.normalized() : anyPerpendicular(f0.tangent);
    f0.binormal = f0.tangent.cross(f0.normal);
    return f0;
}

}  // namespace

Vec3f anyPerpendicular(const Vec3f& v) {
    // Cross with the axis least aligned with v.
    const float ax = std::fabs(v.x), ay = std::fabs(v.y), az = std::fabs(v.z);
    const Vec3f axis = (ax <= ay && ax <= az) ? Vec3f(1, 0, 0) : (ay <= az) ? Vec3f(0, 1, 0) : Vec3f(0, 0, 1);
    return v.cross(axis).normalized();
}

void rotationMinimizingFramesInto(const Vec3f* points, std::size_t count, const Vec3f& initialNormal,
                                  std::vector<Frame>& frames) {
    frames.assign(count, Frame{});
    if (count == 0) return;
    if (count == 1) {
        frames[0].normal = initialNormal.length() > 0 ? initialNormal.normalized() : Vec3f(1, 0, 0);
        frames[0].tangent = anyPerpendicular(frames[0].normal).cross(frames[0].normal).normalized();
        frames[0].binormal = frames[0].tangent.cross(frames[0].normal);
        return;
    }

    frames[0] = firstFrame(points, count, initialNormal);
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const Frame& a = frames[i];
        Frame& b = frames[i + 1];
        b.tangent = tangentAt(points, count, i + 1);

        const Vec3f v1 = points[i + 1] - points[i];
        const float c1 = v1.dot(v1);
        if (c1 < kEpsilon) {  // repeated point: carry the frame over
            b = a;
            continue;
        }
        const Vec3f rL = a.normal - v1 * (2.0f / c1 * v1.dot(a.normal));
        const Vec3f tL = a.tangent - v1 * (2.0f / c1 * v1.dot(a.tangent));
        const Vec3f v2 = b.tangent - tL;
        const float c2 = v2.dot(v2);
        const Vec3f r = c2 < kEpsilon ? rL : rL - v2 * (2.0f / c2 * v2.dot(rL));

        // Re-orthonormalize against float drift.
        b.normal = (r - b.tangent * r.dot(b.tangent)).normalized();
        if (b.normal.length() == 0.0f) b.normal = anyPerpendicular(b.tangent);
        b.binormal = b.tangent.cross(b.normal);
    }
}

void rotationMinimizingFrames4(const Vec3f* const points[4], std::size_t count, const Vec3f initialNormals[4],
                               Frame* const out[4]) {
#if GROOMLAB_FRAMES_SSE2
    if (count >= 2) {
        // rotationMinimizingFramesInto's loop, lane by lane: the same operations in the same
        // order, its branches as per-lane picks.
        Frame first[4];
        for (int k = 0; k < 4; ++k) first[k] = out[k][0] = firstFrame(points[k], count, initialNormals[k]);
        Vec3f t[4], n[4], b[4];
        for (int k = 0; k < 4; ++k) {
            t[k] = first[k].tangent;
            n[k] = first[k].normal;
            b[k] = first[k].binormal;
        }
        V4 at = lanes(t), an = lanes(n), ab = lanes(b);
        const __m128 eps = _mm_set1_ps(kEpsilon), two = _mm_set1_ps(2.0f), zero = _mm_setzero_ps();
        for (std::size_t i = 0; i + 1 < count; ++i) {
            V4 bt = tangentAt4(points, count, i + 1);
            const V4 v1 = lanes(points, i + 1) - lanes(points, i);
            const __m128 c1 = dot(v1, v1);
            const __m128 carry = _mm_cmplt_ps(c1, eps);  // repeated point: carry the frame over
            const __m128 k1 = _mm_div_ps(two, c1);
            const V4 rL = an - v1 * _mm_mul_ps(k1, dot(v1, an));
            const V4 tL = at - v1 * _mm_mul_ps(k1, dot(v1, at));
            const V4 v2 = bt - tL;
            const __m128 c2 = dot(v2, v2);
            const V4 r = pick(_mm_cmplt_ps(c2, eps), rL, rL - v2 * _mm_mul_ps(_mm_div_ps(two, c2), dot(v2, rL)));
            V4 bn = normalized(r - bt * dot(r, bt));
            const int degenerate = _mm_movemask_ps(_mm_andnot_ps(carry, _mm_cmpeq_ps(dot(bn, bn), zero)));
            if (degenerate) {
                Vec3f nn[4], tt[4];
                unpack(bn, nn);
                unpack(bt, tt);
                for (int k = 0; k < 4; ++k) {
                    if (degenerate & (1 << k)) nn[k] = anyPerpendicular(tt[k]);
                }
                bn = lanes(nn);
            }
            const V4 bb = cross(bt, bn);
            at = pick(carry, at, bt);
            an = pick(carry, an, bn);
            ab = pick(carry, ab, bb);
            unpack(at, t);
            unpack(an, n);
            unpack(ab, b);
            for (int k = 0; k < 4; ++k) out[k][i + 1] = Frame{t[k], n[k], b[k]};
        }
        return;
    }
#endif
    std::vector<Frame> frames;
    for (int k = 0; k < 4; ++k) {
        rotationMinimizingFramesInto(points[k], count, initialNormals[k], frames);
        std::copy(frames.begin(), frames.end(), out[k]);
    }
}

std::vector<Frame> rotationMinimizingFrames(const Vec3f* points, std::size_t count, const Vec3f& initialNormal) {
    std::vector<Frame> frames;
    rotationMinimizingFramesInto(points, count, initialNormal, frames);
    return frames;
}

TwistedTurn::TwistedTurn(const Vec3f& from, const Vec3f& to, float angle) {
    const Rotation turn(from, to);
    const float c = pm::cos(angle), s = pm::sin(angle);
    // Each axis turned, then twisted about `to` (rotateAxisAngle's terms).
    auto twist = [&](const Vec3f& x) { return x * c + to.cross(x) * s + to * (to.dot(x) * (1.0f - c)); };
    c0 = twist(turn(Vec3f(1.0f, 0.0f, 0.0f)));
    c1 = twist(turn(Vec3f(0.0f, 1.0f, 0.0f)));
    c2 = twist(turn(Vec3f(0.0f, 0.0f, 1.0f)));
}

void Rotation::addTurned(const Vec3f* from, const char* turned, const Vec3f& to, const float* const* v,
                         const float* w, std::size_t count, std::size_t n, std::size_t stride, float* a,
                         const float* twists) {
    float* ax = a;
    float* ay = a + stride;
    float* az = a + 2 * stride;
    auto twisted = [&](std::size_t i) { return twists && turned[i] && twists[i] != 0.0f; };
    std::size_t j = 0;
#if GROOMLAB_FRAMES_SSE2
    // Up to 16 curves at a time (the next 16 onto their sums). Their rotations first, four at a
    // time: the constructor's dot, cross and 1 / (1 + c) in lanes, its tests on each lane's c.
    // Then four vectors at a time, every curve's four added in turn while the sums stay in
    // registers: operator(), then * w, then +=, the scalar expressions' operations in their order,
    // lane by lane. A twisted curve's matrix is made as TwistedTurn makes it, and applied as it does.
    constexpr std::size_t kGroup = 16;
    const std::size_t end = n & ~std::size_t{3};
    const __m128 one = _mm_set1_ps(1.0f), two = _mm_set1_ps(2.0f);
    const V4 onto{_mm_set1_ps(to.x), _mm_set1_ps(to.y), _mm_set1_ps(to.z)};
    struct Lanes {
        V4 k;
        __m128 cosine, inv, w;
    };
    struct Columns {
        V4 c0, c1, c2;
    };
    auto splat = [](const Vec3f& c) { return V4{_mm_set1_ps(c.x), _mm_set1_ps(c.y), _mm_set1_ps(c.z)}; };
    for (std::size_t first = 0; first < count; first += kGroup) {
        const std::size_t m = std::min(kGroup, count - first);
        Lanes parts[kGroup];
        Kind kinds[kGroup];
        const float* rows[kGroup];
        Columns columns[kGroup];
        for (std::size_t i = 0; i < m; i += 4) {
            Vec3f f[4] = {to, to, to, to};  // (lanes past the last curve: unused)
            for (std::size_t l = 0; l < 4 && i + l < m; ++l) f[l] = from[first + i + l];
            const V4 fl = lanes(f);
            alignas(16) float c[4], kx[4], ky[4], kz[4], inv[4];
            const __m128 cosine = dot(fl, onto);
            const V4 k = cross(fl, onto);
            _mm_store_ps(c, cosine);
            _mm_store_ps(kx, k.x);
            _mm_store_ps(ky, k.y);
            _mm_store_ps(kz, k.z);
            _mm_store_ps(inv, _mm_div_ps(one, _mm_add_ps(one, cosine)));
            for (std::size_t l = 0; l < 4 && i + l < m; ++l) {
                const std::size_t at = i + l;
                Vec3f axis(kx[l], ky[l], kz[l]);
                Kind kind = Kind::General;
                if (twisted(first + at)) {
                    kind = Kind::Twisted;  // (its parts but w unused)
                    const TwistedTurn turn(f[l], to, twists[first + at]);
                    columns[at] = {splat(turn.c0), splat(turn.c1), splat(turn.c2)};
                } else if (!turned[first + at] || c[l] > 1.0f - 1e-6f) {
                    kind = Kind::Identity;  // (its parts unused)
                } else if (c[l] < -1.0f + 1e-6f) {
                    kind = Kind::HalfTurn;
                    axis = anyPerpendicular(f[l]);
                }
                kinds[at] = kind;
                parts[at] = {{_mm_set1_ps(axis.x), _mm_set1_ps(axis.y), _mm_set1_ps(axis.z)}, _mm_set1_ps(c[l]),
                             _mm_set1_ps(inv[l]), _mm_set1_ps(w[first + at])};
                rows[at] = v[first + at];
            }
        }
        bool allGeneral = true;
        for (std::size_t i = 0; i < m; ++i) allGeneral = allGeneral && kinds[i] == Kind::General;
        if (allGeneral) {
            for (std::size_t b = 0; b < end; b += 4) {
                V4 sum{_mm_loadu_ps(ax + b), _mm_loadu_ps(ay + b), _mm_loadu_ps(az + b)};
                for (std::size_t i = 0; i < m; ++i) {
                    const float* o = rows[i] + b;
                    const V4 p{_mm_loadu_ps(o), _mm_loadu_ps(o + stride), _mm_loadu_ps(o + 2 * stride)};
                    const Lanes& l = parts[i];
                    sum = sum + ((p * l.cosine + cross(l.k, p)) + l.k * _mm_mul_ps(dot(l.k, p), l.inv)) * l.w;
                }
                _mm_storeu_ps(ax + b, sum.x);
                _mm_storeu_ps(ay + b, sum.y);
                _mm_storeu_ps(az + b, sum.z);
            }
            continue;
        }
        for (std::size_t b = 0; b < end; b += 4) {
            V4 sum{_mm_loadu_ps(ax + b), _mm_loadu_ps(ay + b), _mm_loadu_ps(az + b)};
            for (std::size_t i = 0; i < m; ++i) {
                const float* o = rows[i] + b;
                const V4 p{_mm_loadu_ps(o), _mm_loadu_ps(o + stride), _mm_loadu_ps(o + 2 * stride)};
                const Lanes& l = parts[i];
                V4 r;
                switch (kinds[i]) {
                    case Kind::Identity: r = p; break;
                    case Kind::HalfTurn: r = l.k * _mm_mul_ps(two, dot(l.k, p)) - p; break;
                    case Kind::General: r = (p * l.cosine + cross(l.k, p)) + l.k * _mm_mul_ps(dot(l.k, p), l.inv); break;
                    case Kind::Twisted: {
                        const Columns& c = columns[i];
                        r = (c.c0 * p.x + c.c1 * p.y) + c.c2 * p.z;
                        break;
                    }
                }
                sum = sum + r * l.w;
            }
            _mm_storeu_ps(ax + b, sum.x);
            _mm_storeu_ps(ay + b, sum.y);
            _mm_storeu_ps(az + b, sum.z);
        }
    }
    j = end;
#endif
    for (; j < n; ++j) {
        for (std::size_t i = 0; i < count; ++i) {
            const float* o = v[i] + j;
            const Vec3f p(o[0], o[stride], o[2 * stride]);
            Vec3f t;
            if (twisted(i)) {
                t = TwistedTurn(from[i], to, twists[i])(p) * w[i];
            } else {
                const Rotation turn = turned[i] ? Rotation(from[i], to) : Rotation();
                t = turn(p) * w[i];
            }
            ax[j] += t.x;
            ay[j] += t.y;
            az[j] += t.z;
        }
    }
}

Vec3f rotateBetween(const Vec3f& v, const Vec3f& from, const Vec3f& to) { return Rotation(from, to)(v); }

}  // namespace groomlab
