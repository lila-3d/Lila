#include "TerrainCore.h"

#include <lila/DeformMath.h>

#include <glm/gtc/matrix_transform.hpp>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace terrain
{
namespace deform = lila::deform;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kPoissonAttempts = 30;
constexpr double kBaseLowBound = 0.0;
constexpr double kBaseHighBound = 2.0;

struct Tables
{
    std::vector<std::pair<std::string, std::string>> sections;
    std::vector<Param> params;
    std::vector<std::string> presetNames;
    json presets = json::object();
};

const Tables& tables()
{
    static const Tables t = [] {
        Tables out;
        const json doc = json::parse(builtInTablesJson());
        for (const json& s : doc.at("sections"))
            out.sections.emplace_back(s.at(0).get<std::string>(), s.at(1).get<std::string>());
        for (const json& p : doc.at("parameters"))
        {
            Param q;
            q.name = p.at("name").get<std::string>();
            q.label = p.at("label").get<std::string>();
            q.type = p.at("type").get<std::string>();
            q.section = p.at("section").get<std::string>();
            q.help = p.value("help", "");
            q.def = p.at("default");
            q.min = p.value("min", 0.0);
            q.max = p.value("max", 0.0);
            if (p.contains("options"))
                q.options = p.at("options").get<std::vector<std::string>>();
            if (p.contains("showIf"))
            {
                q.showIf = p.at("showIf").at(0).get<std::string>();
                q.showValues = p.at("showIf").at(1);
            }
            out.params.push_back(std::move(q));
        }
        for (const json& entry : doc.at("presets"))
        {
            out.presetNames.push_back(entry.at(0).get<std::string>());
            out.presets[out.presetNames.back()] = entry.at(1);
        }
        return out;
    }();
    return t;
}

/** Python's round: halves go to the even neighbour. */
int64_t roundEven(double v) { return int64_t(std::nearbyint(v)); }

double pyMod(double a, double b)
{
    double m = std::fmod(a, b);
    if (m < 0.0)
        m += b;
    return m >= b ? 0.0 : m;
}

double wrap(double value, double half) { return pyMod(value + half, half * 2.0) - half; }

std::vector<glm::dvec2> uniformPoints(Random& rng, int64_t count, double half)
{
    std::vector<glm::dvec2> out;
    out.reserve(size_t(count));
    for (int64_t i = 0; i < count; ++i)
    {
        const double x = rng.uniform(-half, half);
        out.emplace_back(x, rng.uniform(-half, half));
    }
    return out;
}

std::vector<glm::dvec2> clusteredPoints(Random& rng, int64_t count, double half, int64_t clusterCount, double radius, double strength)
{
    const std::vector<glm::dvec2> centres = uniformPoints(rng, std::max<int64_t>(1, clusterCount), half);
    std::vector<glm::dvec2> out;
    out.reserve(size_t(count));
    for (int64_t i = 0; i < count; ++i)
    {
        if (rng.random() < strength)
        {
            const glm::dvec2 c = centres[size_t(rng.randrange(int64_t(centres.size())))];
            const double angle = rng.uniform(0.0, kPi * 2.0);
            const double distance = radius * std::sqrt(rng.random());
            out.emplace_back(wrap(c.x + std::cos(angle) * distance, half), wrap(c.y + std::sin(angle) * distance, half));
        }
        else
        {
            const double x = rng.uniform(-half, half);
            out.emplace_back(x, rng.uniform(-half, half));
        }
    }
    return out;
}

std::vector<glm::dvec2> stratifiedPoints(Random& rng, int64_t count, double half, double jitter)
{
    const double size = half * 2.0;
    const int64_t divisions = std::max<int64_t>(1, int64_t(std::ceil(std::sqrt(double(count)))));
    const double step = size / double(divisions);
    std::vector<glm::dvec2> out;
    out.reserve(size_t(count));
    for (int64_t i = 0; i < count; ++i)
    {
        const int64_t cell = i % (divisions * divisions);
        const int64_t column = cell % divisions;
        const int64_t row = cell / divisions;
        const double ox = 0.5 + rng.uniform(-0.5, 0.5) * jitter;
        const double oy = 0.5 + rng.uniform(-0.5, 0.5) * jitter;
        out.emplace_back(wrap(-half + (double(column) + ox) * step, half), wrap(-half + (double(row) + oy) * step, half));
    }
    return out;
}

std::vector<glm::dvec2> poissonPoints(Random& rng, int64_t count, double half, double minSpacing)
{
    const double size = half * 2.0;
    if (minSpacing <= 0.0)
        return uniformPoints(rng, count, half);
    double cell = minSpacing / std::sqrt(2.0);
    const int64_t divisions = std::max<int64_t>(1, int64_t(std::ceil(size / cell)));
    cell = size / double(divisions);
    std::unordered_map<int64_t, std::vector<glm::dvec2>> grid;
    std::vector<glm::dvec2> out;
    const double squared = minSpacing * minSpacing;
    int64_t attempts = count * kPoissonAttempts;
    auto key = [divisions](int64_t c, int64_t r) { return ((c % divisions + divisions) % divisions) * divisions + ((r % divisions + divisions) % divisions); };

    while (int64_t(out.size()) < count && attempts > 0)
    {
        --attempts;
        const double x = rng.uniform(-half, half);
        const double y = rng.uniform(-half, half);
        const int64_t column = int64_t((x + half) / cell) % divisions;
        const int64_t row = int64_t((y + half) / cell) % divisions;
        bool blocked = false;
        for (int64_t oc = -2; oc <= 2 && !blocked; ++oc)
            for (int64_t orr = -2; orr <= 2 && !blocked; ++orr)
            {
                auto it = grid.find(key(column + oc, row + orr));
                if (it == grid.end())
                    continue;
                for (const glm::dvec2& o : it->second)
                {
                    double dx = std::abs(x - o.x), dy = std::abs(y - o.y);
                    dx = std::min(dx, size - dx);
                    dy = std::min(dy, size - dy);
                    if (dx * dx + dy * dy < squared)
                    {
                        blocked = true;
                        break;
                    }
                }
            }
        if (!blocked)
        {
            grid[key(column, row)].emplace_back(x, y);
            out.emplace_back(x, y);
        }
    }
    return out;
}

std::vector<glm::dvec2> rowPoints(Random& rng, int64_t count, double half, double spacing, double jitter, int64_t cycles, double amplitude)
{
    const double size = half * 2.0;
    const int64_t rows = std::max<int64_t>(1, roundEven(size / std::max(spacing, 0.001)));
    const double rowStep = size / double(rows);
    const int64_t perRow = std::max<int64_t>(1, roundEven(double(count) / double(rows)));
    const double columnStep = size / double(perRow);
    std::vector<glm::dvec2> out;
    out.reserve(size_t(rows * perRow));
    for (int64_t row = 0; row < rows; ++row)
    {
        const double line = -half + (double(row) + 0.5) * rowStep;
        for (int64_t column = 0; column < perRow; ++column)
        {
            const double x = -half + (double(column) + 0.5 + rng.uniform(-0.5, 0.5) * jitter) * columnStep;
            double wave = 0.0;
            if (cycles > 0 && amplitude > 0.0)
                wave = amplitude * std::sin(kPi * 2.0 * double(cycles) * (x + half) / size);
            const double y = line + wave + rng.uniform(-0.5, 0.5) * jitter * rowStep;
            out.emplace_back(wrap(x, half), wrap(y, half));
        }
    }
    return out;
}

template <class T> size_t weightedPick(Random& rng, const std::vector<T>& entries, const std::vector<double>& weights)
{
    double total = 0.0;
    for (double w : weights)
        total += w;
    if (total <= 0.0)
        return size_t(rng.randrange(int64_t(entries.size())));
    const double threshold = rng.random() * total;
    double running = 0.0;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        running += weights[i];
        if (threshold <= running)
            return i;
    }
    return entries.size() - 1;
}

