#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
using json = nlohmann::json;

/** How the resolution gate sits against the film back, as Maya's camera Film Fit. */
enum class FilmFit
{
    Fill,       // the gate fits inside the film gate
    Horizontal, // gate width matches the film width: horizontal FOV is kept
    Vertical,   // gate height matches the film height: vertical FOV is kept
    Overscan,   // the film gate fits inside the gate
};

const char* filmFitName(FilmFit f);
/** Accepts the names above in any case; nullopt when unknown. */
std::optional<FilmFit> parseFilmFit(const std::string& s);

/** True for the built-in views: persp, top, bottom, front, back, left, right. */
bool isViewName(const std::string& name);

/** Per-camera view settings. The built-in views keep theirs here by name; camera nodes as attributes. */
struct CameraSettings
{
    FilmFit filmFit = FilmFit::Fill;
    double overscan = 1.0; // > 1 shows more than the gate around it
    bool resolutionGate = false;
    bool filmGate = false;
    bool gateMask = true;
    double gateMaskOpacity = 0.7;

    json toJson() const;
    void fromJson(const json& j);
};

/** Scene render resolution plus the cameras' gate settings. Saved with the scene as "renderSettings". */
struct RenderSettings
{
    int width = 1920;
    int height = 1080;
    double pixelAspect = 1.0;
    std::map<std::string, CameraSettings> cameras;

    /** Width over height of the rendered image as displayed, pixel aspect included. */
    double deviceAspect() const { return double(width) / double(std::max(height, 1)) * pixelAspect; }
    /**
     * Settings for a camera: a camera node's own attributes when the scene has one by that name, else the entry for
     * the view name (persp, top, ...), defaults when it has none yet.
     */
    CameraSettings camera(const std::string& name) const;
    /** Built-in views only; camera nodes keep theirs as attributes (camera::setSettings). */
    CameraSettings& editCamera(const std::string& name) { return cameras[name]; }

    json toJson() const;
    /** Resets to defaults first, so a scene without settings loads as a fresh one. */
    void fromJson(const json& j);
};

RenderSettings& renderSettings();
/** Notifies listeners after render or camera settings change. */
void renderSettingsChanged();
int addRenderSettingsListener(std::function<void()> fn);
void removeRenderSettingsListener(int id);

struct ResolutionPreset
{
    const char* name;
    int width, height;
};
const std::vector<ResolutionPreset>& resolutionPresets();

/**
 * Half extents of the resolution gate, in the film's units (tangents for a perspective camera, world units for an
 * orthographic one), given the film gate's half extents and the gate's width over height.
 */
glm::dvec2 fitGate(glm::dvec2 film, double gateAspect, FilmFit fit);

/**
 * Half extents of what a viewport of `viewAspect` shows. With a gate aspect the whole gate fits inside the viewport,
 * so the gate's contents never depend on the panel's shape. Without one the viewport itself is the gate, as in Maya
 * with gates hidden. Both scale by overscan.
 */
glm::dvec2 fitView(glm::dvec2 film, double viewAspect, FilmFit fit, double overscan, std::optional<double> gateAspect);

/** Commands: setRenderResolution, cameraSettings. */
void registerRenderSettingsCommands();
} // namespace lila::scene
