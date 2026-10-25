#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/SoftSelect.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
class MeshBvh;
} // namespace lila::geo

namespace lila::scene
{
class Graph;

/**
 * Fields (Cinema 4D's Fields, Maya's falloff nodes): a `field` DAG node holds a stack of layers that together give every
 * point a value from 0 to 1. Each layer computes a raw value, remaps it (input range, then an optional curve), inverts
 * it if asked, and blends it into the value so far by its blend mode and opacity, bottom layer first, starting from 0.
 * The result is clamped to 0..1. Shape and noise layers sit in the field's space, so moving, keying or constraining the
 * field moves them.
 *
 * A deformer reads a field through its `field` input (connectField); soft selection reads one through the
 * /softSelect/field pref (softSelectField).
 */
namespace fields
{
enum class Kind : uint8_t
{
    Shape,      // a falloff shape in the field's space
    Components, // 1 on listed points, 0 elsewhere
    Painted,    // a painted value per point
    Proximity,  // nearness to the field's proximityMesh
    UvRegion,   // a rectangle in UV space
    Noise,      // seeded fractal noise in the field's space
    Attribute,  // a vertex colour set or point attribute channel
};

enum class Shape : uint8_t
{
    Sphere,  // radius 1
    Box,     // half size 1
    Capsule, // radius 1 around a segment along Y of `length`
    Plane,   // a slab: 1 on the XZ plane, 0 at |y| = 1
    Linear,  // a ramp along Y: 0 at y = -1, 1 at y = 1
    Radial,  // the angle around Y: 0 at +X, rising counter-clockwise to 1 a full turn later
    Cone,    // apex at the origin, opening along +Y with half angle `angle`, `length` long
};

enum class Blend : uint8_t
{
    Add,
    Multiply,
    Max,
    Min,
    Subtract,
    Replace,
};

LILA_SCENE_API const char* kindName(Kind k);
LILA_SCENE_API const char* shapeName(Shape s);
LILA_SCENE_API const char* blendName(Blend b);
/** Throw std::invalid_argument for an unknown name. */
LILA_SCENE_API Kind kindFromName(std::string_view name);
LILA_SCENE_API Shape shapeFromName(std::string_view name);
LILA_SCENE_API Blend blendFromName(std::string_view name);

struct LILA_SCENE_API Layer
{
    Kind kind = Kind::Shape;
    std::string name;
    bool enabled = true;
    Blend blend = Blend::Max;
    double opacity = 1.0;
    bool invert = false;
    /** Remap: the raw value's [inMin, inMax] stretches to 0..1 (clamped), then the curve (value in, value out) if any. */
    double inMin = 0.0, inMax = 1.0;
    std::optional<softSelect::Curve> curve;

    /** Shape and noise placement in the field's space. */
    glm::dvec3 translate{0.0}, rotate{0.0}, scale{1.0};
    Shape shape = Shape::Sphere;
    /** Shapes, proximity and UV regions: the fraction of the extent that eases out, 0 a hard edge to 1 all soft. */
    double softness = 0.5;
    double length = 2.0; // capsule segment, cone height
    double angle = 45.0; // cone half angle, degrees

    std::vector<uint32_t> points; // components
    std::vector<float> values;    // painted, by point id; points past its end read 0

    double distance = 10.0; // proximity: world cm where the value reaches 0

    int uvSet = 0;
    glm::dvec2 uvMin{0.0}, uvMax{0.5};

    uint32_t seed = 0;
    double frequency = 1.0; // noise cells per field unit
    int octaves = 3;

    /** Attribute: a point attribute or colour set name, empty for the current colour set; channel 0-3, or -1 for luminance. */
    std::string attribute;
    int channel = -1;

    /** Layer space to field space. */
    glm::dmat4 matrix() const;

    json toJson() const;
    /** Unknown keys are ignored and missing ones keep their defaults; throws std::invalid_argument on bad names. */
    static Layer fromJson(const json& j);
    /** Applies just the keys present, as setFieldLayer does. */
    void update(const json& j);
};

/** A field's layer stack, as stored on its `layers` plug. */
struct LILA_SCENE_API Layers final : Data
{
    std::vector<Layer> layers;
    const char* kind() const override { return "fieldLayers"; }
    size_t bytes() const override;
    json toJson() const override;
    static DataPtr fromJson(const json& j);
};

/** What a field node outputs for its readers: the layers with everything they need, in world space. */
struct LILA_SCENE_API Evaluator final : Data
{
    std::shared_ptr<const Layers> layers;
    glm::dmat4 worldInverse{1.0}; // world to field space
    double opacity = 1.0;
    bool invert = false;
    /** The proximity mesh in world space and its BVH; null when nothing is connected. */
    std::shared_ptr<const geo::Mesh> proximityMesh;
    std::shared_ptr<const geo::MeshBvh> proximity;

    const char* kind() const override { return "fieldEvaluator"; }
    json toJson() const override;
};

/** The shape falloff: 1 up to 1 - softness, easing (smoothstep) to 0 at normalised distance 1. */
LILA_SCENE_API double falloff(double distance, double softness);
/** A shape's raw value at a point in its layer space. */
LILA_SCENE_API double shapeValue(const Layer& l, const glm::dvec3& p);
/** Seeded fractal value noise in 0..1. */
LILA_SCENE_API double noise(const glm::dvec3& p, uint32_t seed, int octaves);
/** Remap, then invert. */
LILA_SCENE_API double remap(const Layer& l, double raw);
/** Blends a layer's value into the value so far. */
LILA_SCENE_API double blend(Blend mode, double base, double value, double opacity);

/**
 * Values for points given in world space. mesh (same point ids) feeds the per-point layers (components, painted, UV,
 * attribute); without it those layers read 0.
 */
LILA_SCENE_API std::vector<float> evaluate(const Evaluator& e, std::span<const glm::dvec3> world, const geo::Mesh* mesh);
/** Values for a mesh's points, placed in the world by meshWorld. */
LILA_SCENE_API std::vector<float> evaluate(const Evaluator& e, const geo::Mesh& mesh, const glm::dmat4& meshWorld);
/** The field node's current evaluator (evaluates the graph); null when the node is not a field. */
LILA_SCENE_API std::shared_ptr<const Evaluator> evaluator(Graph& g, NodeId field);

LILA_SCENE_API bool isField(const Node& n);
/** Field attrs, after the transform's. */
enum Attr
{
    kLayers = Transform::kCount,
    kOpacity,
    kInvert,
    kProximityMesh,
    kProximityMatrix,
    kOutField,
};

/** The field's layers, a copy; and replace them (undoable). */
LILA_SCENE_API std::vector<Layer> layers(Graph& g, NodeId field);
LILA_SCENE_API void setLayers(Graph& g, NodeId field, std::vector<Layer> layers);

/** Soft selection weights scaled by the field named in /softSelect/field, when soft select is on and the field exists. */
LILA_SCENE_API void modulateSoftSelection(const geo::Mesh& mesh, const glm::dmat4& world, std::vector<float>& weights);
inline constexpr const char* kSoftSelectFieldPref = "/softSelect/field";

LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace fields
} // namespace lila::scene
