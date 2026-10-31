// Clay lattice sampling (clay/Clay.h SampleBackend): the distance at every lattice point of a set of bricks, each brick
// running its own filtered program. The math mirrors src/clay/Field.cpp, which stays the reference.

struct ClayPush
{
    float* prims;    // Field::kPrimWords floats per shape
    int* code;       // 4 words per instruction: kind, op, shape, blend bits
    int* bricks;     // xyz per brick
    int* localCode;  // instruction indices, -1 for an intersect that empties its group
    uint* localStart;
    uint* localCount;
    float* out;      // kSamples^3 per brick
    uint first;
    uint count;
    float h;
    uint symmetric;
};

[[vk::push_constant]] ClayPush pc;

static const uint kPrimWords = 24;
static const int kCells = 8;
static const int kSamples = kCells + 3;
static const float kEmpty = 1e30;

float dot2(float2 v) { return dot(v, v); }

float roundCone(float3 p, float r1, float r2, float h)
{
    if (r1 - r2 >= h)
        return length(p) - r1;
    if (r2 - r1 >= h)
        return length(p - float3(0, h, 0)) - r2;
    float2 q = float2(length(p.xz), p.y);
    float b = (r1 - r2) / h;
    float a = sqrt(1.0 - b * b);
    float k = dot(q, float2(-b, a));
    if (k < 0.0)
        return length(q) - r1;
    if (k > a * h)
        return length(q - float2(0.0, h)) - r2;
    return dot(q, float2(a, b)) - r1;
}

float cappedCone(float3 p, float h, float r1, float r2)
{
    float2 q = float2(length(p.xz), p.y);
    float2 k1 = float2(r2, h);
    float2 k2 = float2(r2 - r1, 2.0 * h);
    float2 ca = float2(q.x - min(q.x, q.y < 0.0 ? r1 : r2), abs(q.y) - h);
    float2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot2(k2), 0.0, 1.0);
    float s = (cb.x < 0.0 && ca.y < 0.0) ? -1.0 : 1.0;
    return s * sqrt(min(dot2(ca), dot2(cb)));
}

float shapeDistance(uint prim, float3 world)
{
    float* w = pc.prims + prim * kPrimWords;
    float3 p = float3(dot(float4(w[0], w[1], w[2], w[3]), float4(world, 1.0)), dot(float4(w[4], w[5], w[6], w[7]), float4(world, 1.0)),
                      dot(float4(w[8], w[9], w[10], w[11]), float4(world, 1.0)));
    float scale = w[12];
    int shape = int(w[13]);
    float radius = w[14], endRadius = w[15], len = w[16], rounding = w[17];
    float3 e = float3(w[18], w[19], w[20]);
    float d = kEmpty;
    if (shape == 0)
        d = length(p) - radius;
    else if (shape == 1)
    {
        float3 r = max(e, float3(1e-6));
        float k0 = length(p / r);
        float k1 = length(p / (r * r));
        d = k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -min(r.x, min(r.y, r.z));
    }
    else if (shape == 2)
    {
        float3 ex = max(e, float3(0.0));
        float rnd = clamp(rounding, 0.0, min(ex.x, min(ex.y, ex.z)));
        float3 q = abs(p) - (ex - rnd);
        d = length(max(q, float3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0) - rnd;
    }
    else if (shape == 3)
    {
        float hh = max(len, 0.0);
        d = roundCone(p + float3(0.0, 0.5 * hh, 0.0), max(radius, 0.0), max(endRadius, 0.0), hh);
    }
    else if (shape == 4)
    {
        float ra = max(radius, 0.0), hh = 0.5 * max(len, 0.0);
        float rb = clamp(rounding, 0.0, min(ra, hh));
        float2 dd = float2(length(p.xz) - ra + rb, abs(p.y) - hh + rb);
        d = min(max(dd.x, dd.y), 0.0) + length(max(dd, float2(0.0))) - rb;
    }
    else if (shape == 5)
    {
        float hh = 0.5 * max(len, 0.0);
        float r1 = max(radius, 0.0), r2 = max(endRadius, 0.0);
        float rnd = clamp(rounding, 0.0, min(hh, max(r1, r2)));
        d = cappedCone(p, hh - rnd, max(r1 - rnd, 0.0), max(r2 - rnd, 0.0)) - rnd;
    }
    else if (shape == 6)
    {
        float2 q = float2(length(p.xz) - radius, p.y);
        d = length(q) - max(endRadius, 0.0);
    }
    return d * scale;
}

float smin(float a, float b, float k)
{
    if (k <= 0.0)
        return min(a, b);
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return lerp(b, a, h) - k * h * (1.0 - h);
}

float combine(float acc, float d, int op, float k)
{
    if (op == 0)
        return smin(acc, d, k);
    if (op == 1)
        return -smin(-acc, d, k);
    return -smin(-acc, -d, k);
}

float run(uint start, uint n, float3 p)
{
    float stack[16];
    int top = 0;
    stack[0] = kEmpty;
    for (uint k = 0; k < n; ++k)
    {
        int i = pc.localCode[start + k];
        if (i < 0)
        {
            stack[top] = kEmpty;
            continue;
        }
        int kind = pc.code[i * 4];
        int op = pc.code[i * 4 + 1];
        float blend = asfloat(pc.code[i * 4 + 3]);
        if (kind == 0)
            stack[top] = combine(stack[top], shapeDistance(uint(pc.code[i * 4 + 2]), p), op, blend);
        else if (kind == 1)
        {
            if (top < 15)
            {
                ++top;
                stack[top] = kEmpty;
            }
        }
        else if (top > 0)
        {
            float group = stack[top];
            --top;
            stack[top] = combine(stack[top], group, op, blend);
        }
    }
    return stack[0];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void sampleCS(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.count)
        return;
    uint item = pc.first + tid.x;
    uint per = uint(kSamples * kSamples * kSamples);
    uint brick = item / per;
    uint s = item % per;
    int3 local = int3(int(s % uint(kSamples)), int((s / uint(kSamples)) % uint(kSamples)), int(s / uint(kSamples * kSamples)));
    int3 b = int3(pc.bricks[brick * 3], pc.bricks[brick * 3 + 1], pc.bricks[brick * 3 + 2]);
    float3 p = float3(b * kCells - 1 + local) * pc.h;
    uint start = pc.localStart[brick], n = pc.localCount[brick];
    float d = run(start, n, p);
    if (pc.symmetric != 0)
        d = min(d, run(start, n, float3(-p.x, p.y, p.z)));
    pc.out[item] = d;
}
