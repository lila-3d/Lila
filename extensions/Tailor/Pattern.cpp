#include "Pattern.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace tailor
{
namespace
{
float distance2d(const Vec2& a, const Vec2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

bool validPanel(const Garment& g, int panel) { return panel >= 0 && size_t(panel) < g.panels.size(); }

float outlineRunLength(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    if (n < 2)
        return 0.f;
    float len = 0.f;
    for (int i = from % n; i != to % n; i = (i + 1) % n)
        len += distance2d(o[size_t(i)], o[size_t((i + 1) % n)]);
    return len;
}

bool isSleevePanel(const Panel& p) { return p.name.rfind("Sleeve", 0) == 0; }

bool allSleeve(const Garment& g, const std::vector<SeamSide>& sides)
{
    return !sides.empty() && std::all_of(sides.begin(), sides.end(), [&](const SeamSide& s) {
        return validPanel(g, s.panel) && isSleevePanel(g.panels[size_t(s.panel)]);
    });
}

bool anySleeve(const Garment& g, const std::vector<SeamSide>& sides)
{
    return std::any_of(sides.begin(), sides.end(), [&](const SeamSide& s) { return validPanel(g, s.panel) && isSleevePanel(g.panels[size_t(s.panel)]); });
}

int twinIndex(int k, int n) { return n > 0 ? (n - k % n) % n : 0; }

Vec2 mirrored(const Vec2& v) { return {-v[0], v[1]}; }

bool removeEmptySeams(Garment& g)
{
    const size_t before = g.seams.size();
    std::erase_if(g.seams, [](const Seam& s) {
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
                if (side.from == side.to)
                    return true;
        return false;
    });
    return g.seams.size() != before;
}

/** Applies an old-to-new outline index map (-1 for a deleted point) to a panel's seams and notches. A seam ending on a
 *  deleted point ends at the next surviving one. Returns the number of seams left without length and removed. */
int remapPanel(Garment& g, int panel, const std::vector<int>& map)
{
    const int n = int(map.size());
    auto next = [&](int v) {
        for (int k = 0; k < n; ++k)
            if (const int m = map[size_t((v + k) % n)]; m >= 0)
                return m;
        return 0;
    };
    for (Seam& s : g.seams)
        for (auto* sides : {&s.a, &s.b})
            for (SeamSide& side : *sides)
                if (side.panel == panel)
                {
                    side.from = next(side.from);
                    side.to = next(side.to);
                }
    std::vector<int>& notches = g.panels[size_t(panel)].notches;
    std::vector<int> kept;
    for (int v : notches)
        if (v >= 0 && v < n && map[size_t(v)] >= 0)
            kept.push_back(map[size_t(v)]);
    notches = std::move(kept);
    const size_t before = g.seams.size();
    removeEmptySeams(g);
    return int(before - g.seams.size());
}

/** Rebuilds a panel's linked twin from it; `map` is the panel's old-to-new index map when its point count changed. */
void syncTwin(Garment& g, int panel, const std::vector<int>* map)
{
    Panel& p = g.panels[size_t(panel)];
    const int t = p.twin;
    if (!validPanel(g, t) || t == panel)
        return;
    const int n2 = int(p.outline.size());
    if (map)
    {
        const int n1 = int(map->size());
        if (int(g.panels[size_t(t)].outline.size()) != n1)
        {
            g.panels[size_t(t)].twin = -1;
            g.panels[size_t(panel)].twin = -1;
            return;
        }
        std::vector<int> twinMap(size_t(n1), -1);
        for (int j = 0; j < n1; ++j)
        {
            const int moved = (*map)[size_t(twinIndex(j, n1))];
            twinMap[size_t(j)] = moved < 0 ? -1 : twinIndex(moved, n2);
        }
        remapPanel(g, t, twinMap);
    }
    const Panel& src = g.panels[size_t(panel)];
    std::vector<Vec2> out(static_cast<size_t>(n2));
    for (int j = 0; j < n2; ++j)
        out[size_t(j)] = mirrored(src.outline[size_t(twinIndex(j, n2))]);
    g.panels[size_t(t)].outline = std::move(out);
}

void insertPoint(Garment& g, int panel, int index, const Vec2& at, bool sync)
{
    Panel& p = g.panels[size_t(panel)];
    const int n = int(p.outline.size());
    index = std::clamp(index, 0, n);
    p.outline.insert(p.outline.begin() + index, at);
    std::vector<int> map(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        map[size_t(i)] = i + (i >= index ? 1 : 0);
    remapPanel(g, panel, map);
    if (sync)
        syncTwin(g, panel, &map);
}

/** True when point v lies strictly inside the run from..to, walking forward round an outline of n points. */
bool inside(int v, int from, int to, int n)
{
    for (int i = (from + 1) % n; i != to % n; i = (i + 1) % n)
        if (i == v)
            return true;
    return false;
}

int cutDart(Garment& g, int panel, int edge, float t, float width, float depth)
{
    Panel& p = g.panels[size_t(panel)];
    const int n = int(p.outline.size());
    const Vec2 a = p.outline[size_t(edge)], b = p.outline[size_t((edge + 1) % n)];
    const float len = distance2d(a, b);
    if (width <= 0.f || depth <= 0.f || width > 0.9f * len)
        return -1;
    const Vec2 dir{(b[0] - a[0]) / len, (b[1] - a[1]) / len};
    // The outline runs counter-clockwise, so the inside is on the left.
    const Vec2 in{-dir[1], dir[0]};
    const float c = std::clamp(t * len, 0.5f * width, len - 0.5f * width);
    auto along = [&](float s, float off) { return Vec2{a[0] + dir[0] * s + in[0] * off, a[1] + dir[1] * s + in[1] * off}; };
    const int k = edge + 1;
    insertPoint(g, panel, k, along(c - 0.5f * width, 0.f), false);
    insertPoint(g, panel, k + 1, along(c, depth), false);
    insertPoint(g, panel, k + 2, along(c + 0.5f * width, 0.f), false);
    const int m = n + 3;
    // A seam along the edge sews both sides of the dart, never its legs.
    for (Seam& s : g.seams)
        for (auto* sides : {&s.a, &s.b})
            for (size_t i = 0; i < sides->size(); ++i)
            {
                SeamSide& side = (*sides)[i];
                if (side.panel != panel || !inside(k + 1, side.from, side.to, m))
                    continue;
                const SeamSide after{panel, k + 2, side.to};
                side.to = k;
                sides->insert(sides->begin() + std::ptrdiff_t(i) + 1, after);
                ++i;
            }
    Seam dart;
    dart.a = {{panel, k, k + 1}};
    dart.b = {{panel, k + 1, k + 2}};
    dart.reversed = true;
    g.seams.push_back(dart);
    return int(g.seams.size()) - 1;
}

std::string uniqueName(const Garment& g, const std::string& base)
{
    std::string name = base;
    for (int k = 2; std::any_of(g.panels.begin(), g.panels.end(), [&](const Panel& p) { return p.name == name; }); ++k)
        name = base + std::to_string(k);
    return name;
}

std::string mirroredName(const std::string& name)
{
    for (const auto& [from, to] : {std::pair{"Left", "Right"}, std::pair{"Right", "Left"}, std::pair{"left", "right"}, std::pair{"right", "left"}})
        if (const size_t at = name.find(from); at != std::string::npos)
            return name.substr(0, at) + to + name.substr(at + std::char_traits<char>::length(from));
    return name + "Mirror";
}

/** Outline edges already sewn by a seam, per panel. */
std::vector<std::vector<char>> sewnEdges(const Garment& g)
{
    std::vector<std::vector<char>> sewn(g.panels.size());
    for (size_t p = 0; p < g.panels.size(); ++p)
        sewn[p].assign(g.panels[p].outline.size(), 0);
    for (const Seam& s : g.seams)
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
            {
                if (!validPanel(g, side.panel))
                    continue;
                const int n = int(sewn[size_t(side.panel)].size());
                if (n == 0)
                    continue;
                for (int i = side.from % n; i != side.to % n; i = (i + 1) % n)
                    sewn[size_t(side.panel)][size_t(i)] = 1;
            }
    return sewn;
}

/** Outline points where the outline turns sharply: the ends of a pattern maker's edges. */
std::vector<char> corners(const std::vector<Vec2>& o)
{
    const int n = int(o.size());
    std::vector<char> c(size_t(n), 0);
    for (int i = 0; i < n; ++i)
    {
        const Vec2 &a = o[size_t((i + n - 1) % n)], &b = o[size_t(i)], &d = o[size_t((i + 1) % n)];
        const float ux = b[0] - a[0], uy = b[1] - a[1], vx = d[0] - b[0], vy = d[1] - b[1];
        const float lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
        if (lu < 1e-6f || lv < 1e-6f)
            continue;
        c[size_t(i)] = (ux * vx + uy * vy) / (lu * lv) < std::cos(30.f * std::numbers::pi_v<float> / 180.f);
    }
    return c;
}

struct Edge
{
    SeamSide side;
    float length = 0.f;
    std::array<Vec3, 5> at{};
    Vec3 out{}; // the outline's outward direction at the middle, placed in 3D
};

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
} // namespace

float seamSideLength(const Garment& g, const std::vector<SeamSide>& sides)
{
    float len = 0.f;
    for (const SeamSide& s : sides)
        if (validPanel(g, s.panel))
            len += outlineRunLength(g.panels[size_t(s.panel)].outline, s.from, s.to);
    return len;
}

void insertOutlinePoint(Garment& g, int panel, int index, const Vec2& at)
{
    if (validPanel(g, panel))
        insertPoint(g, panel, index, at, true);
}

int deleteOutlinePoint(Garment& g, int panel, int index)
{
    if (!validPanel(g, panel))
        return 0;
    Panel& p = g.panels[size_t(panel)];
    const int n = int(p.outline.size());
    if (index < 0 || index >= n || n <= 3)
        return 0;
    p.outline.erase(p.outline.begin() + index);
    std::vector<int> map(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        map[size_t(i)] = i < index ? i : (i == index ? -1 : i - 1);
    const int removed = remapPanel(g, panel, map);
    const size_t seams = g.seams.size();
    syncTwin(g, panel, &map);
    return removed + int(seams - g.seams.size());
}

void setOutline(Garment& g, int panel, const std::vector<Vec2>& outline)
{
    if (!validPanel(g, panel) || outline.size() != g.panels[size_t(panel)].outline.size())
        return;
    g.panels[size_t(panel)].outline = outline;
    syncTwin(g, panel, nullptr);
}

int removePanels(Garment& g, std::vector<int> panels)
{
    std::sort(panels.begin(), panels.end());
    panels.erase(std::unique(panels.begin(), panels.end()), panels.end());
    auto removed = [&](int p) { return std::binary_search(panels.begin(), panels.end(), p); };
    auto shifted = [&](int p) { return p - int(std::lower_bound(panels.begin(), panels.end(), p) - panels.begin()); };
    const size_t before = g.seams.size();
    std::erase_if(g.seams, [&](const Seam& s) {
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
                if (removed(side.panel))
                    return true;
        return false;
    });
    for (Seam& s : g.seams)
        for (auto* sides : {&s.a, &s.b})
            for (SeamSide& side : *sides)
                side.panel = shifted(side.panel);
    for (Panel& p : g.panels)
        p.twin = p.twin < 0 || removed(p.twin) ? -1 : shifted(p.twin);
    for (auto it = panels.rbegin(); it != panels.rend(); ++it)
        if (validPanel(g, *it))
            g.panels.erase(g.panels.begin() + *it);
    return int(before - g.seams.size());
}

int addNotch(Garment& g, int panel, const Vec2& at, float snap)
{
    if (!validPanel(g, panel) || g.panels[size_t(panel)].outline.size() < 3)
        return -1;
    const auto& o = g.panels[size_t(panel)].outline;
    const int n = int(o.size());
    int best = 0;
    for (int i = 1; i < n; ++i)
        if (distance2d(o[size_t(i)], at) < distance2d(o[size_t(best)], at))
            best = i;
    int k = best;
    if (distance2d(o[size_t(best)], at) > snap)
    {
        int edge = 0;
        float bestD = 1e30f;
        Vec2 foot{};
        for (int i = 0; i < n; ++i)
        {
            const Vec2 &a = o[size_t(i)], &b = o[size_t((i + 1) % n)];
            const float dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
            const float t = l2 > 0.f ? std::clamp(((at[0] - a[0]) * dx + (at[1] - a[1]) * dy) / l2, 0.f, 1.f) : 0.f;
            const Vec2 q{a[0] + dx * t, a[1] + dy * t};
            if (const float d = distance2d(q, at); d < bestD)
            {
                bestD = d;
                edge = i;
                foot = q;
            }
        }
        k = edge + 1;
        insertPoint(g, panel, k, foot, true);
    }
    auto mark = [](Panel& p, int v) {
        if (std::find(p.notches.begin(), p.notches.end(), v) == p.notches.end())
            p.notches.push_back(v);
    };
    Panel& p = g.panels[size_t(panel)];
    mark(p, k);
    if (validPanel(g, p.twin))
        mark(g.panels[size_t(p.twin)], twinIndex(k, int(p.outline.size())));
    return k;
}

int addDart(Garment& g, int panel, int edge, float t, float width, float depth)
{
    if (!validPanel(g, panel))
        return -1;
    const int n = int(g.panels[size_t(panel)].outline.size());
    if (edge < 0 || edge >= n)
        return -1;
    const int twin = g.panels[size_t(panel)].twin;
    const int seam = cutDart(g, panel, edge, t, width, depth);
    if (seam >= 0 && validPanel(g, twin) && twin != panel && int(g.panels[size_t(twin)].outline.size()) == n)
        cutDart(g, twin, twinIndex(edge + 1, n), 1.f - t, width, depth);
    return seam;
}

int mirrorPanel(Garment& g, int panel, float planeX, bool link)
{
    if (!validPanel(g, panel))
        return -1;
    const Panel& p = g.panels[size_t(panel)];
    Panel q = p;
    q.name = uniqueName(g, mirroredName(p.name));
    const int n = int(p.outline.size());
    for (int j = 0; j < n; ++j)
        q.outline[size_t(j)] = mirrored(p.outline[size_t(twinIndex(j, n))]);
    for (auto& h : q.holes)
    {
        for (Vec2& v : h)
            v = mirrored(v);
        std::reverse(h.begin(), h.end());
    }
    for (auto& l : q.internalLines)
        for (Vec2& v : l)
            v = mirrored(v);
    for (int& v : q.notches)
        v = twinIndex(v, n);
    q.grainAngle = std::numbers::pi_v<float> - p.grainAngle;
    // Pattern x is negated, so the placement's right axis flips too and the panel still faces outward.
    q.origin = {2.f * planeX - p.origin[0], p.origin[1], p.origin[2]};
    q.right = {p.right[0], -p.right[1], -p.right[2]};
    q.up = {-p.up[0], p.up[1], p.up[2]};
    const int index = int(g.panels.size());
    q.twin = link ? panel : -1;
    if (link)
    {
        if (validPanel(g, p.twin))
            g.panels[size_t(p.twin)].twin = -1;
        g.panels[size_t(panel)].twin = index;
    }
    g.panels.push_back(std::move(q));
    return index;
}

void gradePattern(Garment& g, float width, float length)
{
    struct Cap
    {
        SeamSide sleeve;
        std::vector<SeamSide> hole;
        float ease = 1.f;
    };
    std::vector<Cap> caps;
    for (const Seam& s : g.seams)
    {
        const bool sa = allSleeve(g, s.a), sb = allSleeve(g, s.b);
        if (sa == sb || anySleeve(g, sa ? s.b : s.a))
            continue;
        const auto& sleeve = sa ? s.a : s.b;
        const auto& hole = sa ? s.b : s.a;
        // A set-in cap: one run on the sleeve, the armhole in front and back pieces.
        if (sleeve.size() != 1 || hole.size() < 2)
            continue;
        const float lh = seamSideLength(g, hole);
        if (lh > 0.f)
            caps.push_back({sleeve.front(), hole, seamSideLength(g, sleeve) / lh});
    }

    auto scale = [&](std::vector<Vec2>& pts) {
        for (Vec2& v : pts)
            v = {v[0] * width, v[1] * length};
    };
    for (Panel& p : g.panels)
    {
        scale(p.outline);
        for (auto& h : p.holes)
            scale(h);
        for (auto& l : p.internalLines)
            scale(l);
        p.wrapRadius *= width;
    }

    for (const Cap& c : caps)
    {
        auto& o = g.panels[size_t(c.sleeve.panel)].outline;
        const int n = int(o.size());
        std::vector<int> run;
        for (int i = c.sleeve.from % n;; i = (i + 1) % n)
        {
            run.push_back(i);
            if (i == c.sleeve.to % n)
                break;
        }
        std::vector<Vec2> base;
        for (int i : run)
            base.push_back(o[size_t(i)]);
        const float cx = 0.5f * (base.front()[0] + base.back()[0]);
        const float target = c.ease * seamSideLength(g, c.hole);
        auto lengthAt = [&](float k) {
            float len = 0.f;
            for (size_t i = 0; i + 1 < base.size(); ++i)
                len += std::hypot((base[i + 1][0] - base[i][0]) * k, base[i + 1][1] - base[i][1]);
            return len;
        };
        float lo = 0.5f, hi = 2.f;
        if (lengthAt(lo) > target || lengthAt(hi) < target)
            continue;
        for (int it = 0; it < 40; ++it)
        {
            const float mid = 0.5f * (lo + hi);
            (lengthAt(mid) < target ? lo : hi) = mid;
        }
        const float k = 0.5f * (lo + hi);
        for (size_t i = 0; i < run.size(); ++i)
            o[size_t(run[i])] = {cx + (base[i][0] - cx) * k, base[i][1]};
    }
}

std::vector<Seam> suggestSeams(const Garment& g, float maxGap)
{
    const std::vector<std::vector<char>> sewn = sewnEdges(g);
    std::vector<Edge> edges;
    for (size_t pi = 0; pi < g.panels.size(); ++pi)
    {
        const Panel& p = g.panels[pi];
        const auto& o = p.outline;
        const int n = int(o.size());
        if (n < 3 || p.state == PanelState::Deactivated)
            continue;
        const std::vector<char> c = corners(o);
        // Edges run between corners and between sewn and unsewn stretches.
        auto breaks = [&](int i) { return c[size_t(i)] || sewn[pi][size_t(i)] != sewn[pi][size_t((i + n - 1) % n)]; };
        int start = -1;
        for (int i = 0; i < n && start < 0; ++i)
            if (breaks(i))
                start = i;
        if (start < 0)
            continue;
        int from = -1;
        for (int k = 0; k <= n; ++k)
        {
            const int i = (start + k) % n;
            if ((k == n || breaks(i)) && from >= 0)
            {
                Edge e;
                e.side = {int(pi), from, i};
                e.length = outlineRunLength(o, from, i);
                for (int s = 0; s < 5; ++s)
                {
                    // The point s/4 of the way along the run, by arc length.
                    const float want = e.length * float(s) / 4.f;
                    float walked = 0.f;
                    Vec2 at = o[size_t(i)];
                    for (int v = from; v != i; v = (v + 1) % n)
                    {
                        const Vec2 &a = o[size_t(v)], &b = o[size_t((v + 1) % n)];
                        const float l = distance2d(a, b);
                        if (walked + l >= want && l > 0.f)
                        {
                            const float t = (want - walked) / l;
                            at = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
                            break;
                        }
                        walked += l;
                    }
                    e.at[size_t(s)] = placePatternPoint(p, at);
                    if (s == 2)
                    {
                        // Counter-clockwise outline: outward is right of travel. Step along the edge's own chord.
                        const Vec2 a = o[size_t(from)], b = o[size_t(i)];
                        const float dx = b[0] - a[0], dy = b[1] - a[1], l = std::hypot(dx, dy);
                        const float step = 0.01f;
                        const Vec2 off = l > 0.f ? Vec2{at[0] + dy / l * step, at[1] - dx / l * step} : at;
                        const Vec3 d = sub(placePatternPoint(p, off), e.at[2]);
                        const float dl = norm(d);
                        e.out = dl > 0.f ? Vec3{d[0] / dl, d[1] / dl, d[2] / dl} : Vec3{};
                    }
                }
                if (e.length > 1e-4f)
                    edges.push_back(e);
                from = -1;
            }
            if (k < n && !sewn[pi][size_t(i)] && from < 0)
                from = i;
        }
    }

    struct Match
    {
        int a, b;
        bool reversed;
        float gap;
    };
    std::vector<Match> matches;
    auto gap = [](const Edge& a, const Edge& b, bool reversed) {
        float sum = 0.f;
        for (int s = 0; s < 5; ++s)
        {
            const Vec3 &p = a.at[size_t(s)], &q = b.at[size_t(reversed ? 4 - s : s)];
            sum += std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
        }
        return sum / 5.f;
    };
    for (size_t i = 0; i < edges.size(); ++i)
        for (size_t j = i + 1; j < edges.size(); ++j)
        {
            const Edge &a = edges[i], &b = edges[j];
            if (std::max(a.length, b.length) > 1.15f * std::min(a.length, b.length))
                continue;
            const float forward = gap(a, b, false), backward = gap(a, b, true);
            const float d = std::min(forward, backward);
            // Edges a seam closes face each other across the gap; edges side by side (two hems) don't. Touching edges always do.
            const Vec3 across = sub(b.at[2], a.at[2]);
            const float l = norm(across);
            const bool facing = l < 0.01f || (dot(a.out, across) > 0.5f * l && dot(b.out, across) < -0.5f * l);
            if (d <= maxGap && facing)
                matches.push_back({int(i), int(j), backward < forward, d});
        }
    std::sort(matches.begin(), matches.end(), [](const Match& x, const Match& y) { return x.gap < y.gap; });
    std::vector<char> used(edges.size(), 0);
    std::vector<Seam> out;
    for (const Match& m : matches)
    {
        if (used[size_t(m.a)] || used[size_t(m.b)])
            continue;
        used[size_t(m.a)] = used[size_t(m.b)] = 1;
        Seam s;
        s.a = {edges[size_t(m.a)].side};
        s.b = {edges[size_t(m.b)].side};
        s.reversed = m.reversed;
        out.push_back(s);
    }
    return out;
}

std::vector<Pin> resolvePins(const ClothMesh& mesh, const Garment& g)
{
    std::vector<Pin> out;
    for (const GarmentPin& gp : g.pins)
    {
        int best = -1;
        float bestD = 1e30f;
        for (size_t v = 0; v < mesh.uv.size() && v < mesh.vertPanel.size(); ++v)
            if (mesh.vertPanel[v] == gp.panel)
                if (const float d = distance2d(mesh.uv[v], gp.at); d < bestD)
                {
                    bestD = d;
                    best = int(v);
                }
        if (best >= 0 && std::none_of(out.begin(), out.end(), [&](const Pin& p) { return p.vertex == best; }))
            out.push_back({best, gp.target});
    }
    return out;
}

std::vector<Seam> splitSeamAtNotches(const Garment& g, const Seam& seam)
{
    auto split = [&](const std::vector<SeamSide>& sides, std::vector<std::vector<SeamSide>>& groups) {
        groups.assign(1, {});
        for (const SeamSide& s : sides)
        {
            if (!validPanel(g, s.panel))
                return false;
            const Panel& p = g.panels[size_t(s.panel)];
            const int n = int(p.outline.size());
            if (n < 3 || s.from < 0 || s.to < 0)
                return false;
            int start = s.from % n;
            for (int i = (s.from + 1) % n; i != s.to % n; i = (i + 1) % n)
                if (std::find(p.notches.begin(), p.notches.end(), i) != p.notches.end())
                {
                    groups.back().push_back({s.panel, start, i});
                    groups.emplace_back();
                    start = i;
                }
            groups.back().push_back({s.panel, start, s.to % n});
        }
        return true;
    };
    std::vector<std::vector<SeamSide>> ga, gb;
    if (!split(seam.a, ga) || !split(seam.b, gb) || ga.size() != gb.size() || ga.size() < 2)
        return {seam};
    std::vector<Seam> out;
    for (size_t k = 0; k < ga.size(); ++k)
    {
        Seam s = seam;
        s.a = ga[k];
        s.b = gb[seam.reversed ? gb.size() - 1 - k : k];
        out.push_back(std::move(s));
    }
    return out;
}

} // namespace tailor
