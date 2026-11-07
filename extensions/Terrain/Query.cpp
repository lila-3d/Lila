// Targets in words: terrainQuery finds regions and points by predicates over the terrain's fields and masks or by
// named shortcuts (highest peak, valley floor, passes...), stores them as named regions in a Terrain Regions node
// (masks region_<name>), and other terrain commands take 'region:<name>' wherever they take a point, path or polygon.

#include "Query.h"

#include "Analysis.h"
#include "Stamps.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Node.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

using namespace lila;

namespace terrain
{
namespace query_
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;

constexpr const char* kRegionsType = "TerrainRegions";
constexpr const char* kRegionsNode = "Regions";
constexpr const char* kRefPrefix = "region:";

bool zUpScene() { return math::upAxis() == math::UpAxis::Z; }
glm::dvec3 sceneUp() { return zUpScene() ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0); }
double upCoord(const glm::dvec3& w) { return zUpScene() ? w.z : w.y; }
glm::dvec2 ground(const glm::dvec3& w) { return zUpScene() ? glm::dvec2(w.x, w.y) : glm::dvec2(w.x, w.z); }
glm::dvec3 fromGround(glm::dvec2 g) { return zUpScene() ? glm::dvec3(g.x, g.y, 0.0) : glm::dvec3(g.x, 0.0, g.y); }

/** A terrain as the query sees it: local heightfield, placement both ways and scale. */
struct QTerrain
{
    TerrainRef ref;
    glm::dmat4 inv{1.0};
    double across = 1.0, vertical = 1.0;

    glm::dvec3 world(double x, double y, double z) const { return glm::dvec3(ref.world * glm::dvec4(x, y, z, 1.0)); }
    glm::dvec2 local(glm::dvec2 g) const
    {
        const glm::dvec4 q = inv * glm::dvec4(fromGround(g), 1.0);
        return {q.x, q.z};
    }
    json groundJson(double x, double z) const
    {
        const glm::dvec2 g = ground(world(x, 0.0, z));
        return json::array({g.x, g.y});
    }
    json worldJson(double x, double z) const
    {
        const glm::dvec3 w = world(x, ref.local->heightAt(x, z), z);
        return json::array({w.x, w.y, w.z});
    }
    double worldHeight(double x, double y, double z) const { return upCoord(world(x, y, z)); }
    double cellAreaM2() const { return std::pow(ref.local->cell * across, 2.0) * 1e-4; }
};

QTerrain findQ(const std::string& name, const std::string& cmd)
{
    QTerrain q;
    q.ref = findTerrainRef(name, cmd);
    q.inv = glm::inverse(q.ref.world);
    q.across = 0.5 * (glm::length(glm::dvec3(q.ref.world[0])) + glm::length(glm::dvec3(q.ref.world[2])));
    q.vertical = glm::length(glm::dvec3(q.ref.world[1]));
    return q;
}

// ---------------------------------------------------------------------------------------------------- the node

json parseRegions(const std::string& text)
{
    if (text.empty())
        return json::object();
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json::object();
}

std::vector<std::vector<glm::dvec2>> ringsOf(const json& region)
{
    std::vector<std::vector<glm::dvec2>> out;
    for (const json& r : region.value("rings", json::array()))
    {
        std::vector<glm::dvec2> ring;
        for (const json& p : r)
            ring.push_back({p[0].get<double>(), p[1].get<double>()});
        if (ring.size() >= 3)
            out.push_back(std::move(ring));
    }
    return out;
}

std::string regionMask(const std::string& name) { return "region_" + name; }

