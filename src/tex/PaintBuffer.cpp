#include "tex/PaintBuffer.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>

namespace lila::tex::paint
{
glm::vec2 Placement::apply(const glm::vec2& p, const glm::vec2& center) const
{
    const glm::vec2 d = (p - center) * scale;
    const float c = std::cos(rotation), s = std::sin(rotation);
    return center + offset + glm::vec2(d.x * c - d.y * s, d.x * s + d.y * c);
}

glm::vec2 Placement::invert(const glm::vec2& p, const glm::vec2& center) const
{
    const glm::vec2 d = p - center - offset;
    const float c = std::cos(rotation), s = std::sin(rotation);
    const glm::vec2 r(d.x * c + d.y * s, -d.x * s + d.y * c);
    return center + r / std::max(scale, 1e-6f);
}

glm::vec4 sampleImage(const image::Image& img, const glm::vec2& p)
{
    if (img.width <= 0 || img.height <= 0)
        return glm::vec4(0.0f);
    const glm::vec2 q = p - 0.5f;
    const int x0 = int(std::floor(q.x)), y0 = int(std::floor(q.y));
    const float fx = q.x - float(x0), fy = q.y - float(y0);
    auto at = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= img.width || y >= img.height)
            return glm::vec4(0.0f);
        const float* t = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
        return glm::vec4(t[0] * t[3], t[1] * t[3], t[2] * t[3], t[3]);
    };
    const glm::vec4 m = glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
    return m.a > 0.0f ? glm::vec4(glm::vec3(m) / m.a, m.a) : glm::vec4(0.0f);
}

float Stencil::at(const glm::vec2& p, int w, int h) const
{
    if (!active())
        return 1.0f;
    // At scale 1 the image spans the buffer's height, centred.
    const glm::vec2 center(float(w) * 0.5f, float(h) * 0.5f);
    const glm::vec2 q = place.invert(p, center);
    const float k = float(image.height) / float(std::max(h, 1));
    glm::vec2 src = (q - center) * k + glm::vec2(float(image.width), float(image.height)) * 0.5f;
    if (tile)
    {
        src.x = std::fmod(std::fmod(src.x, float(image.width)) + float(image.width), float(image.width));
        src.y = std::fmod(std::fmod(src.y, float(image.height)) + float(image.height), float(image.height));
    }
    else if (src.x < 0.0f || src.y < 0.0f || src.x > float(image.width) || src.y > float(image.height))
        return invert ? 1.0f : 0.0f;
    const glm::vec4 c = sampleImage(image, src);
    const float v = std::clamp((c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f) * c.a, 0.0f, 1.0f);
    return invert ? 1.0f - v : v;
}

void ProjectionBuffer::resize(int width, int height)
{
    mWidth = std::max(1, width);
    mHeight = std::max(1, height);
    mPixels.assign(size_t(mWidth) * size_t(mHeight) * 4, 0.0f);
    placement = {};
    warp.clear();
    ++mVersion;
}

bool ProjectionBuffer::empty() const
{
    for (size_t i = 3; i < mPixels.size(); i += 4)
        if (mPixels[i] > 0.0f)
            return false;
    return true;
}

void ProjectionBuffer::clear()
{
    std::fill(mPixels.begin(), mPixels.end(), 0.0f);
    placement = {};
    warp.clear();
    ++mVersion;
}

