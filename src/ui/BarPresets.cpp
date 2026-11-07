#include "ui/BarPresets.h"

#include "core/Events.h"
#include "core/PrefStore.h"
#include "ui/IconBars.h"
#include "ui/ViewportHeaderConfig.h"

#include <fstream>

namespace lila::ui::barPresets
{
using json = nlohmann::json;

namespace
{
constexpr const char* kPresetsPref = "/ui/barPresets/saved";
constexpr const char* kCurrentPref = "/ui/barPresets/current";
constexpr const char* kLayoutsPref = "/ui/barPresets/layouts";

json savedPresets()
{
    json all = PrefStore::get().value("general", kPresetsPref, json::object());
    return all.is_object() ? all : json::object();
}

json layoutBindings()
{
    json all = PrefStore::get().value("general", kLayoutsPref, json::object());
    return all.is_object() ? all : json::object();
}

void setCurrent(const std::string& name)
{
    PrefStore::get().set("general", kCurrentPref, name);
}
} // namespace

const std::vector<std::string>& bars()
{
    static const std::vector<std::string> all = {kHeader, iconBars::kToolBox, iconBars::kOutliner};
    return all;
}

std::string barLabel(const std::string& bar)
{
    if (bar == kHeader)
        return "Viewport Header";
    if (bar == iconBars::kToolBox)
        return "Tool Box";
    if (bar == iconBars::kOutliner)
        return "Outliner";
    return bar;
}

json capture()
{
    json out = json::object();
    const json header = PrefStore::get().value("general", viewportHeader::kPref, json());
    if (header.is_object())
        out[kHeader] = viewportHeader::config();
    for (const std::string& bar : bars())
        if (bar != kHeader && iconBars::arranged(bar))
            out[bar] = iconBars::arrangement(bar);
    return out;
}

void applyBars(const json& in)
{
    const json b = in.is_object() ? in : json::object();
    if (b.contains(kHeader) && b[kHeader].is_object())
        viewportHeader::setConfig(b[kHeader]);
    else
        PrefStore::get().remove("general", viewportHeader::kPref);
    for (const std::string& bar : bars())
    {
        if (bar == kHeader)
            continue;
        if (b.contains(bar) && b[bar].is_array())
            iconBars::setArrangement(bar, b[bar]);
        else
            iconBars::resetArrangement(bar);
    }
}

std::vector<std::string> names()
{
    std::vector<std::string> out;
    const json all = savedPresets();
    for (auto& [name, _] : all.items())
        out.push_back(name);
    return out;
}

std::string current()
{
    const std::string name = PrefStore::get().value<std::string>("general", kCurrentPref, std::string());
    return savedPresets().contains(name) ? name : std::string();
}

bool activate(const std::string& name)
{
    if (name.empty())
    {
        applyBars(json::object());
        setCurrent({});
        return true;
    }
    const json all = savedPresets();
    if (!all.contains(name))
        return false;
    applyBars(all[name]);
    setCurrent(name);
    return true;
}

void save(const std::string& name)
{
    if (name.empty())
        return;
    json all = savedPresets();
    all[name] = capture();
    PrefStore::get().set("general", kPresetsPref, all);
    setCurrent(name);
}

void remove(const std::string& name)
{
    json all = savedPresets();
    all.erase(name);
    PrefStore::get().set("general", kPresetsPref, all);
    json bound = layoutBindings();
    for (auto it = bound.begin(); it != bound.end();)
        it = it->is_string() && it->get<std::string>() == name ? bound.erase(it) : std::next(it);
    PrefStore::get().set("general", kLayoutsPref, bound);
    if (PrefStore::get().value<std::string>("general", kCurrentPref, std::string()) == name)
        activate({});
}

bool exportTo(const std::string& name, const std::filesystem::path& file, std::string& error)
{
    const json all = savedPresets();
    if (!all.contains(name))
    {
        error = "no preset named \"" + name + "\"";
        return false;
    }
    std::ofstream out(file, std::ios::binary);
    out << json{{"lilaBars", 1}, {"name", name}, {"bars", all[name]}}.dump(2);
    if (!out)
    {
        error = "couldn't write the file";
        return false;
    }
    return true;
}

std::string importFrom(const std::filesystem::path& file, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    const json doc = in ? json::parse(in, nullptr, false) : json();
    if (!doc.is_object() || !doc.contains("bars") || !doc["bars"].is_object())
    {
        error = "not a bar preset file";
        return {};
    }
    std::string base = doc.value("name", file.stem().string());
    if (base.empty())
        base = "Imported";
    json all = savedPresets();
    std::string name = base;
    for (int n = 2; all.contains(name); ++n)
        name = base + " " + std::to_string(n);
    all[name] = doc["bars"];
    PrefStore::get().set("general", kPresetsPref, all);
    return name;
}

void edited()
{
    const std::string name = current();
    if (name.empty())
        return;
    json all = savedPresets();
    all[name] = capture();
    PrefStore::get().set("general", kPresetsPref, all);
}

std::string boundTo(const std::string& layout)
{
    const json bound = layoutBindings();
    auto it = bound.find(layout);
    return it != bound.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void bind(const std::string& layout, const std::string& preset)
{
    json bound = layoutBindings();
    if (preset.empty())
        bound.erase(layout);
    else
        bound[layout] = preset;
    PrefStore::get().set("general", kLayoutsPref, bound);
}

void install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    events::subscribe(events::Event::LayoutChanged, [](const events::EventData& e) {
        const std::string preset = boundTo(e.name);
        if (preset == kDefaultBinding)
            activate({});
        else if (!preset.empty() && preset != current())
            activate(preset);
    });
}
} // namespace lila::ui::barPresets
