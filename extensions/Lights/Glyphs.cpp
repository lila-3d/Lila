#include "Glyphs.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

using namespace lila;
using scene::lights::Kind;

namespace lights_ext
{
namespace
{
constexpr int kCircleSteps = 32;
constexpr double kSize = 10.0;  // cm, the glyph's base radius before localScale
constexpr double kReach = 50.0; // cm, how far spot cones and direction arrows run

/** A circle in the plane spanned by u and v. */
void circle(std::vector<Segment>& out, glm::dvec3 center, glm::dvec3 u, glm::dvec3 v, double radius)
{
    glm::dvec3 prev = center + u * radius;
    for (int i = 1; i <= kCircleSteps; ++i)
    {
        const double a = 2.0 * std::numbers::pi * i / kCircleSteps;
        glm::dvec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
        out.emplace_back(prev, p);
        prev = p;
    }
}

void arrow(std::vector<Segment>& out, glm::dvec3 from, double length)
{
    const glm::dvec3 tip = from + glm::dvec3(0, 0, -length);
    const double head = length * 0.15;
    out.emplace_back(from, tip);
    out.emplace_back(tip, tip + glm::dvec3(head, 0, head));
    out.emplace_back(tip, tip + glm::dvec3(-head, 0, head));
}

const glm::dvec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};

std::vector<Segment> localGlyph(const scene::lights::Settings& s)
{
    std::vector<Segment> out;
    switch (s.kind)
    {
    case Kind::Point:
        circle(out, {}, X, Y, kSize);
        circle(out, {}, Y, Z, kSize);
        circle(out, {}, X, Z, kSize);
        for (glm::dvec3 d : {X, -X, Y, -Y, Z, -Z})
            out.emplace_back(d * (kSize * 1.3), d * (kSize * 2.0));
        break;
    case Kind::Spot:
    {
        circle(out, {}, X, Y, kSize * 0.4);
        const glm::dvec3 end{0, 0, -kReach};
        const double outer = kReach * std::tan(glm::radians(std::clamp(s.outerConeAngle, 0.5, 89.0)));
        circle(out, end, X, Y, outer);
        for (glm::dvec3 d : {X, -X, Y, -Y})
            out.emplace_back(glm::dvec3(0.0), end + d * outer);
        if (s.innerConeAngle > 0.0 && s.innerConeAngle < s.outerConeAngle)
            circle(out, end, X, Y, kReach * std::tan(glm::radians(std::min(s.innerConeAngle, 89.0))));
        break;
    }
    case Kind::Directional:
        circle(out, {}, X, Y, kSize);
        arrow(out, {}, kReach);
        for (glm::dvec3 d : {X, -X, Y, -Y})
            arrow(out, d * kSize, kReach * 0.6);
        break;
    case Kind::Area:
    {
        const double w = std::max(s.width, 1e-3) * 0.5, h = std::max(s.height, 1e-3) * 0.5;
        const glm::dvec3 c[4] = {{-w, -h, 0}, {w, -h, 0}, {w, h, 0}, {-w, h, 0}};
        for (int i = 0; i < 4; ++i)
            out.emplace_back(c[i], c[(i + 1) % 4]);
        out.emplace_back(c[0], c[2]);
        out.emplace_back(c[1], c[3]);
        arrow(out, {}, std::max(kReach * 0.5, std::max(w, h)));
        break;
    }
    case Kind::Disc:
    {
        const double r = std::max(s.width, 1e-3) * 0.5;
        circle(out, {}, X, Y, r);
        arrow(out, {}, std::max(kReach * 0.5, r));
        break;
    }
    case Kind::Dome:
    {
        const double r = kReach * 2.0;
        circle(out, {}, X, Z, r);
        // Upper half only for the two upright rings: a dome, not a sphere.
        for (auto [u, v] : {std::pair{X, Y}, std::pair{Z, Y}})
        {
            glm::dvec3 prev = u * r;
            for (int i = 1; i <= kCircleSteps / 2; ++i)
            {
                const double a = std::numbers::pi * i / (kCircleSteps / 2);
                glm::dvec3 p = (u * std::cos(a) + v * std::sin(a)) * r;
                out.emplace_back(prev, p);
                prev = p;
            }
        }
        break;
    }
    }
    return out;
}
} // namespace

std::vector<Segment> glyph(const scene::Node& light, const glm::dmat4& world)
{
    LILA_ZONE();

    std::vector<Segment> out = localGlyph(scene::lights::settings(light));
    const glm::dmat4 m = world * glm::scale(glm::dmat4(1.0), light.as<glm::dvec3>(scene::Locator::kLocalScale));
    for (auto& [a, b] : out)
    {
        a = glm::dvec3(m * glm::dvec4(a, 1.0));
        b = glm::dvec3(m * glm::dvec4(b, 1.0));
    }
    return out;
}

void draw(ui::drawOverrides::DrawContext& c)
{
    LILA_ZONE();

    for (auto& [a, b] : glyph(c.node, c.world))
    {
        c.frame.lines.push_back({glm::vec3(a), c.color});
        c.frame.lines.push_back({glm::vec3(b), c.color});
    }
}

std::optional<double> pick(const ui::drawOverrides::PickContext& c)
{
    LILA_ZONE();

    QRectF box = QRectF(c.a, c.b).normalized();
    std::optional<double> best;
    for (auto& [a, b] : glyph(c.node, c.world))
    {
        auto p0 = c.view.project(a), p1 = c.view.project(b);
        if (!p0 || !p1)
            continue;
        if (!c.click)
        {
            if (box.contains(*p0) || box.contains(*p1))
                return 0.0;
            continue;
        }
        QPointF d = *p1 - *p0;
        double len2 = QPointF::dotProduct(d, d);
        double t = len2 > 0 ? std::clamp(QPointF::dotProduct(c.a - *p0, d) / len2, 0.0, 1.0) : 0.0;
        QPointF q = *p0 + d * t - c.a;
        double dist = std::hypot(q.x(), q.y());
        if (!best || dist < *best)
            best = dist;
    }
    return best;
}
} // namespace lights_ext
