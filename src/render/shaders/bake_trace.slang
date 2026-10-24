// Ray casts for the bake kernels, included after bake_common.slang with TRACE_HW (ray query against a TLAS per group,
// one BLAS per high) or TRACE_SW (stack traversal of each high's flat MeshBvh) defined.

struct TraceHit
{
    float t;
    uint high;
    uint triangle;
    float2 bary;
    bool front;
};

#if defined(TRACE_HW)

#define TRACE_ENTRY [require(spvRayQueryKHR)]

[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;

bool traceClosest(float3 o, float3 d, float tMax, bool cullBack, inout TraceHit hit)
{
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 0.0;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, cullBack ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE, 0xff, r);
    while (q.Proceed())
    {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    hit.t = q.CommittedRayT();
    hit.high = q.CommittedInstanceID();
    hit.triangle = pc.instanceBase[q.CommittedInstanceIndex()] + q.CommittedPrimitiveIndex();
    hit.bary = q.CommittedTriangleBarycentrics();
    hit.front = q.CommittedTriangleFrontFace();
    return true;
}

bool traceAny(float3 o, float3 d, float tMax)
{
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 0.0;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, r);
    while (q.Proceed())
    {
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// The nearest hit on one high only (the instances whose custom index is high), either facing.
bool traceClosestHigh(float3 o, float3 d, float tMax, uint high, inout TraceHit hit)
{
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 0.0;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, r);
    while (q.Proceed())
    {
        // Committing shortens the ray, so later candidates are only nearer ones.
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CandidateInstanceID() == high)
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    hit.t = q.CommittedRayT();
    hit.high = q.CommittedInstanceID();
    hit.triangle = pc.instanceBase[q.CommittedInstanceIndex()] + q.CommittedPrimitiveIndex();
    hit.bary = q.CommittedTriangleBarycentrics();
    hit.front = q.CommittedTriangleFrontFace();
    return true;
}

// Any hit on one high only (the instance whose custom index is high).
bool traceAnyHigh(float3 o, float3 d, float tMax, uint high)
{
    RayDesc r;
    r.Origin = o;
    r.Direction = d;
    r.TMin = 0.0;
    r.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xff, r);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CandidateInstanceID() == high)
        {
            q.CommitNonOpaqueTriangleHit();
            q.Abort();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

#elif defined(TRACE_SW)

#define TRACE_ENTRY

static const int kStack = 96;

// Per-ray setup of the watertight test (Woop, Benthin, Wald 2013).
struct WatertightRay
{
    float3 o;
    float3 d;
    float3 inv;
    int kx;
    int ky;
    int kz;
    float3 shear;
};

WatertightRay makeRay(float3 o, float3 d)
{
    WatertightRay r;
    r.o = o;
    r.d = d;
    // Zero components get a tiny slope instead: an infinite inverse times a zero slab offset would be NaN.
    r.inv = 1.0 / select(abs(d) < 1e-30, select(d < 0.0, -1e-30, 1e-30), d);
    float3 a = abs(d);
    r.kz = a.x > a.y ? (a.x > a.z ? 0 : 2) : (a.y > a.z ? 1 : 2);
    r.kx = (r.kz + 1) % 3;
    r.ky = (r.kx + 1) % 3;
    if (d[r.kz] < 0.0)
    {
        int s = r.kx;
        r.kx = r.ky;
        r.ky = s;
    }
    r.shear = float3(d[r.kx] / d[r.kz], d[r.ky] / d[r.kz], 1.0 / d[r.kz]);
    return r;
}

// t and the weights of corners 1 and 2; front when the ray meets the counter-clockwise side.
bool intersect(WatertightRay r, float3 a, float3 b, float3 c, float tMax, bool cullBack, out float t, out float2 bary, out bool front)
{
    t = 0.0;
    bary = float2(0.0);
    front = dot(r.d, cross(b - a, c - a)) < 0.0;
    if (cullBack && !front)
        return false;
    float3 A = a - r.o, B = b - r.o, C = c - r.o;
    float Ax = A[r.kx] - r.shear.x * A[r.kz], Ay = A[r.ky] - r.shear.y * A[r.kz];
    float Bx = B[r.kx] - r.shear.x * B[r.kz], By = B[r.ky] - r.shear.y * B[r.kz];
    float Cx = C[r.kx] - r.shear.x * C[r.kz], Cy = C[r.ky] - r.shear.y * C[r.kz];
    // precise keeps these from contracting into FMAs, which would break the exact sign symmetry of shared edges.
    precise float U = Cx * By - Cy * Bx;
    precise float V = Ax * Cy - Ay * Cx;
    precise float W = Bx * Ay - By * Ax;
    if ((U < 0.0 || V < 0.0 || W < 0.0) && (U > 0.0 || V > 0.0 || W > 0.0))
        return false;
    float det = U + V + W;
    if (det == 0.0)
        return false;
    float T = U * (r.shear.z * A[r.kz]) + V * (r.shear.z * B[r.kz]) + W * (r.shear.z * C[r.kz]);
    float inv = 1.0 / det;
    t = T * inv;
    if (!(t >= 0.0 && t <= tMax))
        return false;
    bary = float2(V * inv, W * inv);
    return true;
}

float hitBox(BvhNode n, WatertightRay r, float tMax)
{
    float3 a = (n.lo - r.o) * r.inv;
    float3 b = (n.hi - r.o) * r.inv;
    float3 lo = min(a, b), hi = max(a, b);
    float t0 = max(max(lo.x, lo.y), max(lo.z, 0.0));
    float t1 = min(min(hi.x, hi.y), min(hi.z, tMax));
    // Slabs are padded by an ulp so rays along a face still enter its box.
    return t0 <= t1 * 1.00000024 ? t0 : 1e38;
}

bool traceHigh(uint high, WatertightRay r, bool cullBack, bool any, inout float best, inout TraceHit hit)
{
    HighInfo h = pc.highs[high];
    if (h.triangles == 0)
        return false;
    bool found = false;
    uint stack[kStack];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0)
    {
        BvhNode n = h.nodes[stack[--sp]];
        if (hitBox(n, r, best) >= 1e38)
            continue;
        if (n.count > 0)
        {
            for (uint k = n.first; k < n.first + n.count; ++k)
            {
                float4 a = h.corners[k * 3], b = h.corners[k * 3 + 1], c = h.corners[k * 3 + 2];
                float t;
                float2 bary;
                bool front;
                if (intersect(r, a.xyz, b.xyz, c.xyz, best, cullBack, t, bary, front))
                {
                    if (any)
                        return true;
                    found = true;
                    best = t;
                    hit.t = t;
                    hit.high = high;
                    hit.triangle = asuint(a.w);
                    hit.bary = bary;
                    hit.front = front;
                }
            }
            continue;
        }
        BvhNode l = h.nodes[n.first], rr = h.nodes[n.first + 1];
        float tl = hitBox(l, r, best), tr = hitBox(rr, r, best);
        if (sp + 2 > kStack)
            continue;
        if (tl < tr)
        {
            if (tr < 1e38)
                stack[sp++] = n.first + 1;
            stack[sp++] = n.first;
        }
        else
        {
            if (tl < 1e38)
                stack[sp++] = n.first;
            if (tr < 1e38)
                stack[sp++] = n.first + 1;
        }
    }
    return found;
}