gn::NodeDecl regionsDecl()
{
    return {
        .type = kRegionsType,
        .label = "Terrain Regions",
        .category = "Terrain",
        .doc = "Named regions found by terrainQuery (valley floor, a pass, north slopes...), each written to a mask region_<name> "
               "(1 inside its outlines). Commands take 'region:<name>' wherever they take a point, path or polygon.",
        .props = {{.id = "regions", .defaultValue = "{}", .doc = "The regions, JSON: {name: {rings: [[[x, z], ...]], point: [x, z], query}} in the heightfield's own space.",
                   .editor = "code"}},
        .sockets = {.inputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}, .outputs = {{.id = "heightfield", .type = gn::SocketType::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE();

                gn::GeometryPtr g = c.geometry("heightfield");
                const Heightfield* in = heightfieldOf(*g);
                if (!in)
                    throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
                Heightfield h = *in;
                const json regions = parseRegions(c.prop("regions"));
                for (const auto& [name, region] : regions.items())
                {
                    std::vector<float>& m = h.mask(regionMask(name));
                    std::fill(m.begin(), m.end(), 0.0f);
                    for (const auto& ring : ringsOf(region))
                    {
                        const std::vector<uint8_t> cover = footprintMask(h, footprintFromPolygon(ring));
                        for (size_t i = 0; i < cover.size(); ++i)
                            if (cover[i])
                                m[i] = 1.0f;
                    }
                }
                gn::Geometry out = *g;
                out.heightfield = std::make_shared<const Heightfield>(std::move(h));
                c.out("heightfield", gn::makeGeometry(std::move(out)));
            },
    };
}

/** The terrain's group, read for editing. */
struct RegionsGroup
{
    Node* node = nullptr;
    gn::Group group;
};

RegionsGroup regionsGroup(const QTerrain& t, const std::string& cmd)
{
    Graph& g = Graph::current();
    Node* mod = t.ref.modifier.empty() ? nullptr : g.find(t.ref.modifier);
    Node* node = mod ? gn::groupOfModifier(g, *mod) : nullptr;
    if (!node)
        throw CommandError(cmd + ": the terrain has no geometry-nodes group to keep regions in");
    return {node, gn::readGroup(*node)};
}

gn::GNode* regionsNodeIn(gn::Group& grp)
{
    for (gn::GNode& n : grp.nodes)
        if (n.type == kRegionsType)
            return &n;
    return nullptr;
}

/** The Regions node, added just before the trailing volume, biomes and layers so material rules can read its masks. */
gn::GNode& ensureRegionsNode(gn::Group& grp, const std::string& cmd)
{
    if (gn::GNode* n = regionsNodeIn(grp))
        return *n;
    auto linkInto = [&](const std::string& node, const std::string& socket) -> gn::GLink* {
        for (gn::GLink& l : grp.links)
            if (l.toNode == node && l.toSocket == socket)
                return &l;
        return nullptr;
    };
    static const std::set<std::string> trailing = {"TerrainVolume", "TerrainBiomes", "TerrainLayers", "TerrainGroundCover"};
    std::string to = "GroupOutput", socket = "geometry";
    for (gn::GLink* l = linkInto(to, socket); l; l = linkInto(to, socket))
    {
        const gn::GNode* from = grp.node(l->fromNode);
        if (!from || !trailing.count(from->type))
            break;
        to = from->name;
        socket = "heightfield";
    }
    gn::GLink* l = linkInto(to, socket);
    if (!l)
        throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
    const std::string name = grp.uniqueNodeName(kRegionsNode);
    const gn::GNode* fromNode = grp.node(l->fromNode);
    const glm::dvec2 at = fromNode ? fromNode->position + glm::dvec2(0.0, 160.0) : glm::dvec2(0.0);
    const std::string fromName = l->fromNode, fromSocket = l->fromSocket;
    l->fromNode = name;
    l->fromSocket = "heightfield";
    grp.links.push_back({fromName, fromSocket, name, "heightfield"});
    gn::GNode n;
    n.name = name;
    n.type = kRegionsType;
    n.position = at;
    grp.nodes.push_back(std::move(n));
    return grp.nodes.back();
}

json readStored(const QTerrain& t)
{
    Graph& g = Graph::current();
    Node* mod = t.ref.modifier.empty() ? nullptr : g.find(t.ref.modifier);
    Node* node = mod ? gn::groupOfModifier(g, *mod) : nullptr;
    if (!node)
        return json::object();
    const gn::Group grp = gn::readGroup(*node);
    for (const gn::GNode& n : grp.nodes)
        if (n.type == kRegionsType)
        {
            auto it = n.values.find("regions");
            return it != n.values.end() && it->is_string() ? parseRegions(it->get<std::string>()) : json::object();
        }
    return json::object();
}

void writeStored(const QTerrain& t, const std::string& name, const json* region, const std::string& cmd)
{
    RegionsGroup rg = regionsGroup(t, cmd);
    gn::GNode& n = ensureRegionsNode(rg.group, cmd);
    auto it = n.values.find("regions");
    json stored = it != n.values.end() && it->is_string() ? parseRegions(it->get<std::string>()) : json::object();
    if (region)
        stored[name] = *region;
    else
        stored.erase(name);
    n.values["regions"] = stored.dump();
    try
    {
        gn::commitGroup(Graph::current(), *rg.node, std::move(rg.group));
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

// ---------------------------------------------------------------------------------------------------- fields

/** Chamfer distance (cm, local) from every sample to the nearest set one; infinity when none is set. */
std::vector<float> distanceTo(const Heightfield& h, const std::vector<uint8_t>& set)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> d(h.size(), inf);
    for (size_t i = 0; i < d.size(); ++i)
        if (set[i])
            d[i] = 0.0f;
    const float a = float(h.cell), b = float(h.cell * std::sqrt(2.0));
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            float& v = d[h.index(x, z)];
            if (x > 0)
                v = std::min(v, d[h.index(x - 1, z)] + a);
            if (z > 0)
            {
                v = std::min(v, d[h.index(x, z - 1)] + a);
                if (x > 0)
                    v = std::min(v, d[h.index(x - 1, z - 1)] + b);
                if (x + 1 < h.nx)
                    v = std::min(v, d[h.index(x + 1, z - 1)] + b);
            }
        }
    for (int z = h.nz - 1; z >= 0; --z)
        for (int x = h.nx - 1; x >= 0; --x)
        {
            float& v = d[h.index(x, z)];
            if (x + 1 < h.nx)
                v = std::min(v, d[h.index(x + 1, z)] + a);
            if (z + 1 < h.nz)
            {
                v = std::min(v, d[h.index(x, z + 1)] + a);
                if (x + 1 < h.nx)
                    v = std::min(v, d[h.index(x + 1, z + 1)] + b);
                if (x > 0)
                    v = std::min(v, d[h.index(x - 1, z + 1)] + b);
            }
        }
    return d;
}

/** Degrees of an aspect name (downhill direction, clockwise from north = -Z), or -1. */
double aspectDegrees(const std::string& s)
{
    static const std::map<std::string, double> names = {{"north", 0.0},     {"northeast", 45.0}, {"east", 90.0}, {"southeast", 135.0},
                                                        {"south", 180.0},   {"southwest", 225.0}, {"west", 270.0}, {"northwest", 315.0},
                                                        {"n", 0.0},         {"ne", 45.0},         {"e", 90.0},     {"se", 135.0},
                                                        {"s", 180.0},       {"sw", 225.0},        {"w", 270.0},    {"nw", 315.0}};
    auto it = names.find(s);
    return it == names.end() ? -1.0 : it->second;
}

/** One predicate: a value per sample and the range it must fall in (aspect wraps). */
struct Predicate
{
    std::string key;
    double lo = -std::numeric_limits<double>::infinity();
    double hi = std::numeric_limits<double>::infinity();
    bool wraps = false;
};

Predicate parsePredicate(const std::string& key, const json& v, const std::string& cmd)
{
    Predicate p;
    p.key = key;
    if (key == "aspect" && v.is_string())
    {
        const double c = aspectDegrees(v.get<std::string>());
        if (c < 0.0)
            throw CommandError(cmd + ": aspect is north, northeast, east, ... or [from, to] degrees, not " + v.get<std::string>());
        p.lo = std::fmod(c - 45.0 + 360.0, 360.0);
        p.hi = std::fmod(c + 45.0, 360.0);
        p.wraps = p.lo > p.hi;
        return p;
    }
    if (v.is_number())
    {
        p.lo = v.get<double>();
        return p;
    }
    json r = v;
    if (v.is_object())
    {
        if (v.contains("above"))
            p.lo = v["above"].get<double>();
        if (v.contains("below"))
            p.hi = v["below"].get<double>();
        if (v.contains("range"))
            r = v["range"];
        else
            return p;
    }
    if (!r.is_array() || r.size() != 2)
        throw CommandError(cmd + ": where." + key + " is [min, max] (None for open), a number (at least), or {above, below}");
    if (!r[0].is_null())
        p.lo = r[0].get<double>();
    if (!r[1].is_null())
        p.hi = r[1].get<double>();
    p.wraps = key == "aspect" && p.lo > p.hi;
    return p;
}

bool passes(const Predicate& p, double v)
{
    if (std::isnan(v))
        return false;
    if (p.wraps)
        return v >= p.lo || v <= p.hi;
    return v >= p.lo && v <= p.hi;
}

/** Per-sample values a query can test, worked out on first use. */
class Fields
{
public:
    Fields(const QTerrain& t, std::shared_ptr<const Analysis> an) : mT(t), mAn(std::move(an)) {}

    const std::vector<float>& get(const std::string& key, const std::string& cmd, std::vector<std::string>& warnings)
    {
        if (auto it = mCache.find(key); it != mCache.end())
            return it->second;
        const Heightfield& h = *mT.ref.local;
        std::vector<float> v(h.size(), 0.0f);
        if (key == "height" || key == "relativeHeight")
        {
            double lo = 1e300, hi = -1e300;
            for (int z = 0; z < h.nz; ++z)
                for (int x = 0; x < h.nx; ++x)
                {
                    const glm::dvec2 s = h.samplePos(x, z);
                    const double w = mT.worldHeight(s.x, h.at(x, z), s.y);
                    v[h.index(x, z)] = float(w);
                    lo = std::min(lo, w);
                    hi = std::max(hi, w);
                }
            if (key == "relativeHeight")
                for (float& f : v)
                    f = hi > lo ? float((f - lo) / (hi - lo)) : 0.0f;
        }
        else if (key == "distanceToRoads")
        {
            const std::vector<float>* road = h.findMask("roadSurface");
            std::vector<uint8_t> set(h.size(), 0);
            if (road)
                for (size_t i = 0; i < set.size(); ++i)
                    set[i] = (*road)[i] > 0.5f;
            else
                warnings.push_back("the terrain has no roads (mask roadSurface); distanceToRoads reads as infinitely far");
            v = distanceTo(h, set);
            for (float& f : v)
                f *= float(mT.across);
        }
        else if (key == "distanceToWater")
        {
            const std::vector<float>& d = mAn->field("distanceToWater");
            for (size_t i = 0; i < v.size(); ++i)
                v[i] = d[i] * float(mT.across);
        }
        else if (key == "water")
        {
            for (size_t i = 0; i < v.size(); ++i)
                v[i] = mAn->reachOf()[i] >= 0 || mAn->lakeOf()[i] >= 0 ? 1.0f : 0.0f;
            if (const std::vector<float>* m = h.findMask("water"))
                for (size_t i = 0; i < v.size(); ++i)
                    v[i] = std::max(v[i], (*m)[i]);
        }
        else if (const std::vector<float>* m = h.findMask(key); m && m->size() == h.size())
            v = *m;
        else if (std::find(Analysis::fieldNames().begin(), Analysis::fieldNames().end(), key) != Analysis::fieldNames().end())
            v = mAn->field(key);
        else
        {
            std::string known = "height, relativeHeight, distanceToRoads, water";
            for (const std::string& f : Analysis::fieldNames())
                known += ", " + f;
            for (const auto& [name, values] : h.masks)
                known += ", " + name;
            throw CommandError(cmd + ": nothing named " + key + " to test; use one of " + known);
        }
        return mCache.emplace(key, std::move(v)).first->second;
    }

private:
    const QTerrain& mT;
    std::shared_ptr<const Analysis> mAn;
    std::map<std::string, std::vector<float>> mCache;
};

// ---------------------------------------------------------------------------------------------------- shortcuts

const std::vector<std::string>& shortcutNames()
{
    static const std::vector<std::string> n = {"highestPeak", "lowestPoint", "passes",     "valleyFloor", "ridges",     "flatAreas",
                                               "steepSlopes", "cliffs",      "northSlopes", "southSlopes", "eastSlopes", "westSlopes",
                                               "highGround",  "lowGround",   "nearWater",  "rivers",      "lakes"};
    return n;
}

/** The predicates a region shortcut stands for. */
json shortcutWhere(const std::string& named, const Heightfield& h, Fields& f, std::vector<std::string>& warnings, const std::string& cmd)
{
    const auto range = [&] {
        const glm::dvec2 r = h.range();
        return r.y - r.x;
    }();
    if (named == "valleyFloor")
        return {{"hand", {nullptr, std::max(300.0, 0.04 * range)}}, {"slope", {nullptr, 10.0}}};
    if (named == "ridges")
    {
        std::vector<float> c = f.get("curvature", cmd, warnings);
        std::nth_element(c.begin(), c.begin() + ptrdiff_t(c.size() * 9 / 10), c.end());
        return {{"curvature", {std::max(0.0, double(c[c.size() * 9 / 10])), nullptr}}, {"relativeHeight", {0.35, nullptr}}};
    }
    if (named == "flatAreas")
        return {{"slope", {nullptr, 5.0}}};
    if (named == "steepSlopes")
        return {{"slope", {35.0, nullptr}}};
    if (named == "cliffs")
        return {{"slope", {50.0, nullptr}}};
    if (named == "northSlopes" || named == "southSlopes" || named == "eastSlopes" || named == "westSlopes")
        return {{"aspect", named.substr(0, named.size() - 6)}, {"slope", {10.0, nullptr}}};
    if (named == "highGround")
        return {{"relativeHeight", {0.75, nullptr}}};
    if (named == "lowGround")
        return {{"relativeHeight", {nullptr, 0.25}}};
    if (named == "nearWater")
        return {{"distanceToWater", {nullptr, 3000.0}}};
    if (named == "rivers" || named == "lakes")
        return json::object();
    std::string list;
    for (const std::string& n : shortcutNames())
        list += (list.empty() ? "" : ", ") + n;
    throw CommandError(cmd + ": named is one of " + list);
}

/** Mountain passes: saddles (curving up one way, down the other, nearly level) scored by how far the ground rises along the ridge and falls across it. */
std::vector<std::pair<size_t, double>> findPasses(const Heightfield& h)
{
    const double c = h.cell;
    const int reach = std::max(4, int(std::lround(0.04 * std::max(h.nx, h.nz))));
    std::vector<std::pair<size_t, double>> found;
    std::vector<uint8_t> cand(h.size(), 0);
    std::vector<double> score(h.size(), 0.0);
    for (int z = 1; z + 1 < h.nz; ++z)
        for (int x = 1; x + 1 < h.nx; ++x)
        {
            const double h0 = h.at(x, z);
            const double gx = (h.at(x + 1, z) - h.at(x - 1, z)) / (2.0 * c), gz = (h.at(x, z + 1) - h.at(x, z - 1)) / (2.0 * c);
            if (std::hypot(gx, gz) > 0.2)
                continue;
            const double hxx = (h.at(x + 1, z) - 2.0 * h0 + h.at(x - 1, z)) / (c * c);
            const double hzz = (h.at(x, z + 1) - 2.0 * h0 + h.at(x, z - 1)) / (c * c);
            const double hxz = (h.at(x + 1, z + 1) - h.at(x + 1, z - 1) - h.at(x - 1, z + 1) + h.at(x - 1, z - 1)) / (4.0 * c * c);
            if (hxx * hzz - hxz * hxz >= 0.0)
                continue;
            // Eigenvectors of the Hessian: e1 along which the ground curves up (the ridge), e2 across the pass.
            const double tr = hxx + hzz, disc = std::sqrt(std::max(0.0, 0.25 * (hxx - hzz) * (hxx - hzz) + hxz * hxz));
            const double l1 = 0.5 * tr + disc;
            glm::dvec2 e1 = std::abs(hxz) > 1e-12 ? glm::normalize(glm::dvec2(l1 - hzz, hxz)) : (hxx >= hzz ? glm::dvec2(1, 0) : glm::dvec2(0, 1));
            const glm::dvec2 e2(-e1.y, e1.x);
            const glm::dvec2 p = h.samplePos(x, z);
            const double d = reach * c;
            const double rise = std::min(h.heightAt(p.x + e1.x * d, p.y + e1.y * d), h.heightAt(p.x - e1.x * d, p.y - e1.y * d)) - h0;
            const double drop = h0 - std::max(h.heightAt(p.x + e2.x * d, p.y + e2.y * d), h.heightAt(p.x - e2.x * d, p.y - e2.y * d));
            const double s = std::min(rise, drop);
            if (s <= 0.0)
                continue;
            cand[h.index(x, z)] = 1;
            score[h.index(x, z)] = s;
        }
    std::vector<int32_t> label;
    const int n = components(cand, h.nx, h.nz, label);
    std::vector<std::pair<size_t, double>> best(static_cast<size_t>(n), {0, -1.0});
    for (size_t i = 0; i < label.size(); ++i)
        if (label[i] >= 0 && score[i] > best[size_t(label[i])].second)
            best[size_t(label[i])] = {i, score[i]};
    for (const auto& b : best)
        if (b.second > 0.0)
            found.push_back(b);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
    return found;
}

// ---------------------------------------------------------------------------------------------------- regions

/** Douglas-Peucker over a closed ring. */
std::vector<glm::dvec2> simplifyRing(const std::vector<glm::dvec2>& ring, double tol)
{
    if (ring.size() < 8)
        return ring;
    std::vector<uint8_t> keep(ring.size(), 0);
    const size_t far = [&] {
        size_t k = 0;
        double best = -1.0;
        for (size_t i = 0; i < ring.size(); ++i)
            if (const double d = glm::length(ring[i] - ring[0]); d > best)
            {
                best = d;
                k = i;
            }
        return k;
    }();
    keep[0] = keep[far] = 1;
    std::vector<std::pair<size_t, size_t>> stack = {{0, far}, {far, ring.size()}};
    while (!stack.empty())
    {
        auto [a, b] = stack.back();
        stack.pop_back();
        const glm::dvec2 p = ring[a], q = ring[b % ring.size()];
        double worst = -1.0;
        size_t at = a;
        for (size_t i = a + 1; i < b; ++i)
        {
            const glm::dvec2 r = ring[i];
            const glm::dvec2 e = q - p;
            const double l2 = glm::dot(e, e);
            const double t = l2 > 0.0 ? std::clamp(glm::dot(r - p, e) / l2, 0.0, 1.0) : 0.0;
            if (const double d = glm::length(r - (p + e * t)); d > worst)
            {
                worst = d;
                at = i;
            }
        }
        if (worst > tol)
        {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < ring.size(); ++i)
        if (keep[i])
            out.push_back(ring[i]);
    return out;
}

double ringArea(const std::vector<glm::dvec2>& r)
{
    double a = 0.0;
    for (size_t i = 0; i < r.size(); ++i)
    {
        const glm::dvec2 p = r[i], q = r[(i + 1) % r.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}

struct Region
{
    std::vector<size_t> cells;
    std::vector<std::vector<glm::dvec2>> rings; // local; outer first
    glm::dvec2 centroid{0.0};
    size_t anchor = 0;
    size_t lowest = 0, highest = 0;
};

std::vector<Region> regionsOf(const Heightfield& h, const std::vector<uint8_t>& set, size_t minCells, size_t maxRegions, size_t* dropped)
{
    std::vector<int32_t> label;
    const int n = components(set, h.nx, h.nz, label);
    std::vector<Region> comps(static_cast<size_t>(n));
    for (size_t i = 0; i < label.size(); ++i)
        if (label[i] >= 0)
            comps[size_t(label[i])].cells.push_back(i);
    std::stable_sort(comps.begin(), comps.end(), [](const Region& a, const Region& b) { return a.cells.size() > b.cells.size(); });
    *dropped = 0;
    std::vector<Region> out;
    for (Region& r : comps)
    {
        if (r.cells.size() < minCells || out.size() >= maxRegions)
        {
            ++*dropped;
            continue;
        }
        glm::dvec2 c(0.0);
        r.lowest = r.highest = r.cells.front();
        for (size_t i : r.cells)
        {
            c += h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx)));
            if (h.heights[i] < h.heights[r.lowest])
                r.lowest = i;
            if (h.heights[i] > h.heights[r.highest])
                r.highest = i;
        }
        r.centroid = c / double(r.cells.size());
        double best = 1e300;
        for (size_t i : r.cells)
            if (const double d = glm::length(h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx))) - r.centroid); d < best)
            {
                best = d;
                r.anchor = i;
            }
        std::vector<uint8_t> one(h.size(), 0);
        for (size_t i : r.cells)
            one[i] = 1;
        for (auto& ring : outlines(h, one))
        {
            ring = simplifyRing(ring, h.cell * 0.75);
            if (ring.size() >= 3)
                r.rings.push_back(std::move(ring));
        }
        std::stable_sort(r.rings.begin(), r.rings.end(), [](const auto& a, const auto& b) { return std::abs(ringArea(a)) > std::abs(ringArea(b)); });
        // A single sample has no outline: a square cell around it.
        if (r.rings.empty())
        {
            const glm::dvec2 p = h.samplePos(int(r.anchor % size_t(h.nx)), int(r.anchor / size_t(h.nx)));
            const double e = h.cell * 0.5;
            r.rings.push_back({p + glm::dvec2(-e, -e), p + glm::dvec2(e, -e), p + glm::dvec2(e, e), p + glm::dvec2(-e, e)});
        }
        out.push_back(std::move(r));
    }
    return out;
}

