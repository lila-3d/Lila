// GPU forms of the topology-preserving deformers (scene/DeformEval.h). Each kernel reads mesh-space points and writes
// every point's displacement; the CPU adds it to its double points. The math mirrors src/deform and the deformer
// nodes, which stay the reference. Parameter blocks are 32-bit words laid out as the nodes' gpuProgram pushes them.

struct DeformPush
{
    float* points; // xyz per point
    float* out;    // displacement, xyz per point
    uint* params;
    uint* b0;
    uint* b1;
    uint* b2;
    uint* b3;
    float* src; // delta mush: smoothing input
    float* dst; // delta mush: smoothing output
    uint count;
    uint first;
};

[[vk::push_constant]] DeformPush dc;

static const float kPi = 3.14159265358979;

float pf(uint o) { return asfloat(dc.params[o]); }
float3 p3(uint o) { return float3(pf(o), pf(o + 1), pf(o + 2)); }
/** A column-major 4x4 at o applied to a point. */
float3 xform(uint o, float3 p) { return p3(o) * p.x + p3(o + 4) * p.y + p3(o + 8) * p.z + p3(o + 12); }
/** A column-major 3x3 at o applied to a vector. */
float3 mul3(uint o, float3 v) { return p3(o) * v.x + p3(o + 3) * v.y + p3(o + 6) * v.z; }

float3 load3(float* b, uint i) { return float3(b[i * 3], b[i * 3 + 1], b[i * 3 + 2]); }
void store3(float* b, uint i, float3 v)
{
    b[i * 3] = v.x;
    b[i * 3 + 1] = v.y;
    b[i * 3 + 2] = v.z;
}
float3 point(uint i) { return load3(dc.points, i); }
void displace(uint i, float3 d) { store3(dc.out, i, d); }

float smooth01(float t)
{
    t = clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float along(float y, float lo, float hi)
{
    float span = hi - lo;
    return abs(span) < 1e-12 ? 0.0 : clamp((y - lo) / span, 0.0, 1.0);
}

float sphericalFalloff(float d, float radius, float softness)
{
    if (radius <= 0.0)
        return 1.0;
    float inner = radius * (1.0 - clamp(softness, 0.0, 1.0));
    if (d <= inner)
        return 1.0;
    if (d >= radius)
        return 0.0;
    return 1.0 - smooth01((d - inner) / (radius - inner));
}

float dropoffScale(float dropoff, float t)
{
    float d = clamp(dropoff, -1.0, 1.0);
    return d >= 0.0 ? 1.0 - d * t : 1.0 + d * (1.0 - t);
}

/** sin(x) / x, by its series near 0 where a GPU sine's absolute error would swamp it. */
float sinc(float x)
{
    float x2 = x * x;
    if (abs(x) < 0.5)
        return 1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0)));
    return sin(x) / x;
}

// ------------------------------------------------------------------------------------------------ nonlinear
// params: toHandle (16), back (16), radius, softness, kind, then the kind's own (up to 6) from 35.

float3 bend(float3 p)
{
    float lo = min(pf(36), pf(37)), hi = max(pf(36), pf(37));
    float span = hi - lo;
    float k = span > 1e-12 ? radians(pf(35)) / span : 0.0;
    if (abs(k) < 1e-12)
        return p;
    float yc = clamp(p.y, lo, hi);
    float phi = k * yc;
    // r - (r - x) c and (r - x) s with r = 1 / k, written through sinc so a gentle bend (huge r, tiny phi) keeps its
    // precision: GPU sines are accurate in absolute terms only.
    float sc = sinc(phi), sh = sinc(0.5 * phi);
    float s = phi * sc, c = 1.0 - 0.5 * phi * phi * sh * sh;
    float x = p.x * c + 0.5 * yc * phi * sh * sh;
    float y = yc * sc - p.x * s;
    float beyond = p.y - yc;
    return float3(x + beyond * s, y + beyond * c, p.z);
}