bool traceClosest(float3 o, float3 d, float tMax, bool cullBack, inout TraceHit hit)
{
    WatertightRay r = makeRay(o, d);
    float best = tMax;
    bool found = false;
    for (uint i = 0; i < pc.highCount; ++i)
        found = traceHigh(pc.highList[i], r, cullBack, false, best, hit) || found;
    return found;
}

bool traceAny(float3 o, float3 d, float tMax)
{
    WatertightRay r = makeRay(o, d);
    TraceHit unused;
    for (uint i = 0; i < pc.highCount; ++i)
    {
        float best = tMax;
        if (traceHigh(pc.highList[i], r, false, true, best, unused))
            return true;
    }
    return false;
}

bool traceAnyHigh(float3 o, float3 d, float tMax, uint high)
{
    WatertightRay r = makeRay(o, d);
    TraceHit unused;
    float best = tMax;
    return traceHigh(high, r, false, true, best, unused);
}

bool traceClosestHigh(float3 o, float3 d, float tMax, uint high, inout TraceHit hit)
{
    WatertightRay r = makeRay(o, d);
    float best = tMax;
    return traceHigh(high, r, false, false, best, hit);
}

#endif

// Secondary rays for the map kernels: the occluder scope is the bound TLAS (HW) or pc.highs / highList (SW), except self,
// which keeps to the high the projection hit.
bool occludedScoped(float3 o, float3 d, float tMax, uint high)
{
    return (pc.flags & kFlagScopeSelf) != 0 ? traceAnyHigh(o, d, tMax, high) : traceAny(o, d, tMax);
}

bool closestScoped(float3 o, float3 d, float tMax, uint high, inout TraceHit hit)
{
    return (pc.flags & kFlagScopeSelf) != 0 ? traceClosestHigh(o, d, tMax, high, hit) : traceClosest(o, d, tMax, false, hit);
}

// One HitRecord per sample: the nearest surface of the group's highs along the sample's ray, as CpuBackend finds it.
[shader("compute")]
[numthreads(64, 1, 1)]
TRACE_ENTRY
void projectCS(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= pc.count)
        return;
    uint i = pc.first + id.x;
    TexelSample s = pc.samples[i];
    bool present = s.lowTriangle != kNoHit;
    HitRecord r;
    r.position = float3(0.0);
    r.high = kNoHit;
    r.normal = float3(0.0);
    r.triangle = kNoHit;
    r.bary = float2(0.0);
    r.distance = 0.0;
    r.height = 0.0;
    r.flags = 0;
    r.pad0 = 0;
    r.pad1 = 0;
    r.pad2 = 0;
    bool cullBack = (pc.flags & kFlagCullBack) != 0;
    TraceHit hit;
    hit.t = 0.0;
    hit.high = 0;
    hit.triangle = 0;
    hit.bary = float2(0.0);
    hit.front = true;
    if (present && traceClosest(s.origin, s.direction, s.rayLength, cullBack, hit))
    {
        float3 p = s.origin + s.direction * hit.t;
        r.position = p;
        r.normal = hitNormal(hit.high, hit.triangle, hit.bary);
        r.high = hit.high;
        r.triangle = hit.triangle;
        r.bary = hit.bary;
        r.distance = hit.t;
        r.height = dot(p - s.position, -s.direction);
        r.flags = kHitValid | (hit.front ? 0u : kHitBackface);
    }
    else if (present && cullBack && traceAny(s.origin, s.direction, s.rayLength))
        r.flags = kHitBackface;
    pc.hits[i] = r;
    if ((pc.flags & kFlagResident) != 0 && present)
    {
        InterlockedAdd(pc.stats[0], 1u);
        if ((r.flags & kHitValid) != 0)
            InterlockedAdd(pc.stats[1], 1u);
        if ((r.flags & kHitBackface) != 0)
            InterlockedAdd(pc.stats[2], 1u);
    }
}