struct Layer
{
    double height, scale, band;
};

Layer pickLayer(Random& rng, const json& v)
{
    if (!flag(v, "layersEnabled"))
        return {num(v, "baseHeight"), 1.0, 0.0};
    const std::vector<Layer> layers{{num(v, "layer1Height"), num(v, "layer1Scale"), 0.0},
                                    {num(v, "layer2Height"), num(v, "layer2Scale"), 0.5},
                                    {num(v, "layer3Height"), num(v, "layer3Scale"), 1.0}};
    return layers[weightedPick(rng, layers, {num(v, "layer1Weight"), num(v, "layer2Weight"), num(v, "layer3Weight")})];
}

double pickYaw(Random& rng, const json& v)
{
    const std::string mode = text(v, "rotationMode");
    if (mode == "Range")
        return rng.uniform(num(v, "yawMin"), num(v, "yawMax"));
    if (mode == "Snapped")
    {
        const double snap = std::max(1.0, num(v, "yawSnap"));
        const int64_t steps = std::max<int64_t>(1, roundEven(360.0 / snap));
        const double base = double(rng.randrange(steps)) * snap;
        return base + rng.uniform(-num(v, "yawJitter"), num(v, "yawJitter"));
    }
    if (mode == "Aligned")
        return num(v, "alignAngle") + rng.uniform(-num(v, "yawJitter"), num(v, "yawJitter"));
    return rng.uniform(0.0, 360.0);
}

double hash01(std::initializer_list<int64_t> values)
{
    uint32_t r = 2166136261u;
    for (int64_t v : values)
        r = (r ^ uint32_t(uint64_t(v) & 0xFFFFFFFFu)) * 16777619u;
    return double(r) / 4294967295.0;
}

glm::dvec3 safeNormalize(const glm::dvec3& v)
{
    const double l = glm::length(v);
    return l < 1e-9 ? glm::dvec3(0.0, 1.0, 0.0) : v / l;
}

