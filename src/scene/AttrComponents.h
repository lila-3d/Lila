#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Plug.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
class Graph;
class Node;

/**
 * Attribute components: named groups of added attributes that show under one heading in the Attribute Editor (the
 * attrs' category). Sources list the components Add offers: USD API schemas, extensions, and the user's and studio's
 * saved ones (.lilacomponent files).
 */
namespace attrComponents
{
/** How a component reaches Unreal through LilaLink, chosen per component in its export options. */
enum class UnrealMode : uint8_t
{
    None,
    ActorTags,      // "name.attr=value" actor tags, or the bare values with tags "value"
    AssetUserData,  // a ULilaComponentUserData holding the values by name
    Component,      // an actor component of unrealClass with matching properties
    DataAsset,      // a data asset of unrealClass, referenced from asset user data
    Custom,         // a registered exporter, unrealExporter
};
LILA_SCENE_API const char* unrealModeName(UnrealMode m);
/** Throws std::invalid_argument. */
LILA_SCENE_API UnrealMode unrealModeNamed(std::string_view name);

struct LILA_SCENE_API Component
{
    /** The heading it shows under, and the category its attrs carry on an object. */
    std::string name;
    /** Where it comes from: "usd", "user", "studio", or the extension that registered it. */
    std::string source;
    std::string doc;
    /** Attr names are without the component; adding one keeps them as they are and sets category to name. */
    std::vector<AttrDef> attrs;
    /** A USD API schema the object applies while it carries the component (added to usdApiSchemas). */
    std::string usdSchema;
    UnrealMode unreal = UnrealMode::AssetUserData;
    /** The UE class for Component and DataAsset; the exporter's name for Custom. */
    std::string unrealClass;
    /**
     * ActorTags: "keyValue" (empty too) tags "name.attr=value"; "value" tags the bare values: a string or number as it
     * is, each element of a list, a true bool's attr name, nothing for false or empty.
     */
    std::string unrealTags;

    json toJson() const;
    /** Throws std::invalid_argument. */
    static Component fromJson(const json& j);
};

/** Lists components; called each time Add opens. */
using Source = std::function<std::vector<Component>()>;
LILA_SCENE_API Registry<Source>& sources();
/** For extensions: owned by ctx and removed when it unloads. */
LILA_SCENE_API bool addSource(ext::Context& ctx, const std::string& id, Source source);

/** Every source's components, the saved ones last; a later one with a name already listed is left out. */
LILA_SCENE_API std::vector<Component> all();
LILA_SCENE_API std::optional<Component> find(std::string_view name);

/** The folder saved components go to: the user's, or the first studio folder. Empty when there is no studio folder. */
LILA_SCENE_API std::filesystem::path folder(bool studio);
/** Writes the component to its folder, replacing one of the same name. Returns the file. Throws std::runtime_error. */
LILA_SCENE_API std::filesystem::path save(const Component& c, bool studio);
/** Deletes a saved component's file; false when none of that name is saved there. */
LILA_SCENE_API bool removeSaved(std::string_view name, bool studio);

/** The node's added attrs under the category as a component, with the export settings stored on the node. */
LILA_SCENE_API Component fromNode(const Node& n, const std::string& category);
/**
 * Adds the component's attrs to the node under its heading (skipping ones already there) and its USD schema to
 * usdApiSchemas, recording undo. Returns the plugs added.
 */
LILA_SCENE_API std::vector<std::string> apply(Graph& g, NodeId node, const Component& c);

/** The object's per-component Unreal export choice, stored in its lilaComponentExport string attr as JSON. */
LILA_SCENE_API json exportSettings(const Node& n);
/** tags is keyValue or value (see Component::unrealTags); empty leaves keyValue. Throws std::invalid_argument for another. */
LILA_SCENE_API void setExportSettings(Graph& g, NodeId node, const std::string& component, UnrealMode mode, const std::string& unrealClass,
                                      const std::string& tags = {});

/**
 * The object's components as LilaLink sends them: [{name, mode, class?, tags?, attrs: {attr: value}}], in the order their
 * first attr appears, uncategorised added attrs as the component "". Components exported as none are left out; hidden
 * attrs and usdApiSchemas are not data. Values are JSON: numbers, bools, strings, [x, y, z], an enum's label, a list
 * for an array attr. Empty when the object has no added attrs.
 */
LILA_SCENE_API json unrealPayload(Graph& g, NodeId node);

/**
 * The component exporters Custom can name, by name, each holding its description: the ones the connected Unreal
 * editor lists (owner "lila.unreal") and those extensions register.
 */
LILA_SCENE_API Registry<std::string>& unrealExporters();
/** For extensions: owned by ctx and removed when it unloads. False when the name is taken. */
LILA_SCENE_API bool addUnrealExporter(ext::Context& ctx, const std::string& name, std::string doc = {});
} // namespace attrComponents

namespace attrCommands
{
/** renameAttr, editAttr, reorderAttr, copyAttr, listConnections and the component library commands. */
LILA_SCENE_API void registerCommands();
} // namespace attrCommands
} // namespace lila::scene
