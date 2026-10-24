#include "bake/Backend.h"

#include "bake/CpuBackend.h"
#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace lila::bake
{
namespace
{
std::mutex gMutex;

std::map<std::string, BackendFactory>& factories()
{
    static std::map<std::string, BackendFactory> f{{"cpu", [] { return std::make_unique<CpuBackend>(); }}};
    return f;
}
} // namespace

uint32_t packRgba8(const glm::vec4& c)
{
    uint32_t out = 0;
    for (int k = 0; k < 4; ++k)
        out |= uint32_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f)) << (8 * k);
    return out;
}

glm::vec4 unpackRgba8(uint32_t c)
{
    return {float(c & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f, float((c >> 16) & 0xff) / 255.0f, float(c >> 24) / 255.0f};
}

void TextureBuffer::allocate()
{
    const size_t n = texelCount();
    samples.assign(n, 0);
    if (def && def->resolve == Resolve::Exact)
    {
        exact.assign(n, 0);
        votes.assign(n, 0);
    }
    else
    {
        sum.assign(n, glm::vec4(0.0f));
        weight.assign(n, 0.0f);
    }
}

void TextureBuffer::resolve()
{
    LILA_ZONE();

    const size_t n = texelCount();
    const glm::vec4 bg = def ? def->background : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    value.assign(n, bg);
    covered.assign(n, 0);
    const Resolve mode = def ? def->resolve : Resolve::Average;
    for (size_t i = 0; i < n; ++i)
    {
        switch (mode)
        {
        case Resolve::Exact:
            if (votes[i])
            {
                value[i] = unpackRgba8(exact[i]);
                covered[i] = 1;
            }
            break;
        case Resolve::Coverage:
            if (samples[i])
            {
                const float f = std::min(1.0f, float(samples[i]) / float(std::max(1, samplesPerTexel)));
                value[i] = glm::vec4(f, f, f, 1.0f);
                covered[i] = 1;
            }
            break;
        case Resolve::Average:
            if (weight[i] > 0.0f)
            {
                value[i] = sum[i] / weight[i];
                covered[i] = 1;
            }
            break;
        }
    }
}

void Backend::shade(const MapDef& def, const MapContext& ctx, std::span<const TexelSample> samples, std::span<const HitRecord> hits,
                    std::span<glm::vec4> out, std::span<uint8_t> valid) const
{
    if (def.cpu)
        def.cpu(ctx, samples, hits, out, valid);
}

void Backend::dilate(TextureBuffer& tex, int padding) const { dilateCpu(tex.value, tex.covered, tex.width, tex.height, padding); }

void dilateCpu(std::span<glm::vec4> values, std::span<uint8_t> covered, int width, int height, int padding)
{
    LILA_ZONE();

    if (padding == 0 || width <= 0 || height <= 0)
        return;
    const size_t n = size_t(width) * size_t(height);
    std::vector<uint32_t> frontier, next;
    for (size_t i = 0; i < n; ++i)
        if (covered[i])
            frontier.push_back(uint32_t(i));
    if (frontier.empty())
        return;
    // filled marks texels reached by an earlier ring; covered itself stays the original coverage.
    std::vector<uint8_t> filled(covered.begin(), covered.end());
    for (int ring = 0; (padding < 0 || ring < padding) && !frontier.empty(); ++ring)
    {
        next.clear();
        for (uint32_t i : frontier)
        {
            const int x = int(i % uint32_t(width)), y = int(i / uint32_t(width));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    const size_t j = size_t(ny) * size_t(width) + size_t(nx);
                    if (filled[j])
                        continue;
                    // Edge neighbours before diagonal ones within a ring, so the nearer seed wins.
                    if (dx != 0 && dy != 0)
                        continue;
                    filled[j] = 1;
                    values[j] = values[i];
                    next.push_back(uint32_t(j));
                }
        }
        for (uint32_t i : frontier)
        {
            const int x = int(i % uint32_t(width)), y = int(i / uint32_t(width));
            for (int dy : {-1, 1})
                for (int dx : {-1, 1})
                {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    const size_t j = size_t(ny) * size_t(width) + size_t(nx);
                    if (filled[j])
                        continue;
                    filled[j] = 1;
                    values[j] = values[i];
                    next.push_back(uint32_t(j));
                }
        }
        frontier.swap(next);
    }
}

void registerBackend(const std::string& name, BackendFactory factory)
{
    std::lock_guard lock(gMutex);
    factories()[name] = std::move(factory);
}

void unregisterBackend(const std::string& name)
{
    std::lock_guard lock(gMutex);
    if (name != "cpu")
        factories().erase(name);
}

std::unique_ptr<Backend> createBackend(std::string_view name)
{
    std::lock_guard lock(gMutex);
    auto it = factories().find(std::string(name));
    return it == factories().end() ? nullptr : it->second();
}

std::vector<std::string> backendNames()
{
    std::lock_guard lock(gMutex);
    std::vector<std::string> out;
    for (const auto& [k, v] : factories())
        out.push_back(k);
    return out;
}
} // namespace lila::bake
