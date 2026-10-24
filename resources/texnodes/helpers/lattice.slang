//! Lattice noise along a direction that still tiles. A direction (a, b) with small integer components and integer
//! frequencies along and across it make uv shifts of whole textures integer shifts of the noise lattice; hashing lattice
//! points modulo those shifts makes the noise repeat exactly over uv 0 to 1.
//! requires: noise

/** The period lattice in Hermite form: (a, 0) and (b, c), every lattice point reduced by them. */
struct LatticePeriod
{
    int a;
    int b;
    int c;
};

int latticeFloorDiv(int x, int y)
{
    int q = x / y;
    return (x % y != 0 && ((x < 0) != (y < 0))) ? q - 1 : q;
}

/** Periods spanned by v1 and v2 (non-parallel). */
LatticePeriod latticeMake(int2 v1, int2 v2)
{
    // Extended gcd of the y components: s * v1.y + t * v2.y = g.
    int r0 = v1.y, r1 = v2.y, s0 = 1, s1 = 0, t0 = 0, t1 = 1;
    for (int guard = 0; guard < 64 && r1 != 0; ++guard)
    {
        int q = r0 / r1;
        int r2 = r0 - q * r1;
        r0 = r1;
        r1 = r2;
        int s2 = s0 - q * s1;
        s0 = s1;
        s1 = s2;
        int t2 = t0 - q * t1;
        t0 = t1;
        t1 = t2;
    }
    if (r0 < 0)
    {
        r0 = -r0;
        s0 = -s0;
        t0 = -t0;
    }
    LatticePeriod l;
    l.c = max(r0, 1);
    l.b = s0 * v1.x + t0 * v2.x;
    l.a = max(abs(v1.x * v2.y - v1.y * v2.x) / l.c, 1);
    return l;
}

int2 latticeReduce(int2 p, LatticePeriod l)
{
    int k = latticeFloorDiv(p.y, l.c);
    p -= k * int2(l.b, l.c);
    p.x = ((p.x % l.a) + l.a) % l.a;
    return p;
}

/** The small integer direction closest to the angle (degrees), components up to 4. */
int2 latticeDirection(float degrees)
{
    float a = radians(degrees);
    float2 d = float2(cos(a), sin(a));
    int2 best = int2(1, 0);
    float bestDot = -1.0;
    for (int y = 0; y <= 4; ++y)
    {
        for (int x = -4; x <= 4; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            float2 v = float2(float(x), float(y));
            float k = abs(dot(d, v)) / length(v);
            if (k > bestDot + 1e-5)
            {
                bestDot = k;
                best = int2(x, y);
            }
        }
    }
    return best;
}

float2 latticeGradient(int2 cell, LatticePeriod l, uint seed)
{
    float a = texRand(uint2(latticeReduce(cell, l)), seed) * 6.28318530718;
    return float2(cos(a), sin(a));
}

/** Gradient noise in 0 to 1 at q (lattice coordinates). */
float latticePerlin(float2 q, LatticePeriod l, uint seed)
{
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    float2 u = noiseFade(f);
    float n00 = dot(latticeGradient(c, l, seed), f);
    float n10 = dot(latticeGradient(c + int2(1, 0), l, seed), f - float2(1.0, 0.0));
    float n01 = dot(latticeGradient(c + int2(0, 1), l, seed), f - float2(0.0, 1.0));
    float n11 = dot(latticeGradient(c + int2(1, 1), l, seed), f - float2(1.0, 1.0));
    return lerp(lerp(n00, n10, u.x), lerp(n01, n11, u.x), u.y) * 0.70710678 + 0.5;
}

/** Value noise in 0 to 1 at q; smooth 0 interpolates linearly along x, 1 with the quintic fade. */
float latticeValue(float2 q, LatticePeriod l, uint seed, float smoothX)
{
    float2 i = floor(q);
    float2 f = q - i;
    int2 c = int2(i);
    float2 u = noiseFade(f);
    u.x = lerp(f.x, u.x, smoothX);
    float a = texRand(uint2(latticeReduce(c, l)), seed);
    float b = texRand(uint2(latticeReduce(c + int2(1, 0), l)), seed);
    float e = texRand(uint2(latticeReduce(c + int2(0, 1), l)), seed);
    float g = texRand(uint2(latticeReduce(c + int2(1, 1), l)), seed);
    return lerp(lerp(a, b, u.x), lerp(e, g, u.x), u.y);
}

/** Lattice coordinates of uv for direction dir with whole frequencies along and across it, and their period. */
float2 latticeCoords(float2 uv, int2 dir, int along, int across)
{
    float2 d = float2(dir);
    return float2(dot(uv, d) * float(along), dot(uv, float2(-d.y, d.x)) * float(across));
}

LatticePeriod latticePeriodFor(int2 dir, int along, int across)
{
    return latticeMake(int2(dir.x * along, -dir.y * across), int2(dir.y * along, dir.x * across));
}
