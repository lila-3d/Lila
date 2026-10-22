#include "scene/SoftSelect.h"

#include "cmds/Command.h"
#include "core/PrefStore.h"

#include <algorithm>
#include <cmath>

namespace lila::scene::softSelect
{
namespace
{
constexpr double kEps = 1e-9;

std::vector<Key> defaultKeys()
{
    return {{0.0, 1.0, Interp::Smooth}, {1.0, 0.0, Interp::Smooth}};
}

Curve spline(std::initializer_list<std::pair<double, double>> pts)
{
    std::vector<Key> keys;
    for (auto [p, v] : pts)
        keys.push_back({p, v, Interp::Spline});
    return Curve(std::move(keys));
}

double distanceToSegment(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
{
    glm::dvec2 ab = b - a;
    double len2 = ab.x * ab.x + ab.y * ab.y;
    double t = len2 > 0.0 ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0, 1.0) : 0.0;
    glm::dvec2 d = p - (a + ab * t);
    return std::sqrt(d.x * d.x + d.y * d.y);
}

json userPresetsJson()
{
    json j = PrefStore::get().value("general", kUserPresets, json::array());
    return j.is_array() ? j : json::array();
}
} // namespace

const char* interpName(Interp i)
{
    switch (i)
    {
    case Interp::Linear: return "linear";
    case Interp::Smooth: return "smooth";
    case Interp::Flat: return "flat";
    case Interp::Spline: break;
    }
    return "spline";
}

Interp interpFromName(std::string_view name, Interp fallback)
{
    if (name == "linear")
        return Interp::Linear;
    if (name == "smooth")
        return Interp::Smooth;
    if (name == "spline")
        return Interp::Spline;
    if (name == "flat" || name == "none" || name == "step")
        return Interp::Flat;
    return fallback;
}

Curve::Curve() : Curve(defaultKeys()) {}

Curve::Curve(std::vector<Key> keys) : mKeys(std::move(keys))
{
    if (mKeys.empty())
        mKeys = defaultKeys();
    sortAndClamp();
}

void Curve::sortAndClamp()
{
    for (Key& k : mKeys)
    {
        k.pos = std::clamp(std::isfinite(k.pos) ? k.pos : 0.0, 0.0, 1.0);
        k.value = std::clamp(std::isfinite(k.value) ? k.value : 0.0, 0.0, 1.0);
    }
    std::stable_sort(mKeys.begin(), mKeys.end(), [](const Key& a, const Key& b) { return a.pos < b.pos; });
    updateTangents();
}

void Curve::updateTangents()
{
    const size_t n = mKeys.size();
    mTangents.assign(n, 0.0);
    // Interior tangents are the weighted harmonic mean of the neighbouring slopes (Fritsch-Butland), zero at extrema.
    for (size_t k = 1; k + 1 < n; ++k)
    {
        double h0 = mKeys[k].pos - mKeys[k - 1].pos, h1 = mKeys[k + 1].pos - mKeys[k].pos;
        if (h0 <= kEps || h1 <= kEps)
            continue;
        double d0 = (mKeys[k].value - mKeys[k - 1].value) / h0, d1 = (mKeys[k + 1].value - mKeys[k].value) / h1;
        if (d0 * d1 <= 0.0)
            continue;
        mTangents[k] = 3.0 * (h0 + h1) / ((2.0 * h1 + h0) / d0 + (h1 + 2.0 * h0) / d1);
    }
}

double Curve::evaluate(double t) const
{
    if (mKeys.size() == 1 || t <= mKeys.front().pos)
        return mKeys.front().value;
    if (t >= mKeys.back().pos)
        return mKeys.back().value;
    auto it = std::upper_bound(mKeys.begin(), mKeys.end(), t, [](double x, const Key& k) { return x < k.pos; });
    size_t k = size_t(it - mKeys.begin()) - 1;
    const Key& a = mKeys[k];
    const Key& b = mKeys[k + 1];
    double h = b.pos - a.pos;
    if (h <= kEps)
        return b.value;
    double u = (t - a.pos) / h, v = a.value;
    switch (a.interp)
    {
    case Interp::Linear: v = a.value + (b.value - a.value) * u; break;
    case Interp::Smooth: v = a.value + (b.value - a.value) * u * u * (3.0 - 2.0 * u); break;
    case Interp::Flat: v = a.value; break;
    case Interp::Spline:
    {
        double u2 = u * u, u3 = u2 * u;
        v = (2 * u3 - 3 * u2 + 1) * a.value + (u3 - 2 * u2 + u) * h * mTangents[k] + (-2 * u3 + 3 * u2) * b.value +
            (u3 - u2) * h * mTangents[k + 1];
        break;
    }
    }
    return std::clamp(v, 0.0, 1.0);
}

int Curve::addKey(Key k)
{
    k.pos = std::clamp(k.pos, 0.0, 1.0);
    k.value = std::clamp(k.value, 0.0, 1.0);
    auto at = std::upper_bound(mKeys.begin(), mKeys.end(), k.pos, [](double x, const Key& e) { return x < e.pos; });
    int index = int(at - mKeys.begin());
    mKeys.insert(at, k);
    updateTangents();
    return index;
}

void Curve::removeKey(int index)
{
    if (mKeys.size() <= 2 || index < 0 || index >= int(mKeys.size()))
        return;
    mKeys.erase(mKeys.begin() + index);
    updateTangents();
}

int Curve::moveKey(int index, double pos, double value)
{
    if (index < 0 || index >= int(mKeys.size()))
        return index;
    Key k = mKeys[index];
    mKeys.erase(mKeys.begin() + index);
    k.pos = pos;
    k.value = value;
    return addKey(k);
}

void Curve::setInterp(int index, Interp i)
{
    if (index >= 0 && index < int(mKeys.size()))
        mKeys[index].interp = i;
}

json Curve::toJson() const
{
    json out = json::array();
    for (const Key& k : mKeys)
        out.push_back({k.pos, k.value, interpName(k.interp)});
    return out;
}

Curve Curve::fromJson(const json& j)
{
    if (!j.is_array())
        return Curve();
    std::vector<Key> keys;
    for (const json& e : j)
    {
        Key k;
        if (e.is_array() && e.size() >= 2 && e[0].is_number() && e[1].is_number())
        {
            k.pos = e[0].get<double>();
            k.value = e[1].get<double>();
            if (e.size() >= 3 && e[2].is_string())
                k.interp = interpFromName(e[2].get<std::string>());
        }
        else if (e.is_object() && e.contains("pos") && e.contains("value") && e["pos"].is_number() && e["value"].is_number())
        {
            k.pos = e["pos"].get<double>();
            k.value = e["value"].get<double>();
            if (e.contains("interp") && e["interp"].is_string())
                k.interp = interpFromName(e["interp"].get<std::string>());
        }
        else
            continue;
        keys.push_back(k);
    }
    return keys.size() >= 2 ? Curve(std::move(keys)) : Curve();
}

std::vector<glm::dvec2> simplify(const std::vector<glm::dvec2>& points, double epsilon)
{
    if (points.size() <= 2)
        return points;
    std::vector<char> keep(points.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, points.size() - 1}};
    while (!stack.empty())
    {
        auto [lo, hi] = stack.back();
        stack.pop_back();
        double worst = -1.0;
        size_t at = lo;
        for (size_t i = lo + 1; i < hi; ++i)
            if (double d = distanceToSegment(points[i], points[lo], points[hi]); d > worst)
            {
                worst = d;
                at = i;
            }
        if (worst > epsilon)
        {
            keep[at] = 1;
            stack.push_back({lo, at});
            stack.push_back({at, hi});
        }
    }
    std::vector<glm::dvec2> out;
    for (size_t i = 0; i < points.size(); ++i)
        if (keep[i])
            out.push_back(points[i]);
    return out;
}