json statsOf(const std::vector<float>& v, const std::vector<size_t>& cells)
{
    double lo = 1e300, hi = -1e300, sum = 0.0;
    size_t count = 0;
    for (size_t i : cells)
    {
        const double x = v[i];
        if (!std::isfinite(x))
            continue;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        sum += x;
        ++count;
    }
    if (count == 0)
        return nullptr;
    return {{"min", lo}, {"max", hi}, {"mean", sum / double(count)}};
}

json cellPoint(const QTerrain& t, size_t i)
{
    const Heightfield& h = *t.ref.local;
    const glm::dvec2 p = h.samplePos(int(i % size_t(h.nx)), int(i / size_t(h.nx)));
    return t.worldJson(p.x, p.y);
}

json ringJson(const QTerrain& t, const std::vector<glm::dvec2>& ring)
{
    json out = json::array();
    for (const glm::dvec2& p : ring)
        out.push_back(t.groundJson(p.x, p.y));
    return out;
}

json localRingJson(const std::vector<glm::dvec2>& ring)
{
    json out = json::array();
    for (const glm::dvec2& p : ring)
        out.push_back(json::array({p.x, p.y}));
    return out;
}

// ---------------------------------------------------------------------------------------------------- the command

json runQuery(const Args& a)
{
    LILA_ZONE();

    const std::string cmd = "terrainQuery";
    const std::vector<std::string> objects = a.objects();
    if (objects.size() != 1)
        throw CommandError(cmd + ": give one terrain");
    const QTerrain t = findQ(objects[0], cmd);
    const Heightfield& h = *t.ref.local;

    if (a.has("remove"))
    {
        const std::string name = a.get<std::string>("remove", "");
        if (!readStored(t).contains(name))
            throw CommandError(cmd + ": no region named " + name);
        writeStored(t, name, nullptr, cmd);
        return {{"removed", name}};
    }
    if (a.get<bool>("list", false))
    {
        json out = json::object();
        const json stored = readStored(t);
        for (const auto& [name, r] : stored.items())
        {
            json rings = json::array();
            for (const auto& ring : ringsOf(r))
                rings.push_back(ringJson(t, ring));
            json one = {{"mask", regionMask(name)}, {"query", r.value("query", json())}, {"rings", rings}};
            if (r.contains("point"))
                one["point"] = t.worldJson(r["point"][0].get<double>(), r["point"][1].get<double>());
            out[name] = one;
        }
        return {{"regions", out}, {"revision", revision(h)}};
    }

    AnalysisParams ap;
    ap.riverArea = std::max(1.0, a.get<double>("riverArea", 250000.0));
    if (a.has("seaLevel"))
        ap.seaLevel = a.get<double>("seaLevel", 0.0);
    const auto an = analyse(t.ref.local, ap);
    Fields fields(t, an);
    std::vector<std::string> warnings;

    const std::string named = a.get<std::string>("named", "");
    json where = a.has("where") ? a.flags["where"] : json::object();
    if (!where.is_object())
        throw CommandError(cmd + ": where is a dict of {field or mask: [min, max]}");
    std::vector<size_t> points;
    std::vector<uint8_t> set(h.size(), 1);
    const size_t maxRegions = size_t(std::max<int64_t>(1, a.get<int64_t>("maxRegions", 8)));

    if (named == "highestPeak" || named == "lowestPoint")
    {
        const std::vector<float>& height = fields.get("height", cmd, warnings);
        const bool top = named == "highestPeak";
        size_t best = 0;
        for (size_t i = 1; i < height.size(); ++i)
            if (top ? height[i] > height[best] : height[i] < height[best])
                best = i;
        points.push_back(best);
        // The region around it: ground within 3 % of the range of it, joined to it.
        const glm::dvec2 r = h.range();
        const float band = float(0.03 * (r.y - r.x));
        for (size_t i = 0; i < set.size(); ++i)
            set[i] = top ? h.heights[i] >= h.heights[best] - band : h.heights[i] <= h.heights[best] + band;
        std::vector<int32_t> label;
        components(set, h.nx, h.nz, label);
        for (size_t i = 0; i < set.size(); ++i)
            set[i] = label[i] == label[best];
    }
    else if (named == "passes")
    {
        std::fill(set.begin(), set.end(), 0);
        for (const auto& [i, s] : findPasses(h))
        {
            if (points.size() >= maxRegions)
                break;
            points.push_back(i);
        }
        // A disc of three samples round each pass, so it has an outline to stamp and route to.
        for (size_t p : points)
        {
            const int px = int(p % size_t(h.nx)), pz = int(p / size_t(h.nx));
            for (int dz = -3; dz <= 3; ++dz)
                for (int dx = -3; dx <= 3; ++dx)
                    if (dx * dx + dz * dz <= 9 && px + dx >= 0 && pz + dz >= 0 && px + dx < h.nx && pz + dz < h.nz)
                        set[h.index(px + dx, pz + dz)] = 1;
        }
        if (points.empty())
            warnings.push_back("no saddle between higher ground was found; the terrain has no pass");
    }
    else if (named == "rivers" || named == "lakes")
    {
        for (size_t i = 0; i < set.size(); ++i)
            set[i] = named == "rivers" ? an->reachOf()[i] >= 0 : an->lakeOf()[i] >= 0;
    }
    else if (!named.empty())
    {
        const json implied = shortcutWhere(named, h, fields, warnings, cmd);
        for (const auto& [k, v] : implied.items())
            if (!where.contains(k))
                where[k] = v;
    }

    for (const auto& [key, value] : where.items())
    {
        if (key == "region")
        {
            const json stored = readStored(t);
            const std::string rn = value.get<std::string>();
            if (!stored.contains(rn))
                throw CommandError(cmd + ": where.region names no stored region " + rn);
            std::vector<uint8_t> inside(h.size(), 0);
            for (const auto& ring : ringsOf(stored[rn]))
            {
                const std::vector<uint8_t> cover = footprintMask(h, footprintFromPolygon(ring));
                for (size_t i = 0; i < cover.size(); ++i)
                    inside[i] |= cover[i];
            }
            for (size_t i = 0; i < set.size(); ++i)
                set[i] &= inside[i];
            continue;
        }
        const Predicate p = parsePredicate(key, value, cmd);
        const std::vector<float>& v = fields.get(key, cmd, warnings);
        for (size_t i = 0; i < set.size(); ++i)
            if (set[i] && !passes(p, v[i]))
                set[i] = 0;
    }

    const double cellM2 = t.cellAreaM2();
    const size_t minCells = size_t(std::max(1.0, std::ceil(a.get<double>("minArea", 0.0) / cellM2)));
    size_t dropped = 0;
    std::vector<Region> regions = regionsOf(h, set, minCells, maxRegions, &dropped);
    // A point query keeps one region per point, in the points' order.
    if (!points.empty())
    {
        std::vector<Region> ordered;
        for (size_t p : points)
            for (Region& r : regions)
                if (std::find(r.cells.begin(), r.cells.end(), p) != r.cells.end())
                {
                    Region one = r;
                    one.anchor = p;
                    ordered.push_back(std::move(one));
                    break;
                }
        regions = std::move(ordered);
    }

    const std::vector<float>& height = fields.get("height", cmd, warnings);
    const std::vector<float>& slope = fields.get("slope", cmd, warnings);
    size_t total = 0;
    for (uint8_t s : set)
        total += s;
    json out = {{"named", named.empty() ? json() : json(named)}, {"where", where}, {"cells", int64_t(total)}, {"areaM2", double(total) * cellM2},
                {"coverage", double(total) / double(h.size())}};
    json regionsJson = json::array(), pointsJson = json::array();
    for (size_t k = 0; k < regions.size(); ++k)
    {
        const Region& r = regions[k];
        json holes = json::array();
        for (size_t i = 1; i < r.rings.size(); ++i)
            holes.push_back(ringJson(t, r.rings[i]));
        regionsJson.push_back({{"index", int64_t(k)},
                               {"areaM2", double(r.cells.size()) * cellM2},
                               {"cells", int64_t(r.cells.size())},
                               {"centroid", t.groundJson(r.centroid.x, r.centroid.y)},
                               {"point", cellPoint(t, r.anchor)},
                               {"lowest", cellPoint(t, r.lowest)},
                               {"highest", cellPoint(t, r.highest)},
                               {"polygon", ringJson(t, r.rings.front())},
                               {"otherRings", holes},
                               {"height", statsOf(height, r.cells)},
                               {"slope", statsOf(slope, r.cells)}});
        pointsJson.push_back(cellPoint(t, r.anchor));
    }
    out["regions"] = regionsJson;
    out["points"] = pointsJson;
    out["dropped"] = int64_t(dropped);
    if (regions.empty())
        warnings.push_back("nothing matched; loosen the ranges");

    if (a.has("store"))
    {
        const std::string name = a.get<std::string>("store", "");
        if (name.empty() || !Graph::validName(name))
            throw CommandError(cmd + ": store is a plain name (letters, digits, underscores)");
        if (regions.empty())
            throw CommandError(cmd + ": nothing matched, so there is no region to store as " + name);
        const int64_t pick = a.get<int64_t>("pick", -1);
        if (pick >= int64_t(regions.size()))
            throw CommandError(cmd + ": pick is below the " + std::to_string(regions.size()) + " regions found");
        json rings = json::array(), parts = json::array();
        for (size_t k = 0; k < regions.size(); ++k)
        {
            if (pick >= 0 && int64_t(k) != pick)
                continue;
            json partRings = json::array();
            for (const auto& ring : regions[k].rings)
                partRings.push_back(localRingJson(ring));
            // Outer rings mark the region; inner ones are holes and stay out of the mask.
            rings.push_back(partRings.front());
            const Heightfield& hh = h;
            const glm::dvec2 a0 = hh.samplePos(int(regions[k].anchor % size_t(hh.nx)), int(regions[k].anchor / size_t(hh.nx)));
            parts.push_back({{"point", {a0.x, a0.y}}, {"ring", partRings.front()}});
        }
        json stored = {{"rings", rings}, {"parts", parts}, {"point", parts[0]["point"]}, {"query", {{"named", named}, {"where", where}}}};
        writeStored(t, name, &stored, cmd);
        out["stored"] = name;
        out["mask"] = regionMask(name);
        out["reference"] = std::string(kRefPrefix) + name;
    }
    for (const std::string& w : warnings)
        log::warn("{}: {}", cmd, w);
    out["warnings"] = warnings;
    out["revision"] = revision(*findTerrainRef(objects[0], cmd).local);
    return out;
}