float3 twist(float3 p)
{
    float t = along(p.y, pf(37), pf(38));
    float a = radians(pf(35) + (pf(36) - pf(35)) * t);
    float s = sin(a), c = cos(a);
    return float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

float3 taper(float3 p)
{
    float t = along(p.y, pf(38), pf(39));
    float s = pf(35) + (pf(36) - pf(35)) * t + pf(37) * sin(kPi * t);
    return float3(p.x * s, p.y, p.z * s);
}

float3 squash(float3 p)
{
    float lo = min(pf(38), pf(39)), hi = max(pf(38), pf(39));
    float sy = max(1.0 + pf(35), 1e-3);
    float yc = clamp(p.y, lo, hi);
    float y = yc * sy + (p.y - yc);
    float t = along(p.y, lo, hi);
    float m = clamp(pf(37), 1e-3, 1.0 - 1e-3);
    float bump = t < m ? smooth01(t / m) : smooth01((1.0 - t) / (1.0 - m));
    float sxz = 1.0 + pf(36) * (1.0 / sqrt(sy) - 1.0) * bump;
    return float3(p.x * sxz, y, p.z * sxz);
}

float3 sine(float3 p)
{
    float lo = min(pf(39), pf(40)), hi = max(pf(39), pf(40));
    float yc = clamp(p.y, lo, hi);
    float ext = max(abs(lo), abs(hi));
    float wl = abs(pf(36)) < 1e-9 ? 1e-9 : pf(36);
    float amp = pf(35) * dropoffScale(pf(38), ext > 0.0 ? abs(yc) / ext : 0.0);
    return float3(p.x + amp * sin(2.0 * kPi * (yc / wl + pf(37))), p.y, p.z);
}

float3 wave(float3 p)
{
    float r = sqrt(p.x * p.x + p.z * p.z);
    if (r < pf(39) || r > pf(40))
        return p;
    float wl = abs(pf(36)) < 1e-9 ? 1e-9 : pf(36);
    float amp = pf(35) * dropoffScale(pf(38), pf(40) > 0.0 ? r / pf(40) : 0.0);
    return float3(p.x, p.y + amp * sin(2.0 * kPi * (r / wl - pf(37))), p.z);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void nonlinearCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    float3 p = point(i);
    float3 h = xform(0, p);
    float w = sphericalFalloff(length(h), pf(32), pf(33));
    if (w <= 0.0)
    {
        displace(i, float3(0.0));
        return;
    }
    uint kind = dc.params[34];
    float3 f = kind == 0 ? bend(h) : kind == 1 ? twist(h) : kind == 2 ? taper(h) : kind == 3 ? squash(h) : kind == 4 ? sine(h) : wave(h);
    displace(i, xform(16, lerp(h, f, w)) - p);
}

// ------------------------------------------------------------------------------------------------ ffd
// params: toBase (16), toLocal (16), n.xyz, basis, outsideFalloff, then the lattice displacements (xyz) from 37.

static const int kMaxLattice = 32;

int axisWeights(int n, float s, uint basis, out int idx[kMaxLattice], out float w[kMaxLattice])
{
    if (n < 2)
    {
        idx[0] = 0;
        w[0] = 1.0;
        return 1;
    }
    s = clamp(s, 0.0, 1.0);
    if (basis == 0)
    {
        float x = s * float(n - 1);
        int i0 = clamp(int(floor(x)), 0, n - 2);
        float f = x - float(i0);
        idx[0] = i0;
        w[0] = 1.0 - f;
        idx[1] = i0 + 1;
        w[1] = f;
        return 2;
    }
    if (basis == 1)
    {
        int d = n - 1;
        float binom = 1.0;
        for (int i = 0; i <= d; ++i)
        {
            // Powers by repeated products: pow(0, 0) is undefined on the GPU.
            float a = 1.0, b = 1.0;
            for (int k = 0; k < i; ++k)
                a *= s;
            for (int k = 0; k < d - i; ++k)
                b *= 1.0 - s;
            idx[i] = i;
            w[i] = binom * a * b;
            binom = binom * float(d - i) / float(i + 1);
        }
        return n;
    }
    float x = s * float(n - 1);
    int base = int(floor(x));
    int count = 0;
    for (int i = base - 1; i <= base + 2; ++i)
    {
        float t = abs(x - float(i));
        float v = t < 1.0 ? (4.0 - 6.0 * t * t + 3.0 * t * t * t) / 6.0 : t < 2.0 ? (2.0 - t) * (2.0 - t) * (2.0 - t) / 6.0 : 0.0;
        if (v <= 0.0)
            continue;
        idx[count] = clamp(i, 0, n - 1);
        w[count] = v;
        ++count;
    }
    return count;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void ffdCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    float3 p = point(i);
    float3 q = xform(0, p);
    float dist = length(max(abs(q) - float3(0.5), float3(0.0)));
    float outside = pf(36);
    float fall = outside > 0.0 ? sphericalFalloff(dist, outside, 1.0) : (dist > 1e-9 ? 0.0 : 1.0);
    if (fall <= 0.0)
    {
        displace(i, float3(0.0));
        return;
    }
    int3 n = int3(int(dc.params[32]), int(dc.params[33]), int(dc.params[34]));
    uint basis = dc.params[35];
    float3 stu = clamp(q + float3(0.5), float3(0.0), float3(1.0));
    int ix[kMaxLattice], iy[kMaxLattice], iz[kMaxLattice];
    float wx[kMaxLattice], wy[kMaxLattice], wz[kMaxLattice];
    int cx = axisWeights(n.x, stu.x, basis, ix, wx);
    int cy = axisWeights(n.y, stu.y, basis, iy, wy);
    int cz = axisWeights(n.z, stu.z, basis, iz, wz);
    float3 d = float3(0.0);
    for (int c = 0; c < cz; ++c)
        for (int b = 0; b < cy; ++b)
        {
            float wzy = wz[c] * wy[b];
            uint row = uint(n.x) * (uint(iy[b]) + uint(n.y) * uint(iz[c]));
            for (int a = 0; a < cx; ++a)
                d += p3(37 + (row + uint(ix[a])) * 3) * (wx[a] * wzy);
        }
    displace(i, (xform(16, q + d) - p) * fall);
}

// ------------------------------------------------------------------------------------------------ kelvinlet
// params: geom (16), the inverse's 3x3 (9), centre (3), scales, eps (3), weights (3), a, b, profile denominator, flags
// (1 grab, 2 twist, 4 scale, 8 pinch), grab force (3), twist axis (3), twist angle, scale amount, pinch F (9).

float3 kelvinletGrab(float3 r, float3 f, float eps, float a, float b)
{
    float re2 = dot(r, r) + eps * eps;
    float re = sqrt(re2);
    float re3 = re2 * re;
    return ((a - b) / re + a * eps * eps / (2.0 * re3)) * f + (b / re3) * dot(r, f) * r;
}

float3 kelvinletAffine(float3 r, float eps, float a, float b)
{
    float re2 = dot(r, r) + eps * eps;
    float re = sqrt(re2);
    float re3 = re2 * re, re5 = re3 * re2;
    float profile = 1.0 / re3 + 1.5 * eps * eps / re5;
    float3 Fr = mul3(47, r);
    float3 FTr = float3(dot(p3(47), r), dot(p3(50), r), dot(p3(53), r));
    float tr = pf(47) + pf(51) + pf(55);
    float3 symR = Fr + FTr + tr * r;
    return -a * profile * Fr + b * (symR / re3 - (3.0 / re5) * dot(r, Fr) * r);
}

float affineProfile(float r2, float eps)
{
    float re2 = r2 + eps * eps;
    float re = sqrt(re2);
    float re3 = re2 * re;
    return 1.0 / re3 + 1.5 * eps * eps / (re3 * re2);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void kelvinletCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    float3 r = xform(0, point(i)) - p3(25);
    int n = int(dc.params[28]);
    float a = pf(35), b = pf(36), den = pf(37);
    uint flags = dc.params[38];
    float3 u = float3(0.0);
    float profile = 0.0;
    if ((flags & 6) != 0 && den != 0.0)
    {
        float num = 0.0;
        for (int k = 0; k < n; ++k)
            num += pf(32 + k) * affineProfile(dot(r, r), pf(29 + k));
        profile = num / den;
    }
    if ((flags & 1) != 0)
        for (int k = 0; k < n; ++k)
            u += pf(32 + k) * kelvinletGrab(r, p3(39), pf(29 + k), a, b);
    if ((flags & 2) != 0)
    {
        float3 axis = p3(42);
        float angle = pf(45) * profile;
        float c = cos(angle), s = sin(angle);
        u += r * c + cross(axis, r) * s + axis * dot(axis, r) * (1.0 - c) - r;
    }
    if ((flags & 4) != 0)
        u += r * (pf(46) * profile);
    if ((flags & 8) != 0)
        for (int k = 0; k < n; ++k)
            u += pf(32 + k) * kelvinletAffine(r, pf(29 + k), a, b);
    displace(i, mul3(16, u));
}

// ------------------------------------------------------------------------------------------------ cage
// Linear: params the inverse geom's 3x3 (9), nv, nf, vertex displacements (nv xyz), face terms (nf xyz); b0 the vertex
// coordinates (points x nv), b1 the face ones (points x nf).

[shader("compute")]
[numthreads(64, 1, 1)]
void cageLinearCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    uint nv = dc.params[9], nf = dc.params[10];
    float3 u = float3(0.0);
    uint row = i * nv;
    for (uint j = 0; j < nv; ++j)
        u += p3(11 + j * 3) * asfloat(dc.b0[row + j]);
    uint fo = 11 + nv * 3;
    row = i * nf;
    for (uint f = 0; f < nf; ++f)
        u += p3(fo + f * 3) * asfloat(dc.b1[row + f]);
    displace(i, mul3(0, u));
}

