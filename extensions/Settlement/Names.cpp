// Settlement: street and district names from a naming culture, stable by the ids of the streets they belong to.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace settlement
{
namespace
{
using Words = std::vector<const char*>;

/** Words a culture builds names from: roots, and per class the words that go before or after them. */
struct Culture
{
    const char* name;
    Words roots;
    std::array<Words, 5> classWords; // arterial, collector, local, alley, path
    bool wordFirst = false;          // "Rue X" rather than "X Street"
    Words districtRoots;
    Words districtEnds; // joined onto a root ("Ash" + "ford"); empty: the root alone
    const char* districtPrefix = "";
};

const std::vector<Culture>& cultures()
{
    static const std::vector<Culture> all = {
        {"oldEnglish",
         {"Mill", "Church", "Market", "Bridge", "Castle", "Abbey", "Kings", "Queens", "Bishops", "Friars", "Tanners", "Weavers",
          "Smiths", "Coopers", "Bakers", "Cheap", "Wool", "Salt", "Fish", "Horse", "Sheep", "Barley", "Oak", "Ash", "Elm",
          "Thorn", "Holly", "Willow", "Hazel", "Rose", "Swan", "Crown", "Mint", "Cross", "Chapel", "Priory", "Manor", "Hall",
          "Green", "Moor", "Fen", "Marsh", "Brook", "Well", "Spring", "Stone", "Flint", "Chalk", "Hay", "Corn", "Malt", "Bell",
          "Lamb", "Fox", "Hart", "Raven", "Pudding", "Silver", "Copper", "Iron", "Water", "Wind", "High", "Broad", "Long",
          "Narrow", "Old", "Shambles", "Butchers", "Glovers", "Potters", "Ropers", "Dyers", "Mercers", "Chandlers"},
         {Words{"Street", "Road", "Way", "Gate"}, Words{"Street", "Lane", "Row", "Gate"}, Words{"Lane", "Row", "Close", "Yard", "Court"},
          Words{"Alley", "Passage", "Wynd", "Ginnel", "Snicket"}, Words{"Walk", "Path", "Steps", "Twitten"}},
         false,
         {"North", "South", "East", "West", "Ash", "Oak", "Mill", "Kings", "Church", "Market", "Stone", "Thorn", "Wool", "Salt",
          "Brook", "Elm", "Hay", "Fox", "Raven", "Bell", "Bishops", "Holly", "Swan", "Marsh"},
         {"gate", "field", "ford", "bury", "wick", "ton", "ham", "stead", "cross", "moor", "dale", "side", "wood", "end", "hythe"}},
        {"latin",
         {"Appia", "Aurelia", "Flaminia", "Iulia", "Claudia", "Sacra", "Nova", "Lata", "Recta", "Salaria", "Tiburtina",
          "Ostiensis", "Cornelia", "Valeria", "Aemilia", "Cassia", "Domitia", "Augusta", "Minervae", "Martis", "Iovis",
          "Veneris", "Fortunae", "Mercurii", "Saturni", "Dianae", "Apollinis", "Vestae", "Fori", "Portae", "Fontis", "Pontis",
          "Castrorum", "Fabrorum", "Pistorum", "Lanariorum", "Vinariorum", "Argentariorum", "Tuscus", "Iugarius", "Patricius"},
         {Words{"Via"}, Words{"Via", "Vicus"}, Words{"Vicus"}, Words{"Angiportus", "Semita"}, Words{"Semita", "Clivus", "Scalae"}},
         true,
         {"Palatium", "Aventinus", "Caelius", "Esquiliae", "Viminalis", "Quirinalis", "Subura", "Velabrum", "Carinae",
          "Campus Martius", "Forum", "Emporium", "Transtiberim", "Collis Hortulorum", "Insula", "Circus", "Vicus Longus",
          "Argiletum", "Ianiculum", "Pincius"},
         {}},
        {"french",
         {"de la Paix", "du Marché", "de l'Église", "des Tanneurs", "du Château", "de la Fontaine", "des Fleurs", "du Moulin",
          "Saint-Jacques", "Saint-Martin", "Saint-Michel", "Saint-Denis", "Saint-Honoré", "Saint-Antoine", "des Orfèvres",
          "de la Poste", "du Pont", "des Écoles", "Victor Hugo", "Voltaire", "Molière", "des Lilas", "des Peupliers",
          "du Faubourg", "de la Gare", "des Halles", "de la République", "de la Liberté", "Lafayette", "Colbert", "Pasteur",
          "des Arts", "du Temple", "des Jardins", "de la Croix", "du Puits", "des Remparts", "des Carmes", "des Bouchers",
          "de la Monnaie", "des Cordeliers"},
         {Words{"Boulevard", "Avenue"}, Words{"Rue", "Avenue"}, Words{"Rue"}, Words{"Ruelle", "Passage", "Impasse"},
          Words{"Allée", "Sentier", "Escalier"}},
         true,
         {"Saint-Germain", "Latin", "du Marais", "des Halles", "de la Gare", "du Port", "Saint-Paul", "des Batignolles",
          "de Montmartre", "Saint-Roch", "du Temple", "de la Cité", "Saint-Victor", "des Ternes", "de Grenelle", "de Belleville",
          "Saint-Lazare", "de la Roquette", "des Invalides", "de l'Arsenal"},
         {},
         "Quartier "},
        {"spanish",
         {"Mayor", "Real", "del Sol", "de la Luna", "del Carmen", "de San Juan", "de Santiago", "de la Paz", "del Rosario",
          "de los Reyes", "de las Flores", "del Mercado", "de la Iglesia", "del Río", "de la Fuente", "del Puente", "de Alcalá",
          "de Toledo", "de Cervantes", "de la Constitución", "de la Merced", "de San Francisco", "de los Olivos", "del Pilar",
          "de la Victoria", "del Castillo", "de la Plata", "de los Herreros", "de la Cruz", "del Puerto", "de las Huertas",
          "de los Tintes", "del Agua", "de la Feria"},
         {Words{"Avenida", "Paseo"}, Words{"Calle"}, Words{"Calle"}, Words{"Callejón", "Pasaje"}, Words{"Sendero", "Escalinata"}},
         true,
         {"de San Juan", "del Carmen", "de Santa Cruz", "Alto", "Bajo", "del Puerto", "de la Merced", "de Triana", "del Albaicín",
          "de la Catedral", "de San Pedro", "de la Judería", "de San Lorenzo", "del Castillo", "de las Huertas", "de San Marcos",
          "de la Magdalena", "del Pilar"},
         {},
         "Barrio "},
        {"usGrid",
         {"Main", "Oak", "Maple", "Elm", "Pine", "Cedar", "Washington", "Lincoln", "Jefferson", "Madison", "Franklin", "Park",
          "Lake", "Hill", "Church", "Center", "Market", "Union", "Liberty", "Spring", "Walnut", "Cherry", "Chestnut", "Broadway",
          "Highland", "Ridge", "River", "Mill", "Jackson", "Adams", "Grant", "Monroe", "Locust", "Sycamore", "Hickory"},
         {Words{"Avenue", "Boulevard"}, Words{"Street", "Avenue"}, Words{"Street", "Drive", "Place"}, Words{"Alley"}, Words{"Walk", "Trail"}},
         false,
         {"Midtown", "Uptown", "Old Town", "Riverside", "Eastside", "Westside", "Northside", "Southside", "Hillcrest",
          "Lakeview", "Fairview", "Oak Park", "Mill District", "Arts District", "Warehouse District", "University Heights",
          "Brookside", "Greenwood", "Elmwood", "Ironbound"},
         {}},
        {"sector",
         {},
         {Words{"Trunk"}, Words{"Corridor"}, Words{"Spur"}, Words{"Duct"}, Words{"Walkway"}},
         true,
         {},
         {}},
    };
    return all;
}

const Culture& cultureNamed(const std::string& name)
{
    for (const Culture& c : cultures())
        if (name == c.name)
            return c;
    std::string known;
    for (const Culture& c : cultures())
        known += (known.empty() ? "" : ", ") + std::string(c.name);
    throw std::runtime_error("unknown naming culture '" + name + "' (" + known + ")");
}

const char* pick(const Words& w, uint64_t h) { return w[size_t(h % w.size())]; }

std::string ordinal(int n)
{
    const int tens = n % 100;
    const char* suffix = tens >= 11 && tens <= 13 ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

/** A name for the street keyed h of class c; attempt picks another when the first is taken. */
std::string cultureStreetName(const Culture& c, StreetClass cls, uint64_t h, int attempt)
{
    const size_t ci = size_t(std::min(cls, StreetClass::Path));
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
    {
        const std::string word = pick(c.classWords[ci], k);
        const int n = 1 + int(hash64(k) % 99);
        if (cls <= StreetClass::Arterial)
            return word + " " + std::to_string(n);
        return word + " " + char('A' + int(hash64(k ^ 7) % 26)) + std::to_string(n);
    }
    const std::string root = pick(c.roots, k), word = pick(c.classWords[ci], hash64(k ^ 0x51));
    return c.wordFirst ? word + " " + root : root + " " + word;
}

std::string cultureDistrictName(const Culture& c, uint64_t h, int attempt)
{
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
        return "Sector " + std::to_string(1 + int(k % 40)) + char('A' + int(hash64(k) % 26));
    std::string root = pick(c.districtRoots, k);
    if (!c.districtEnds.empty())
        root += pick(c.districtEnds, hash64(k ^ 0x33));
    return c.districtPrefix + root;
}

int findRootOf(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
        x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
    return x;
}

/** Runs of street edges carried on through junctions by their straightest continuation of the same class. */
std::vector<std::vector<int>> strokes(const StreetGraph& g)
{
    std::vector<int> parent(g.edges.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        std::vector<int> es;
        for (int e : g.incident(int(n)))
            if (g.isStreet(e))
                es.push_back(e);
        auto away = [&](int e) {
            const StreetEdge& x = g.edges[size_t(e)];
            const Vec2 d = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p - g.nodes[n].p;
            const double len = glm::length(d);
            return len > 1e-9 ? d / len : Vec2(1.0, 0.0);
        };
        struct Pair
        {
            double straight;
            int a, b;
        };
        std::vector<Pair> pairs;
        for (size_t i = 0; i < es.size(); ++i)
            for (size_t j = i + 1; j < es.size(); ++j)
            {
                if (g.edges[size_t(es[i])].cls != g.edges[size_t(es[j])].cls)
                    continue;
                const double straight = -glm::dot(away(es[i]), away(es[j]));
                // Through a plain node a street bends as it likes; at a junction it carries on only within 40 degrees.
                if (es.size() == 2 || straight >= 0.766)
                    pairs.push_back({straight, es[i], es[j]});
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) {
            return x.straight != y.straight ? x.straight > y.straight : std::pair(x.a, x.b) < std::pair(y.a, y.b);
        });
        std::set<int> used;
        for (const Pair& p : pairs)
            if (!used.count(p.a) && !used.count(p.b))
            {
                used.insert(p.a);
                used.insert(p.b);
                parent[size_t(findRootOf(parent, p.a))] = findRootOf(parent, p.b);
            }
    }
    std::map<int, std::vector<int>> byRoot;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
            byRoot[findRootOf(parent, int(e))].push_back(int(e));
    std::vector<std::vector<int>> out;
    for (auto& [root, edges] : byRoot)
        out.push_back(std::move(edges));
    return out;
}

/** Quarters between the main roads: faces of the arterials, the outline, water and exclusions. */
void findDistricts(Layout& l)
{
    StreetGraph major = l.graph;
    for (size_t e = 0; e < major.edges.size(); ++e)
        if (major.edges[e].alive && major.edges[e].cls > StreetClass::Arterial && major.edges[e].cls < StreetClass::Boundary)
            major.removeEdge(int(e));
    auto q = [](Vec2 p) { return uint64_t(std::llround(p.x / 10.0)) * 0x100000001B3ull ^ uint64_t(std::llround(p.y / 10.0)); };
    for (const Face& f : planarFaces(major))
    {
        if (f.outline.size() < 3)
            continue;
        std::set<uint64_t> around;
        for (int e : f.outline.tag)
        {
            const StreetEdge& x = major.edges[size_t(e)];
            if (x.street != 0)
                around.insert(x.street);
            else
                around.insert(hash64(q(major.nodes[size_t(x.a)].p)) ^ hash64(q(major.nodes[size_t(x.b)].p)));
        }
        uint64_t id = 0x6A09E667F3BCC908ull;
        for (uint64_t k : around)
            id = hash64(id ^ k);
        District d;
        d.id = id & ((1ull << 48) - 1); // small enough for any JSON reader
        d.outline = f.outline;
        d.area = area(f.outline) / kM2;
        d.center = centroid(f.outline);
        l.districts.push_back(std::move(d));
    }
    std::sort(l.districts.begin(), l.districts.end(), [](const District& a, const District& b) { return a.id < b.id; });
}

/** Unit direction along the grid's own axes: u along it, v across it. */
struct GridAxes
{
    Vec2 u, v;
};

/** "North 3rd Street" or "East 2nd Avenue" for a stroke running along the grid; empty when it does not. */
std::string gridName(const Layout& l, const std::vector<int>& edges, const GridAxes& ax)
{
    const StreetGraph& g = l.graph;
    Vec2 dir(0.0), mid(0.0);
    double total = 0.0;
    for (int e : edges)
    {
        const Vec2 a = g.nodes[size_t(g.edges[size_t(e)].a)].p, b = g.nodes[size_t(g.edges[size_t(e)].b)].p;
        Vec2 d = b - a;
        const double len = glm::length(d);
        if (len < 1e-9)
            continue;
        if (glm::dot(d, dir) < 0.0)
            d = -d;
        dir += d;
        mid += (a + b) * 0.5 * len;
        total += len;
    }
    if (total <= 0.0 || glm::length(dir) < 1e-9)
        return {};
    dir /= glm::length(dir);
    mid /= total;
    const double along = std::abs(glm::dot(dir, ax.u)), across = std::abs(glm::dot(dir, ax.v));
    const Vec2 off = mid - l.center;
    if (along > 0.966) // within 15 degrees of the grid's run: a numbered street, counted across the grid
    {
        const int k = int(std::lround(glm::dot(off, ax.v) / std::max(l.profile.gridDepth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "South " : "North ") + ordinal(std::abs(k) + 1) + " Street";
    }
    if (across > 0.966)
    {
        const int k = int(std::lround(glm::dot(off, ax.u) / std::max(l.profile.gridWidth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "East " : "West ") + ordinal(std::abs(k) + 1) + " Avenue";
    }
    return {};
}
} // namespace

std::vector<std::string> cultureNames()
{
    std::vector<std::string> out;
    for (const Culture& c : cultures())
        out.emplace_back(c.name);
    return out;
}

std::string defaultCulture(std::string_view profileName)
{
    std::string n(profileName);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("scifi") || has("cyber") || has("sector") || has("arcology"))
        return "sector";
    if (has("roman") || has("castrum"))
        return "latin";
    if (has("colonial") || has("spanish") || has("medina"))
        return "spanish";
    if (n.starts_with("us") || has("manhattan") || has("midwest") || has("grid"))
        return "usGrid";
    if (has("european") || has("haussmann") || has("baroque") || has("french") || has("renaissance"))
        return "french";
    return "oldEnglish";
}

void nameStreets(Layout& l, const Site& site)
{
    LILA_ZONE();

    l.culture = site.culture.empty() ? defaultCulture(l.profile.name) : site.culture;
    const Culture& culture = cultureNamed(l.culture);
    l.streetNames.clear();
    l.districts.clear();
    findDistricts(l);

    const StreetGraph& g = l.graph;
    std::map<uint64_t, std::string> given;
    for (const StreetOverride& o : site.overrides)
        if (!o.name.empty())
            given[o.street] = o.name;
    std::map<int, std::string> pathNames;
    for (const SitePath& p : site.paths)
        if (!p.name.empty())
            pathNames[p.id] = p.name;

    struct Named
    {
        uint64_t key;
        std::vector<int> edges;
        std::string name;
        bool fixed = false; // given by the user, or a grid number: shared names are meant
    };
    std::vector<Named> runs;
    const double gridRad = glm::radians(l.profile.gridAngle);
    const GridAxes ax{{std::cos(gridRad), std::sin(gridRad)}, {-std::sin(gridRad), std::cos(gridRad)}};
    const bool grid = std::string_view(culture.name) == "usGrid";
    for (std::vector<int>& edges : strokes(g))
    {
        Named n;
        n.key = ~uint64_t(0);
        uint64_t givenStreet = ~uint64_t(0);
        for (int e : edges)
        {
            const StreetEdge& x = g.edges[size_t(e)];
            n.key = std::min(n.key, hash64(x.street));
            if (auto it = given.find(x.street); it != given.end() && x.street < givenStreet)
            {
                givenStreet = x.street;
                n.name = it->second;
            }
        }
        if (n.name.empty())
            for (int e : edges)
                if (auto it = pathNames.find(g.edges[size_t(e)].path); it != pathNames.end())
                {
                    n.name = it->second;
                    break;
                }
        if (n.name.empty() && grid && g.edges[size_t(edges.front())].cls <= StreetClass::Local)
            n.name = gridName(l, edges, ax);
        n.fixed = !n.name.empty();
        n.edges = std::move(edges);
        runs.push_back(std::move(n));
    }
    // Lowest key first, so a street keeps its name whatever is added elsewhere unless two pick the same.
    std::sort(runs.begin(), runs.end(), [](const Named& a, const Named& b) { return a.key < b.key; });
    std::set<std::string> taken;
    for (const Named& n : runs)
        if (n.fixed)
            taken.insert(n.name);
    for (Named& n : runs)
    {
        if (n.fixed)
            continue;
        const StreetClass cls = g.edges[size_t(n.edges.front())].cls;
        for (int attempt = 0; attempt < 64 && n.name.empty(); ++attempt)
            if (std::string s = cultureStreetName(culture, cls, n.key, attempt); !taken.count(s))
                n.name = std::move(s);
        if (n.name.empty())
            n.name = cultureStreetName(culture, cls, n.key, 0) + " " + std::to_string(n.key % 1000);
        taken.insert(n.name);
    }
    for (const Named& n : runs)
        for (int e : n.edges)
            l.streetNames[g.edges[size_t(e)].street] = n.name;

    std::map<uint64_t, std::string> districtGiven(site.districtNames.begin(), site.districtNames.end());
    std::set<std::string> districtTaken;
    for (const District& d : l.districts)
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
            districtTaken.insert(it->second);
    for (District& d : l.districts)
    {
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
        {
            d.name = it->second;
            continue;
        }
        if (grid && contains(d.outline.pts, l.center))
            d.name = "Downtown";
        for (int attempt = 0; attempt < 64 && d.name.empty(); ++attempt)
            if (std::string s = cultureDistrictName(culture, d.id, attempt); !districtTaken.count(s))
                d.name = std::move(s);
        if (d.name.empty())
            d.name = cultureDistrictName(culture, d.id, 0) + " " + std::to_string(d.id % 100);
        districtTaken.insert(d.name);
    }
}

std::string streetName(const Layout& l, uint64_t street)
{
    auto it = l.streetNames.find(street);
    return it != l.streetNames.end() ? it->second : std::string();
}
} // namespace settlement
