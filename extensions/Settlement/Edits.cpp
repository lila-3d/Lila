// Settlement: the user's edits (streets, routes, exclusions, road profiles) read from their JSON form.

#include "SettlementCore.h"
#include "Parcels.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace settlement
{
namespace
{
glm::dvec3 editPoint(const json& p, const std::string& what)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw std::runtime_error(what + ": each point is [x, z] or [x, y, z]");
    for (const json& v : p)
        if (!v.is_number())
            throw std::runtime_error(what + ": point coordinates are numbers");
    if (p.size() == 2)
        return {p[0].get<double>(), std::numeric_limits<double>::quiet_NaN(), p[1].get<double>()};
    return {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
}

std::vector<glm::dvec3> editPoints(const json& list, const std::string& what)
{
    std::vector<glm::dvec3> out;
    if (!list.is_array())
        throw std::runtime_error(what + " is a list of points");
    for (const json& p : list)
        out.push_back(editPoint(p, what));
    return out;
}

SitePath pathFrom(const json& j, bool routed)
{
    const char* kind = routed ? "route" : "street";
    if (!j.is_object())
        throw std::runtime_error(std::string(kind) + "s are objects");
    SitePath p;
    p.routed = routed;
    p.id = j.value("id", -1);
    p.name = j.value("name", std::string());
    const std::string what = std::string(kind) + " " + std::to_string(p.id);
    p.pts = editPoints(j.value(routed ? "stops" : "points", json::array()), what);
    if (p.pts.size() < 2)
        throw std::runtime_error(what + " needs two or more " + (routed ? "stops" : "points"));
    if (const std::string c = j.value("class", std::string(routed ? "local" : "collector")); !parseClass(c, &p.cls) || p.cls >= StreetClass::Boundary)
        throw std::runtime_error(what + ": unknown class '" + c + "' (arterial, collector, local, alley, path)");
    p.profile = j.value("profile", std::string());
    p.width = j.value("width", 0.0);
    if (const std::string m = j.value("mode", std::string("auto")); !parsePathMode(m, &p.mode))
        throw std::runtime_error(what + ": unknown mode '" + m + "' (auto, ground, surface)");
    return p;
}
} // namespace

const char* pathModeName(PathMode m)
{
    switch (m)
    {
    case PathMode::Ground: return "ground";
    case PathMode::Surface: return "surface";
    case PathMode::Auto: return "auto";
    }
    return "auto";
}

bool parsePathMode(std::string_view name, PathMode* out)
{
    for (PathMode m : {PathMode::Ground, PathMode::Surface, PathMode::Auto})
        if (name == pathModeName(m))
        {
            *out = m;
            return true;
        }
    return false;
}

void applyEdits(Site& site, const json& edits)
{
    if (edits.is_null())
        return;
    if (!edits.is_object())
        throw std::runtime_error("edits must be a JSON object");
    for (const json& s : edits.value("streets", json::array()))
        site.paths.push_back(pathFrom(s, false));
    for (const json& r : edits.value("routes", json::array()))
        site.paths.push_back(pathFrom(r, true));
    for (const json& x : edits.value("exclusions", json::array()))
    {
        if (!x.is_object())
            throw std::runtime_error("exclusions are objects");
        Exclusion e;
        e.id = x.value("id", -1);
        e.name = x.value("name", std::string());
        e.source = x.value("source", std::string());
        e.margin = std::max(0.0, x.value("margin", 0.0));
        for (const glm::dvec3& p : editPoints(x.value("polygon", json::array()), "exclusion " + std::to_string(e.id)))
            e.pts.push_back({p.x, p.z});
        if (e.pts.size() < 3)
            throw std::runtime_error("exclusion " + std::to_string(e.id) + " needs three or more points");
        site.exclusions.push_back(std::move(e));
    }
    for (const json& o : edits.value("streetOverrides", json::array()))
    {
        if (!o.is_object() || !o.contains("street") || !o["street"].is_number_integer())
            throw std::runtime_error("street overrides are objects with an integer street id");
        StreetOverride so;
        so.street = o["street"].get<uint64_t>();
        so.remove = o.value("remove", false);
        if (o.contains("class"))
        {
            const std::string c = o["class"].get<std::string>();
            if (!parseClass(c, &so.cls) || so.cls >= StreetClass::Boundary)
                throw std::runtime_error("street override " + std::to_string(so.street) + ": unknown class '" + c + "'");
            so.hasClass = true;
        }
        so.profile = o.value("profile", std::string());
        so.width = std::max(0.0, o.value("width", 0.0));
        so.name = o.value("name", std::string());
        site.overrides.push_back(std::move(so));
    }
    if (const json profiles = edits.value("roadProfiles", json::object()); profiles.is_object())
        for (const auto& [name, value] : profiles.items())
        {
            RoadProfile r;
            if (value.is_object() && value.contains("base"))
                if (!builtinRoadProfile(value["base"].get<std::string>(), &r))
                    throw std::runtime_error("road profile " + name + ": unknown base '" + value["base"].get<std::string>() + "'");
            json fields = value;
            if (fields.is_object())
                fields.erase("base");
            applyJson(r, fields);
            r.name = name;
            site.roadProfiles.push_back(r);
        }
    if (const json naming = edits.value("naming", json::object()); naming.is_object())
    {
        site.culture = naming.value("culture", std::string());
        if (const json districts = naming.value("districts", json::object()); districts.is_object())
            for (const auto& [id, name] : districts.items())
            {
                uint64_t key = 0;
                if (std::from_chars(id.data(), id.data() + id.size(), key).ec != std::errc() || !name.is_string())
                    throw std::runtime_error("naming: districts are {id: name}");
                site.districtNames.emplace_back(key, name.get<std::string>());
            }
    }
    // Paths keep their order by id, so the network does not depend on how the JSON was written.
    std::stable_sort(site.paths.begin(), site.paths.end(), [](const SitePath& a, const SitePath& b) { return a.id < b.id; });
    applyParcelEdits(site, edits);
}
} // namespace settlement