template <class F> void ProjectionBuffer::forDab(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil, F&& fn)
{
    if (mWidth <= 0 || d.radius <= 0.0f)
        return;
    const int x0 = std::max(0, int(std::floor(d.center.x - d.radius))), x1 = std::min(mWidth - 1, int(std::ceil(d.center.x + d.radius)));
    const int y0 = std::max(0, int(std::floor(d.center.y - d.radius))), y1 = std::min(mHeight - 1, int(std::ceil(d.center.y + d.radius)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
            float w = f(double(glm::length(p - d.center) / d.radius)) * std::clamp(d.opacity, 0.0f, 1.0f);
            if (w <= 0.0f)
                continue;
            if (stencil && stencil->active())
                w *= stencil->at(p, mWidth, mHeight);
            if (w <= 0.0f)
                continue;
            fn(&mPixels[(size_t(y) * size_t(mWidth) + size_t(x)) * 4], p, w);
        }
    ++mVersion;
}

namespace
{
/** Lays value (straight rgb, coverage a) over a texel by w, as the paint kernel's Normal blend. */
void over(float* t, const glm::vec4& v, float w)
{
    const float k = w * v.a;
    const float a = t[3];
    const float na = k + a * (1.0f - k);
    for (int c = 0; c < 3; ++c)
        t[c] = na > 0.0f ? (v[c] * k + t[c] * a * (1.0f - k)) / na : v[c];
    t[3] = na;
}
} // namespace

void ProjectionBuffer::paint(const BufferDab& d, const brush::FalloffTable& f, const glm::vec3& color, const Stencil* stencil)
{
    forDab(d, f, stencil, [&](float* t, const glm::vec2&, float w) { over(t, glm::vec4(color, 1.0f), w); });
}

void ProjectionBuffer::erase(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil)
{
    forDab(d, f, stencil, [&](float* t, const glm::vec2&, float w) { t[3] *= 1.0f - w; });
}

void ProjectionBuffer::blur(const BufferDab& d, const brush::FalloffTable& f, float kernel, const Stencil* stencil)
{
    const std::vector<float> before = mPixels;
    const image::Image src{mWidth, mHeight, before};
    const int k = std::clamp(int(std::ceil(kernel)), 1, 32);
    const int step = std::max(1, k / 4);
    const float sigma = std::max(0.5f, float(k) * 0.5f);
    forDab(d, f, stencil, [&](float* t, const glm::vec2& p, float w) {
        glm::vec4 sum(0.0f);
        float weights = 0.0f;
        for (int dy = -k; dy <= k; dy += step)
            for (int dx = -k; dx <= k; dx += step)
            {
                const float g = std::exp(-float(dx * dx + dy * dy) / (2.0f * sigma * sigma));
                const glm::vec4 s = sampleImage(src, p + glm::vec2(float(dx), float(dy)));
                sum += glm::vec4(glm::vec3(s) * s.a, s.a) * g;
                weights += g;
            }
        const glm::vec4 b = sum / weights;
        const glm::vec4 blurred(b.a > 0.0f ? glm::vec3(b) / b.a : glm::vec3(t[0], t[1], t[2]), b.a);
        for (int c = 0; c < 4; ++c)
            t[c] = t[c] + (blurred[c] - t[c]) * w;
    });
}

void ProjectionBuffer::smudge(const BufferDab& d, const brush::FalloffTable& f, const Stencil* stencil)
{
    const std::vector<float> before = mPixels;
    const image::Image src{mWidth, mHeight, before};
    const glm::vec2 delta = d.center - d.previous;
    forDab(d, f, stencil, [&](float* t, const glm::vec2& p, float w) {
        const glm::vec4 s = sampleImage(src, p - delta);
        for (int c = 0; c < 4; ++c)
            t[c] = t[c] + (s[c] - t[c]) * w;
    });
}

void ProjectionBuffer::clone(const BufferDab& d, const brush::FalloffTable& f, const image::Image& source, const glm::vec2& offset, const Stencil* stencil)
{
    forDab(d, f, stencil, [&](float* t, const glm::vec2& p, float w) {
        const glm::vec4 s = sampleImage(source, p + offset);
        over(t, s, w);
    });
}

void ProjectionBuffer::place(const image::Image& img, const glm::vec4& rect, float opacity)
{
    LILA_ZONE();
    if (img.width <= 0 || mWidth <= 0)
        return;
    glm::vec4 r = rect;
    if (r.z <= 0.0f || r.w <= 0.0f)
    {
        const float s = std::min(float(mWidth) / float(img.width), float(mHeight) / float(img.height));
        r = {(float(mWidth) - img.width * s) * 0.5f, (float(mHeight) - img.height * s) * 0.5f, img.width * s, img.height * s};
    }
    const int x0 = std::max(0, int(std::floor(r.x))), x1 = std::min(mWidth - 1, int(std::ceil(r.x + r.z)));
    const int y0 = std::max(0, int(std::floor(r.y))), y1 = std::min(mHeight - 1, int(std::ceil(r.y + r.w)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
            const glm::vec2 q((p.x - r.x) / r.z * float(img.width), (p.y - r.y) / r.w * float(img.height));
            if (q.x < 0.0f || q.y < 0.0f || q.x > float(img.width) || q.y > float(img.height))
                continue;
            glm::vec4 s = sampleImage(img, glm::clamp(q, glm::vec2(0.5f), glm::vec2(float(img.width), float(img.height)) - 0.5f));
            over(&mPixels[(size_t(y) * size_t(mWidth) + size_t(x)) * 4], s, std::clamp(opacity, 0.0f, 1.0f));
        }
    ++mVersion;
}

bool ProjectionBuffer::transformed() const
{
    if (!placement.identity())
        return true;
    for (const glm::vec2& o : warp)
        if (o != glm::vec2(0.0f))
            return true;
    return false;
}

glm::vec2& ProjectionBuffer::warpPoint(int i, int j)
{
    const size_t n = size_t(warpCells + 1);
    if (warp.size() != n * n)
        warp.assign(n * n, glm::vec2(0.0f));
    return warp[size_t(std::clamp(j, 0, warpCells)) * n + size_t(std::clamp(i, 0, warpCells))];
}

glm::vec2 ProjectionBuffer::warpRest(int i, int j) const
{
    return {float(mWidth) * float(i) / float(warpCells), float(mHeight) * float(j) / float(warpCells)};
}

glm::vec2 ProjectionBuffer::warpOffset(const glm::vec2& p) const
{
    const size_t n = size_t(warpCells + 1);
    if (warp.size() != n * n || mWidth <= 0)
        return glm::vec2(0.0f);
    const float gx = std::clamp(p.x / float(mWidth) * float(warpCells), 0.0f, float(warpCells) - 1e-4f);
    const float gy = std::clamp(p.y / float(mHeight) * float(warpCells), 0.0f, float(warpCells) - 1e-4f);
    const int i = int(gx), j = int(gy);
    const float fx = gx - float(i), fy = gy - float(j);
    auto w = [&](int a, int b) { return warp[size_t(b) * n + size_t(a)]; };
    return glm::mix(glm::mix(w(i, j), w(i + 1, j), fx), glm::mix(w(i, j + 1), w(i + 1, j + 1), fx), fy);
}

image::Image ProjectionBuffer::resolved() const
{
    LILA_ZONE();
    image::Image out{mWidth, mHeight, {}};
    if (!transformed())
    {
        out.rgba = mPixels;
        return out;
    }
    out.rgba.assign(mPixels.size(), 0.0f);
    const image::Image src{mWidth, mHeight, mPixels};
    const glm::vec2 center(float(mWidth) * 0.5f, float(mHeight) * 0.5f);
    for (int y = 0; y < mHeight; ++y)
        for (int x = 0; x < mWidth; ++x)
        {
            const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
            // The warp moves content by its control offsets: read from where the content came from.
            const glm::vec2 q = placement.invert(p - warpOffset(p), center);
            const glm::vec4 s = sampleImage(src, q);
            float* t = &out.rgba[(size_t(y) * size_t(mWidth) + size_t(x)) * 4];
            t[0] = s.r;
            t[1] = s.g;
            t[2] = s.b;
            t[3] = std::clamp(s.a, 0.0f, 1.0f);
        }
    return out;
}

void ProjectionBuffer::bake()
{
    if (!transformed())
        return;
    mPixels = resolved().rgba;
    placement = {};
    warp.clear();
    ++mVersion;
}
} // namespace lila::tex::paint
