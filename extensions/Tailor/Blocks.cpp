#include "Blocks.h"

#include <algorithm>
#include <cmath>

namespace tailor
{
namespace blocks_detail
{

constexpr float kPi = 3.14159265358979f;
constexpr float kStep = 0.01f; // curve sampling
constexpr float kGap = 0.025f; // body to panel at placement

Vec3 add3(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul3(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 cross3(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 norm3(const Vec3& a)
{
    const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l > 0.f ? mul3(a, 1.f / l) : a;
}

float dist2d(const Vec2& a, const Vec2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

struct Builder
{
    std::vector<Vec2> pts;

    int add(float x, float y)
    {
        pts.push_back({x, y});
        return int(pts.size()) - 1;
    }

    /** Elliptical arc about c from angle t0 to t1, without its start point. */
    int arc(Vec2 c, float a, float b, float t0, float t1, bool includeEnd)
    {
        const float len = std::abs(t1 - t0) * std::sqrt((a * a + b * b) * 0.5f);
        const int n = std::max(2, int(std::ceil(len / kStep)));
        for (int i = 1; i <= n; ++i)
        {
            if (i == n && !includeEnd)
                break;
            const float t = t0 + (t1 - t0) * float(i) / float(n);
            add(c[0] + a * std::cos(t), c[1] + b * std::sin(t));
        }
        return int(pts.size()) - 1;
    }
};

std::vector<Vec2> mirrorX(const std::vector<Vec2>& p)
{
    const size_t n = p.size();
    std::vector<Vec2> m(n);
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2& q = p[(n - k) % n];
        m[k] = {-q[0], q[1]};
    }
    return m;
}

int mirrorIndex(int i, int n) { return (n - i) % n; }

/** The run on a mirrored outline that matches run `s` on the original. */
SeamSide mirrorRun(const SeamSide& s, int n, int panel) { return {panel, mirrorIndex(s.to, n), mirrorIndex(s.from, n)}; }

float runLength(const std::vector<Vec2>& o, int from, int to)
{
    const int n = int(o.size());
    float len = 0.f;
    for (int i = from; i != to; i = (i + 1) % n)
        len += dist2d(o[i], o[(i + 1) % n]);
    return len;
}

Seam seam(std::vector<SeamSide> a, std::vector<SeamSide> b, bool reversed = true)
{
    Seam s;
    s.a = std::move(a);
    s.b = std::move(b);
    s.reversed = reversed;
    return s;
}

/**
 * Symmetric panel from its +x half: `half` runs from the hem up the side to the neck point, the neck
 * (half-ellipse of half width neckA, depth neckB) joins it to its mirror. plus/minus give outline
 * indices of each half point and its mirror image.
 */
struct Symmetric
{
    std::vector<Vec2> outline;
    std::vector<int> plus, minus;

    SeamSide plusRun(int panel, int r0, int r1) const { return {panel, plus[r0], plus[r1]}; }
    SeamSide minusRun(int panel, int r0, int r1) const { return {panel, minus[r1], minus[r0]}; }
};

Symmetric symmetric(const std::vector<Vec2>& half, float neckY, float neckA, float neckB)
{
    Symmetric s;
    Builder b;
    const int h = int(half.size());
    b.add(-half[0][0], half[0][1]);
    for (const Vec2& p : half)
        s.plus.push_back(b.add(p[0], p[1]));
    if (neckB > 0.f)
        b.arc({0.f, neckY}, neckA, neckB, 0.f, -kPi, false);
    s.minus.assign(h, 0);
    for (int r = h - 1; r >= 1; --r)
        s.minus[r] = b.add(-half[r][0], half[r][1]);
    s.outline = std::move(b.pts);
    return s;
}

/**
 * Wrap radius for a panel whose centre line sits z0 in front of the body axis. Narrow panels wrap the
 * body cylinder; wider ones get a larger radius so their edges stop short of the side plane and front
 * and back panels never cross.
 */
float lensRadius(float halfWidth, float z0)
{
    if (halfWidth <= z0 * kPi * 0.5f)
        return z0;
    const float target = z0 - 0.005f;
    float lo = z0, hi = halfWidth * halfWidth / target + z0;
    for (int i = 0; i < 40; ++i)
    {
        const float r = 0.5f * (lo + hi);
        (r * (1.f - std::cos(halfWidth / r)) > target ? lo : hi) = r;
    }
    return hi;
}

void place(Panel& p, const Vec3& origin, const Vec3& up, const Vec3& normal, float wrap)
{
    p.origin = origin;
    p.up = norm3(up);
    p.right = norm3(cross3(p.up, norm3(normal)));
    p.wrapRadius = wrap;
}

/** Front (+Z) or back (-Z) panel centred on the body axis, pattern origin at the hem. */
void placeAround(Panel& p, float hemY, float z0, float halfWidth, bool front)
{
    const float s = front ? 1.f : -1.f;
    place(p, {0.f, hemY, s * z0}, {0.f, 1.f, 0.f}, {0.f, 0.f, s}, lensRadius(halfWidth, z0));
}

Panel makePanel(std::string name, std::vector<Vec2> outline)
{
    Panel p;
    p.name = std::move(name);
    p.outline = std::move(outline);
    return p;
}

Fabric fabric(const std::string& name)
{
    Fabric f;
    f.name = name;
    if (name == "Linen")
    {
        f.stretchWarp = 5000.f, f.stretchWeft = 4000.f, f.bendWarp = f.bendWeft = 3e-5f, f.density = 0.18f;
    }
    else if (name == "Wool")
    {
        f.stretchWarp = 3000.f, f.stretchWeft = 2500.f, f.bendWarp = f.bendWeft = 8e-5f, f.bendBias = 6e-5f;
        f.density = 0.35f, f.thickness = 0.002f, f.friction = 0.5f;
    }
    else if (name == "Leather")
    {
        f.stretchWarp = f.stretchWeft = 20000.f, f.shear = 2000.f, f.bendWarp = f.bendWeft = f.bendBias = 2e-3f;
        f.strainLimit = 0.03f, f.density = 0.6f, f.thickness = 0.003f, f.friction = 0.6f;
    }
    else if (name == "Canvas")
    {
        f.stretchWarp = f.stretchWeft = 12000.f, f.bendWarp = f.bendWeft = 3e-4f, f.density = 0.3f;
    }
    else if (name == "Banner")
    {
        f.stretchWarp = f.stretchWeft = 6000.f, f.bendWarp = f.bendWeft = 1e-5f, f.density = 0.12f;
    }
    return f;
}

Garment withFabric(const std::string& name)
{
    Garment g;
    g.fabrics = {fabric(name)};
    return g;
}

struct HoodPanels
{
    int p = -1, m = -1;
    SeamSide neckP, neckM; // front to back on +X, back to front on -X
};

/** Hood side panels at +X and -X with the crown seam; the neck edges are left for the caller. */
HoodPanels addHood(Garment& g, const Body& body, float neckLength, float depth, float height)
{
    const float d0 = std::max(neckLength, 0.1f);
    const float d = std::max(depth, d0);
    const float h = std::max(height, 0.15f);
    const float f = std::min(0.12f, d0 * 0.45f);
    const float yc = std::min(0.12f, h * 0.3f);
    Builder b;
    b.add(-f, 0.f);
    b.add(d0 - f, 0.f);
    b.add(d - f, yc);
    b.arc({-f, yc}, d, h - yc, 0.f, kPi * 0.5f, true);
    const int n = int(b.pts.size());

    HoodPanels hp;
    hp.p = int(g.panels.size());
    hp.m = hp.p + 1;
    Panel pp = makePanel("Hood L", b.pts);
    Panel pm = makePanel("Hood R", mirrorX(b.pts));
    pp.particleDistance = pm.particleDistance = 0.015f;
    const float hx = body.head / (2.f * kPi) + 0.03f;
    place(pp, {hx, body.neckY(), 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, 0.f);
    place(pm, {-hx, body.neckY(), 0.f}, {0.f, 1.f, 0.f}, {-1.f, 0.f, 0.f}, 0.f);
    g.panels.push_back(std::move(pp));
    g.panels.push_back(std::move(pm));

    const SeamSide crown{hp.p, 1, n - 1};
    g.seams.push_back(seam({crown}, {mirrorRun(crown, n, hp.m)}));
    hp.neckP = {hp.p, 0, 1};
    hp.neckM = mirrorRun(hp.neckP, n, hp.m);
    return hp;
}

/** Widest |x| of a closed outline along the horizontal line at y. */
float halfWidthAt(const std::vector<Vec2>& o, float y)
{
    float w = 0.f;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& a = o[i];
        const Vec2& b = o[(i + 1) % o.size()];
        if ((a[1] - y) * (b[1] - y) > 0.f || a[1] == b[1])
            continue;
        const float t = (y - a[1]) / (b[1] - a[1]);
        w = std::max(w, std::abs(a[0] + (b[0] - a[0]) * t));
    }
    return w;
}

} // namespace blocks_detail

using namespace blocks_detail;

float Body::chestRadius() const { return chest / (2.f * kPi); }

const char* sleeveStyleName(SleeveStyle s)
{
    static const char* names[] = {"set-in", "cuffed", "bishop", "bell", "puff", "cap", "flutter", "leg-of-mutton", "poet",
                                  "drop-shoulder", "batwing"};
    const int i = int(s);
    return i >= 0 && i < int(SleeveStyle::Count) ? names[i] : "set-in";
}

namespace blocks_detail
{
/** What a sleeve style drafts: cap height as a share of armhole depth, cap ease, wrist and cuff, default length. */
struct SleeveShape
{
    float capHeight = 0.65f;
    float capEase = 0.04f; // cap seam longer than the armhole by this fraction; more gathers
    float wrist = 0.75f;   // wrist width over bicep width (before sleeveFlare)
    float cuff = 0.f;      // cuff band height, 0 for none
    float length = -1.f;   // default sleeve length as a share of the arm; -1 keeps the block's
    float drop = 0.f;      // default dropped shoulder, metres
};

SleeveShape sleeveShape(SleeveStyle s)
{
    switch (s)
    {
    case SleeveStyle::Cuffed: return {0.55f, 0.04f, 0.7f, 0.06f};
    case SleeveStyle::Bishop: return {0.6f, 0.05f, 1.25f, 0.05f, 0.95f};
    case SleeveStyle::Bell: return {0.6f, 0.04f, 2.2f, 0.f, 0.95f};
    case SleeveStyle::Puff: return {0.85f, 0.4f, 1.3f, 0.04f, 0.2f};
    case SleeveStyle::Cap: return {0.6f, 0.03f, 1.1f, 0.f, 0.1f};
    case SleeveStyle::Flutter: return {0.55f, 0.03f, 2.4f, 0.f, 0.25f};
    case SleeveStyle::LegOfMutton: return {0.9f, 0.5f, 0.6f, 0.f, 0.95f};
    case SleeveStyle::Poet: return {0.7f, 0.25f, 1.3f, 0.05f, 0.95f};
    case SleeveStyle::DropShoulder: return {0.25f, 0.f, 0.8f, 0.f, -1.f, 0.08f};
    default: return {};
    }
}

/** Bell-shaped sleeve cap from (-w/2, 0) over (0, -h) to (w/2, 0), y towards the shoulder negative. Excludes the ends. */
std::vector<Vec2> capCurve(float w, float h)
{
    std::vector<Vec2> out;
    const int n = std::max(8, int(std::ceil(w / kStep)));
    for (int i = 1; i < n; ++i)
    {
        const float t = -1.f + 2.f * float(i) / float(n);
        out.push_back({0.5f * w * t, -h * 0.5f * (1.f + std::cos(kPi * t))});
    }
    return out;
}

float capLength(float w, float h)
{
    std::vector<Vec2> c = capCurve(w, h);
    c.insert(c.begin(), Vec2{-0.5f * w, 0.f});
    c.push_back({0.5f * w, 0.f});
    float len = 0.f;
    for (size_t i = 0; i + 1 < c.size(); ++i)
        len += dist2d(c[i], c[i + 1]);
    return len;
}

/** Quadratic Bezier points from a towards b around control c, without a and b. */
std::vector<Vec2> bezier(const Vec2& a, const Vec2& c, const Vec2& b)
{
    std::vector<Vec2> out;
    const int n = std::max(3, int(std::ceil((dist2d(a, c) + dist2d(c, b)) / kStep)));
    for (int i = 1; i < n; ++i)
    {
        const float t = float(i) / float(n), u = 1.f - t;
        out.push_back({u * u * a[0] + 2.f * u * t * c[0] + t * t * b[0], u * u * a[1] + 2.f * u * t * c[1] + t * t * b[1]});
    }
    return out;
}
} // namespace blocks_detail

Garment top(const Body& body, const TopParams& p)
{
    Garment g = withFabric(p.openFront ? "Wool" : "Linen");
    const SleeveStyle style = SleeveStyle(std::clamp(p.sleeveStyle, 0, int(SleeveStyle::Count) - 1));
    const SleeveShape shape = sleeveShape(style);
    const bool batwing = style == SleeveStyle::Batwing && p.sleeveLength > 0.02f;
    const float shoulderY = body.shoulderY();
    const float len = std::clamp(p.length, 0.25f, shoulderY - 0.02f);
    const float hemY = shoulderY - len;
    const float ease = std::max(p.ease, 0.f);
    const float wc = body.chest * (1.f + ease) * 0.25f;
    float wh = wc;
    if (hemY < body.hipY())
        wh = std::max(wh, body.hip * (1.f + ease) * 0.25f);
    wh += std::max(p.hemFlare, 0.f);
    const float sx = body.shoulderWidth * 0.5f;
    const float nx = std::min(body.neck * (1.f + ease) / kPi * 0.58f + 0.01f, sx - 0.04f);
    const float yT = len;
    const float yN = len + 0.035f;

    // Armhole depth from the bicep the sleeve needs, then the shoulder carried past the shoulder point when dropped.
    const float dx = std::abs(wc - sx);
    const float bicepHalf = body.chest * 0.33f * (1.f + std::max(p.sleeveEase, 0.f)) * 0.5f;
    const float drop = std::max(p.dropShoulder > 0.f ? p.dropShoulder : shape.drop, 0.f);
    float ya = std::max(yT - std::sqrt(std::max(bicepHalf * bicepHalf - dx * dx, 0.0036f)), 0.06f);
    ya = std::max(ya - 0.5f * drop - (batwing ? 0.14f : 0.f), 0.06f);
    const Vec2 neckPt{nx, yN};
    const Vec2 shoulderDir{sx - nx, yT - yN};
    const float sl = std::hypot(shoulderDir[0], shoulderDir[1]);
    const Vec2 tip{sx + shoulderDir[0] / sl * drop, yT + shoulderDir[1] / sl * drop};
    const float slit = std::clamp(p.slit, 0.f, ya - 0.05f);

    std::vector<Vec2> half{{wh, 0.f}};
    int rSide = 0;
    if (slit > 0.005f)
    {
        half.push_back({wh + (wc - wh) * slit / ya, slit});
        rSide = 1;
    }
    const int rU = int(half.size());
    half.push_back({wc, ya});
    int rS, rWrist0 = -1, rWrist1 = -1;
    const float th = body.armAngle;
    const float ls = std::min(shape.length > 0.f ? shape.length * body.armLength : p.sleeveLength, body.armLength);
    if (batwing)
    {
        // Cut on: the underarm sweeps out along the arm, the top runs from the wrist back up to the neck.
        const Vec2 armDir{std::cos(th), -std::sin(th)};
        const float wristHalf = std::max(0.07f, bicepHalf * 0.45f) * (1.f + std::max(p.sleeveFlare, 0.f));
        const Vec2 shoulderEnd = tip;
        const Vec2 wristTop{shoulderEnd[0] + armDir[0] * ls, shoulderEnd[1] + armDir[1] * ls};
        const Vec2 n{armDir[1], -armDir[0]}; // below the arm line
        const Vec2 wristBottom{wristTop[0] + n[0] * 2.f * wristHalf, wristTop[1] + n[1] * 2.f * wristHalf};
        for (const Vec2& q : bezier({wc, ya}, {wc + 0.05f, ya + 0.02f}, wristBottom))
            half.push_back(q);
        rWrist0 = int(half.size());
        half.push_back(wristBottom);
        rWrist1 = int(half.size());
        half.push_back(wristTop);
        rS = int(half.size());
        half.push_back(shoulderEnd);
    }
    else
    {
        // Set-in armholes scoop: down from the shoulder, then out to the underarm. Dropped ones run nearly straight.
        const float straight = std::min(drop / 0.08f, 1.f);
        const Vec2 control{tip[0] + (wc - tip[0]) * 0.5f * straight, ya + (tip[1] - ya) * (0.25f + 0.25f * straight)};
        std::vector<Vec2> curve = bezier({wc, ya}, control, tip);
        for (const Vec2& q : curve)
            half.push_back(q);
        rS = int(half.size());
        half.push_back(tip);
    }
    const int rN = int(half.size());
    half.push_back(neckPt);

    struct Runs { SeamSide side, arm, shoulder, wrist; };
    Runs fPlus, fMinus, bPlus, bMinus;
    const float z0 = 2.f * wc / kPi + kGap;
    // Batwing fronts lie flat: wrapped round the torso, the sleeves would curl round it too.
    auto placeBody = [&](Panel& panel, float z, bool front) {
        if (batwing)
            place(panel, {0.f, hemY, front ? z : -z}, {0.f, 1.f, 0.f}, {0.f, 0.f, front ? 1.f : -1.f}, 0.f);
        else
            placeAround(panel, hemY, z, wh, front);
    };

    if (!p.openFront)
    {
        const Symmetric f = symmetric(half, yN, nx, std::clamp(p.neckDepth, 0.01f, yN * 0.8f));
        Panel front = makePanel("Front", f.outline);
        placeBody(front, z0, true);
        g.panels.push_back(std::move(front));
        fPlus = {f.plusRun(0, rSide, rU), f.plusRun(0, rU, rS), f.plusRun(0, rS, rN), {}};
        fMinus = {f.minusRun(0, rSide, rU), f.minusRun(0, rU, rS), f.minusRun(0, rS, rN), {}};
        if (batwing)
        {
            fPlus = {f.plusRun(0, rSide, rWrist0), {}, f.plusRun(0, rWrist1, rN), {}};
            fMinus = {f.minusRun(0, rSide, rWrist0), {}, f.minusRun(0, rWrist1, rN), {}};
        }
    }
    else
    {
        const float ov = std::clamp(p.overlap, 0.f, nx - 0.01f);
        Builder b;
        b.add(-ov, 0.f);
        for (const Vec2& q : half)
            b.add(q[0], q[1]);
        b.add(-ov, yN - std::clamp(p.neckDepth, 0.02f, yN - 0.05f));
        const int n = int(b.pts.size());
        Panel fl = makePanel("Front L", b.pts);
        Panel fr = makePanel("Front R", mirrorX(b.pts));
        // The wearer's left laps over the right, so it sits a little further out.
        placeBody(fl, z0 + 0.012f, true);
        placeBody(fr, z0, true);
        g.panels.push_back(std::move(fl));
        g.panels.push_back(std::move(fr));
        auto run = [](int r0, int r1) { return SeamSide{0, 1 + r0, 1 + r1}; };
        if (batwing)
            fPlus = {run(rSide, rWrist0), {}, run(rWrist1, rN), {}};
        else
            fPlus = {run(rSide, rU), run(rU, rS), run(rS, rN), {}};
        fMinus = {mirrorRun(fPlus.side, n, 1), batwing ? SeamSide{} : mirrorRun(fPlus.arm, n, 1), mirrorRun(fPlus.shoulder, n, 1), {}};
    }

    const int bk = int(g.panels.size());
    const Symmetric bs = symmetric(half, yN, nx, 0.02f);
    Panel back = makePanel("Back", bs.outline);
    placeBody(back, z0, false);
    g.panels.push_back(std::move(back));
    if (batwing)
    {
        bPlus = {bs.plusRun(bk, rSide, rWrist0), {}, bs.plusRun(bk, rWrist1, rN), {}};
        bMinus = {bs.minusRun(bk, rSide, rWrist0), {}, bs.minusRun(bk, rWrist1, rN), {}};
    }
    else
    {
        bPlus = {bs.plusRun(bk, rSide, rU), bs.plusRun(bk, rU, rS), bs.plusRun(bk, rS, rN), {}};
        bMinus = {bs.minusRun(bk, rSide, rU), bs.minusRun(bk, rU, rS), bs.minusRun(bk, rS, rN), {}};
    }

    // The back faces -Z, so its +x half lies on the wearer's right (-X).
    g.seams.push_back(seam({fPlus.side}, {bMinus.side}));
    g.seams.push_back(seam({fMinus.side}, {bPlus.side}));
    g.seams.push_back(seam({fPlus.shoulder}, {bMinus.shoulder}));
    g.seams.push_back(seam({fMinus.shoulder}, {bPlus.shoulder}));

    if (batwing || p.sleeveLength <= 0.02f || ls <= 0.02f)
        return g;

    // Sleeve: cap length matched to the armhole (front + back) plus the style's ease, solved for bicep width at the
    // style's cap height. Pattern y runs from the bicep line (0) down the arm; the cap rises to -capHeight.
    const float armhole = runLength(g.panels[size_t(fPlus.arm.panel)].outline, fPlus.arm.from, fPlus.arm.to);
    const float depth = std::max(tip[1] - ya, 0.05f);
    const float h = shape.capHeight * depth;
    const float target = 2.f * armhole * (1.f + shape.capEase);
    float lo = 0.05f, hi = target;
    for (int i = 0; i < 40; ++i)
    {
        const float w = 0.5f * (lo + hi);
        (capLength(w, h) < target ? lo : hi) = w;
    }
    const float w = lo;
    const float wristW = w * shape.wrist * (1.f + std::max(p.sleeveFlare, 0.f));
    const float cuffH = shape.cuff > 0.f ? std::min(shape.cuff, ls * 0.5f) : 0.f;
    const float sleeveLen = ls - cuffH;
    Builder sb;
    sb.add(-0.5f * w, 0.f);
    for (const Vec2& q : capCurve(w, h))
        sb.add(q[0], q[1]);
    const int capEnd = sb.add(0.5f * w, 0.f);
    sb.add(0.5f * wristW, sleeveLen);
    sb.add(-0.5f * wristW, sleeveLen);
    const float rs = w / (2.f * kPi);
    // The cuff fits the wrist with a little ease; a fuller sleeve gathers into it along the seam.
    // Short sleeves band the upper arm, long ones the wrist.
    const float armGirth = ls < 0.4f * body.armLength ? 0.8f * w : std::max(body.armLength * 0.27f, 0.16f);
    const float cuffW = std::min(wristW, armGirth);
    for (float s : {1.f, -1.f})
    {
        const int sp = int(g.panels.size());
        Panel sleeve = makePanel(s > 0.f ? "Sleeve L" : "Sleeve R", sb.pts);
        const Vec3 up{s * std::cos(th), -std::sin(th), 0.f};
        const Vec3 nrm{s * std::sin(th), std::cos(th), 0.f};
        // On the arm's axis: the shoulder joint sits a little inside and below the shoulder point.
        const Vec3 joint{s * (sx - 0.03f + drop * std::cos(th)), shoulderY - 0.04f - drop * std::sin(th), 0.f};
        const Vec3 bicep = add3(joint, mul3(up, h));
        place(sleeve, add3(bicep, mul3(nrm, rs)), up, nrm, rs);
        g.panels.push_back(std::move(sleeve));
        if (s > 0.f)
            g.seams.push_back(seam({fPlus.arm, bMinus.arm}, {{sp, 0, capEnd}}));
        else
            g.seams.push_back(seam({bPlus.arm, fMinus.arm}, {{sp, 0, capEnd}}));
        g.seams.push_back(seam({{sp, capEnd, capEnd + 1}}, {{sp, capEnd + 2, 0}}));
        if (cuffH > 0.f)
        {
            const int cp = int(g.panels.size());
            Panel cuff = makePanel(s > 0.f ? "Cuff L" : "Cuff R", {{-0.5f * cuffW, 0.f}, {0.5f * cuffW, 0.f}, {0.5f * cuffW, cuffH}, {-0.5f * cuffW, cuffH}});
            const Vec3 at = add3(add3(bicep, mul3(up, sleeveLen)), mul3(nrm, cuffW / (2.f * kPi)));
            place(cuff, at, up, nrm, cuffW / (2.f * kPi));
            g.panels.push_back(std::move(cuff));
            g.seams.push_back(seam({{sp, capEnd + 1, capEnd + 2}}, {{cp, 0, 1}}));
            g.seams.push_back(seam({{cp, 1, 2}}, {{cp, 3, 0}}));
        }
    }
    return g;
}

Garment tee(const Body& body, const TeeParams& p) { return top(body, p); }
Garment tunic(const Body& body, const TunicParams& p) { return top(body, p); }
Garment robe(const Body& body, const RobeParams& p) { return top(body, p); }

Garment trousers(const Body& body, const TrousersParams& p)
{
    Garment g = withFabric("Cotton");
    const float waistY = body.waistY();
    const float len = std::clamp(p.length, waistY - body.crotchY() + 0.08f, waistY - 0.02f);
    const float hemY = waistY - len;
    const float yc = body.crotchY() - hemY;
    const float yw = len;
    float yh = body.hipY() - hemY;
    if (yh < yc + 0.03f || yh > yw - 0.03f)
        yh = 0.5f * (yc + yw);
    const float ease = std::max(p.ease, 0.f);
    const float tx = body.hip * 0.6f * (1.f + ease) * 0.25f;
    const float hx = std::clamp(p.hemCircumference, 0.2f, 1.2f) * 0.25f;
    const float cf = -tx;
    const float hipQ = body.hip * (1.f + ease) * 0.25f;
    const float waistQ = body.waist * (1.f + ease) * 0.25f;

    struct Shape { std::vector<Vec2> o; int crotch; };
    // The inner hem corner keeps the same offset from the crotch point front and back, so inseams match.
    const float innerGap = -hx - (cf - 0.045f);
    auto shape = [&](float ext, float rise) {
        Builder b;
        b.add(cf - ext + innerGap, 0.f);
        b.add(hx, 0.f);
        b.add(tx, yc);
        b.add(cf + hipQ, yh);
        b.add(cf + 0.01f + waistQ, yw);
        b.add(cf + 0.01f, yw + rise);
        b.add(cf, yh);
        // Quarter ellipse down and in to the crotch point.
        const int c = b.arc({cf - ext, yh}, ext, yh - yc, 0.f, -kPi * 0.5f, true);
        return Shape{b.pts, c};
    };
    const Shape f = shape(0.045f, 0.f);
    const Shape bk = shape(0.09f, std::clamp(p.backRise, 0.f, 0.1f));
    const int nf = int(f.o.size()), nb = int(bk.o.size());

    const float legX = std::max(body.hip * 0.095f, tx - 0.02f);
    const float z0 = body.hip * 0.6f / (2.f * kPi) + kGap + 0.03f;
    const float halfW = tx + 0.09f;
    const Vec3 up{0.f, 1.f, 0.f};
    auto leg = [&](const char* name, std::vector<Vec2> o, float x, float zs) {
        Panel pn = makePanel(name, std::move(o));
        place(pn, {x, hemY, zs * z0}, up, {0.f, 0.f, zs}, lensRadius(halfW, z0));
        g.panels.push_back(std::move(pn));
    };
    leg("Front L", f.o, legX, 1.f);                // 0
    leg("Back L", mirrorX(bk.o), legX, -1.f);      // 1
    leg("Front R", mirrorX(f.o), -legX, 1.f);      // 2
    leg("Back R", bk.o, -legX, -1.f);              // 3

    const SeamSide fOut{0, 1, 4}, fIn{0, f.crotch, 0}, fCentre{0, 5, f.crotch};
    const SeamSide bOut{3, 1, 4}, bIn{3, bk.crotch, 0}, bCentre{3, 5, bk.crotch};
    g.seams.push_back(seam({fOut}, {mirrorRun(bOut, nb, 1)}));
    g.seams.push_back(seam({fIn}, {mirrorRun(bIn, nb, 1)}));
    g.seams.push_back(seam({mirrorRun(fOut, nf, 2)}, {bOut}));
    g.seams.push_back(seam({mirrorRun(fIn, nf, 2)}, {bIn}));
    g.seams.push_back(seam({fCentre}, {mirrorRun(fCentre, nf, 2)}));
    g.seams.push_back(seam({mirrorRun(bCentre, nb, 1)}, {bCentre}));
    return g;
}

Garment skirt(const Body& body, const SkirtParams& p)
{
    Garment g = withFabric("Cotton");
    const float waistY = body.waistY();
    const float drop = waistY - body.hipY();
    const float len = std::clamp(p.length, drop + 0.05f, waistY - 0.02f);
    const float hemY = waistY - len;
    const float ease = std::max(p.ease, 0.f);
    const float hipX = body.hip * (1.f + ease) * 0.25f;
    const float waistX = body.waist * (1.f + ease) * 0.25f;
    const float hemX = hipX + std::max(p.flare, 0.f);
    const std::vector<Vec2> half{{hemX, 0.f}, {hipX, len - drop}, {waistX, len}};
    const float z0 = 2.f * hipX / kPi + kGap;
    for (int i = 0; i < 2; ++i)
    {
        const Symmetric s = symmetric(half, len, waistX, 0.f);
        Panel pn = makePanel(i == 0 ? "Front" : "Back", s.outline);
        placeAround(pn, hemY, z0, hemX, i == 0);
        g.panels.push_back(std::move(pn));
    }
    const Symmetric s = symmetric(half, len, waistX, 0.f);
    g.seams.push_back(seam({s.plusRun(0, 0, 2)}, {s.minusRun(1, 0, 2)}));
    g.seams.push_back(seam({s.minusRun(0, 0, 2)}, {s.plusRun(1, 0, 2)}));
    return g;
}

Garment hood(const Body& body, const HoodParams& p)
{
    Garment g = withFabric("Wool");
    addHood(g, body, body.neck * 0.62f, p.depth, p.height);
    return g;
}

Garment drape(const Body& body, const DrapeParams& p)
{
    Garment g = withFabric("Wool");
    const float neckY = body.neckY();
    const float len = std::clamp(p.length, 0.25f, neckY - 0.02f);
    const float hemY = neckY - len;
    const float nx = body.neck / kPi * 0.6f + 0.01f;
    const float sx = body.shoulderWidth * 0.5f + 0.04f;
    const float yS = len - (neckY - body.shoulderY()) - 0.01f;
    const float hw = sx + std::max(p.flare, 0.f);
    const std::vector<Vec2> half{{hw, 0.f}, {sx, yS}, {nx, len}};

    const float cov = std::clamp(p.frontCoverage, 0.f, 1.f);
    const float ov = std::clamp(p.overlap, 0.f, nx - 0.01f);
    const float xin = -ov + (nx - 0.03f + ov) * (1.f - cov);
    Builder b;
    b.add(xin, 0.f);
    for (const Vec2& q : half)
        b.add(q[0], q[1]);
    const int fLast = b.arc({xin, len}, nx - xin, 0.06f, 0.f, -kPi * 0.5f, true);
    const int nf = int(b.pts.size());

    const float z0 = p.clearance > 0.f ? p.clearance : body.chestRadius() + 0.08f;
    Panel fl = makePanel("Front L", b.pts);
    Panel fr = makePanel("Front R", mirrorX(b.pts));
    placeAround(fl, hemY, z0 + 0.012f, hw, true);
    placeAround(fr, hemY, z0, hw, true);
    const Symmetric bs = symmetric(half, len, nx, 0.015f);
    Panel back = makePanel("Back", bs.outline);
    placeAround(back, hemY, z0, hw, false);
    g.panels = {std::move(fl), std::move(fr), std::move(back)};

    const int r0 = p.closedSides ? 0 : 1;
    const SeamSide flSide{0, 1 + r0, 3};
    g.seams.push_back(seam({flSide}, {bs.minusRun(2, r0, 2)}));
    g.seams.push_back(seam({mirrorRun(flSide, nf, 1)}, {bs.plusRun(2, r0, 2)}));

    if (p.hood)
    {
        const SeamSide flNeck{0, 3, fLast};
        const SeamSide frNeck = mirrorRun(flNeck, nf, 1);
        const SeamSide bkNeck{2, bs.plus[2], bs.minus[2]};
        const float neckLen = runLength(g.panels[0].outline, flNeck.from, flNeck.to)
                              + runLength(g.panels[1].outline, frNeck.from, frNeck.to)
                              + runLength(g.panels[2].outline, bkNeck.from, bkNeck.to);
        const HoodPanels hp = addHood(g, body, neckLen * 0.5f, 0.32f, p.hoodHeight);
        g.seams.push_back(seam({frNeck, bkNeck, flNeck}, {hp.neckP, hp.neckM}));
    }
    return g;
}

Garment cloak(const Body& body, const CloakParams& p) { return drape(body, p); }
Garment cape(const Body& body, const CapeParams& p) { return drape(body, p); }

Garment overShoulder(const Body& body, const OverShoulderParams& p)
{
    Garment g = withFabric("Wool");
    const float topY = body.shoulderY() + 0.03f;
    const float len = std::clamp(p.length, 0.2f, topY - 0.02f);
    const float hemY = topY - len;
    const float nx0 = std::max(p.neckWidth * 0.5f, 0.07f);
    const float w2 = std::max(p.width * 0.5f, nx0 + 0.05f);
    const float nx = std::min(nx0, w2 - 0.05f);

    std::vector<Vec2> half{{w2, 0.f}};
    int rT0 = -1, rT1 = -1;
    const float tie = std::clamp(p.sideTie, 0.f, len * 0.5f);
    if (tie > 0.005f)
    {
        const float tc = std::clamp(body.waistY() - hemY, 0.02f + tie * 0.5f, len - 0.1f - tie * 0.5f);
        rT0 = int(half.size());
        half.push_back({w2, tc - tie * 0.5f});
        rT1 = int(half.size());
        half.push_back({w2, tc + tie * 0.5f});
    }
    const int rT = int(half.size());
    half.push_back({w2, len});
    const int rN = int(half.size());
    half.push_back({nx, len});

    const float z0 = body.chestRadius() + kGap + 0.03f;
    const Symmetric f = symmetric(half, len, nx, std::min(0.11f, len * 0.5f));
    const Symmetric bk = symmetric(half, len, nx, 0.03f);
    Panel front = makePanel("Front", f.outline);
    Panel back = makePanel("Back", bk.outline);
    placeAround(front, hemY, z0, w2, true);
    placeAround(back, hemY, z0, w2, false);
    g.panels = {std::move(front), std::move(back)};

    g.seams.push_back(seam({f.plusRun(0, rT, rN)}, {bk.minusRun(1, rT, rN)}));
    g.seams.push_back(seam({f.minusRun(0, rT, rN)}, {bk.plusRun(1, rT, rN)}));
    if (rT0 >= 0)
    {
        g.seams.push_back(seam({f.plusRun(0, rT0, rT1)}, {bk.minusRun(1, rT0, rT1)}));
        g.seams.push_back(seam({f.minusRun(0, rT0, rT1)}, {bk.plusRun(1, rT0, rT1)}));
    }
    return g;
}

Garment poncho(const Body& body, const PonchoParams& p) { return overShoulder(body, p); }
Garment tabard(const Body& body, const TabardParams& p) { return overShoulder(body, p); }

Garment belt(const Body& body, const BeltParams& p, std::vector<BlockAnchor>* anchors)
{
    Garment g = withFabric("Leather");
    const float target = body.waist * (1.f + std::max(p.ease, 0.f));
    const float fitR = p.fitRadius > 0.f ? p.fitRadius : body.waist / (2.f * kPi) + 0.04f;
    // Cut to fit loosely around the placement radius; warp shrink pulls it in to the target length.
    const float cut = 2.f * kPi * fitR;
    g.fabrics[0].shrinkWarp = std::min(1.f, target / cut);
    const float h = std::clamp(p.width, 0.01f, 0.2f);
    Panel pn = makePanel("Belt", {{-cut * 0.5f, 0.f}, {cut * 0.5f, 0.f}, {cut * 0.5f, h}, {-cut * 0.5f, h}});
    pn.particleDistance = std::min(0.01f, h * 0.25f);
    const float y = body.waistY() + p.height - h * 0.5f;
    place(pn, {0.f, y, -fitR}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f}, fitR);
    g.panels.push_back(std::move(pn));
    g.seams.push_back(seam({{0, 1, 2}}, {{0, 3, 0}}));
    if (anchors && p.buckle)
        anchors->push_back({"buckle", 0, {cut * 0.5f, h * 0.5f}, {0.f, y + h * 0.5f, fitR}});
    return g;
}

Garment sack(const Body& body, const SackParams& p)
{
    Garment g = withFabric("Canvas");
    const float w = std::clamp(p.width, 0.1f, 2.f), h = std::clamp(p.height, 0.1f, 2.f);
    const std::vector<Vec2> o{{-w * 0.5f, 0.f}, {w * 0.5f, 0.f}, {w * 0.5f, h}, {-w * 0.5f, h}};
    const float x = body.hip / kPi * 0.5f + w * 0.5f + 0.15f;
    const float y = std::max(0.05f, body.hipY() - h * 0.5f);
    for (float s : {1.f, -1.f})
    {
        Panel pn = makePanel(s > 0.f ? "Front" : "Back", o);
        place(pn, {x, y, s * 0.02f}, {0.f, 1.f, 0.f}, {0.f, 0.f, s}, 0.f);
        g.panels.push_back(std::move(pn));
    }
    g.seams.push_back(seam({{0, 0, 1}}, {{1, 0, 1}}));
    g.seams.push_back(seam({{0, 1, 2}}, {{1, 3, 0}}));
    g.seams.push_back(seam({{0, 3, 0}}, {{1, 1, 2}}));
    return g;
}

Garment banner(const Body& body, const BannerParams& p)
{
    Garment g = withFabric("Banner");
    const float w = std::clamp(p.width, 0.1f, 5.f), h = std::clamp(p.height, 0.1f, 5.f);
    Builder b;
    Vec3 origin;
    if (p.vertical)
    {
        const float tail = std::clamp(p.tail, 0.f, h * 0.45f);
        b.add(0.f, 0.f);
        if (tail > 0.f)
            b.add(w * 0.5f, tail);
        b.add(w, 0.f);
        b.add(w, h);
        b.add(0.f, h);
        origin = {body.shoulderWidth * 0.5f + 0.4f, std::max(0.05f, body.height + 0.5f - h), 0.f};
    }
    else
    {
        const float tail = std::clamp(p.tail, 0.f, w * 0.45f);
        b.add(0.f, 0.f);
        b.add(w, 0.f);
        if (tail > 0.f)
            b.add(w - tail, h * 0.5f);
        b.add(w, h);
        b.add(0.f, h);
        origin = {body.shoulderWidth * 0.5f + 0.4f, body.height * 0.9f, 0.f};
    }
    Panel pn = makePanel(p.vertical ? "Banner" : "Flag", b.pts);
    place(pn, origin, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, 0.f);
    g.panels.push_back(std::move(pn));
    return g;
}

Garment tent(const Body&, const TentParams& p)
{
    Garment g = withFabric("Canvas");
    const int n = std::clamp(p.sides, 3, 16);
    const float r = std::max(p.radius, 0.3f);
    const float h = std::max(p.height, 0.3f);
    const float apothem = r * std::cos(kPi / float(n));
    const float hb = r * std::sin(kPi / float(n));
    const float slant = std::hypot(apothem, h);
    for (int i = 0; i < n; ++i)
    {
        const float phi = 2.f * kPi * float(i) / float(n);
        const float sp = std::sin(phi), cp = std::cos(phi);
        Panel pn = makePanel("Side " + std::to_string(i + 1), {{-hb, 0.f}, {hb, 0.f}, {0.f, slant}});
        place(pn, {sp * apothem, 0.f, cp * apothem}, {-apothem * sp, h, -apothem * cp}, {h * sp, apothem, h * cp}, 0.f);
        g.panels.push_back(std::move(pn));
    }
    for (int i = 0; i < n; ++i)
    {
        if (p.door && i == n - 1)
            continue;
        g.seams.push_back(seam({{i, 1, 2}}, {{(i + 1) % n, 2, 0}}));
    }
    return g;
}

int appendGarment(Garment& dst, const Garment& src, int layer)
{
    const int p0 = int(dst.panels.size());
    const int f0 = int(dst.fabrics.size());
    dst.fabrics.insert(dst.fabrics.end(), src.fabrics.begin(), src.fabrics.end());
    for (Panel pn : src.panels)
    {
        pn.fabric += f0;
        pn.layer += layer;
        dst.panels.push_back(std::move(pn));
    }
    for (Seam s : src.seams)
    {
        for (SeamSide& a : s.a)
            a.panel += p0;
        for (SeamSide& b : s.b)
            b.panel += p0;
        dst.seams.push_back(std::move(s));
    }
    return p0;
}

Garment cloakTunicBelt(const Body& body, const OutfitParams& p, std::vector<BlockAnchor>* anchors)
{
    TunicParams tp;
    tp.length = p.tunicLength;
    const Garment t = tunic(body, tp);

    // Belt loose around the tunic's widest point at the waist, cinched by its fabric.
    const Panel& front = t.panels[0];
    const float hx = halfWidthAt(front.outline, body.waistY() - front.origin[1]);
    const float z0 = front.origin[2];
    const float r = front.wrapRadius;
    const float th = r > 0.f ? hx / r : 0.f;
    const float reach = std::max(z0, std::hypot(r * std::sin(th), z0 - r * (1.f - std::cos(th))));
    BeltParams bp;
    bp.ease = p.beltEase;
    bp.fitRadius = reach + 0.015f;
    std::vector<BlockAnchor> beltAnchors;
    const Garment bt = belt(body, bp, &beltAnchors);

    CloakParams cp;
    cp.length = p.cloakLength;
    cp.hood = p.hood;
    cp.clearance = std::max(z0, bp.fitRadius) + 0.06f;
    const Garment ck = cloak(body, cp);

    Garment out;
    out.fabrics.clear();
    appendGarment(out, t, 0);
    const int beltPanel = appendGarment(out, bt, 1);
    appendGarment(out, ck, 2);
    if (anchors)
        for (BlockAnchor a : beltAnchors)
        {
            a.panel += beltPanel;
            anchors->push_back(std::move(a));
        }
    return out;
}

// Registry

namespace blocks_detail
{

template <class P>
struct Field
{
    const char* name;
    float P::*f = nullptr;
    int P::*i = nullptr;
    bool P::*b = nullptr;
    float min = 0.f, max = 1.f;

    Field(const char* n, float P::*m, float lo, float hi) : name(n), f(m), min(lo), max(hi) {}
    Field(const char* n, int P::*m, int lo, int hi) : name(n), i(m), min(float(lo)), max(float(hi)) {}
    Field(const char* n, bool P::*m) : name(n), b(m) {}
};

template <class P>
using Generator = std::function<Garment(const Body&, const P&, std::vector<BlockAnchor>*)>;

template <class P>
Block makeBlock(const char* id, const char* label, std::vector<Field<P>> fields, Generator<P> gen)
{
    Block blk{id, label, {}, {}};
    const P defaults{};
    for (const Field<P>& f : fields)
    {
        BlockParam bp{f.name, 0.f, f.min, f.max, BlockParam::Kind::Float};
        if (f.f)
            bp.defaultValue = defaults.*f.f;
        else if (f.i)
            bp.defaultValue = float(defaults.*f.i), bp.kind = BlockParam::Kind::Int;
        else
            bp.defaultValue = defaults.*f.b ? 1.f : 0.f, bp.kind = BlockParam::Kind::Bool;
        blk.params.push_back(std::move(bp));
    }
    blk.build = [fields = std::move(fields), gen = std::move(gen)](const Body& body, const BlockParamValues& values,
                                                                  std::vector<BlockAnchor>* anchors) {
        P p{};
        for (const Field<P>& f : fields)
        {
            const auto it = values.find(f.name);
            if (it == values.end())
                continue;
            const float v = std::clamp(it->second, f.min, f.max);
            if (f.f)
                p.*f.f = v;
            else if (f.i)
                p.*f.i = int(std::lround(v));
            else
                p.*f.b = v >= 0.5f;
        }
        return gen(body, p, anchors);
    };
    return blk;
}

template <class P>
Generator<P> plain(Garment (*fn)(const Body&, const P&))
{
    return [fn](const Body& b, const P& p, std::vector<BlockAnchor>*) { return fn(b, p); };
}

template <class P>
std::vector<Field<P>> topFields()
{
    return {{"length", &P::length, 0.25f, 1.6f},
            {"ease", &P::ease, 0.f, 1.f},
            {"sleeveLength", &P::sleeveLength, 0.f, 0.75f},
            {"sleeveEase", &P::sleeveEase, 0.f, 1.5f},
            {"sleeveFlare", &P::sleeveFlare, 0.f, 2.f},
            {"hemFlare", &P::hemFlare, 0.f, 0.6f},
            {"slit", &P::slit, 0.f, 0.8f},
            {"neckDepth", &P::neckDepth, 0.01f, 0.6f},
            {"openFront", &P::openFront},
            {"overlap", &P::overlap, 0.f, 0.2f},
            {"sleeveStyle", &P::sleeveStyle, 0, int(SleeveStyle::Count) - 1},
            {"dropShoulder", &P::dropShoulder, 0.f, 0.2f}};
}

template <class P>
std::vector<Field<P>> drapeFields()
{
    return {{"length", &P::length, 0.25f, 1.6f},
            {"flare", &P::flare, 0.f, 1.2f},
            {"frontCoverage", &P::frontCoverage, 0.f, 1.f},
            {"overlap", &P::overlap, 0.f, 0.15f},
            {"closedSides", &P::closedSides},
            {"hood", &P::hood},
            {"hoodHeight", &P::hoodHeight, 0.25f, 0.6f}};
}

template <class P>
std::vector<Field<P>> overShoulderFields()
{
    return {{"width", &P::width, 0.3f, 2.f},
            {"length", &P::length, 0.2f, 1.6f},
            {"neckWidth", &P::neckWidth, 0.14f, 0.5f},
            {"sideTie", &P::sideTie, 0.f, 0.3f}};
}

std::vector<Block> makeBlocks()
{
    std::vector<Block> out;
    out.push_back(makeBlock<TeeParams>("tee", "T-Shirt", topFields<TeeParams>(), plain(&tee)));
    out.push_back(makeBlock<TunicParams>("tunic", "Tunic", topFields<TunicParams>(), plain(&tunic)));
    out.push_back(makeBlock<RobeParams>("robe", "Robe", topFields<RobeParams>(), plain(&robe)));
    out.push_back(makeBlock<DropShoulderTopParams>("dropShoulderTop", "Drop-Shoulder Top", topFields<DropShoulderTopParams>(), [](const Body& b, const DropShoulderTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<ShirtParams>("shirt", "Shirt", topFields<ShirtParams>(), [](const Body& b, const ShirtParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PeasantBlouseParams>("peasantBlouse", "Peasant Blouse", topFields<PeasantBlouseParams>(), [](const Body& b, const PeasantBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PuffBlouseParams>("puffBlouse", "Puff-Sleeve Blouse", topFields<PuffBlouseParams>(), [](const Body& b, const PuffBlouseParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<PoetShirtParams>("poetShirt", "Poet Shirt", topFields<PoetShirtParams>(), [](const Body& b, const PoetShirtParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<BellSleeveDressParams>("bellSleeveDress", "Bell-Sleeve Dress", topFields<BellSleeveDressParams>(), [](const Body& b, const BellSleeveDressParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<FlutterTopParams>("flutterTop", "Flutter-Sleeve Top", topFields<FlutterTopParams>(), [](const Body& b, const FlutterTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<CapSleeveTopParams>("capSleeveTop", "Cap-Sleeve Top", topFields<CapSleeveTopParams>(), [](const Body& b, const CapSleeveTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<BatwingTopParams>("batwingTop", "Batwing Top", topFields<BatwingTopParams>(), [](const Body& b, const BatwingTopParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<LegOfMuttonParams>("legOfMuttonBlouse", "Leg-of-Mutton Blouse", topFields<LegOfMuttonParams>(), [](const Body& b, const LegOfMuttonParams& p, std::vector<BlockAnchor>*) { return top(b, p); }));
    out.push_back(makeBlock<TrousersParams>("trousers", "Trousers",
                                            {{"length", &TrousersParams::length, 0.3f, 1.2f},
                                             {"ease", &TrousersParams::ease, 0.f, 0.6f},
                                             {"hemCircumference", &TrousersParams::hemCircumference, 0.2f, 1.2f},
                                             {"backRise", &TrousersParams::backRise, 0.f, 0.1f}},
                                            plain(&trousers)));
    out.push_back(makeBlock<SkirtParams>("skirt", "Skirt",
                                         {{"length", &SkirtParams::length, 0.15f, 1.1f},
                                          {"ease", &SkirtParams::ease, 0.f, 0.5f},
                                          {"flare", &SkirtParams::flare, 0.f, 1.f}},
                                         plain(&skirt)));
    out.push_back(makeBlock<HoodParams>("hood", "Hood",
                                        {{"depth", &HoodParams::depth, 0.2f, 0.5f}, {"height", &HoodParams::height, 0.25f, 0.6f}},
                                        plain(&hood)));
    out.push_back(makeBlock<CloakParams>("cloak", "Cloak", drapeFields<CloakParams>(), plain(&cloak)));
    out.push_back(makeBlock<CapeParams>("cape", "Cape", drapeFields<CapeParams>(), plain(&cape)));
    out.push_back(makeBlock<PonchoParams>("poncho", "Poncho", overShoulderFields<PonchoParams>(), plain(&poncho)));
    out.push_back(makeBlock<TabardParams>("tabard", "Tabard", overShoulderFields<TabardParams>(), plain(&tabard)));
    out.push_back(makeBlock<BeltParams>("belt", "Belt",
                                        {{"height", &BeltParams::height, -0.3f, 0.2f},
                                         {"width", &BeltParams::width, 0.01f, 0.2f},
                                         {"ease", &BeltParams::ease, 0.f, 0.5f},
                                         {"fitRadius", &BeltParams::fitRadius, 0.f, 0.6f},
                                         {"buckle", &BeltParams::buckle}},
                                        Generator<BeltParams>(&belt)));
    out.push_back(makeBlock<SackParams>("sack", "Sack",
                                        {{"width", &SackParams::width, 0.1f, 2.f}, {"height", &SackParams::height, 0.1f, 2.f}},
                                        plain(&sack)));
    out.push_back(makeBlock<BannerParams>("banner", "Banner / Flag",
                                          {{"width", &BannerParams::width, 0.1f, 5.f},
                                           {"height", &BannerParams::height, 0.1f, 5.f},
                                           {"vertical", &BannerParams::vertical},
                                           {"tail", &BannerParams::tail, 0.f, 2.f}},
                                          plain(&banner)));
    out.push_back(makeBlock<TentParams>("tent", "Tent",
                                        {{"radius", &TentParams::radius, 0.3f, 5.f},
                                         {"height", &TentParams::height, 0.3f, 5.f},
                                         {"sides", &TentParams::sides, 3, 16},
                                         {"door", &TentParams::door}},
                                        plain(&tent)));
    out.push_back(makeBlock<OutfitParams>("cloakTunicBelt", "Tunic, Belt and Hooded Cloak",
                                          {{"tunicLength", &OutfitParams::tunicLength, 0.5f, 1.3f},
                                           {"cloakLength", &OutfitParams::cloakLength, 0.4f, 1.5f},
                                           {"beltEase", &OutfitParams::beltEase, 0.f, 0.3f},
                                           {"hood", &OutfitParams::hood}},
                                          Generator<OutfitParams>(&cloakTunicBelt)));
    return out;
}

} // namespace blocks_detail

const std::vector<Block>& blocks()
{
    static const std::vector<Block> all = makeBlocks();
    return all;
}

const Block* findBlock(const std::string& id)
{
    for (const Block& b : blocks())
        if (b.id == id)
            return &b;
    return nullptr;
}

std::vector<std::string> blockNames()
{
    std::vector<std::string> out;
    for (const Block& b : blocks())
        out.push_back(b.id);
    return out;
}

bool buildBlock(const std::string& id, Garment& out, std::string* error, const Body& body, const BlockParamValues& values,
                std::vector<BlockAnchor>* anchors)
{
    const Block* b = findBlock(id);
    if (!b)
    {
        if (error)
            *error = "unknown block '" + id + "'";
        return false;
    }
    out = b->build(body, values, anchors);
    return true;
}

} // namespace tailor