Curve curveFromStroke(const std::vector<glm::dvec2>& stroke, double epsilon)
{
    if (stroke.empty())
        return Curve();
    std::vector<glm::dvec2> pts = stroke;
    if (pts.back().x < pts.front().x)
        std::reverse(pts.begin(), pts.end());
    std::vector<glm::dvec2> mono;
    for (glm::dvec2 p : pts)
    {
        p = {std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, 0.0, 1.0)};
        if (mono.empty() || p.x > mono.back().x + 1e-6)
            mono.push_back(p);
    }
    if (mono.front().x > 0.0)
        mono.insert(mono.begin(), {0.0, mono.front().y});
    if (mono.back().x < 1.0)
        mono.push_back({1.0, mono.back().y});
    std::vector<Key> keys;
    for (const glm::dvec2& p : simplify(mono, epsilon))
        keys.push_back({p.x, p.y, Interp::Spline});
    return Curve(std::move(keys));
}

const std::vector<Preset>& builtinPresets()
{
    static const std::vector<Preset> presets = [] {
        std::vector<Preset> p;
        p.push_back({"Smooth", Curve()});
        p.push_back({"Linear", Curve({{0.0, 1.0, Interp::Linear}, {1.0, 0.0, Interp::Linear}})});
        p.push_back({"Sharp", spline({{0.0, 1.0}, {0.15, 0.55}, {0.5, 0.12}, {1.0, 0.0}})});
        p.push_back({"Dome", spline({{0.0, 1.0}, {0.5, 0.87}, {0.85, 0.5}, {1.0, 0.0}})});
        p.push_back({"Bell", spline({{0.0, 1.0}, {0.25, 0.8}, {0.5, 0.3}, {0.75, 0.06}, {1.0, 0.0}})});
        p.push_back({"Spike", spline({{0.0, 1.0}, {0.08, 0.3}, {0.5, 0.03}, {1.0, 0.0}})});
        p.push_back({"Plateau", spline({{0.0, 1.0}, {0.6, 1.0}, {1.0, 0.0}})});
        p.push_back({"Inverse", Curve({{0.0, 0.0, Interp::Smooth}, {1.0, 1.0, Interp::Smooth}})});
        p.push_back({"Ring", spline({{0.0, 1.0}, {0.25, 0.15}, {0.5, 0.7}, {1.0, 0.0}})});
        return p;
    }();
    return presets;
}

