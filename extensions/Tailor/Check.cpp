// Pattern check: what a pattern maker would flag on a garment, each with a tip on what to change.

#include "Blocks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace tailor
{
namespace
{
float runLen(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    if (n < 2)
        return 0.f;
    float len = 0.f;
    for (int i = from % n; i != to % n; i = (i + 1) % n)
        len += std::hypot(o[size_t((i + 1) % n)][0] - o[size_t(i)][0], o[size_t((i + 1) % n)][1] - o[size_t(i)][1]);
    return len;
}

float sideLen(const Garment& g, const std::vector<SeamSide>& sides)
{
    float len = 0.f;
    for (const SeamSide& s : sides)
        if (s.panel >= 0 && size_t(s.panel) < g.panels.size())
            len += runLen(g.panels[size_t(s.panel)].outline, s.from, s.to);
    return len;
}

std::string cm(float metres)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f cm", metres * 100.f);
    return buf;
}

std::string pct(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f%%", f * 100.f);
    return buf;
}

bool isSleeve(const Garment& g, const std::vector<SeamSide>& sides)
{
    for (const SeamSide& s : sides)
        if (s.panel >= 0 && size_t(s.panel) < g.panels.size() && g.panels[size_t(s.panel)].name.rfind("Sleeve", 0) == 0)
            return true;
    return false;
}

/** How far the run bulges from the straight line between its ends: a sleeve cap's height. */
float runHeight(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    const Vec2 a = o[size_t(from % n)], b = o[size_t(to % n)];
    const float dx = b[0] - a[0], dy = b[1] - a[1], l = std::hypot(dx, dy);
    if (l < 1e-6f)
        return 0.f;
    float h = 0.f;
    for (int i = from % n; i != to % n; i = (i + 1) % n)
        h = std::max(h, std::abs((o[size_t(i)][0] - a[0]) * dy - (o[size_t(i)][1] - a[1]) * dx) / l);
    return h;
}

bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d)
{
    auto orient = [](const Vec2& p, const Vec2& q, const Vec2& r) { return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]); };
    const float d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)) && d1 != 0.f && d2 != 0.f && d3 != 0.f && d4 != 0.f;
}
} // namespace

