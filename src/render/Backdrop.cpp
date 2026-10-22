#include "render/Backdrop.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>

namespace lila::render
{
namespace
{
float luminance(glm::vec3 c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); }

float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

/** mesh.slang's neutralTonemap. */
glm::vec3 neutralTonemap(glm::vec3 c)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;
    float x = std::min(c.r, std::min(c.g, c.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c -= offset;
    float peak = std::max(c.r, std::max(c.g, c.b));
    if (peak < startCompression)
        return c;
    const float d = 1.0f - startCompression;
    float newPeak = 1.0f - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return glm::mix(c, glm::vec3(newPeak), g);
}

/** mesh.slang's toEnv. */
glm::vec3 toEnv(glm::vec3 d, const FrameState& s)
{
    if (s.zUp)
        d = glm::vec3(d.x, d.z, -d.y);
    float r = glm::radians(s.envRotation);
    float c = std::cos(r), sn = std::sin(r);
    return {c * d.x + sn * d.z, d.y, -sn * d.x + c * d.z};
}

/** Face and texel of a direction, the way Vulkan selects cube faces. */
size_t texel(glm::vec3 d, int size)
{
    glm::vec3 a = glm::abs(d);
    int face;
    float sc, tc, ma;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x > 0 ? 0 : 1;
        sc = d.x > 0 ? -d.z : d.z;
        tc = -d.y;
        ma = a.x;
    }
    else if (a.y >= a.z)
    {
        face = d.y > 0 ? 2 : 3;
        sc = d.x;
        tc = d.y > 0 ? d.z : -d.z;
        ma = a.y;
    }
    else
    {
        face = d.z > 0 ? 4 : 5;
        sc = d.z > 0 ? d.x : -d.x;
        tc = -d.y;
        ma = a.z;
    }
    ma = std::max(ma, 1e-12f);
    int x = std::clamp(int((sc / ma + 1.0f) * 0.5f * float(size)), 0, size - 1);
    int y = std::clamp(int((tc / ma + 1.0f) * 0.5f * float(size)), 0, size - 1);
    return (size_t(face) * size_t(size) + size_t(y)) * size_t(size) + size_t(x);
}
} // namespace

std::shared_ptr<const EnvProbe> makeEnvProbe(const EnvironmentData& env, int maxFace)
{
    if (env.mips.empty() || env.size <= 0)
        return nullptr;
    auto probe = std::make_shared<EnvProbe>();
    int mips = int(env.mips.size());
    probe->environmentMips = mips;
    probe->firstMip = mips - 1;
    for (int m = 0; m < mips; ++m)
        if (std::max(1, env.size >> m) <= maxFace)
        {
            probe->firstMip = m;
            break;
        }
    for (int m = probe->firstMip; m < mips; ++m)
    {
        int size = std::max(1, env.size >> m);
        const std::vector<uint16_t>& src = env.mips[size_t(m)];
        size_t texels = size_t(size) * size_t(size) * 6;
        if (src.size() < texels * 4)
            break;
        std::vector<glm::vec3> px(texels);
        for (size_t i = 0; i < texels; ++i)
            px[i] = {glm::unpackHalf1x16(src[i * 4]), glm::unpackHalf1x16(src[i * 4 + 1]), glm::unpackHalf1x16(src[i * 4 + 2])};
        probe->sizes.push_back(size);
        probe->mips.push_back(std::move(px));
    }
    if (probe->mips.empty())
        return nullptr;
    return probe;
}

glm::vec3 sampleProbe(const EnvProbe& probe, glm::vec3 dir, float lod)
{
    float local = std::clamp(lod - float(probe.firstMip), 0.0f, float(probe.mips.size() - 1));
    int m0 = int(local);
    int m1 = std::min(m0 + 1, int(probe.mips.size()) - 1);
    float t = local - float(m0);
    glm::vec3 a = probe.mips[size_t(m0)][texel(dir, probe.sizes[size_t(m0)])];
    glm::vec3 b = probe.mips[size_t(m1)][texel(dir, probe.sizes[size_t(m1)])];
    return glm::mix(a, b, t);
}

std::vector<float> backdropLuminance(const FrameState& s, const EnvProbe* probe, int cols, int rows)
{
    cols = std::max(cols, 1);
    rows = std::max(rows, 1);
    std::vector<float> out(size_t(cols) * size_t(rows));
    if (!probe || !s.envBackground)
    {
        glm::vec3 flat(srgbToLinear(s.background.r), srgbToLinear(s.background.g), srgbToLinear(s.background.b));
        std::fill(out.begin(), out.end(), std::clamp(luminance(flat), 0.0f, 1.0f));
        return out;
    }
    glm::dmat4 inv = glm::inverse(s.proj * s.view);
    float lod = std::clamp(s.envBlur, 0.0f, 1.0f) * float(probe->environmentMips - 1);
    float saturation = std::clamp(s.envSaturation, 0.0f, 1.0f);
    float gamma = 1.0f / std::max(s.gamma, 0.01f);
    // A few rays per cell, so a cell straddling a bright softbox does not hinge on one texel.
    constexpr int kSub = 3;
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col)
        {
            float sum = 0.0f;
            for (int sy = 0; sy < kSub; ++sy)
                for (int sx = 0; sx < kSub; ++sx)
                {
                    glm::dvec2 ndc((double(col) + (sx + 0.5) / kSub) / cols * 2.0 - 1.0, (double(row) + (sy + 0.5) / kSub) / rows * 2.0 - 1.0);
                    glm::dvec4 nearH = inv * glm::dvec4(ndc, 1.0, 1.0);
                    glm::dvec4 farH = inv * glm::dvec4(ndc, 0.001, 1.0);
                    glm::dvec3 d = glm::dvec3(farH) / farH.w - glm::dvec3(nearH) / nearH.w;
                    if (glm::dot(d, d) < 1e-30)
                        continue;
                    glm::vec3 c = sampleProbe(*probe, toEnv(glm::vec3(glm::normalize(d)), s), lod) * s.envIntensity * s.envBackgroundExposure;
                    c = glm::mix(glm::vec3(luminance(c)), c, saturation);
                    c = glm::pow(glm::max(neutralTonemap(c * s.exposure), glm::vec3(0.0f)), glm::vec3(gamma));
                    sum += luminance(c);
                }
            out[size_t(row) * size_t(cols) + size_t(col)] = std::clamp(sum / float(kSub * kSub), 0.0f, 1.0f);
        }
    return out;
}
} // namespace lila::render
