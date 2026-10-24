#include "brush/Stroke.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>

namespace lila::brush
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/** Deterministic 0..1 for a dab and a channel of variation. */
double random01(uint32_t seed, uint32_t index, uint32_t channel)
{
    return double(hash(seed * 0x9E3779B9u ^ hash(index * 4u + channel))) / 4294967296.0;
}

glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}
} // namespace

// ------------------------------------------------------------------------------------------------ StrokeSampler

void StrokeSampler::begin(const BrushSettings& s, const StrokeInput& in, double radiusPx)
{
    mActive = true;
    mMode = s.mode;
    mSpacing = s.spacing;
    mLazy = s.lazyRadius;
    mSmoothing = s.smoothing;
    mJitter = s.jitter;
    mAngle = s.angle * kPi / 180.0;
    mFollow = s.followStroke;
    mSeed = s.seed;
    setRadius(radiusPx);
    mStart = mCursor = mBrush = mLast = in.position;
    mPressure = mLastPressure = in.pressure;
    mDir = glm::dvec2(0.0);
    mDistance = 0.0;
    mIndex = 0;
    mOut.clear();
    if (mMode == StrokeMode::Freehand)
        push(in.position, in.pressure, glm::dvec2(0.0));
}

void StrokeSampler::setRadius(double radiusPx)
{
    mRadius = std::max(radiusPx, 1e-6);
    mStep = std::max(1.0, mRadius * mSpacing);
}

void StrokeSampler::move(const StrokeInput& in)
{
    if (!mActive)
        return;
    mCursor = in.position;
    mPressure = in.pressure;
    if (mMode == StrokeMode::Line)
    {
        mBrush = mCursor;
        return;
    }
    glm::dvec2 target = mCursor;
    if (mLazy > 0.0)
    {
        // The brush hangs on a string of length lazy behind the cursor and only moves when it pulls taut.
        const glm::dvec2 d = mCursor - mBrush;
        const double len = glm::length(d);
        target = len > mLazy ? mBrush + d * ((len - mLazy) / len) : mBrush;
    }
    if (mSmoothing > 0.0)
        target = mBrush + (target - mBrush) * (1.0 - mSmoothing);
    mBrush = target;
    stepTo(mBrush, in.pressure);
}

void StrokeSampler::end()
{
    if (!mActive)
        return;
    if (mMode == StrokeMode::Line)
    {
        const glm::dvec2 d = mCursor - mStart;
        const double len = glm::length(d);
        push(mStart, mLastPressure, len > 0.0 ? d / len : glm::dvec2(0.0));
        stepTo(mCursor, mPressure);
    }
    mActive = false;
}

void StrokeSampler::cancel()
{
    mActive = false;
    mOut.clear();
}

void StrokeSampler::stepTo(glm::dvec2 target, double pressure)
{
    const glm::dvec2 d = target - mLast;
    const double len = glm::length(d);
    if (len <= 0.0)
        return;
    const glm::dvec2 dir = d / len;
    const int count = int(len / mStep);
    for (int k = 1; k <= count; ++k)
    {
        const double f = k * mStep / len;
        mDistance += mStep;
        push(mLast + d * f, mLastPressure + (pressure - mLastPressure) * f, dir);
    }
    if (count > 0)
    {
        const double f = count * mStep / len;
        mLast += d * f;
        mLastPressure += (pressure - mLastPressure) * f;
    }
}

void StrokeSampler::push(glm::dvec2 at, double pressure, glm::dvec2 dir)
{
    StrokeSample s;
    s.index = mIndex++;
    s.position = at;
    s.pressure = std::clamp(pressure, 0.0, 1.0);
    s.direction = dir;
    s.distance = mDistance;
    if (glm::length(dir) > 0.0)
        mDir = dir;
    s.angle = mAngle;
    if (mFollow && glm::length(mDir) > 0.0)
        s.angle += std::atan2(-mDir.y, mDir.x);
    if (mJitter.any())
    {
        if (mJitter.position > 0.0)
        {
            const double a = random01(mSeed, s.index, 0) * 2.0 * kPi, r = std::sqrt(random01(mSeed, s.index, 1));
            s.position += glm::dvec2(std::cos(a), std::sin(a)) * (r * mJitter.position * mRadius);
        }
        s.size = 1.0 - mJitter.size * random01(mSeed, s.index, 2);
        s.opacity = 1.0 - mJitter.opacity * random01(mSeed, s.index, 3);
        s.angle += (random01(mSeed, s.index, 4) * 2.0 - 1.0) * mJitter.angle * kPi / 180.0;
    }
    mOut.push_back(s);
}

