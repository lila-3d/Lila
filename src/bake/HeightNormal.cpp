#include "bake/HeightNormal.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::bake
{
namespace
{
/** Bilinear, wrapping, at uv (v down the rows, as the image is stored). */
float sampleHeight(const image::Image& h, double u, double v)
{
    const double x = u * h.width - 0.5, y = v * h.height - 0.5;
    const double fx = std::floor(x), fy = std::floor(y);
    const float tx = float(x - fx), ty = float(y - fy);
    auto at = [&](int ix, int iy) {
        ix = ((ix % h.width) + h.width) % h.width;
        iy = ((iy % h.height) + h.height) % h.height;
        return h.rgba[(size_t(iy) * size_t(h.width) + size_t(ix)) * 4];
    };
    const int x0 = int(fx), y0 = int(fy);
    const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return a + (b - a) * ty;
}

/**
 * The height's normal at texel (x, y) of a w x h grid, OpenGL. Central differences one height texel apart; with covered,
 * an uncovered neighbour of a same-sized height reads as the centre.
 */
glm::vec3 deriveAt(const image::Image& height, glm::dvec2 slope, int x, int y, int w, int h, const std::vector<uint8_t>* covered)
{
    const double u = (x + 0.5) / w, v = (y + 0.5) / h;
    const double eu = 1.0 / height.width, ev = 1.0 / height.height;
    if (covered && height.width == w && height.height == h)
    {
        auto at = [&](int ix, int iy) {
            ix = ((ix % w) + w) % w;
            iy = ((iy % h) + h) % h;
            const size_t i = size_t(iy) * size_t(w) + size_t(ix);
            return (*covered)[i] ? height.rgba[i * 4] : height.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
        };
        const double du = (at(x + 1, y) - at(x - 1, y)) / (2.0 * eu);
        // Rows run down the image; v runs up it.
        const double dv = (at(x, y - 1) - at(x, y + 1)) / (2.0 * ev);
        return glm::normalize(glm::vec3(float(-du * slope.x), float(-dv * slope.y), 1.0f));
    }
    const double du = (sampleHeight(height, u + eu, v) - sampleHeight(height, u - eu, v)) / (2.0 * eu);
    const double dv = (sampleHeight(height, u, v - ev) - sampleHeight(height, u, v + ev)) / (2.0 * ev);
    return glm::normalize(glm::vec3(float(-du * slope.x), float(-dv * slope.y), 1.0f));
}

glm::vec3 decode(const float* c, bool greenUp)
{
    glm::vec3 n(c[0] * 2.0f - 1.0f, c[1] * 2.0f - 1.0f, c[2] * 2.0f - 1.0f);
    if (!greenUp)
        n.y = -n.y;
    const float len = glm::length(n);
    return len > 1e-6f ? n / len : glm::vec3(0.0f, 0.0f, 1.0f);
}

void encode(float* c, glm::vec3 n, bool greenUp)
{
    if (!greenUp)
        n.y = -n.y;
    c[0] = n.x * 0.5f + 0.5f;
    c[1] = n.y * 0.5f + 0.5f;
    c[2] = n.z * 0.5f + 0.5f;
    c[3] = 1.0f;
}

void checkHeight(const image::Image& height)
{
    if (height.width <= 0 || height.height <= 0 || height.rgba.size() < size_t(height.width) * size_t(height.height) * 4)
        throw std::invalid_argument("height map is empty");
}
} // namespace

glm::vec3 reorientNormal(const glm::vec3& base, const glm::vec3& detail)
{
    const glm::vec3 t = base + glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 u = detail * glm::vec3(-1.0f, -1.0f, 1.0f);
    // base pointing straight down has no defined rotation; it can't come from a height or a sane normal map.
    if (t.z < 1e-5f)
        return detail;
    const glm::vec3 r = t * glm::dot(t, u) / t.z - u;
    const float len = glm::length(r);
    return len > 1e-6f ? r / len : glm::vec3(0.0f, 0.0f, 1.0f);
}

image::Image heightNormals(const image::Image& height, glm::dvec2 slope, bool greenUp)
{
    LILA_ZONE();
    checkHeight(height);
    image::Image out{height.width, height.height, std::vector<float>(height.rgba.size())};
    tbb::parallel_for(0, height.height, [&](int y) {
        for (int x = 0; x < height.width; ++x)
            encode(&out.rgba[(size_t(y) * size_t(height.width) + size_t(x)) * 4], deriveAt(height, slope, x, y, height.width, height.height, nullptr), greenUp);
    });
    return out;
}

image::Image detailNormals(const image::Image& normal, const image::Image& height, glm::dvec2 slope, double fadeDegrees, bool greenUp,
                           const std::vector<uint8_t>* covered)
{
    LILA_ZONE();
    checkHeight(height);
    const int w = normal.width, h = normal.height;
    if (covered && covered->size() != size_t(w) * size_t(h))
        covered = nullptr;
    // Full detail below half the fade angle, none above one and a half times it.
    const bool fade = fadeDegrees > 0.0;
    const double lo = std::cos(glm::radians(std::min(fadeDegrees * 0.5, 89.0))), hi = std::cos(glm::radians(std::min(fadeDegrees * 1.5, 89.9)));
    std::vector<glm::vec3> bases(size_t(w) * size_t(h));
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
            bases[size_t(y) * size_t(w) + size_t(x)] = deriveAt(height, slope, x, y, w, h, covered);
    });
    image::Image out{w, h, std::vector<float>(normal.rgba.size())};
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const glm::vec3 n = decode(&normal.rgba[i * 4], greenUp);
            const glm::vec3 base = bases[i];
            glm::vec3 d = reorientNormal(glm::vec3(-base.x, -base.y, base.z), n);
            if (fade)
            {
                // The steepest slope next to the texel too: a texel a wall only partly covers is part of that wall.
                float flattest = base.z;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        flattest = std::min(flattest, bases[size_t((y + dy + h) % h) * size_t(w) + size_t((x + dx + w) % w)].z);
                const float steep = float(std::clamp((double(flattest) - hi) / std::max(lo - hi, 1e-6), 0.0, 1.0));
                // Only a normal that disagrees with the height there missed the wall; one that agrees (a carved slope, its
                // facets aside) is kept whole.
                const double off = glm::degrees(std::acos(std::clamp(double(d.z), -1.0, 1.0)));
                const float agrees = float(std::clamp((fadeDegrees * 0.75 - off) / (fadeDegrees * 0.5), 0.0, 1.0));
                const float keep = std::max(steep, agrees);
                const float s = keep * keep * (3.0f - 2.0f * keep);
                d = glm::normalize(glm::mix(glm::vec3(0.0f, 0.0f, 1.0f), d, s));
            }
            encode(&out.rgba[i * 4], d, greenUp);
        }
    });
    return out;
}

image::Image combineNormals(const image::Image& detail, const image::Image& height, glm::dvec2 slope, bool greenUp)
{
    LILA_ZONE();
    checkHeight(height);
    const int w = detail.width, h = detail.height;
    image::Image out{w, h, std::vector<float>(detail.rgba.size())};
    tbb::parallel_for(0, h, [&](int y) {
        for (int x = 0; x < w; ++x)
        {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            encode(&out.rgba[i * 4], reorientNormal(deriveAt(height, slope, x, y, w, h, nullptr), decode(&detail.rgba[i * 4], greenUp)), greenUp);
        }
    });
    return out;
}
} // namespace lila::bake
