#pragma once

#include "core/Export.h"

#include "scene/Node.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Character poses: named sets of channel values stored by node name, applied back by name. A pose file is JSON:
 *   { "name", "character", "template", "tags": [...], "thumbnail": base64 PNG,
 *     "nodes": { nodeName: { "translate": [x, y, z], "rotate": [x, y, z, w], "scale": [x, y, z], attr: value } } }
 * Rotations are quaternions (x, y, z, w) of the rotate channel alone, so they apply under any rotate order; a three
 * element "rotate" is read as Euler degrees in the target's rotate order. Node names carry no namespace.
 */
namespace lila::scene::poses
{
using json = nlohmann::json;

/** Where a pose lives. Lookups try them in this order, so a scene pose shadows a workspace one of the same name. */
enum class Tier : uint8_t
{
    Scene,     // saved in the scene file, undoable
    Workspace, // <workspace>/.lila/poses/<character>/<name>.json
    User,      // <user config>/poses/<character>/<name>.json
    Template,  // registered by extensions (rig templates), read only
};
inline constexpr Tier kTiers[] = {Tier::Scene, Tier::Workspace, Tier::User, Tier::Template};
LILA_SCENE_API const char* tierName(Tier t);
LILA_SCENE_API std::optional<Tier> tierFromName(std::string_view name);

struct Pose
{
    std::string name;
    std::string character; // library group: the template id, or the root joint's name without namespace
    std::string templateId;
    std::vector<std::string> tags;
    std::string thumbnail; // PNG bytes; empty for none
    json nodes = json::object();

    LILA_SCENE_API json toJson() const;
    /** Throws std::invalid_argument when j is not a pose. */
    LILA_SCENE_API static Pose fromJson(const json& j);
};

/** bind, ref and test (any case): every character's fixed slots, which can't be renamed and delete only with force. */
LILA_SCENE_API bool isReserved(std::string_view name);
LILA_SCENE_API std::string stripNamespace(std::string_view name);

/** The opposite side's name (arm_l -> arm_r, LeftHand -> RightHand), or name itself for a centre node. */
LILA_SCENE_API std::string mirrorNodeName(const std::string& name);

/** A character found from a node in it: the top joint of its skeleton, or the top of its hierarchy without one. */
struct Character
{
    std::string id;
    std::string templateId;
    NodeId root = kNoNode;
    std::string nameSpace;
    /** Root plus every transform under it, shapes excluded. */
    std::vector<NodeId> nodes;
};
LILA_SCENE_API Character characterOf(Graph& g, NodeId node);

/** Lets an extension (rig templates) name characters itself; return nullopt to fall back to the root joint. */
using CharacterResolver = std::function<std::optional<Character>(Graph& g, NodeId node)>;
LILA_SCENE_API void setCharacterResolver(CharacterResolver fn);

/** Channel names a filter understands: translate/t, rotate/r, scale/s, or any attr name. Empty takes everything. */
LILA_SCENE_API bool channelWanted(const std::vector<std::string>& channels, const std::string& attr);

/** The current values of nodes' translate, rotate, scale and other keyable scalar channels. */
LILA_SCENE_API Pose capture(Graph& g, const std::vector<NodeId>& nodes, const std::vector<std::string>& channels = {});

struct ApplyOptions
{
    double weight = 1.0; // 0 keeps the current pose, 1 is the stored one
    bool mirror = false;
    char mirrorAxis = 'x'; // the axis the mirror plane faces
    std::vector<std::string> channels;
    /** Extra names to try per pose node name: { "spine_01": ["Spine1", "spine1"] }. */
    json aliases = json::object();
    /** Nodes to match against; empty searches the whole scene. */
    std::vector<NodeId> scope;
    /** Preferred namespace when several matches differ only by it. */
    std::string nameSpace;
};

struct ApplyReport
{
    int nodes = 0;    // nodes changed
    int channels = 0; // channels set
    std::vector<std::string> missing; // pose names with no match
    std::vector<std::string> locked;  // plugs left alone because they are locked or driven by something not a curve

    LILA_SCENE_API json toJson() const;
};

/** Sets the matched channels through Graph::setValue (undoable; animated channels go through the driven-set hook). */
LILA_SCENE_API ApplyReport apply(Graph& g, const Pose& pose, const ApplyOptions& opts = {});

/**
 * The pose for the other side: sided names swap (joints keep their rotation and negate translate, the way mirrored
 * joints behave), centre and non-joint nodes reflect across the mirror plane. g and scope tell joints apart by name.
 */
LILA_SCENE_API Pose mirrored(Graph& g, const Pose& pose, const std::vector<NodeId>& scope = {}, char axis = 'x');

// Library

struct Entry
{
    Pose pose;
    Tier tier = Tier::Scene;
    std::filesystem::path file; // file tiers only
};

/** <workspace>/.lila/poses, empty with no workspace open; <user config>/poses. */
LILA_SCENE_API std::filesystem::path tierDir(Tier t, bool create = false);

/**
 * Poses for a character (every character when empty), one per name: the first tier that has it, scene first. all
 * keeps the shadowed copies too. Reserved names sort first, then by name.
 */
LILA_SCENE_API std::vector<Entry> list(const std::string& character = {}, std::optional<Tier> tier = {}, bool all = false);
LILA_SCENE_API std::optional<Entry> find(const std::string& character, const std::string& name, std::optional<Tier> tier = {});
/** Every character any tier has poses for, sorted. */
LILA_SCENE_API std::vector<std::string> characters();

/** Adds or replaces. Throws std::runtime_error for the template tier or a file that can't be written. */
LILA_SCENE_API void save(const Pose& pose, Tier tier);
/** False when that tier has no such pose. */
LILA_SCENE_API bool remove(const std::string& character, const std::string& name, Tier tier);
/** Throws std::runtime_error when the source is missing or the target name is taken in that tier. */
LILA_SCENE_API void rename(const std::string& character, const std::string& from, const std::string& to, Tier tier);

/** Template tier: owner is the extension id, so unregisterOwned drops them when it unloads. */
LILA_SCENE_API void registerTemplatePose(const Pose& pose, const std::string& owner = {});
/** Name aliases for a character's poses, merged under ApplyOptions::aliases. */
LILA_SCENE_API void registerAliases(const std::string& character, const json& aliases, const std::string& owner = {});
LILA_SCENE_API void unregisterOwned(const std::string& owner);

/** Scene tier: saved with the scene as "poses": [pose, ...]. fromJson and clear skip undo. */
LILA_SCENE_API json sceneToJson();
LILA_SCENE_API void sceneFromJson(const json& j);
LILA_SCENE_API void clearScene();

/** Called after any library change (any tier), undo and redo included. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

/** PNG of the given nodes for savePose's thumbnail; the UI installs one that renders the active view. */
LILA_SCENE_API void setThumbnailProvider(std::function<std::string(const std::vector<NodeId>&)> fn);

LILA_SCENE_API std::string base64Encode(std::string_view bytes);
LILA_SCENE_API std::string base64Decode(std::string_view text);

/** savePose, applyPose, mirrorPose, listPoses, deletePose, renamePose. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::poses
