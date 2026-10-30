#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lila::render
{
struct FrameState;
}

namespace lila::ui::renderOutput
{
/** One rendered picture's settings. Sizes of 0 take the scene's render resolution. */
struct Options
{
    std::string camera; // a camera node; empty uses the active view's camera
    int width = 0, height = 0;
    double scale = 1.0; // of the resolution, as a screen percentage over 100
    int samples = 64;   // jittered frames averaged per picture: antialiasing, depth of field and motion blur converge with more
    bool motionBlur = false;
    int motionSamples = 8; // moments across the shutter, out of samples
    bool transparent = false;
    bool hdr = false; // keep the scene's linear light too, for EXR
    /** Instead of the camera: an orbit around the subject (a turntable), yaw and pitch in degrees, framed to fit. */
    bool orbit = false;
    double orbitYaw = 0.0, orbitPitch = -15.0;
    /** Degrees added to the environment's rotation (a turntable's light spin). */
    double environmentSpin = 0.0;
    /** "beauty", "wireframe" (edges over the beauty), or "id": each object a flat colour from its path, exact, for compositing mattes. */
    std::string pass = "beauty";
};

/** The colour an object gets in the id pass: a hash of its path, saturated and mid-bright so neighbours stay apart. */
LILA_UI_API glm::vec3 idColor(const std::string& path);

struct Frame
{
    int width = 0, height = 0;
    std::vector<uint8_t> bgra; // display-encoded, top row first
    std::vector<float> hdr;    // RGBA linear light, when asked for
};

/** The scene at `seconds` through the camera, as Options says. Throws std::runtime_error. */
LILA_UI_API Frame render(const Options& o, double seconds);

/** Writes a frame by the path's extension: png, jpg, tga, tif, exr (linear light) or psd. Throws std::runtime_error. */
LILA_UI_API void write(const Frame& f, const std::string& path, bool transparent);

/** Where renders go when nothing says: renders/<scene>.####.png beside the scene, or a Lila Renders folder at home. */
LILA_UI_API std::string defaultPattern();
/** pattern with its run of # replaced by the frame number, zero padded to the run's length; added before the extension if missing. */
LILA_UI_API std::string framePath(const std::string& pattern, int frame);

/** Commands: render, renderSequence. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::renderOutput