// ---------------------------------------------------------------------------------------------------- references

enum class RefKind : uint8_t
{
    Point,
    Points,
    Polygon,
};

struct RefFlag
{
    const char* command;
    const char* flag;
    RefKind kind;
};

constexpr RefFlag kRefFlags[] = {
    {"terrainAddLake", "at", RefKind::Point},         {"terrainAddLake", "polygon", RefKind::Polygon},
    {"terrainAddRiver", "path", RefKind::Points},     {"terrainAddRoad", "path", RefKind::Points},
    {"terrainRoute", "start", RefKind::Point},        {"terrainRoute", "end", RefKind::Point},
    {"terrainStampRegion", "footprint", RefKind::Polygon}, {"terrainStampPad", "footprint", RefKind::Polygon},
    {"terrainCave", "path", RefKind::Points},         {"terrainTunnel", "path", RefKind::Points},
    {"terrainOverhang", "path", RefKind::Points},     {"terrainArch", "from", RefKind::Point},
    {"terrainArch", "to", RefKind::Point},            {"terrainVolumeStamp", "center", RefKind::Point},
    {"terrainSample", "points", RefKind::Points},     {"terrainStroke", "points", RefKind::Points},
    {"terrainStroke", "path", RefKind::Points},       {"terrainConstrain", "path", RefKind::Points},
    {"terrainProfile", "path", RefKind::Points},      {"terrainRegionStats", "polygon", RefKind::Polygon},
};