// Somigliana: params geom (16), its inverse (16), rotation (9), translation (3), nv, nf, vertex displacements in the
// rotated frame (nv xyz), face tractions (nf xyz); b0 and b1 hold 3x3 row-major coordinates per vertex and face.

float3 rowMul(uint* m, uint o, float3 v)
{
    return float3(asfloat(m[o]) * v.x + asfloat(m[o + 1]) * v.y + asfloat(m[o + 2]) * v.z,
                  asfloat(m[o + 3]) * v.x + asfloat(m[o + 4]) * v.y + asfloat(m[o + 5]) * v.z,
                  asfloat(m[o + 6]) * v.x + asfloat(m[o + 7]) * v.y + asfloat(m[o + 8]) * v.z);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cageSomiglianaCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    uint nv = dc.params[44], nf = dc.params[45];
    float3 u = float3(0.0);
    for (uint j = 0; j < nv; ++j)
        u += rowMul(dc.b0, (i * nv + j) * 9, p3(46 + j * 3));
    uint fo = 46 + nv * 3;
    for (uint f = 0; f < nf; ++f)
        u += rowMul(dc.b1, (i * nf + f) * 9, p3(fo + f * 3));
    float3 p = point(i);
    float3 x = xform(0, p);
    displace(i, xform(16, mul3(32, x + u) + p3(41)) - p);
}