glm::dvec3 orthogonalize(const glm::dvec3& v, const glm::dvec3& against)
{
    glm::dvec3 r = v - against * glm::dot(v, against);
    if (glm::length(r) < 1e-6)
    {
        const glm::dvec3 fallback = std::abs(against.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
        r = fallback - against * glm::dot(fallback, against);
    }
    return safeNormalize(r);
}

/** The handle placement matched to the mesh, world space: Y along the length from the base, X through the thickness. */
glm::dmat4 derivedHandle(const Source& s, bool atBase)
{
    const Axes& a = s.axes;
    glm::dvec3 point = (a.min + a.max) * 0.5;
    if (atBase)
        point[a.length] = a.baseSign > 0.0 ? a.min[a.length] : a.max[a.length];
    const glm::dvec3 position(s.world * glm::dvec4(point, 1.0));
    const glm::dvec3 lengthRow(s.world[a.length]);
    const glm::dvec3 by = safeNormalize(lengthRow * a.baseSign);
    const glm::dvec3 bx = orthogonalize(safeNormalize(glm::dvec3(s.world[a.thickness])), by);
    const glm::dvec3 bz = glm::cross(bx, by);
    const double half = std::max(a.extents[a.length] * 0.5 * glm::length(lengthRow), 1e-4);
    glm::dmat4 h(1.0);
    h[0] = glm::dvec4(bx * half, 0.0);
    h[1] = glm::dvec4(by * half, 0.0);
    h[2] = glm::dvec4(bz * half, 0.0);
    h[3] = glm::dvec4(position, 1.0);
    return h;
}

glm::dmat4 eulerXYZ(const glm::dvec3& degrees)
{
    glm::dmat4 r(1.0);
    r = glm::rotate(r, glm::radians(degrees.z), glm::dvec3(0, 0, 1));
    r = glm::rotate(r, glm::radians(degrees.y), glm::dvec3(0, 1, 0));
    r = glm::rotate(r, glm::radians(degrees.x), glm::dvec3(1, 0, 0));
    return r;
}

int upIndex(const json& v) { return text(v, "upAxis") == "Z" ? 2 : 1; }
} // namespace

const std::vector<Param>& parameters() { return tables().params; }

const Param* findParam(std::string_view name)
{
    for (const Param& p : tables().params)
        if (p.name == name)
            return &p;
    return nullptr;
}

const std::vector<std::pair<std::string, std::string>>& sections() { return tables().sections; }
const std::vector<std::string>& builtInPresets() { return tables().presetNames; }
const std::string& defaultPreset() { return tables().presetNames.front(); }

const json* builtInOverrides(const std::string& name)
{
    const json& p = tables().presets;
    auto it = p.find(name);
    return it == p.end() ? nullptr : &*it;
}

json defaults()
{
    json out = json::object();
    for (const Param& p : tables().params)
        out[p.name] = p.def;
    return out;
}

json coerce(const Param& p, const json& value)
{
    try
    {
        if (p.type == "float")
            return value.is_number() ? value.get<double>() : value.is_boolean() ? double(value.get<bool>()) : std::stod(value.get<std::string>());
        if (p.type == "int")
        {
            const double d = value.is_number() ? value.get<double>() : value.is_boolean() ? double(value.get<bool>()) : std::stod(value.get<std::string>());
            return roundEven(d);
        }
        if (p.type == "bool")
        {
            if (value.is_boolean())
                return value.get<bool>();
            if (value.is_number())
                return value.get<double>() != 0.0;
            if (value.is_string())
                return !value.get<std::string>().empty();
            return !value.is_null();
        }
        if (p.type == "enum")
        {
            const std::string t = value.is_string() ? value.get<std::string>() : value.dump();
            return std::find(p.options.begin(), p.options.end(), t) != p.options.end() ? json(t) : p.def;
        }
    }
    catch (const std::exception&)
    {
        return p.def;
    }
    return value;
}

json sanitize(const json& values)
{
    json out = defaults();
    if (!values.is_object())
        return out;
    for (auto it = values.begin(); it != values.end(); ++it)
        if (const Param* p = findParam(it.key()))
            out[p->name] = coerce(*p, it.value());
    return out;
}

json presetSettings(const std::string& name, const json& userPresets)
{
    if (userPresets.is_object() && userPresets.contains(name))
        return sanitize(userPresets.at(name));
    const json* o = builtInOverrides(name);
    return sanitize(o ? *o : json::object());
}

bool isVisible(const Param& p, const json& values)
{
    if (p.showIf.empty())
        return true;
    auto it = values.find(p.showIf);
    if (it == values.end())
        return false;
    for (const json& allowed : p.showValues)
        if (allowed == *it)
            return true;
    return false;
}

double num(const json& v, const char* name)
{
    auto it = v.find(name);
    if (it != v.end() && it->is_number())
        return it->get<double>();
    const Param* p = findParam(name);
    return p && p->def.is_number() ? p->def.get<double>() : 0.0;
}

int64_t whole(const json& v, const char* name) { return roundEven(num(v, name)); }

bool flag(const json& v, const char* name)
{
    auto it = v.find(name);
    if (it != v.end() && it->is_boolean())
        return it->get<bool>();
    const Param* p = findParam(name);
    return p && p->def.is_boolean() && p->def.get<bool>();
}

