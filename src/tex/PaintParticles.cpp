#include "tex/PaintParticles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lila::tex::paint
{
namespace
{
uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/** Uniform in [0, 1), from a seed and up to three counters. */
float rnd(uint32_t seed, uint32_t a, uint32_t b, uint32_t c)
{
    const uint64_t h = mix(mix(mix(uint64_t(seed) << 32 | a) ^ b) ^ (uint64_t(c) << 17));
    return float(h >> 40) / float(1ull << 24);
}

/** A point on the surface within reach of p, as a dab; false when there is none. */
bool snap(const PaintMesh& mesh, const glm::vec3& p, float reach, float radius, float opacity, Dab& out)
{
    geo::BvhClosest hit;
    if (!mesh.bvh->closestPoint(glm::dvec3(p), hit, std::max(double(reach), 1e-6)) || hit.triangle >= mesh.triangleCount() ||
        !mesh.paintable[hit.triangle])
        return false;
    out.triangle = uint32_t(hit.triangle);
    out.u = float(hit.bary.y);
    out.v = float(hit.bary.z);
    out.radius = radius;
    out.pressure = 1.0f;
    out.opacity = std::clamp(opacity, 0.0f, 1.0f);
    out.angle = 0.0f;
    return true;
}

glm::vec3 tangentOf(const glm::vec3& v, const glm::vec3& n)
{
    const glm::vec3 t = v - n * glm::dot(v, n);
    const float l = glm::length(t);
    return l > 1e-6f ? t / l : glm::vec3(0.0f);
}
} // namespace

nlohmann::json ParticleSpec::toJson() const
{
    return {{"kind", kind},     {"count", count}, {"length", length}, {"size", size},
            {"spread", spread}, {"rate", rate},   {"seed", seed},     {"down", {down.x, down.y, down.z}}};
}

ParticleSpec ParticleSpec::fromJson(const nlohmann::json& j)
{
    ParticleSpec s;
    if (!j.is_object())
        return s;
    s.kind = j.value("kind", s.kind);
    if (s.kind != "drips" && s.kind != "rain" && s.kind != "sparks")
        throw std::invalid_argument("particles kind must be drips, rain or sparks, not " + s.kind);
    s.count = std::clamp(j.value("count", s.count), 0, 64);
    s.length = std::clamp(j.value("length", s.length), 0.0f, 100.0f);
    s.size = std::clamp(j.value("size", s.size), 0.01f, 2.0f);
    s.spread = std::clamp(j.value("spread", s.spread), 0.0f, 10.0f);
    s.rate = std::clamp(j.value("rate", s.rate), 0.0f, 1.0f);
    s.seed = j.value("seed", s.seed);
    if (j.contains("down") && j["down"].is_array() && j["down"].size() == 3)
        s.down = {j["down"][0].get<float>(), j["down"][1].get<float>(), j["down"][2].get<float>()};
    if (glm::length(s.down) < 1e-6f)
        s.down = {0.0f, -1.0f, 0.0f};
    s.down = glm::normalize(s.down);
    return s;
}

std::vector<Dab> emitParticles(const PaintMesh& mesh, const Dab& from, uint32_t index, const ParticleSpec& spec)
{
    std::vector<Dab> out;
    if (from.triangle >= mesh.triangleCount() || spec.count <= 0 || rnd(spec.seed, index, 0xFFFFu, 0) >= spec.rate)
        return out;
    const float R = from.radius;
    const glm::vec3 c = mesh.point(from), n0 = mesh.normal(from.triangle, from.u, from.v);
    const glm::vec3 t0 = tipFrame(n0), b0 = glm::cross(n0, t0);
    for (int i = 0; i < spec.count; ++i)
    {
        const uint32_t pi = uint32_t(i);
        const float ang = rnd(spec.seed, index, pi, 1) * 6.2831853f;
        const float dist = std::sqrt(rnd(spec.seed, index, pi, 2)) * spec.spread * R * (spec.kind == "rain" ? 2.0f : 1.0f);
        const glm::vec3 start = c + (t0 * std::cos(ang) + b0 * std::sin(ang)) * dist;
        float r = R * spec.size * (0.6f + 0.8f * rnd(spec.seed, index, pi, 3));
        Dab d;
        if (!snap(mesh, start, R * (spec.spread + 1.0f) * 2.0f, r, from.opacity, d))
            continue;
        d.copy = from.copy;
        out.push_back(d);
        glm::vec3 p = mesh.point(d);
        glm::vec3 n = mesh.normal(d.triangle, d.u, d.v);
        glm::vec3 dir;
        float travel = spec.length * R;
        if (spec.kind == "sparks")
        {
            const float a = rnd(spec.seed, index, pi, 4) * 6.2831853f;
            dir = tangentOf(t0 * std::cos(a) + b0 * std::sin(a), n);
            travel *= 0.5f + rnd(spec.seed, index, pi, 5);
        }
        else
        {
            dir = tangentOf(spec.down, n);
            if (spec.kind == "rain")
                travel *= 0.15f * rnd(spec.seed, index, pi, 5);
            else
                travel *= 0.4f + 0.6f * rnd(spec.seed, index, pi, 5);
        }
        if (glm::length(dir) == 0.0f)
            continue;
        float opacity = from.opacity, walked = 0.0f;
        for (uint32_t step = 0; walked < travel && step < 512; ++step)
        {
            const float len = std::max(r * 0.5f, R * 0.02f);
            // Drips meander a little; sparks fly straight and fade.
            const float wobble = spec.kind == "drips" ? (rnd(spec.seed, index, pi, 100 + step) - 0.5f) * 0.6f : 0.0f;
            const glm::vec3 side = glm::cross(n, dir);
            p += (dir + side * wobble) * len;
            walked += len;
            if (spec.kind == "sparks")
            {
                r *= 0.97f;
                opacity = from.opacity * (1.0f - walked / travel);
            }
            else if (spec.kind == "drips")
                r *= 0.99f;
            Dab next;
            if (!snap(mesh, p, len * 4.0f, r, opacity, next))
                break;
            next.copy = from.copy;
            out.push_back(next);
            p = mesh.point(next);
            n = mesh.normal(next.triangle, next.u, next.v);
            const glm::vec3 along = spec.kind == "sparks" ? dir : spec.down;
            dir = tangentOf(along, n);
            if (glm::length(dir) == 0.0f)
                break;
        }
    }
    return out;
}

std::vector<Dab> withParticles(const PaintMesh& mesh, std::span<const Dab> dabs, uint32_t firstIndex, const ParticleSpec& spec)
{
    std::vector<Dab> out;
    for (size_t i = 0; i < dabs.size(); ++i)
    {
        out.push_back(dabs[i]);
        std::vector<Dab> extra = emitParticles(mesh, dabs[i], firstIndex + uint32_t(i), spec);
        out.insert(out.end(), extra.begin(), extra.end());
    }
    return out;
}
} // namespace lila::tex::paint