// ------------------------------------------------------------------------------------------------ wrap
// params: geom (16), its inverse (16), mode, scale compensation, driver point count, then every driver's world points
// (xyz) one after another. b0: influence offsets (points + 1), b1: 16 words per influence (see the wrap node).

float3 driverPoint(uint k) { return p3(35 + k * 3); }

float3 triangleFramed(float3 a, float3 b, float3 c, float3 o)
{
    float3 x = b - a;
    float3 n = cross(b - a, c - a);
    float lx = length(x), ln = length(n);
    if (lx < 1e-30 || ln < 1e-30)
        return o;
    x /= lx;
    n /= ln;
    return x * o.x + cross(n, x) * o.y + n * o.z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void wrapCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    uint k0 = dc.b0[i], k1 = dc.b0[i + 1];
    if (k0 == k1)
    {
        displace(i, float3(0.0));
        return;
    }
    float3 p = point(i);
    float3 t = xform(0, p);
    uint mode = dc.params[32];
    float sc = pf(33);
    float3 sum = float3(0.0);
    for (uint k = k0; k < k1; ++k)
    {
        uint o = k * 16;
        if (dc.b1[o + 3] == 0)
        {
            displace(i, float3(0.0));
            return;
        }
        float3 a = driverPoint(dc.b1[o]), b = driverPoint(dc.b1[o + 1]), c = driverPoint(dc.b1[o + 2]);
        float3 bary = float3(asfloat(dc.b1[o + 4]), asfloat(dc.b1[o + 5]), asfloat(dc.b1[o + 6]));
        float3 off = float3(asfloat(dc.b1[o + 7]), asfloat(dc.b1[o + 8]), asfloat(dc.b1[o + 9]));
        float w = asfloat(dc.b1[o + 10]), restArea = asfloat(dc.b1[o + 11]);
        float3 anchor = float3(asfloat(dc.b1[o + 12]), asfloat(dc.b1[o + 13]), asfloat(dc.b1[o + 14]));
        float3 q = a * bary.x + b * bary.y + c * bary.z;
        bool hasRest = restArea > 0.0;
        if (mode == 2)
            sum += q * w;
        else if (mode == 1 && hasRest)
            sum += (t + q - anchor) * w;
        else
        {
            float scale = 1.0;
            if (hasRest && sc != 0.0)
                scale = 1.0 + sc * (sqrt(0.5 * length(cross(b - a, c - a)) / restArea) - 1.0);
            sum += (q + triangleFramed(a, b, c, off * scale)) * w;
        }
    }
    displace(i, xform(16, sum) - p);
}

