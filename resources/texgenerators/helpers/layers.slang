//! Layer stack compositing and mask generator kernels.
//! requires: blend, normals

/** blendMode's modes plus 10: signed add around 0.5 (heights, where 0.5 is neutral). */
float3 layerBlendMode(int mode, float3 b, float3 f)
{
    if (mode == 10)
        return b + f - 0.5;
    return blendMode(mode, b, f);
}

float3 layerDirection(float azimuth, float elevation, int upAxis)
{
    float a = radians(azimuth);
    float e = radians(elevation);
    float2 h = float2(cos(a), sin(a)) * cos(e);
    return upAxis == 1 ? float3(h.x, h.y, sin(e)) : float3(h.x, sin(e), h.y);
}

float layerColorDistance(float4 a, float4 b)
{
    float3 d = abs(a.rgb - b.rgb);
    return max(d.x, max(d.y, d.z));
}
