// trimCheck's measurements: every island of a trim-mapped mesh against the trim it lies on.

#include "Check.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

namespace trims
{
namespace
{
double wrapPx(double x, double m) { return x - std::floor(x / m) * m; }

/** Styles whose smallest feature is their detail (rivets, slats, glyph strokes) rather than their bevel. */
bool detailed(Style s)
{
    switch (s)
    {
    case Style::Rivets:
    case Style::Bolts:
    case Style::Vents:
    case Style::Tread:
    case Style::Ribs:
    case Style::Fluting:
    case Style::Dentils:
    case Style::Meander:
    case Style::Glyphs:
    case Style::Chipped:
    case Style::Grating:
    case Style::Conduit:
    case Style::Hazard: return true;
    default: return false;
    }
}

/** The largest k (up to 12) with every border of the trim that clamps islands on a multiple of 2^k px. */
int gridLevel(const Sheet& s, const Trim& t)
{
    std::vector<int> borders;
    if (t.kind != uv::TrimKind::StripU)
        borders.insert(borders.end(), {t.min.x, t.max.x});
    if (t.kind != uv::TrimKind::StripV)
        borders.insert(borders.end(), {t.min.y, t.max.y});
    int level = 12;
    for (int b : borders)
        if (b != 0 && b != s.width && b != s.height)
            while (level > 0 && b % (1 << level) != 0)
                --level;
    return level;
}

/** Angle off the nearest quarter turn, degrees, and that quarter turn. */
std::pair<double, int> quarterOf(double degrees)
{
    const int k = int(std::lround(degrees / 90.0));
    return {std::abs(degrees - k * 90.0), ((k % 4) + 4) % 4};
}
} // namespace

json IslandCheck::toJson(const Sheet& sheet) const
{
    return {{"faces", int64_t(faces.size())},
            {"firstFace", faces.empty() ? int64_t(-1) : int64_t(faces.front())},
            {"trim", trim >= 0 ? json(sheet.trims[size_t(trim)].name) : json(nullptr)},
            {"pxPerM", pxPerM},
            {"densityError", densityError},
            {"outsidePx", outsidePx},
            {"marginPx", marginPx},
            {"quarterTurns", quarterTurns},
            {"skewDeg", skewDeg},
            {"mirrored", mirrored},
            {"stretch", stretch},
            {"trimsCrossed", trimsCrossed},
            {"cleanToMip", cleanToMip},
            {"featurePx", featurePx},
            {"repeatsWith", repeatsWith},
            {"issues", issues}};
}

std::map<std::string, int> MeshCheck::issueCounts() const
{
    std::map<std::string, int> out;
    for (const IslandCheck& is : islands)
        for (const std::string& i : is.issues)
            ++out[i];
    return out;
}

json MeshCheck::toJson(const Sheet& sheet, bool all) const
{
    json list = json::array();
    size_t bad = 0;
    for (size_t i = 0; i < islands.size(); ++i)
    {
        bad += !islands[i].issues.empty();
        if (all || !islands[i].issues.empty())
        {
            json j = islands[i].toJson(sheet);
            j["index"] = int64_t(i);
            list.push_back(std::move(j));
        }
    }
    double lo = 1e300, hi = 0.0;
    for (const IslandCheck& is : islands)
        if (is.pxPerM > 0.0)
        {
            lo = std::min(lo, is.pxPerM);
            hi = std::max(hi, is.pxPerM);
        }
    return {{"islands", int64_t(islands.size())},
            {"withIssues", int64_t(bad)},
            {"ok", bad == 0},
            {"targetPxPerM", targetPxPerM},
            {"pxPerM", islands.empty() ? json(nullptr) : json({{"min", lo}, {"max", hi}})},
            {"issues", issueCounts()},
            {"details", list}};
}

MeshCheck checkMesh(const geo::Mesh& mesh, const Sheet& sheet, const CheckOptions& opts)
{
    LILA_ZONE();

    MeshCheck out;
    const double target = opts.targetPxPerM > 0.0 ? opts.targetPxPerM : sheet.pxPerM;
    out.targetPxPerM = target;
    if (mesh.cornerAttrs().get<glm::vec2>(opts.uvSet).empty())
        return out;
    std::span<const int32_t> trimAttr = mesh.faceAttrs().get<int32_t>(kFaceTrim);
    std::vector<geo::Index> faces;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && (trimAttr.empty() || uv::trimMapped(trimAttr, f)))
            faces.push_back(f);
    if (faces.empty())
        return out;
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = opts.uvSet;
    co.pinAttr = {};
    const std::vector<uv::Chart> charts = uv::buildCharts(mesh, faces, co);