// ------------------------------------------------------------------------------------------------ delta mush
// params: step, pin borders, displacement, adjacency point count. b0: neighbour offsets (points + 1) then neighbours,
// b1: corner offsets (points + 1) then (next, previous) point pairs, b2: 1 per border point, b3: rest deltas (xyz).

[shader("compute")]
[numthreads(64, 1, 1)]
void deltaMushSmoothCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    float3 s = load3(dc.src, i);
    uint n = dc.params[3];
    if (i >= n)
    {
        store3(dc.dst, i, s);
        return;
    }
    uint k0 = dc.b0[i], k1 = dc.b0[i + 1];
    if (k0 == k1 || (dc.params[1] != 0 && dc.b2[i] != 0))
    {
        store3(dc.dst, i, s);
        return;
    }
    float3 sum = float3(0.0);
    for (uint k = k0; k < k1; ++k)
        sum += load3(dc.src, dc.b0[n + 1 + k]);
    float3 avg = sum / float(k1 - k0);
    store3(dc.dst, i, s + (avg - s) * pf(0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void deltaMushApplyCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= dc.count)
        return;
    uint i = dc.first + tid.x;
    uint n = dc.params[3];
    float3 s = load3(dc.src, i);
    float3 local = float3(asfloat(dc.b3[i * 3]), asfloat(dc.b3[i * 3 + 1]), asfloat(dc.b3[i * 3 + 2])) * pf(2);
    float3 d = local;
    if (i < n)
    {
        float3 normal = float3(0.0);
        for (uint k = dc.b1[i]; k < dc.b1[i + 1]; ++k)
        {
            uint o = n + 1 + k * 2;
            normal += cross(load3(dc.src, dc.b1[o]) - s, load3(dc.src, dc.b1[o + 1]) - s);
        }
        float ln = length(normal);
        uint k0 = dc.b0[i], k1 = dc.b0[i + 1];
        if (ln > 0.0 && k0 != k1)
        {
            normal /= ln;
            float3 t = float3(0.0);
            for (uint k = k0; k < k1 && length(t) < 1e-12; ++k)
            {
                float3 e = load3(dc.src, dc.b0[n + 1 + k]) - s;
                t = e - normal * dot(e, normal);
            }
            if (length(t) < 1e-12)
                t = abs(normal.x) < 0.9 ? cross(normal, float3(1, 0, 0)) : cross(normal, float3(0, 1, 0));
            t = normalize(t);
            d = t * local.x + cross(normal, t) * local.y + normal * local.z;
        }
    }
    displace(i, s + d - point(i));
}
