#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * The auto-poser: a small network that, from the current pose and the points pinned where the animator wants them,
 * predicts where the rest of a humanoid's body goes. Full-body IK then poses the skeleton toward the prediction and
 * meets the pins exactly. Trained offline on public mocap (tools/autopose); only the weights ship.
 */
namespace lila::scene::autopose
{
inline constexpr int kPointCount = 19;

/** The points it reasons about, named after the UE5 Manny joints they sit on. */
inline constexpr std::array<const char*, kPointCount> kPoints = {
    "pelvis",     "spine_02",   "spine_04", "neck_01", "head",       "upperarm_l", "lowerarm_l",
    "hand_l",     "thigh_l",    "calf_l",   "foot_l",  "ball_l",     "upperarm_r", "lowerarm_r",
    "hand_r",     "thigh_r",    "calf_r",   "foot_r",  "ball_r",
};

using Points = std::array<glm::dvec3, kPointCount>;
using Pins = std::array<std::optional<glm::dvec3>, kPointCount>;

class LILA_SCENE_API Model
{
public:
    /** Reads a .lilanet file; null, with the reason in error, when it is missing or malformed. */
    static std::unique_ptr<Model> load(const std::filesystem::path& file, std::string* error = nullptr);

    /** World positions of every point. current is the pose now, pins the world targets of the points being held or moved. */
    Points predict(const Points& current, const Pins& pins) const;

private:
    struct Layer
    {
        int rows = 0, cols = 0;
        bool silu = false;
        std::vector<float> weights, bias;
    };
    std::vector<Layer> mLayers;
};

/** The model that ships in resources/models/autopose.lilanet, read once; null when it is not there. */
LILA_SCENE_API const Model* shipped();
} // namespace lila::scene::autopose