bool isRef(const json& v) { return v.is_string() && v.get<std::string>().starts_with(kRefPrefix); }

json resolveFlag(const std::string& terrain, const json& v, RefKind kind, const std::string& cmd)
{
    if (kind == RefKind::Point || kind == RefKind::Polygon)
        return isRef(v) ? resolveRegionRef(terrain, v.get<std::string>(), kind == RefKind::Polygon, cmd) : v;
    if (isRef(v))
        throw CommandError(cmd + ": a path is a list of points; put " + v.get<std::string>() + " in it as one point");
    if (!v.is_array())
        return v;
    json out = json::array();
    for (const json& p : v)
        out.push_back(isRef(p) ? resolveRegionRef(terrain, p.get<std::string>(), false, cmd) : p);
    return out;
}
} // namespace query_

using namespace query_;

json storedRegions(const std::string& terrain) { return readStored(findQ(terrain, "terrainQuery")); }

json resolveRegionRef(const std::string& terrain, const std::string& ref, bool polygon, const std::string& cmd)
{
    std::string name = ref.substr(std::string(kRefPrefix).size());
    int64_t part = -1;
    if (const size_t slash = name.find('/'); slash != std::string::npos)
    {
        try
        {
            part = std::stoll(name.substr(slash + 1));
        }
        catch (const std::exception&)
        {
            throw CommandError(cmd + ": " + ref + " is region:<name> or region:<name>/<index>");
        }
        name = name.substr(0, slash);
    }
    const QTerrain t = findQ(terrain, cmd);
    const json stored = readStored(t);
    if (!stored.contains(name))
    {
        std::string have;
        for (const auto& [n, r] : stored.items())
            have += (have.empty() ? "" : ", ") + n;
        throw CommandError(cmd + ": no region named " + name + " on the terrain (stored: " + (have.empty() ? "none; terrainQuery(store=) makes one" : have) + ")");
    }
    const json& r = stored[name];
    const json parts = r.value("parts", json::array());
    if (part >= int64_t(parts.size()))
        throw CommandError(cmd + ": region " + name + " has " + std::to_string(parts.size()) + " parts");
    const json& one = part >= 0 ? parts[size_t(part)] : parts.empty() ? r : parts[0];
    if (polygon)
    {
        std::vector<glm::dvec2> ring;
        for (const json& p : one.contains("ring") ? one["ring"] : r["rings"][0])
            ring.push_back({p[0].get<double>(), p[1].get<double>()});
        return ringJson(t, ring);
    }
    const json p = one.value("point", r.value("point", json()));
    if (!p.is_array())
        throw CommandError(cmd + ": region " + name + " has no point");
    return t.groundJson(p[0].get<double>(), p[1].get<double>());
}