std::vector<StrokeSample> StrokeSampler::take() { return std::exchange(mOut, {}); }

// ------------------------------------------------------------------------------------------------ dabs

double radiusPixels(const BrushSettings& s, const SurfaceHit& hit)
{
    if (s.radiusUnit == RadiusUnit::Screen)
        return s.radius;
    return s.worldRadius * hit.worldToLocal / std::max(hit.pixelSize, 1e-12);
}

Dab makeDab(const BrushSettings& s, const StrokeSample& sample, const SurfaceHit& hit, const Dab* previous)
{
    Dab d;
    d.center = hit.center;
    d.normal = glm::length(hit.normal) > 0.0 ? glm::normalize(hit.normal) : glm::dvec3(0.0, 1.0, 0.0);
    d.viewDir = hit.viewDir;
    d.eye = hit.eye;
    d.pressure = sample.pressure;
    // A light touch still leaves a dab: size never drops below 5%.
    const double size = std::max(0.05, s.pressureSize.map(sample.pressure)) * sample.size;
    const double base = s.radiusUnit == RadiusUnit::Screen ? s.radius * hit.pixelSize : s.worldRadius * hit.worldToLocal;
    d.radius = base * size;
    d.opacity = s.pressureOpacity.map(sample.pressure) * sample.opacity;
    d.flow = s.flow * s.pressureFlow.map(sample.pressure);
    d.direction = previous ? hit.center - previous->center : glm::dvec3(0.0);
    d.angle = sample.angle;
    d.roundness = s.roundness;
    d.alpha = s.alpha;
    d.screen = sample.position;
    d.index = sample.index;
    if (!roundTip(d))
    {
        glm::dvec3 r = hit.right - d.normal * glm::dot(d.normal, hit.right);
        if (glm::length(r) < 1e-9)
            r = glm::cross(d.normal, std::abs(d.normal.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        d.tangent = rotateAbout(glm::normalize(r), d.normal, d.angle);
    }
    return d;
}

// ------------------------------------------------------------------------------------------------ Stroke

bool Stroke::begin(const BrushSettings& s, const StrokeInput& in, SurfaceQuery query, DabSink& sink, bool requireHit)
{
    mSettings = s;
    mSettings.ensureAlpha();
    mQuery = std::move(query);
    mSink = &sink;
    mDabs.clear();
    double radiusPx = s.radius;
    SurfaceHit hit;
    const bool over = mQuery && mQuery(in.position, hit);
    if (requireHit && s.mode == StrokeMode::Freehand && !over)
        return false;
    if (over)
        radiusPx = radiusPixels(mSettings, hit);
    mSampler.begin(mSettings, in, radiusPx);
    flush();
    return true;
}

void Stroke::move(const StrokeInput& in)
{
    if (!mSampler.active())
        return;
    mSampler.move(in);
    flush();
}

void Stroke::end()
{
    if (!mSampler.active())
        return;
    mSampler.end();
    flush();
    mSink = nullptr;
}

void Stroke::cancel()
{
    mSampler.cancel();
    mSink = nullptr;
}

void Stroke::flush()
{
    LILA_ZONE();

    for (const StrokeSample& s : mSampler.take())
    {
        SurfaceHit hit;
        if (!mQuery || !mQuery(s.position, hit))
            continue;
        if (mSettings.radiusUnit == RadiusUnit::World)
            mSampler.setRadius(radiusPixels(mSettings, hit));
        Dab d = makeDab(mSettings, s, hit, mDabs.empty() ? nullptr : &mDabs.back());
        mDabs.push_back(d);
        if (mSink)
            mSink->dab(d);
    }
}
} // namespace lila::brush