    const glm::dvec2 px(sheet.width, sheet.height);
    const glm::dvec3 up(0.0, 1.0, 0.0);
    const double pxPerCmOnScreen = opts.screenHeightPx / (2.0 * opts.distanceCm * std::tan(glm::radians(opts.fovDeg) * 0.5));
    std::vector<int> islandOf(mesh.faceCapacity(), -1);
    // Same trim, offset along it (px), turn and mirroring: two islands that would show identical texels.
    std::vector<std::tuple<int, long, int, bool>> keys(charts.size());
    out.islands.resize(charts.size());

    for (size_t i = 0; i < charts.size(); ++i)
    {
        const uv::Chart& ch = charts[i];
        IslandCheck& c = out.islands[i];
        c.faces = ch.faces;
        for (geo::Index f : ch.faces)
            islandOf[f] = int(i);

        std::vector<glm::dvec3> p(ch.pos.size());
        for (size_t v = 0; v < p.size(); ++v)
            p[v] = glm::dvec3(opts.world * glm::dvec4(ch.pos[v], 1.0));
        // uv in sheet px, y down like the texture.
        std::vector<glm::dvec2> q(ch.uv.size());
        for (size_t v = 0; v < q.size(); ++v)
            q[v] = glm::dvec2(ch.uv[v].x * px.x, (1.0 - ch.uv[v].y) * px.y);

        std::map<int, size_t> votes;
        if (!trimAttr.empty())
            for (geo::Index f : ch.faces)
                ++votes[trimAttr[f] - 1];
        double area3d = 0.0, areaUv = 0.0, stretchSum = 0.0;
        glm::dvec2 upGrad(0.0), xGrad(0.0);
        glm::dvec3 normal(0.0);
        std::set<int> crossed;
        for (const auto& t : ch.tris)
        {
            const glm::dvec3 e1 = p[t[1]] - p[t[0]], e2 = p[t[2]] - p[t[0]];
            const glm::dvec3 n = glm::cross(e1, e2);
            const double a = glm::length(n) * 0.5;
            if (a <= 1e-12)
                continue;
            area3d += a;
            normal += n;
            // uv in px with y up, so a positive area keeps the 3D winding.
            const glm::dvec2 d1(q[t[1]].x - q[t[0]].x, q[t[0]].y - q[t[1]].y), d2(q[t[2]].x - q[t[0]].x, q[t[0]].y - q[t[2]].y);
            const double det = d1.x * d2.y - d2.x * d1.y;
            areaUv += det * 0.5;
            // Jacobian from the triangle's own 2D frame (cm) to px; its singular values are the scale along each way.
            const glm::dvec3 ux = glm::normalize(e1), vy = glm::normalize(glm::cross(n, e1));
            const glm::dmat2 from(glm::dvec2(glm::length(e1), 0.0), glm::dvec2(glm::dot(e2, ux), glm::dot(e2, vy)));
            const glm::dmat2 to(d1, d2);
            if (std::abs(glm::determinant(from)) > 1e-18)
            {
                const glm::dmat2 j = to * glm::inverse(from);
                const double fro = j[0][0] * j[0][0] + j[0][1] * j[0][1] + j[1][0] * j[1][0] + j[1][1] * j[1][1];
                const double dt = std::abs(glm::determinant(j));
                const double disc = std::sqrt(std::max(0.0, fro * fro - 4.0 * dt * dt));
                const double s1 = std::sqrt(std::max(0.0, (fro + disc) * 0.5)), s2 = std::sqrt(std::max(0.0, (fro - disc) * 0.5));
                if (s2 > 1e-12)
                    stretchSum += (s1 / s2 - 1.0) * a;
            }
            // Gradients of world height and world X over px (y up).
            if (std::abs(det) > 1e-18)
            {
                auto grad = [&](double f0, double f1, double f2) {
                    const double g1 = f1 - f0, g2 = f2 - f0;
                    return glm::dvec2((g1 * d2.y - g2 * d1.y) / det, (g2 * d1.x - g1 * d2.x) / det) * std::abs(det);
                };
                upGrad += grad(glm::dot(p[t[0]], up), glm::dot(p[t[1]], up), glm::dot(p[t[2]], up));
                xGrad += grad(p[t[0]].x, p[t[1]].x, p[t[2]].x);
            }
            // The centre and corners pulled slightly in, so a corner on a trim's own border doesn't count as the neighbour.
            const glm::dvec2 centre = (ch.uv[t[0]] + ch.uv[t[1]] + ch.uv[t[2]]) / 3.0;
            crossed.insert(sheet.trimAt(centre));
            for (int k = 0; k < 3; ++k)
                crossed.insert(sheet.trimAt(glm::mix(ch.uv[t[k]], centre, 0.02)));
        }
        c.trimsCrossed = int(crossed.size());
        if (!votes.empty())
            c.trim = std::ranges::max_element(votes, {}, [](const auto& kv) { return kv.second; })->first;
        else
        {
            glm::dvec2 centre(0.0);
            for (const glm::dvec2& u : ch.uv)
                centre += u;
            c.trim = ch.uv.empty() ? -1 : sheet.trimAt(centre / double(ch.uv.size()));
        }
        if (c.trim >= int(sheet.trims.size()))
            c.trim = -1;
        c.mirrored = areaUv < 0.0;
        c.pxPerM = area3d > 0.0 ? std::sqrt(std::abs(areaUv) / (area3d * 1e-4)) : 0.0;
        c.densityError = c.pxPerM > 0.0 ? std::log2(c.pxPerM / target) : 0.0;
        c.stretch = area3d > 0.0 ? stretchSum / area3d : 0.0;
        if (glm::length(normal) > 0.0)
            normal = glm::normalize(normal);
        const bool flat = std::abs(glm::dot(normal, up)) > std::cos(glm::radians(10.0));
        if (!flat && glm::length(upGrad) > 0.0)
        {
            // 0 when world up runs up +V, 90 when it runs along +U.
            const auto [skew, k] = quarterOf(glm::degrees(std::atan2(upGrad.x, upGrad.y)));
            c.skewDeg = skew;
            c.quarterTurns = k;
        }
        else
        {
            c.quarterTurns = -1;
            c.skewDeg = glm::length(xGrad) > 0.0 ? quarterOf(glm::degrees(std::atan2(xGrad.x, xGrad.y))).first : 0.0;
        }

        glm::dvec2 lo(1e300), hi(-1e300);
        for (const glm::dvec2& v : q)
        {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
        long offset = 0;
        if (c.trim >= 0)
        {
            const Trim& t = sheet.trims[size_t(c.trim)];
            const bool clampX = t.kind != uv::TrimKind::StripU, clampY = t.kind != uv::TrimKind::StripV;
            double outside = 0.0, margin = 1e300;
            if (clampY)
            {
                outside = std::max({outside, t.min.y - lo.y, hi.y - t.max.y});
                margin = std::min({margin, lo.y - t.min.y, t.max.y - hi.y});
            }
            if (clampX)
            {
                outside = std::max({outside, t.min.x - lo.x, hi.x - t.max.x});
                margin = std::min({margin, lo.x - t.min.x, t.max.x - hi.x});
            }
            c.outsidePx = std::max(0.0, outside);
            c.marginPx = margin;
            const int grid = gridLevel(sheet, t);
            c.cleanToMip = margin < -0.01 ? 0 : grid;
            const double featureSheetPx =
                t.kind == uv::TrimKind::Flat || t.profile.style == Style::Flat
                    ? 0.0
                    : (detailed(t.profile.style) ? std::min(t.profile.bevelPx, t.profile.detailPx) : t.profile.bevelPx);
            c.featurePx = c.pxPerM > 0.0 ? featureSheetPx / (c.pxPerM / 100.0) * pxPerCmOnScreen : 0.0;

            if (std::abs(c.densityError) > opts.densityTolerance)
                c.issues.push_back("density");
            if (c.outsidePx > 0.5)
                c.issues.push_back("outside");
            if (c.trimsCrossed > 1)
                c.issues.push_back("crossesTrims");
            if (!t.allowRotate && c.quarterTurns > 0)
                c.issues.push_back("rotated");
            if (c.skewDeg > opts.maxSkew)
                c.issues.push_back("skewed");
            if (c.mirrored && !t.allowMirror)
                c.issues.push_back("mirrored");
            if (c.stretch > opts.maxStretch && t.fit != uv::TrimFit::Fill)
                c.issues.push_back("stretched");
            if (c.cleanToMip < std::min(sheet.mipLevel, 12))
                c.issues.push_back("mipBleed");
            if (featureSheetPx > 0.0 && c.featurePx < opts.minFeaturePx)
                c.issues.push_back("unreadable");
            if (t.kind == uv::TrimKind::StripU)
                offset = std::lround(wrapPx(lo.x, px.x));
            else if (t.kind == uv::TrimKind::StripV)
                offset = std::lround(wrapPx(lo.y, px.y));
        }
        else
            c.issues.push_back("unmapped");
        keys[i] = {c.trim, offset, c.quarterTurns, c.mirrored};
    }

    // Repetition: neighbours across an edge that would show the same texels side by side.
    for (size_t i = 0; i < charts.size(); ++i)
    {
        IslandCheck& c = out.islands[i];
        if (c.trim < 0)
            continue;
        for (geo::Index f : c.faces)
        {
            mesh.forEachFaceCorner(f, [&](geo::Index k) {
                if (c.repeatsWith >= 0)
                    return;
                const geo::Index other = mesh.oppositeFace(mesh.cornerEdge(k), f);
                if (other == geo::kInvalid || other >= islandOf.size())
                    return;
                const int j = islandOf[other];
                if (j >= 0 && size_t(j) != i && keys[size_t(j)] == keys[i])
                    c.repeatsWith = j;
            });
            if (c.repeatsWith >= 0)
                break;
        }
        if (c.repeatsWith >= 0)
            c.issues.push_back("repeats");
    }
    return out;
}
} // namespace trims