std::string text(const json& v, const char* name)
{
    auto it = v.find(name);
    if (it != v.end() && it->is_string())
        return it->get<std::string>();
    const Param* p = findParam(name);
    return p && p->def.is_string() ? p->def.get<std::string>() : std::string();
}

std::vector<glm::dvec2> scatterPoints(const json& v, Random& rng)
{
    const double half = num(v, "tileSize") * 0.5;
    const int64_t count = std::max<int64_t>(1, whole(v, "count"));
    const std::string mode = text(v, "distribution");
    if (mode == "Clustered")
        return clusteredPoints(rng, count, half, whole(v, "clusterCount"), num(v, "clusterRadius"), num(v, "clusterStrength"));
    if (mode == "Stratified")
        return stratifiedPoints(rng, count, half, num(v, "stratifiedJitter"));
    if (mode == "Poisson")
        return poissonPoints(rng, count, half, num(v, "minSpacing"));
    if (mode == "Rows")
        return rowPoints(rng, count, half, num(v, "rowSpacing"), num(v, "rowJitter"), whole(v, "rowWaveCycles"), num(v, "rowWaveAmplitude"));
    return uniformPoints(rng, count, half);
}

std::vector<glm::dvec2> wrappedPositions(double x, double y, double half, double margin)
{
    std::vector<double> xs{x}, ys{y};
    if (x < -half + margin)
        xs.push_back(x + half * 2.0);
    else if (x > half - margin)
        xs.push_back(x - half * 2.0);
    if (y < -half + margin)
        ys.push_back(y + half * 2.0);
    else if (y > half - margin)
        ys.push_back(y - half * 2.0);
    std::vector<glm::dvec2> out;
    for (double ex : xs)
        for (double ey : ys)
            out.emplace_back(ex, ey);
    return out;
}

double clusterCoverage(const json& v)
{
    const double area = num(v, "tileSize") * num(v, "tileSize");
    if (area <= 0.0)
        return 0.0;
    const double r = num(v, "clusterRadius");
    return double(whole(v, "clusterCount")) * kPi * r * r / area;
}

std::string describeClustering(const json& v)
{
    const double coverage = clusterCoverage(v);
    std::string verdict = coverage >= 1.0    ? "no clumping, the discs blanket the tile"
                          : coverage >= 0.6  ? "very faint clumping"
                          : coverage >= 0.35 ? "soft clumping"
                          : coverage >= 0.08 ? "clear clumping"
                                             : "tight, isolated clumps";
    if (num(v, "clusterStrength") < 0.5 && coverage < 1.0)
        verdict += ", diluted by loose scatter";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", coverage);
    return "Clusters cover " + std::string(buf) + "x the tile: " + verdict + ".";
}

double lowestHeight(const json& v)
{
    if (!flag(v, "layersEnabled"))
        return num(v, "baseHeight");
    const std::array<std::pair<double, double>, 3> layers{{{num(v, "layer1Weight"), num(v, "layer1Height")},
                                                           {num(v, "layer2Weight"), num(v, "layer2Height")},
                                                           {num(v, "layer3Weight"), num(v, "layer3Height")}}};
    double used = std::numeric_limits<double>::infinity(), all = std::numeric_limits<double>::infinity();
    for (const auto& [weight, height] : layers)
    {
        all = std::min(all, height);
        if (weight > 0.0)
            used = std::min(used, height);
    }
    return std::isfinite(used) ? used : all;
}

bool modifiesGeometry(const json& v)
{
    for (const char* f : {"bendEnabled", "flareEnabled", "twistEnabled", "sineEnabled", "latticeEnabled", "jitterEnabled", "flattenEnabled", "vertexColours"})
        if (flag(v, f))
            return true;
    return false;
}

std::string presetStem(const std::string& preset)
{
    std::string out;
    for (char c : preset)
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128)
            out += c;
    return out.empty() ? "Terrain" : out;
}

std::string nodeStem(const std::string& preset)
{
    std::string out;
    bool gap = false;
    for (char c : preset.empty() ? std::string("Terrain") : preset)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 128)
        {
            if (gap && !out.empty())
                out += '_';
            out += c;
            gap = false;
        }
        else
            gap = true;
    }
    return out.empty() ? "Terrain" : out;
}

glm::dvec3 defaultIdColour(int64_t id)
{
    static const glm::dvec3 palette[] = {{1.00, 0.00, 0.00}, {0.00, 1.00, 0.00}, {0.00, 0.30, 1.00}, {1.00, 1.00, 0.00},
                                         {1.00, 0.00, 1.00}, {0.00, 1.00, 1.00}, {1.00, 0.50, 0.00}, {0.50, 0.00, 1.00},
                                         {0.00, 1.00, 0.50}, {1.00, 0.00, 0.50}, {0.50, 1.00, 0.00}, {0.00, 0.50, 0.50},
                                         {1.00, 1.00, 1.00}, {0.50, 0.50, 0.50}, {0.60, 0.30, 0.10}, {0.20, 0.40, 0.20}};
    if (id < 0)
        return glm::dvec3(0.0);
    return palette[size_t(id % 16)];
}