std::vector<PatternIssue> checkPattern(const Garment& g, const Body* body)
{
    std::vector<PatternIssue> out;
    auto add = [&](PatternIssue::Level level, int panel, int seam, std::string message, std::string tip) {
        out.push_back({level, panel, seam, std::move(message), std::move(tip)});
    };

    for (size_t k = 0; k < g.panels.size(); ++k)
    {
        const Panel& p = g.panels[k];
        const auto& o = p.outline;
        if (o.size() < 3)
        {
            add(PatternIssue::Error, int(k), -1, p.name + " has fewer than 3 points", "Delete the panel or add points to its outline.");
            continue;
        }
        bool crossing = false;
        for (size_t i = 0; i < o.size() && !crossing; ++i)
            for (size_t j = i + 2; j < o.size() && !crossing; ++j)
                if (!(i == 0 && j + 1 == o.size()))
                    crossing = segmentsCross(o[i], o[(i + 1) % o.size()], o[j], o[(j + 1) % o.size()]);
        if (crossing)
            add(PatternIssue::Error, int(k), -1, p.name + "'s outline crosses itself", "Drag the crossing points apart in the Pattern Editor.");
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (const Vec2& v : o)
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), y0 = std::min(y0, v[1]), y1 = std::max(y1, v[1]);
        if (std::min(x1 - x0, y1 - y0) < 2.f * p.particleDistance)
            add(PatternIssue::Warning, int(k), -1, p.name + " is only " + cm(std::min(x1 - x0, y1 - y0)) + " across, too small for its " +
                                                       cm(p.particleDistance) + " cloth",
                "Lower particleDistance on the garment, or for this panel, so it gets at least two rows of cloth.");
        // Grain should run along the length or on the true bias; anything between hangs and stretches unevenly.
        const float off = std::fmod(std::abs(p.grainAngle) * 180.f / 3.14159265f, 90.f);
        if (off > 8.f && off < 82.f && std::abs(off - 45.f) > 8.f)
            add(PatternIssue::Info, int(k), -1, p.name + "'s grain is " + std::to_string(int(off)) + " degrees off",
                "Set the grain along the panel's length, or at 45 degrees for a bias cut that drapes softly.");
    }

    for (size_t s = 0; s < g.seams.size(); ++s)
    {
        const Seam& seam = g.seams[s];
        const float la = sideLen(g, seam.a), lb = sideLen(g, seam.b);
        if (la <= 0.f || lb <= 0.f)
            continue;
        const bool sleeveCap = isSleeve(g, seam.a) != isSleeve(g, seam.b) && (seam.a.size() > 1 || seam.b.size() > 1);
        const float cap = isSleeve(g, seam.a) ? la : lb, hole = isSleeve(g, seam.a) ? lb : la;
        if (sleeveCap)
        {
            const float ease = cap / hole - 1.f;
            if (ease < -0.005f)
                add(PatternIssue::Error, -1, int(s), "Sleeve cap is " + cm(hole - cap) + " shorter than the armhole",
                    "The sleeve will pull the armhole in. Widen the sleeve at the bicep or raise the cap until it is 1 to 4 cm longer.");
            else if (ease > 0.06f && ease < 0.15f)
                add(PatternIssue::Warning, -1, int(s), "Sleeve cap has " + cm(cap - hole) + " of ease (" + pct(ease) + ")",
                    "Too much to ease in smoothly except in wool; it will pucker. Lower the cap, or gather it on purpose as a puff.");
            else if (ease >= 0.15f)
                add(PatternIssue::Info, -1, int(s), "Sleeve cap gathers " + cm(cap - hole) + " into the armhole",
                    "A gathered cap (puff, leg-of-mutton). For a smooth set-in sleeve keep the ease at 1 to 4 cm.");

            // A low, flat cap is a dropped-shoulder sleeve: the body's shoulder has to come down onto the arm to meet it.
            const SeamSide& sl = isSleeve(g, seam.a) ? seam.a.front() : seam.b.front();
            const float h = runHeight(g.panels[size_t(sl.panel)].outline, sl.from, sl.to);
            const float width = std::hypot(g.panels[size_t(sl.panel)].outline[size_t(sl.to)][0] - g.panels[size_t(sl.panel)].outline[size_t(sl.from)][0],
                                           g.panels[size_t(sl.panel)].outline[size_t(sl.to)][1] - g.panels[size_t(sl.panel)].outline[size_t(sl.from)][1]);
            // A cap cut in several pieces (a slit sleeve) can't be judged from one of them.
            const bool wholeCap = (isSleeve(g, seam.a) ? seam.a : seam.b).size() == 1;
            if (body && wholeCap && width > 0.f && h < 0.3f * width)
            {
                float tipX = 0.f;
                for (const SeamSide& bs : isSleeve(g, seam.a) ? seam.b : seam.a)
                {
                    const auto& o = g.panels[size_t(bs.panel)].outline;
                    const Vec2& a = o[size_t(bs.from % int(o.size()))];
                    const Vec2& b = o[size_t(bs.to % int(o.size()))];
                    tipX = std::max(tipX, std::abs(a[1] > b[1] ? a[0] : b[0]));
                }
                const float need = body->shoulderWidth * 0.5f + 0.04f;
                if (tipX < need)
                    add(PatternIssue::Warning, -1, int(s), "The sleeve cap is low and flat but the shoulder ends at the shoulder point",
                        "A flat cap is a dropped-shoulder sleeve: carry the shoulder line about " + cm(need + 0.04f - tipX) +
                            " past the shoulder point and lower the armhole to match, or raise the cap for a set-in sleeve.");
            }
            continue;
        }
        const float diff = std::abs(la - lb), ratio = std::max(la, lb) / std::min(la, lb) - 1.f;
        if (seam.shrink < 1.f || ratio >= 0.15f)
        {
            if (ratio >= 0.15f)
                add(PatternIssue::Info, -1, int(s), "Seam " + std::to_string(s) + " gathers " + cm(std::max(la, lb)) + " into " + cm(std::min(la, lb)),
                    "Intended gathers are fine; otherwise make both sides the same length.");
        }
        else if (diff > 0.01f && ratio > 0.02f)
            add(PatternIssue::Warning, -1, int(s), "Seam " + std::to_string(s) + " sides differ by " + cm(diff),
                "Sewn sides should match within a few millimetres unless one is eased or gathered. True the seam: lengthen the short "
                "side or shorten the long one.");
    }

    if (body)
    {
        // Girth round the chest: the width of every innermost panel at chest height, as the pattern is placed.
        float girth = 0.f;
        int crossing = 0;
        int lowest = 1 << 30;
        for (const Panel& p : g.panels)
            lowest = std::min(lowest, p.layer);
        for (const Panel& p : g.panels)
        {
            if (p.layer != lowest || p.name.rfind("Sleeve", 0) == 0 || std::abs(p.up[1]) < 0.9f)
                continue;
            const float y = body->chestY() - p.origin[1];
            float lo = 1e9f, hi = -1e9f;
            const auto& o = p.outline;
            for (size_t i = 0; i < o.size(); ++i)
            {
                const Vec2 &a = o[i], &b = o[(i + 1) % o.size()];
                if ((a[1] - y) * (b[1] - y) > 0.f || a[1] == b[1])
                    continue;
                const float x = a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]);
                lo = std::min(lo, x), hi = std::max(hi, x);
            }
            if (hi > lo)
            {
                girth += hi - lo;
                ++crossing;
            }
        }
        if (crossing >= 2)
        {
            const float ease = girth / body->chest - 1.f;
            if (ease < 0.f)
                add(PatternIssue::Error, -1, -1, "The garment is " + cm(body->chest - girth) + " smaller round the chest than the body",
                    "It can't close round the body. Add width to the side seams, or use a stretch fabric and expect it to pull.");
            else if (ease < 0.04f)
                add(PatternIssue::Warning, -1, -1, "Only " + cm(girth - body->chest) + " of ease round the chest",
                    "Woven fabric needs about 8 cm to move in; this fits only a knit. Add width at the side seams.");
        }
    }
    return out;
}
} // namespace tailor
