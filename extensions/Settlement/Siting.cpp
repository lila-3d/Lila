// Settlement: siting rules as weighted terms with presets per kind of place, and the score they give a candidate site.

#include "SettlementTerrain.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace settlement
{
namespace
{
struct Preset
{
    const char* kind;
    SitingRules rules;
};

SitingRules rulesOf(const char* kind, double flat, double water, double trade, double defense, double resource, double climate, double flood)
{
    SitingRules r;
    r.kind = kind;
    r.flat = flat;
    r.water = water;
    r.trade = trade;
    r.defense = defense;
    r.resource = resource;
    r.climate = climate;
    r.flood = flood;
    return r;
}

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p = {
        {"town", rulesOf("town", 0.45, 0.4, 0.15, 0.0, 0.1, 0.05, 0.8)},
        {"market", rulesOf("market", 0.35, 0.2, 0.45, 0.0, 0.1, 0.0, 0.8)},
        {"port", rulesOf("port", 0.25, 0.45, 0.2, 0.0, 0.4, 0.0, 0.5)},
        {"mining", rulesOf("mining", 0.15, 0.1, 0.05, 0.05, 0.6, 0.0, 0.6)},
        {"fort", rulesOf("fort", 0.15, 0.05, 0.15, 0.65, 0.0, 0.0, 0.9)},
        {"farming", rulesOf("farming", 0.35, 0.25, 0.05, 0.0, 0.45, 0.2, 0.5)},
        {"mill", rulesOf("mill", 0.2, 0.3, 0.1, 0.0, 0.5, 0.0, 0.4)},
    };
    return p;
}

double SitingRules::*weightOf(SiteTerm t)
{
    switch (t)
    {
    case SiteTerm::Flat: return &SitingRules::flat;
    case SiteTerm::Water: return &SitingRules::water;
    case SiteTerm::Trade: return &SitingRules::trade;
    case SiteTerm::Defense: return &SitingRules::defense;
    case SiteTerm::Resource: return &SitingRules::resource;
    case SiteTerm::Climate: return &SitingRules::climate;
    case SiteTerm::Flood: return &SitingRules::flood;
    case SiteTerm::Count: break;
    }
    return &SitingRules::flat;
}
} // namespace

std::vector<std::string> sitingKinds()
{
    std::vector<std::string> out;
    for (const Preset& p : presets())
        out.push_back(p.kind);
    return out;
}

bool sitingPreset(std::string_view kind, SitingRules* out)
{
    for (const Preset& p : presets())
        if (kind == p.kind)
        {
            *out = p.rules;
            return true;
        }
    return false;
}

const char* termName(SiteTerm t)
{
    switch (t)
    {
    case SiteTerm::Flat: return "flat";
    case SiteTerm::Water: return "water";
    case SiteTerm::Trade: return "trade";
    case SiteTerm::Defense: return "defense";
    case SiteTerm::Resource: return "resource";
    case SiteTerm::Climate: return "climate";
    case SiteTerm::Flood: return "flood";
    case SiteTerm::Count: break;
    }
    return "flat";
}

json toJson(const SitingRules& r)
{
    json j = {{"kind", r.kind}};
    for (size_t t = 0; t < size_t(SiteTerm::Count); ++t)
        j[termName(SiteTerm(t))] = r.*weightOf(SiteTerm(t));
    return j;
}

void applyJson(SitingRules& r, const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("siting rules must be a JSON object");
    if (j.contains("kind"))
    {
        if (!j["kind"].is_string() || !sitingPreset(j["kind"].get<std::string>(), &r))
        {
            std::string kinds;
            for (const std::string& k : sitingKinds())
                kinds += (kinds.empty() ? "" : ", ") + k;
            throw std::runtime_error("unknown siting kind " + j["kind"].dump() + " (" + kinds + ")");
        }
    }
    for (auto& [k, v] : j.items())
    {
        if (k == "kind")
            continue;
        bool done = false;
        for (size_t t = 0; t < size_t(SiteTerm::Count) && !done; ++t)
            if (k == termName(SiteTerm(t)))
            {
                if (!v.is_number() || v.get<double>() < 0.0)
                    throw std::runtime_error("siting weight " + k + " takes a number of 0 or more");
                r.*weightOf(SiteTerm(t)) = v.get<double>();
                done = true;
            }
        if (!done)
            throw std::runtime_error("unknown siting field '" + k + "' (kind, flat, water, trade, defense, resource, climate, flood)");
    }
}

SiteScore scoreSite(const SiteFacts& f, const SitingRules& r, double jitter)
{
    SiteScore s;
    s.p = f.p;
    json terms = json::object();
    double sum = 0.0, weights = 0.0;
    for (size_t t = 0; t < size_t(SiteTerm::Count); ++t)
    {
        const SiteTerm term = SiteTerm(t);
        const double w = r.*weightOf(term);
        json j = {{"weight", w}};
        if (!f.known[t])
        {
            j["value"] = nullptr;
            j["why"] = "no data on this ground";
            terms[termName(term)] = j;
            continue;
        }
        j["value"] = f.value[t];
        j["why"] = f.why[t];
        if (term != SiteTerm::Flood)
        {
            sum += w * f.value[t];
            weights += w;
            j["contribution"] = w * f.value[t];
        }
        terms[termName(term)] = j;
    }
    double score = weights > 0.0 ? sum / weights : 0.0;
    if (f.known[size_t(SiteTerm::Flood)])
    {
        const double cut = std::clamp(r.flood * f.value[size_t(SiteTerm::Flood)], 0.0, 1.0);
        score *= 1.0 - cut;
        terms["flood"]["contribution"] = -cut;
    }
    score *= f.edge * f.gate;
    s.score = score + jitter;
    s.reasons = {{"kind", r.kind}, {"terms", terms}, {"edge", f.edge}};
    if (f.gate < 1.0)
        s.reasons["buildable"] = f.gateWhy;
    return s;
}

std::vector<SiteScore> pickSites(std::vector<SiteScore> all, int count, double spacing)
{
    LILA_ZONE();

    std::stable_sort(all.begin(), all.end(), [](const SiteScore& x, const SiteScore& y) { return x.score > y.score; });
    std::vector<SiteScore> out;
    for (SiteScore& s : all)
    {
        if (int(out.size()) >= count)
            break;
        bool clear = true;
        for (const SiteScore& c : out)
            clear = clear && glm::length(c.p - s.p) >= spacing;
        if (clear)
            out.push_back(std::move(s));
    }
    return out;
}
} // namespace settlement