const Preset* findBuiltin(std::string_view name)
{
    for (const Preset& p : builtinPresets())
        if (p.name == name)
            return &p;
    return nullptr;
}

Curve currentCurve()
{
    json j = PrefStore::get().value("general", kCurve);
    return j.is_null() ? Curve() : Curve::fromJson(j);
}

void setCurrentCurve(const Curve& c)
{
    PrefStore::get().set("general", kCurve, c.toJson());
}

std::vector<Preset> userPresets()
{
    std::vector<Preset> out;
    for (const json& e : userPresetsJson())
        if (e.is_object() && e.contains("name") && e["name"].is_string())
            out.push_back({e["name"].get<std::string>(), Curve::fromJson(e.value("keys", json()))});
    return out;
}

void saveUserPreset(const std::string& name, const Curve& c)
{
    json list = userPresetsJson();
    json entry = {{"name", name}, {"keys", c.toJson()}};
    bool replaced = false;
    for (json& e : list)
        if (e.is_object() && e.value("name", std::string()) == name)
        {
            e = entry;
            replaced = true;
        }
    if (!replaced)
        list.push_back(entry);
    PrefStore::get().set("general", kUserPresets, list);
}

bool deleteUserPreset(const std::string& name)
{
    json list = userPresetsJson(), kept = json::array();
    for (const json& e : list)
        if (!(e.is_object() && e.value("name", std::string()) == name))
            kept.push_back(e);
    if (kept.size() == list.size())
        return false;
    PrefStore::get().set("general", kUserPresets, kept);
    return true;
}

bool renameUserPreset(const std::string& from, const std::string& to)
{
    if (to.empty() || from == to)
        return false;
    json list = userPresetsJson();
    bool found = false;
    for (const json& e : list)
        if (e.is_object() && e.value("name", std::string()) == to)
            return false;
    for (json& e : list)
        if (e.is_object() && e.value("name", std::string()) == from)
        {
            e["name"] = to;
            found = true;
        }
    if (found)
        PrefStore::get().set("general", kUserPresets, list);
    return found;
}

bool surfaceFalloff()
{
    return PrefStore::get().value<std::string>("general", kFalloffMode, "volume") == "surface";
}

void registerCommands()
{
    using T = ArgType;
    cmds::registerCommand({
        .name = "softSelectCurve",
        .doc = "Set or query the soft select falloff curve (weight against distance from the selection, 0 at the selection, "
               "1 at the radius). With no flags it returns the current curve.",
        .flags = {{"keys", "k", T::List, nullptr, "Keys as [[pos, value, interp], ...]; interp is linear, smooth, spline or flat."},
                  {"preset", "p", T::String, nullptr, "Apply a built-in or saved preset by name."},
                  {"falloffMode", "fm", T::String, nullptr, "Distance measured through space or along the surface.", {"volume", "surface"}},
                  {"save", "", T::String, nullptr, "Save the current curve (after keys/preset) as a user preset with this name."},
                  {"remove", "", T::String, nullptr, "Delete the named user preset."},
                  {"rename", "", T::String, nullptr, "User preset to rename; the new name goes in to."},
                  {"to", "", T::String, nullptr, "New name for rename."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("keys"))
            {
                Curve c = Curve::fromJson(a.flags["keys"]);
                setCurrentCurve(c);
            }
            if (a.has("preset"))
            {
                std::string name = a.get<std::string>("preset");
                if (const Preset* p = findBuiltin(name))
                    setCurrentCurve(p->curve);
                else
                {
                    bool found = false;
                    for (const Preset& u : userPresets())
                        if (u.name == name)
                        {
                            setCurrentCurve(u.curve);
                            found = true;
                            break;
                        }
                    if (!found)
                        throw CommandError("softSelectCurve: no preset named '" + name + "'");
                }
            }
            if (a.has("falloffMode"))
            {
                std::string mode = a.get<std::string>("falloffMode");
                if (mode != "volume" && mode != "surface")
                    throw CommandError("softSelectCurve: falloffMode is volume or surface");
                PrefStore::get().set("general", kFalloffMode, mode);
            }
            if (a.has("save"))
            {
                std::string name = a.get<std::string>("save");
                if (name.empty() || findBuiltin(name))
                    throw CommandError("softSelectCurve: '" + name + "' can't be used as a preset name");
                saveUserPreset(name, currentCurve());
            }
            if (a.has("remove") && !deleteUserPreset(a.get<std::string>("remove")))
                throw CommandError("softSelectCurve: no user preset named '" + a.get<std::string>("remove") + "'");
            if (a.has("rename") && !renameUserPreset(a.get<std::string>("rename"), a.get<std::string>("to")))
                throw CommandError("softSelectCurve: can't rename '" + a.get<std::string>("rename") + "' to '" + a.get<std::string>("to") + "'");
            json users = json::array();
            for (const Preset& p : userPresets())
                users.push_back(p.name);
            return {{"keys", currentCurve().toJson()},
                    {"falloffMode", surfaceFalloff() ? "surface" : "volume"},
                    {"userPresets", users}};
        },
    });
}
} // namespace lila::scene::softSelect