bool registerQuery(ext::Context& ctx)
{
    if (!gn::registerDecl(ctx, regionsDecl()))
        return false;
    std::string shortcuts;
    for (const std::string& n : shortcutNames())
        shortcuts += (shortcuts.empty() ? "" : ", ") + n;
    ctx.registerCommand({
        .name = "terrainQuery",
        .doc = "Find places on a terrain in words: regions where predicates hold over its fields and masks, or a named shortcut, "
               "returned as world-space polygons with area, centroid, a point inside, lowest and highest points and height and "
               "slope stats, largest first. store= keeps the result as a named region (mask region_<name>, kept in a Terrain "
               "Regions node) that other terrain commands take as 'region:<name>' (or 'region:<name>/<k>' for its k-th part) "
               "wherever they take a point, path point or polygon.\n\n"
               "where keys: height (cm along the up axis), relativeHeight (0 lowest to 1 highest), slope and aspect (degrees; "
               "aspect also 'north', 'southwest', ... as a 90 degree sector of the downhill direction), curvature (per m, "
               "positive on ridges), flow (0 to 1), distanceToWater and distanceToRoads (cm), water, region (inside a stored "
               "region), every terrainField field (hand, prominence, buildability, ...) and every mask. A value is [min, max] "
               "(None for open), a number (at least), or {above, below}.\n\n"
               "named: " + shortcuts + ". highestPeak, lowestPoint and passes are points (passes: saddles ranked by how high "
               "the ridge rises either side). list=True lists stored regions; remove=<name> drops one.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"where", "w", T::Any, nullptr, "{field or mask: [min, max]}, all must hold."},
                  {"named", "nm", T::String, "", "A shortcut: " + shortcuts + "."},
                  {"minArea", "ma", T::Float, 0.0, "Leave out regions smaller than this, m2.", {}, {}, 0.0},
                  {"maxRegions", "mr", T::Int, int64_t(8), "Most regions (or points) returned, largest first.", {}, {}, 1.0, 256.0},
                  {"store", "st", T::String, nullptr, "Keep the result as this named region (mask region_<name>)."},
                  {"pick", "pk", T::Int, int64_t(-1), "store: keep only the region with this index; -1 keeps all."},
                  {"list", "ls", T::Bool, false, "List the stored regions."},
                  {"remove", "rm", T::String, nullptr, "Drop the stored region of this name."},
                  {"riverArea", "ra", T::Float, 250000.0, "Drainage area, m2, past which water is a river.", {}, {}, 1.0},
                  {"seaLevel", "sea", T::Float, nullptr, "Sea level along the up axis, cm, for water and distanceToWater."}},
        .run = [](const Args& a) { return runQuery(a); },
        .examples = {"terrainQuery('Terrain', named='valleyFloor', minArea=20000, store='valley')['regions'][0]['polygon']",
                     "terrainQuery('Terrain', where={'aspect': 'north', 'height': [150000, None]})",
                     "terrainQuery('Terrain', named='passes', maxRegions=3, store='pass', pick=0)\n"
                     "terrainAddRoad('Terrain', id='passRoad', path=['region:valley', 'region:pass', [40000, 0]])"},
    });
    return true;
}

