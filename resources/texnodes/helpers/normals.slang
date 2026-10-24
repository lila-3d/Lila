//! Tangent-space normals stored as colours (0 to 1, OpenGL: green is +v).

float3 normalDecode(float4 c) { return c.xyz * 2.0 - 1.0; }
float4 normalEncode(float3 n) { return float4(normalize(n) * 0.5 + 0.5, 1.0); }

/** Reoriented normal mapping: detail rotated onto base. */
float3 normalRnm(float3 base, float3 detail)
{
    float3 t = base + float3(0.0, 0.0, 1.0);
    float3 u = detail * float3(-1.0, -1.0, 1.0);
    return t * dot(t, u) / t.z - u;
}
