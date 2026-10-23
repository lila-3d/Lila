#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
using json = nlohmann::json;
/** Meshes flow through the graph as shared immutable snapshots; an edit makes a new one. */
using MeshPtr = std::shared_ptr<const geo::Mesh>;

/**
 * Any other immutable payload a plug carries (skin weights, influence palettes), shared like meshes. A kind registered
 * with data::registerKind can be stored; toJson must write a "kind" key holding that name.
 */
struct Data
{
    virtual ~Data() = default;
    virtual const char* kind() const = 0;
    virtual nlohmann::json toJson() const = 0;
    /** Approximate heap bytes, for the undo memory budget. */
    virtual size_t bytes() const { return 256; }
};
using DataPtr = std::shared_ptr<const Data>;
using NodeId = uint64_t;
constexpr NodeId kNoNode = 0;

enum class PlugType : uint8_t
{
    Bool,
    Int,
    Float,
    Vec3,
    Matrix,
    String,
    Enum, // stored as Int
    Mesh,
    Data,
};

using PlugValue = std::variant<bool, int64_t, double, glm::dvec3, glm::dmat4, std::string, MeshPtr, DataPtr>;

enum AttrFlags : uint32_t
{
    AttrInput = 1 << 0,
    AttrOutput = 1 << 1,
    AttrKeyable = 1 << 2,
    AttrStorable = 1 << 3,
    AttrHidden = 1 << 4,
    AttrFile = 1 << 5, // String holding a file path: editors offer a file browser
    AttrResetOnDisconnect = 1 << 6, // only meaningful while driven: losing the connection puts the default back
};

struct AttrDef
{
    std::string name;
    std::string shortName;
    PlugType type = PlugType::Float;
    PlugValue defaultValue = 0.0;
    uint32_t flags = AttrInput | AttrStorable;
    std::vector<std::string> enumNames;      // Enum only
    std::array<std::string, 3> childNames;   // Vec3: long names, e.g. translateX
    std::array<std::string, 3> childShort;   // Vec3: tx, ty, tz
    std::string doc;
    /**
     * What a scene file means by leaving this attr out, when that differs from defaultValue: files omit values equal
     * to the default, so a default that changes keeps its old value here and older files load unchanged.
     */
    std::optional<PlugValue> fileDefault;
    /** An older name scene files may store this attr under. Only loading reads it. */
    std::string fileAlias;

    /** The value scene files omit and assume when the attr is absent. */
    const PlugValue& storedDefault() const { return fileDefault ? *fileDefault : defaultValue; }
};

/** node.attr, optionally one component of a Vec3 (node.tx). */
struct PlugRef
{
    NodeId node = kNoNode;
    int attr = -1;
    int component = -1;

    bool valid() const { return node != kNoNode && attr >= 0; }
    bool operator==(const PlugRef&) const = default;
    auto operator<=>(const PlugRef&) const = default;
};

LILA_SCENE_API json toJson(const PlugValue& v);
/** Converts j to the attr's type, or throws std::invalid_argument. */
LILA_SCENE_API PlugValue fromJson(const AttrDef& def, const json& j);
LILA_SCENE_API PlugValue defaultFor(PlugType type);
LILA_SCENE_API const char* typeName(PlugType type);

/**
 * toJson of each value, in order; meshes and data convert in parallel. packed writes meshes with meshToJsonPacked, for a
 * document headed straight into a binary scene file.
 */
LILA_SCENE_API std::vector<json> toJsonAll(const std::vector<const PlugValue*>& values, bool packed = false);
struct FromJsonResult
{
    PlugValue value;
    std::string error; // fromJson's message when it threw; value is then unset
};
/** fromJson of each (attr, json), in order; meshes decode in parallel. */
LILA_SCENE_API std::vector<FromJsonResult> fromJsonAll(const std::vector<std::pair<const AttrDef*, const json*>>& items);

namespace data
{
using FromJson = std::function<DataPtr(const json&)>;
/** Every Data kind scene files can load, owner-tagged. */
LILA_SCENE_API Registry<FromJson>& kinds();
/** Lets a Data kind load from scene files. Registering a kind again replaces its loader. */
LILA_SCENE_API void registerKind(const std::string& kind, FromJson fromJson);
/**
 * For extensions: owned by ctx and removed when it unloads. False, with a log message, if the kind is taken. Values
 * already in the scene keep working while the extension is loaded; files holding the kind then fail to load it.
 */
LILA_SCENE_API bool registerKind(ext::Context& ctx, const std::string& kind, FromJson fromJson);
} // namespace data
} // namespace lila::scene
