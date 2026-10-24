//! Simplex-style gradient noise on a triangular lattice that tiles. The lattice has `cells` points across u and an even
//! number of rows across v, chosen so its triangles are close to equilateral; rows shift by half a cell, so an even row
//! count wraps onto whole cells.

int simplexRows(int cells) { return max(2, 2 * int(round(float(cells) / (2.0 * 0.8660254)))); }

float2 simplexGradient(int2 c, int cells, int rows, uint seed)
{
    int j = ((c.y % rows) + rows) % rows;
    int i = c.x + (c.y - j) / 2;
    i = ((i % cells) + cells) % cells;
    float a = texRand(uint2(uint(i), uint(j)), seed) * 6.28318530718;
    return float2(cos(a), sin(a));
}

/** About -1 to 1. */
float simplexNoise(float2 uv, int cells, int rows, uint seed)
{
    float rowHeight = float(cells) / float(rows);
    float2 q = uv * float(cells);
    float t = q.y / rowHeight;
    float2 s = float2(q.x - 0.5 * t, t);
    float2 b = floor(s);
    float2 f = s - b;
    int2 c = int2(b);
    int2 corner1 = f.x + f.y < 1.0 ? int2(0, 0) : int2(1, 1);
    float sum = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        int2 l = c + (k == 0 ? corner1 : (k == 1 ? int2(1, 0) : int2(0, 1)));
        float2 pos = float2(float(l.x) + 0.5 * float(l.y), float(l.y) * rowHeight);
        float2 d = q - pos;
        float w = 0.6 - dot(d, d);
        if (w > 0.0)
        {
            w *= w;
            sum += w * w * dot(simplexGradient(l, cells, rows, seed), d);
        }
    }
    return sum * 30.0;
}

/** Octaves double the lattice, so every octave tiles; the sum is renormalised to 0 to 1. */
float simplexFbm(float2 uv, int scale, int octaves, float persistence, uint seed)
{
    int cells = max(scale, 1);
    int rows = simplexRows(cells);
    float sum = 0.0;
    float weight = 0.0;
    float amp = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        sum += amp * simplexNoise(uv, cells, rows, seed + uint(o) * 0x9e3779b9u);
        weight += amp;
        amp *= persistence;
        cells *= 2;
        rows *= 2;
    }
    return weight > 0.0 ? saturate(0.5 + 0.5 * sum / weight) : 0.5;
}
