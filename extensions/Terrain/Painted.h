#pragma once

#include "Inputs.h"
#include "Terrain.h"

#include <lila/ImageIO.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
/** What a drawn constraint holds while erosion runs. */
enum class ConstraintKind : uint8_t
{
    /** A crest raised by height, its line held so erosion never cuts through it. */
    Ridge,
    /** A trough lowered by height, its floor held. */
    Valley,
    /** A step of height: the left side (along the curve) up, the right side down, kept sharp. */
    Cliff,
    /** A channel height deep whose bed only falls along the curve, so water follows it. */
    River,
    /** Level ground inside a closed curve at the mean height along it. */
    Flat,
};

const std::vector<std::string>& constraintKindNames();
bool parseConstraintKind(std::string_view name, ConstraintKind* out);

/** One drawn constraint, in the heightfield's own space (cm). */
struct Constraint
{
    std::string id;
    ConstraintKind kind = ConstraintKind::Ridge;
    /** A path of [x, z] points, when it follows no curve. */
    std::vector<glm::dvec2> path;
    /** The Object Info node of the curve it follows. */
    std::string object;
    /** Rise of a ridge, depth of a valley or river, drop of a cliff, cm. */
    double height = 5000.0;
    /** Full width across, cm. */
    double width = 6000.0;
    /** 0 to 1: how firmly erosion is held to it. */
    double strength = 1.0;

    nlohmann::json toJson() const;
    static Constraint fromJson(const nlohmann::json& j);
};

struct ConstraintStats
{
    /** Samples it shaped, and those held through erosion. */
    size_t touched = 0;
    size_t held = 0;
    double lowest = 0.0;
    double highest = 0.0;
};

/**
 * Shapes the heights along the lines (ridge, valley, cliff, river; flat takes closed lines) and writes masks constraint
 * (weight) and constraintHeight (the height held), which erode() keeps, plus constraintRidge, constraintValley,
 * constraintCliff, constraintRiver or constraintFlat (0 to 1 coverage).
 */
ConstraintStats applyConstraint(Heightfield& h, const Constraint& c, const std::vector<std::vector<glm::dvec2>>& lines);

/** The colour classes a sketch is read as. */
struct SketchStats
{
    size_t ridge = 0;
    size_t valley = 0;
    size_t river = 0;
    size_t flat = 0;
    size_t grey = 0;
    /** The brightness level size removed from the grey base (1/255 for 8-bit). */
    double levels = 0.0;
};

/**
 * Sketch mode: low-saturation pixels are a grey base height (black low, white high); red strokes are ridges, green
 * valleys, blue rivers and yellow flat ground. The base is denoised, the strokes shape it and are held as constraints
 * while it erodes, so the drawn lines survive. The image spans width along X, centred on center, its top row the -Z edge.
 */
Heightfield sketchTerrain(const lila::image::Image& img, glm::dvec2 center, double width, const ImageOptions& o, SketchStats* stats = nullptr);

/** Box-filter approximation of a Gaussian (three passes), any sigma at the same cost; edges clamp. */
void gaussianApprox(std::vector<float>& v, int w, int h, double sigma);
} // namespace terrain
