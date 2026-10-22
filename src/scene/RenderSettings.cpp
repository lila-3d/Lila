#include "scene/RenderSettings.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "scene/Camera.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace lila::scene
{
namespace
{
struct State
{
    RenderSettings settings;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
};

State& state()
{
    static State s;
    return s;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

json resolutionJson()
{
    const RenderSettings& r = renderSettings();
    return {{"width", r.width}, {"height", r.height}, {"pixelAspect", r.pixelAspect}, {"deviceAspect", r.deviceAspect()}};
}
} // namespace

const char* filmFitName(FilmFit f)
{
    switch (f)
    {
    case FilmFit::Fill: return "fill";
    case FilmFit::Horizontal: return "horizontal";
    case FilmFit::Vertical: return "vertical";
    case FilmFit::Overscan: return "overscan";
    }
    return "fill";
}

std::optional<FilmFit> parseFilmFit(const std::string& s)
{
    for (FilmFit f : {FilmFit::Fill, FilmFit::Horizontal, FilmFit::Vertical, FilmFit::Overscan})
        if (lower(s) == filmFitName(f))
            return f;
    return std::nullopt;
}

json CameraSettings::toJson() const
{
    return {{"filmFit", filmFitName(filmFit)}, {"overscan", overscan},     {"resolutionGate", resolutionGate},
            {"filmGate", filmGate},             {"gateMask", gateMask},     {"gateMaskOpacity", gateMaskOpacity}};
}

void CameraSettings::fromJson(const json& j)
{
    if (!j.is_object())
        return;
    if (j.contains("filmFit") && j["filmFit"].is_string())
        filmFit = parseFilmFit(j["filmFit"].get<std::string>()).value_or(filmFit);
    overscan = std::clamp(j.value("overscan", overscan), 0.01, 100.0);
    resolutionGate = j.value("resolutionGate", resolutionGate);
    filmGate = j.value("filmGate", filmGate);
    gateMask = j.value("gateMask", gateMask);
    gateMaskOpacity = std::clamp(j.value("gateMaskOpacity", gateMaskOpacity), 0.0, 1.0);
}

bool isViewName(const std::string& name)
{
    for (const char* v : {"persp", "top", "bottom", "front", "back", "left", "right"})
        if (name == v)
            return true;
    return false;
}

CameraSettings RenderSettings::camera(const std::string& name) const
{
    if (!isViewName(name))
        if (const Node* n = Graph::current().find(name); n && n->type().isA("camera"))
            return camera::settings(*n);
    auto it = cameras.find(name);
    return it == cameras.end() ? CameraSettings{} : it->second;
}

json RenderSettings::toJson() const
{
    json cams = json::object();
    for (const auto& [name, c] : cameras)
        cams[name] = c.toJson();
    return {{"width", width}, {"height", height}, {"pixelAspect", pixelAspect}, {"cameras", std::move(cams)}};
}

void RenderSettings::fromJson(const json& j)
{
    *this = RenderSettings{};
    if (!j.is_object())
        return;
    width = std::clamp(j.value("width", width), 1, 65536);
    height = std::clamp(j.value("height", height), 1, 65536);
    pixelAspect = std::clamp(j.value("pixelAspect", pixelAspect), 0.01, 100.0);
    if (j.contains("cameras") && j["cameras"].is_object())
        for (auto& [name, c] : j["cameras"].items())
            cameras[name].fromJson(c);
}

RenderSettings& renderSettings()
{
    return state().settings;
}

void renderSettingsChanged()
{
    // Copied so a listener can add or remove listeners.
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}

int addRenderSettingsListener(std::function<void()> fn)
{
    State& s = state();
    s.listeners.emplace(s.nextListener, std::move(fn));
    return s.nextListener++;
}

void removeRenderSettingsListener(int id)
{
    state().listeners.erase(id);
}

const std::vector<ResolutionPreset>& resolutionPresets()
{
    static const std::vector<ResolutionPreset> presets = {
        {"HD 1080", 1920, 1080},      {"QHD 1440", 2560, 1440}, {"UHD 4K", 3840, 2160},
        {"Square 1080", 1080, 1080},  {"Portrait 1080x1920", 1080, 1920},
    };
    return presets;
}

glm::dvec2 fitGate(glm::dvec2 film, double gateAspect, FilmFit fit)
{
    gateAspect = std::max(gateAspect, 1e-6);
    double filmAspect = film.x / std::max(film.y, 1e-12);
    bool matchWidth = fit == FilmFit::Horizontal || (fit == FilmFit::Fill && gateAspect >= filmAspect) ||
                      (fit == FilmFit::Overscan && gateAspect < filmAspect);
    return matchWidth ? glm::dvec2(film.x, film.x / gateAspect) : glm::dvec2(film.y * gateAspect, film.y);
}

glm::dvec2 fitView(glm::dvec2 film, double viewAspect, FilmFit fit, double overscan, std::optional<double> gateAspect)
{
    viewAspect = std::max(viewAspect, 1e-6);
    if (!gateAspect)
        return fitGate(film, viewAspect, fit) * overscan;
    glm::dvec2 gate = fitGate(film, *gateAspect, fit);
    glm::dvec2 view = viewAspect >= gate.x / gate.y ? glm::dvec2(gate.y * viewAspect, gate.y) : glm::dvec2(gate.x, gate.x / viewAspect);
    return view * overscan;
}

void registerRenderSettingsCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "setRenderResolution",
        .doc = "Query or set the scene's render resolution. preset is one of 'HD 1080', 'QHD 1440', 'UHD 4K', "
               "'Square 1080', 'Portrait 1080x1920'. deviceAspect sets the pixel aspect so the image displays at that "
               "width over height. Returns {width, height, pixelAspect, deviceAspect}.",
        .flags = {
            {"width", "w", T::Int, nullptr, "Pixels."},
            {"height", "h", T::Int, nullptr, "Pixels."},
            {"preset", "p", T::String, nullptr, "A named resolution."},
            {"pixelAspect", "pa", T::Float, nullptr, "Pixel width over height."},
            {"deviceAspect", "da", T::Float, nullptr, "Displayed image width over height."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            RenderSettings& r = renderSettings();
            if (a.get<bool>("query"))
                return resolutionJson();
            int w = r.width, h = r.height;
            if (a.has("preset"))
            {
                std::string name = lower(a.get<std::string>("preset"));
                auto& ps = resolutionPresets();
                auto it = std::find_if(ps.begin(), ps.end(), [&](const ResolutionPreset& p) { return lower(p.name) == name; });
                if (it == ps.end())
                    throw CommandError("setRenderResolution: unknown preset '" + a.get<std::string>("preset") + "'");
                w = it->width;
                h = it->height;
            }
            w = a.get<int>("width", w);
            h = a.get<int>("height", h);
            if (w < 1 || h < 1 || w > 65536 || h > 65536)
                throw CommandError("setRenderResolution: width and height must be 1 to 65536");
            double pa = a.get<double>("pixelAspect", r.pixelAspect);
            if (a.has("deviceAspect"))
                pa = a.get<double>("deviceAspect") / (double(w) / h);
            if (!(pa > 0.0))
                throw CommandError("setRenderResolution: aspect must be positive");
            r.width = w;
            r.height = h;
            r.pixelAspect = std::clamp(pa, 0.01, 100.0);
            renderSettingsChanged();
            return resolutionJson();
        },
    });

    cmds::registerCommand({
        .name = "cameraSettings",
        .doc = "Query or set a camera's gate display and film fit. The camera is a view name ('persp', 'top', 'front', "
               "...; default 'persp') or a camera node, which keeps them as its own attributes. filmFit is 'fill', 'horizontal', 'vertical' or 'overscan'. Returns the settings.",
        .positionalDoc = "camera",
        .flags = {
            {"filmFit", "ff", T::String, nullptr, "How the resolution gate fits the film back."},
            {"overscan", "os", T::Float, nullptr, "View size relative to the gate; 1.0 fits it exactly."},
            {"gate", "g", T::Bool, nullptr, "Display the resolution gate."},
            {"filmGate", "fg", T::Bool, nullptr, "Display the film gate."},
            {"gateMask", "gm", T::Bool, nullptr, "Darken outside the gate."},
            {"maskOpacity", "mo", T::Float, nullptr, "Gate mask opacity, 0 to 1."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = a.positional.empty() ? "persp" : a.positional[0].get<std::string>();
            if (name.empty())
                throw CommandError("cameraSettings: camera name is empty");
            RenderSettings& r = renderSettings();
            if (a.get<bool>("query"))
                return r.camera(name).toJson();
            CameraSettings c = r.camera(name);
            if (a.has("filmFit"))
            {
                auto f = parseFilmFit(a.get<std::string>("filmFit"));
                if (!f)
                    throw CommandError("cameraSettings: filmFit is fill, horizontal, vertical or overscan");
                c.filmFit = *f;
            }
            if (a.has("overscan"))
            {
                double o = a.get<double>("overscan");
                if (!(o > 0.0))
                    throw CommandError("cameraSettings: overscan must be positive");
                c.overscan = std::clamp(o, 0.01, 100.0);
            }
            c.resolutionGate = a.get<bool>("gate", c.resolutionGate);
            c.filmGate = a.get<bool>("filmGate", c.filmGate);
            c.gateMask = a.get<bool>("gateMask", c.gateMask);
            if (a.has("maskOpacity"))
                c.gateMaskOpacity = std::clamp(a.get<double>("maskOpacity"), 0.0, 1.0);
            if (Node* n = isViewName(name) ? nullptr : Graph::current().find(name); n && n->type().isA("camera"))
            {
                // Gate display is view state, so it stays off the undo stack like the views' own settings.
                UndoStack& u = UndoStack::get();
                bool was = u.enabled();
                u.setEnabled(false);
                camera::setSettings(Graph::current(), n->id(), c);
                u.setEnabled(was);
            }
            else
                r.editCamera(name) = c;
            renderSettingsChanged();
            return c.toJson();
        },
    });
}
} // namespace lila::scene