void registerRegionRefs(ext::Context& ctx)
{
    std::set<std::string> done;
    for (const RefFlag& rf : kRefFlags)
    {
        if (done.count(rf.command))
            continue;
        done.insert(rf.command);
        const CommandDef* d = ops::find(rf.command);
        if (!d)
            continue;
        CommandDef wrapped = *d;
        std::map<std::string, RefKind> flags;
        for (const RefFlag& f : kRefFlags)
            if (std::string(f.command) == rf.command)
                flags[f.flag] = f.kind;
        for (FlagDef& f : wrapped.flags)
            if (auto it = flags.find(f.name); it != flags.end())
            {
                f.doc += it->second == RefKind::Points ? " Points may be 'region:<name>'." : " Or 'region:<name>'.";
                // The command checks the shape itself once references are resolved; a reference is a string.
                if (it->second != RefKind::Points)
                    f.type = T::Any;
            }
        auto inner = wrapped.run;
        const std::string name = rf.command;
        wrapped.run = [inner, flags, name](const Args& a) -> json {
            bool any = false;
            for (const auto& [flag, kind] : flags)
                if (a.has(flag))
                {
                    const json& v = a.flags[flag];
                    any = any || isRef(v) || (v.is_array() && std::any_of(v.begin(), v.end(), isRef));
                }
            if (!any)
                return inner(a);
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError(name + ": name the terrain to resolve region references on");
            Args resolved = a;
            for (const auto& [flag, kind] : flags)
                if (a.has(flag))
                    resolved.flags[flag] = resolveFlag(objects.front(), a.flags[flag], kind, name);
            return inner(resolved);
        };
        ctx.registerCommand(std::move(wrapped));
    }
}
} // namespace terrain