glm::dvec3 idColour(int64_t id, const json& palette)
{
    if (palette.is_object())
    {
        auto it = palette.find(std::to_string(id));
        if (it != palette.end() && it->is_array() && it->size() >= 3 && (*it)[0].is_number() && (*it)[1].is_number() && (*it)[2].is_number())
            return {(*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>()};
    }
    return defaultIdColour(id);
}

std::string idMaterialName(int64_t id) { return std::string(kIdMaterialPrefix) + (id < 0 ? "n" : "") + std::to_string(id < 0 ? -id : id); }

std::string toHex(const glm::dvec3& c)
{
    auto channel = [](double v) { return int(std::clamp<int64_t>(roundEven(v * 255.0), 0, 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", channel(c.r), channel(c.g), channel(c.b));
    return buf;
}

json idLegend(const json& active, const json& palette, const json& values)
{
    json legend = json::object();
    auto add = [&](const std::string& name, int64_t id) {
        const std::string key = std::to_string(id);
        if (!legend.contains(key))
        {
            const glm::dvec3 c = idColour(id, palette);
            legend[key] = {{"colour", {c.r, c.g, c.b}}, {"sources", json::array()}};
        }
        legend[key]["sources"].push_back(name);
    };
    for (const json& s : active)
        add(s.value("name", ""), s.contains("objectId") && s["objectId"].is_number() ? s["objectId"].get<int64_t>() : 0);
    if (values.is_object() && flag(values, "groundPlane"))
        add("Ground Plane", whole(values, "groundId"));
    return legend;
}

nlohmann::ordered_json defaultColourRecipes(const json& legend)
{
    struct Recipe
    {
        glm::dvec3 base, root, occlusion, perInstance, tip;
        double roughness;
    };
    static const Recipe scatter{{0.44, 0.54, 0.24}, {0.32, 0.42, 0.19}, {0.32, 0.42, 0.19}, {0.32, 0.42, 0.19}, {0.57, 0.66, 0.30}, 0.45};
    static const Recipe ground{{0.19, 0.13, 0.09}, {0.12, 0.08, 0.05}, {0.04, 0.03, 0.02}, {0.22, 0.16, 0.10}, {0.28, 0.21, 0.14}, 0.70};
    std::vector<int64_t> ids;
    if (legend.is_object())
        for (auto it = legend.begin(); it != legend.end(); ++it)
            ids.push_back(std::stoll(it.key()));
    std::sort(ids.begin(), ids.end());
    nlohmann::ordered_json out = nlohmann::ordered_json::object();
    for (int64_t id : ids)
    {
        const Recipe& r = id < 0 ? ground : scatter;
        out[std::to_string(id)] = {{"base", toHex(r.base)},   {"root", toHex(r.root)}, {"occlusion", toHex(r.occlusion)},
                                   {"perInstance", toHex(r.perInstance)}, {"tip", toHex(r.tip)},   {"roughness", r.roughness}};
    }
    return out;
}

Axes sourceAxes(const glm::dvec3& boxMin, const glm::dvec3& boxMax, const std::string& lengthAxis, const std::string& baseEnd)
{
    Axes a;
    a.min = boxMin;
    a.max = boxMax;
    a.extents = boxMax - boxMin;
    a.length = 0;
    if (lengthAxis == "X" || lengthAxis == "Y" || lengthAxis == "Z")
        a.length = lengthAxis[0] - 'X';
    else
        for (int i = 1; i < 3; ++i)
            if (a.extents[i] > a.extents[a.length])
                a.length = i;
    std::vector<int> rest;
    for (int i = 0; i < 3; ++i)
        if (i != a.length)
            rest.push_back(i);
    // Python's stable sort, descending: the first of equal extents stays first.
    std::stable_sort(rest.begin(), rest.end(), [&](int l, int r) { return a.extents[l] > a.extents[r]; });
    a.width = rest[0];
    a.thickness = rest[1];
    if (baseEnd == "Min")
        a.baseSign = 1.0;
    else if (baseEnd == "Max")
        a.baseSign = -1.0;
    else
        a.baseSign = std::abs(boxMin[a.length]) <= std::abs(boxMax[a.length]) ? 1.0 : -1.0;
    return a;
}

std::string describeAxes(const Axes& a)
{
    return std::string("grows ") + (a.baseSign > 0.0 ? "+" : "-") + kAxisNames[size_t(a.length)] + ", thin on " + kAxisNames[size_t(a.thickness)];
}

double pivotReach(const glm::dvec3& boxMin, const glm::dvec3& boxMax, const glm::dvec3& pivot, const glm::dmat4& world)
{
    const double scale = std::max({glm::length(glm::dvec3(world[0])), glm::length(glm::dvec3(world[1])), glm::length(glm::dvec3(world[2]))});
    double reach = 0.0;
    for (double x : {boxMin.x, boxMax.x})
        for (double y : {boxMin.y, boxMax.y})
            for (double z : {boxMin.z, boxMax.z})
                reach = std::max(reach, glm::length(glm::dvec3(x, y, z) - pivot));
    return reach * scale;
}

void prepareSource(Source& s, const std::string& lengthAxis, const std::string& baseEnd)
{
    s.mesh.compact();
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const glm::dvec3& p : s.mesh.positions())
    {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    if (s.mesh.positions().empty())
        lo = hi = glm::dvec3(0.0);
    s.axes = sourceAxes(lo, hi, lengthAxis, baseEnd);
    s.reach = pivotReach(lo, hi, s.rotatePivot, s.world);
    const int length = s.axes.length;
    const double low = s.axes.min[length];
    const double span = std::max(s.axes.extents[length], 1e-9);
    s.lengthParams.clear();
    for (const glm::dvec3& p : s.mesh.positions())
    {
        double along = (p[length] - low) / span;
        if (s.axes.baseSign < 0.0)
            along = 1.0 - along;
        s.lengthParams.push_back(float(std::clamp(along, 0.0, 1.0)));
    }
}

Plan plan(const json& v, const std::vector<Source>& sources)
{
    Plan out;
    if (sources.empty())
        return out;
    Random rng(uint64_t(whole(v, "seed")));
    const double half = num(v, "tileSize") * 0.5;
    std::vector<double> weights;
    for (const Source& s : sources)
        weights.push_back(std::max(0.0, s.weight));

    const std::vector<glm::dvec2> points = scatterPoints(v, rng);
    out.scatterPoints = points.size();
    if (points.empty())
    {
        out.warnings.push_back("The current distribution settings produced no scatter points.");
        return out;
    }
    if (double(points.size()) < double(whole(v, "count")) * 0.9)
        out.warnings.push_back("Only " + std::to_string(points.size()) + " of " + std::to_string(whole(v, "count")) +
                               " points fit. Lower Min Spacing or the count.");

    const double tilt = num(v, "maxTilt");
    const double shear = num(v, "maxShear");
    const double stretch = 1.0 + std::max({num(v, "lengthVariation"), num(v, "widthVariation"), num(v, "thicknessVariation")});
    for (size_t index = 0; index < points.size(); ++index)
    {
        Placement p;
        p.scatterIndex = index;
        p.source = weightedPick(rng, sources, weights);
        const Layer layer = pickLayer(rng, v);
        const double scale = rng.uniform(num(v, "minScale"), num(v, "maxScale")) * layer.scale;
        p.x = points[index].x;
        p.y = points[index].y;
        p.height = layer.height + sources[p.source].heightOffset;
        p.seed = whole(v, "seed") + int64_t(index);
        p.variation = rng.random();
        p.band = layer.band;
        p.scale = scale;
        p.lengthFactor = 1.0 + rng.uniform(-1.0, 1.0) * num(v, "lengthVariation");
        p.widthFactor = 1.0 + rng.uniform(-1.0, 1.0) * num(v, "widthVariation");
        p.thicknessFactor = 1.0 + rng.uniform(-1.0, 1.0) * num(v, "thicknessVariation");
        p.yaw = pickYaw(rng, v);
        p.tiltA = rng.uniform(-tilt, tilt);
        p.tiltB = rng.uniform(-tilt, tilt);
        p.bend = rng.uniform(num(v, "minBend"), num(v, "maxBend"));
        p.twist = rng.uniform(num(v, "minTwist"), num(v, "maxTwist"));
        p.sine = rng.uniform(num(v, "minSineAmplitude"), num(v, "maxSineAmplitude"));
        p.sineOffset = rng.uniform(0.0, 2.0);
        p.flareStart = rng.uniform(num(v, "minFlareStart"), num(v, "maxFlareStart"));
        p.flareEnd = rng.uniform(num(v, "minFlareEnd"), num(v, "maxFlareEnd"));
        p.flatten = rng.uniform(num(v, "minFlatten"), num(v, "maxFlatten"));
        p.shearXY = rng.uniform(-shear, shear);
        p.shearXZ = rng.uniform(-shear, shear);
        p.shearYZ = rng.uniform(-shear, shear);

        std::vector<glm::dvec2> positions{{p.x, p.y}};
        if (flag(v, "wrapEdges"))
            positions = wrappedPositions(p.x, p.y, half, sources[p.source].reach * scale * stretch);
        for (const glm::dvec2& at : positions)
        {
            Placement copy = p;
            copy.x = at.x;
            copy.y = at.y;
            out.placements.push_back(copy);
        }
    }
    return out;
}

std::vector<glm::dvec3> deformPoints(const json& v, const Source& s, const Placement& p)
{
    std::vector<glm::dvec3> pts(s.mesh.positions().begin(), s.mesh.positions().end());
    const Axes& a = s.axes;

    // Each enabled nonlinear deforms in its own handle space, bend, twist and sine from the base and flare from the centre.
    struct Stage
    {
        HandleKind kind;
        const char* flag;
        bool atBase;
    };
    static const Stage stages[] = {{kBend, "bendEnabled", true}, {kTwist, "twistEnabled", true}, {kSine, "sineEnabled", true}, {kFlare, "flareEnabled", false}};
    bool anyStage = false;
    for (const Stage& st : stages)
        anyStage = anyStage || flag(v, st.flag);
    if (anyStage)
    {
        const glm::dmat4 inv = glm::inverse(s.world);
        for (const Stage& st : stages)
        {
            if (!flag(v, st.flag))
                continue;
            const std::optional<AuthoredHandle>& authored = s.authored[st.kind];
            const glm::dmat4 h = authored ? authored->world : derivedHandle(s, st.atBase);
            const double low = authored ? authored->low : (st.atBase ? kBaseLowBound : -1.0);
            const double high = authored ? authored->high : (st.atBase ? kBaseHighBound : 1.0);
            const glm::dmat4 toHandle = glm::inverse(h) * s.world;
            const glm::dmat4 back = inv * h;
            for (glm::dvec3& q : pts)
            {
                const glm::dvec3 hp(toHandle * glm::dvec4(q, 1.0));
                glm::dvec3 d = hp;
                switch (st.kind)
                {
                case kBend:
                    d = deform::bend(hp, {.angle = p.bend * (high - low), .low = low, .high = high});
                    break;
                case kTwist:
                    d = deform::twist(hp, {.startAngle = 0.0, .endAngle = p.twist, .low = low, .high = high});
                    break;
                case kSine:
                    d = deform::sine(hp, {.amplitude = p.sine, .wavelength = std::max(0.01, num(v, "sineWavelength")), .offset = p.sineOffset,
                                          .dropoff = 0.0, .low = low, .high = high});
                    break;
                case kFlare:
                    d = deform::taper(hp, {.startScale = p.flareStart, .endScale = p.flareEnd, .curve = 0.0, .low = low, .high = high});
                    break;
                default:
                    break;
                }
                q = glm::dvec3(back * glm::dvec4(d, 1.0));
            }
        }
    }

    if (flag(v, "jitterEnabled"))
    {
        const double amplitude = num(v, "jitterAmplitude") * a.extents[a.thickness];
        const double frequency = std::max(num(v, "jitterFrequency") / std::max(a.extents[a.length], 1e-4), 1e-4);
        if (amplitude > 0.0)
            for (glm::dvec3& q : pts)
            {
                const int64_t cx = int64_t(std::floor(q.x * frequency));
                const int64_t cy = int64_t(std::floor(q.y * frequency));
                const int64_t cz = int64_t(std::floor(q.z * frequency));
                q.x += (hash01({cx, cy, cz, p.seed}) - 0.5) * 2.0 * amplitude;
                q.y += (hash01({cy, cz, cx, p.seed + 1}) - 0.5) * 2.0 * amplitude;
                q.z += (hash01({cz, cx, cy, p.seed + 2}) - 0.5) * 2.0 * amplitude;
            }
    }

    if (flag(v, "latticeEnabled") && !pts.empty())
    {
        const int d = int(std::max<int64_t>(2, whole(v, "latticeDivisions")));
        const glm::ivec3 n(d);
        const double amplitude = num(v, "latticeAmplitude") * a.extents[a.length];
        Random rng(uint64_t(p.seed));
        std::vector<glm::dvec3> offsets(size_t(d * d * d));
        for (int i = 0; i < d; ++i)
            for (int j = 0; j < d; ++j)
                for (int k = 0; k < d; ++k)
                {
                    glm::dvec3& o = offsets[deform::latticeIndex(n, i, j, k)];
                    o.x = rng.uniform(-amplitude, amplitude);
                    o.y = rng.uniform(-amplitude, amplitude);
                    o.z = rng.uniform(-amplitude, amplitude);
                }
        glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const glm::dvec3& q : pts)
        {
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
        const glm::dvec3 size = glm::max(hi - lo, glm::dvec3(1e-9));
        for (glm::dvec3& q : pts)
            q += deform::ffdDisplacement(n, offsets, (q - lo) / size, deform::FfdBasis::Bernstein);
    }
    return pts;
}

InstanceGeometry buildInstance(const json& v, const Source& s, const Placement& p)
{
    InstanceGeometry out;
    const std::vector<glm::dvec3> deformed = deformPoints(v, s, p);
    const Axes& a = s.axes;

    glm::dmat4 scaleShear(1.0);
    glm::dvec3 scale(1.0);
    scale[a.length] = p.scale * p.lengthFactor;
    scale[a.width] = p.scale * p.widthFactor;
    scale[a.thickness] = p.scale * p.thicknessFactor;
    glm::dmat4 shear(1.0);
    if (flag(v, "shearEnabled"))
    {
        // x' = x + xy * y + xz * z, y' = y + yz * z: the shear a transform applies after its scale.
        shear[1][0] = p.shearXY;
        shear[2][0] = p.shearXZ;
        shear[2][1] = p.shearYZ;
    }
    scaleShear = shear * glm::scale(glm::dmat4(1.0), scale);
    const glm::dmat4 objectEdit = glm::translate(glm::dmat4(1.0), s.scalePivot) * scaleShear * glm::translate(glm::dmat4(1.0), -s.scalePivot);

    const int up = upIndex(v);
    const bool zUp = up == 2;
    const glm::dmat4 spin = zUp ? eulerXYZ({p.tiltA, p.tiltB, p.yaw}) : eulerXYZ({p.tiltA, p.yaw, p.tiltB});
    const glm::dvec3 pivot(s.world * objectEdit * glm::dvec4(s.rotatePivot, 1.0));
    glm::dvec3 target = zUp ? glm::dvec3(p.x, p.y, p.height) : glm::dvec3(p.x, p.height, p.y);

    const glm::dmat4 placed = spin * glm::translate(glm::dmat4(1.0), -pivot) * s.world * objectEdit;
    out.world.resize(deformed.size());
    double lowest = std::numeric_limits<double>::max();
    for (size_t i = 0; i < deformed.size(); ++i)
    {
        out.world[i] = glm::dvec3(placed * glm::dvec4(deformed[i], 1.0));
        lowest = std::min(lowest, out.world[i][up] + target[up]);
    }
    if (flag(v, "snapBaseToGround") && !deformed.empty())
    {
        const double offset = p.height - lowest;
        if (std::abs(offset) >= 1e-6)
            target[up] += offset;
    }
    for (glm::dvec3& w : out.world)
        w += target;
    out.nodeMatrix = glm::translate(glm::dmat4(1.0), target) * spin * glm::translate(glm::dmat4(1.0), -pivot) * s.world;

    if (flag(v, "flattenEnabled") && std::abs(p.flatten - 1.0) >= 1e-6 && !out.world.empty())
    {
        double low = std::numeric_limits<double>::max();
        for (const glm::dvec3& w : out.world)
            low = std::min(low, w[up]);
        for (glm::dvec3& w : out.world)
            w[up] = low + (w[up] - low) * p.flatten;
    }

    if (flag(v, "vertexColours"))
    {
        out.colours.resize(out.world.size());
        for (size_t i = 0; i < out.world.size(); ++i)
            out.colours[i] = glm::vec4(i < s.lengthParams.size() ? s.lengthParams[i] : 0.0f, float(p.variation), float(p.band), 1.0f);
    }
    return out;
}

std::vector<InstanceGeometry> buildAll(const json& v, const std::vector<Source>& sources, const Plan& plan)
{
    std::vector<InstanceGeometry> out(plan.placements.size());
    tbb::parallel_for(size_t(0), plan.placements.size(), [&](size_t i) {
        const Placement& p = plan.placements[i];
        out[i] = buildInstance(v, sources[p.source], p);
    });
    return out;
}

geo::Mesh tilePlane(double tileSize, const std::string& upAxis, int subdivisions)
{
    const int n = std::max(1, subdivisions);
    const double half = tileSize * 0.5;
    const double step = tileSize / double(n);
    const bool zUp = upAxis == "Z";
    std::vector<glm::dvec3> points;
    std::vector<glm::vec2> uvs;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const double a = -half + double(i) * step;
            const double b = -half + double(j) * step;
            // Y up runs v toward -Z, so the winding still faces +Y.
            points.push_back(zUp ? glm::dvec3(a, b, 0.0) : glm::dvec3(a, 0.0, -b));
            uvs.emplace_back(float(double(i) / double(n)), float(double(j) / double(n)));
        }
    std::vector<geo::Index> sizes, indices;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const geo::Index c = geo::Index(j * (n + 1) + i);
            sizes.push_back(4);
            for (geo::Index k : {c, c + 1, c + geo::Index(n + 2), c + geo::Index(n + 1)})
                indices.push_back(k);
        }
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, indices);
    geo::Attribute& uv = m.cornerAttrs().add(geo::attr::uvName(0), geo::AttrType::Float2);
    std::span<glm::vec2> values = uv.values<glm::vec2>();
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            values[c] = uvs[m.cornerPoint(c)];
    return m;
}

HeightProfile profileOf(std::vector<double>& heights)
{
    HeightProfile out;
    out.hits = heights.size();
    if (heights.empty())
        return out;
    std::sort(heights.begin(), heights.end());
    const size_t n = heights.size();
    out.p50 = heights[n / 2];
    out.p99 = heights[std::min(n - 1, size_t(double(n) * 0.99))];
    out.top = heights.back();
    return out;
}

std::string maskStem(const std::string& mapPath)
{
    std::string stem = mapPath;
    const size_t slash = stem.find_last_of("/\\");
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem = stem.substr(0, dot);
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* suffix : kVertexMapSuffixes)
    {
        const std::string s = suffix;
        if (lower.size() >= s.size() && lower.compare(lower.size() - s.size(), s.size(), s) == 0)
        {
            stem = stem.substr(0, stem.size() - s.size());
            while (!stem.empty() && stem.back() == '_')
                stem.pop_back();
            break;
        }
    }
    return stem;
}
} // namespace terrain
