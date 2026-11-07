// terrainCheck's road and water checks, read from what terrainRoads and terrainWater report.

#include "Checks.h"

#include <lila/Command.h>
#include <lila/Profile.h>

#include <algorithm>
#include <format>

using namespace lila;

namespace terrain
{
namespace checks_
{
using json = nlohmann::json;

json run(const char* command, const std::string& terrain)
{
    if (!ops::find(command))
        return nullptr;
    try
    {
        return ops::execute(command, {{"args", {terrain}}});
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** j[key] when j is an object holding an object or array there, else fallback. */
json member(const json& j, const char* key, json fallback)
{
    if (!j.is_object())
        return fallback;
    auto it = j.find(key);
    return it != j.end() && it->type() == fallback.type() ? *it : fallback;
}

double number(const json& j, const char* key, double fallback)
{
    if (!j.is_object())
        return fallback;
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

std::string text(const json& j, const char* key)
{
    if (!j.is_object())
        return {};
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

glm::dvec3 vec3Of(const json& p) { return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()}; }

/** Whether water stands at a point of the heightfield's own XZ: authored water masks, or a river of order 3 and up. */
bool waterAt(const Analysis& an, glm::dvec2 p)
{
    const Heightfield& h = an.heightfield();
    for (const char* m : {"water", "riverBed", "lakeBed", "sea"})
        if (h.findMask(m) && h.maskAt(m, p.x, p.y) > 0.5)
            return true;
    const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    const int32_t r = an.reachOf()[h.index(x, z)];
    return r >= 0 && an.reaches()[size_t(r)].order >= 3;
}

void roadIssues(const std::string& terrain, const Analysis& an, const glm::dmat4& inv, double tol, json& issues)
{
    const json roads = run("terrainRoads", terrain);
    if (!roads.is_object())
        return;
    const json layers = member(roads, "layers", json::object());
    for (const auto& [layer, list] : layers.items())
        for (const json& r : list)
        {
            const std::string about = layer + "/" + text(r, "id");
            const json line = member(r, "centreline", json::array());
            if (line.empty())
                continue;
            const json at = line[line.size() / 2];
            const json design = member(r, "design", json::object());
            const double maxGrade = number(design, "maxGrade", 0.0), maxBank = number(design, "maxBank", 0.0);
            if (maxGrade > 0.0 && number(r, "maxGrade", 0.0) > maxGrade + tol)
                issues.push_back({{"type", "roadGrade"}, {"about", about}, {"position", at},
                                  {"message", std::format("grade {:.1f}% over its {:.1f}%", number(r, "maxGrade", 0.0), maxGrade)}});
            if (maxBank > 0.0 && number(r, "maxBank", 0.0) > maxBank + tol)
                issues.push_back({{"type", "roadBank"}, {"about", about}, {"position", at},
                                  {"message", std::format("bank {:.1f}% over its {:.1f}%", number(r, "maxBank", 0.0), maxBank)}});
            const json along = member(r, "along", json::array());
            const json spans = member(r, "spans", json::array());
            const json culverts = member(r, "culverts", json::array());
            const Heightfield& h = an.heightfield();
            for (size_t k = 0; k < line.size() && k < along.size(); ++k)
            {
                const glm::dvec3 w = vec3Of(line[k]);
                const glm::dvec4 q = inv * glm::dvec4(w, 1.0);
                if (!waterAt(an, {q.x, q.z}))
                    continue;
                if (!along[k].is_number())
                    continue;
                const double s = along[k].get<double>();
                const bool spanned = std::any_of(spans.begin(), spans.end(), [&](const json& sp) {
                    return !text(sp, "kind").empty() && s >= number(sp, "from", 0.0) - 1.0 && s <= number(sp, "to", 0.0) + 1.0;
                });
                const bool culverted = std::any_of(culverts.begin(), culverts.end(), [&](const json& c) { return glm::length(vec3Of(c) - w) < 3.0 * h.cell; });
                if (spanned || culverted)
                    continue;
                issues.push_back({{"type", "roadWater"}, {"about", about}, {"position", line[k]},
                                  {"message", std::format("crosses water {:.0f} cm along with no bridge or culvert", s)}});
                break;
            }
        }
}

void waterIssues(const std::string& terrain, json& issues)
{
    const json w = run("terrainWater", terrain);
    if (!w.is_object())
        return;
    const json water = member(w, "water", json::object());
    for (const json& s : member(water, "splines", json::array()))
    {
        if (text(s, "kind") != "river")
            continue;
        const json pts = member(s, "points", json::array());
        for (size_t k = 1; k < pts.size(); ++k)
        {
            const double bedRise = number(pts[k], "bedHeight", 0.0) - number(pts[k - 1], "bedHeight", 0.0);
            const double levelRise = number(pts[k], "waterLevel", 0.0) - number(pts[k - 1], "waterLevel", 0.0);
            if (bedRise > 1.0 || levelRise > 1.0)
            {
                const bool bed = bedRise >= levelRise;
                issues.push_back({{"type", "riverUphill"}, {"about", text(s, "id")}, {"position", member(pts[k], "position", json::array())},
                                  {"message", std::format("{} rises {:.0f} cm downstream", bed ? "bed" : "water level", bed ? bedRise : levelRise)}});
                break;
            }
        }
    }
    json lakePositions = json::object();
    for (const json& l : member(water, "locators", json::array()))
        if (text(l, "kind") == "lakeLevel")
            lakePositions[text(l, "id")] = member(l, "position", json::array());
    for (const json& l : member(w, "lakes", json::array()))
    {
        const std::string kind = text(l, "kind"), id = text(l, "id");
        if (kind == "sea" || kind == "pond")
            continue;
        const double level = number(l, "level", 0.0), spill = number(l, "spillHeight", level), dam = number(l, "damHeight", 0.0);
        const json at = lakePositions.contains(id) ? lakePositions[id] : json();
        if (level < spill - 10.0)
            issues.push_back({{"type", "lakeNoOutlet"}, {"about", id}, {"position", at},
                              {"message", std::format("level {:.0f} cm is {:.0f} cm below its rim, so nothing flows out", level, spill - level)}});
        else if (level > spill + 10.0 && dam <= 0.0)
            issues.push_back({{"type", "lakeOverflow"}, {"about", id}, {"position", at},
                              {"message", std::format("level {:.0f} cm is above its rim with no dam", level)}});
    }
}
} // namespace checks_

nlohmann::json featureIssues(const std::string& terrain, const Analysis& an, const glm::dmat4& world, double gradeTolerance)
{
    LILA_ZONE();

    json issues = json::array();
    checks_::roadIssues(terrain, an, glm::inverse(world), gradeTolerance, issues);
    checks_::waterIssues(terrain, issues);
    return issues;
}
} // namespace terrain
