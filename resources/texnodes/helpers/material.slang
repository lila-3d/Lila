//! Mixing whole materials channel by channel.
//! requires: normals

/** t = 0 gives a, 1 gives b. Normals mix as vectors and renormalise. */
TexMaterial materialLerp(TexMaterial a, TexMaterial b, float t)
{
    TexMaterial m;
    m.baseColor = lerp(a.baseColor, b.baseColor, t);
    m.roughness = lerp(a.roughness, b.roughness, t);
    m.metallic = lerp(a.metallic, b.metallic, t);
    m.normal = normalEncode(lerp(normalDecode(a.normal), normalDecode(b.normal), t) + float3(0.0, 0.0, 1e-4));
    m.height = lerp(a.height, b.height, t);
    m.ao = lerp(a.ao, b.ao, t);
    m.emissive = lerp(a.emissive, b.emissive, t);
    m.opacity = lerp(a.opacity, b.opacity, t);
    return m;
}
