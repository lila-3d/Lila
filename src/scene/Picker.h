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

namespace lila::ext
{
class Context;
}

namespace lila::scene
{
class Graph;
}

/**
 * Control picker documents: pages of clickable shapes that select, toggle, pose or run commands on a character's nodes.
 * A document is JSON (a .lilapicker file holds exactly one):
 *   { "name", "character", "template", "namespace",
 *     "pages": [ { "name", "background": path or data URI, "backgroundOpacity", "width", "height",
 *                  "items": [ { "id", "shape": rect|circle|polygon|text|image, "x", "y", "w", "h",
 *                               "points": [[x, y], ...] (polygon, relative to x, y), "color": "#rrggbb",
 *                               "textColor", "label", "image", "targets": [node, ...],
 *                               "action": { "kind": select|toggleAttr|applyPose|runCommand, ... } } ] } ] }
 * Coordinates are page units with y down. Targets are node names without a namespace, so one document drives every
 * character that carries those names under its own namespace.
 */
namespace lila::scene::picker
{
using json = nlohmann::json;

enum class Shape : uint8_t
{
    Rect,
    Circle,
    Polygon,
    Text,
    Image,
};
LILA_SCENE_API const char* shapeName(Shape s);
LILA_SCENE_API std::optional<Shape> shapeFromName(std::string_view name);

enum class ActionKind : uint8_t
{
    Select,     // select the targets
    ToggleAttr, // flip attr on each target between values[0] and values[1]
    ApplyPose,  // apply a library pose to the targets' character
    RunCommand, // run command with call; call.args defaults to the resolved targets
};
LILA_SCENE_API const char* actionName(ActionKind k);
LILA_SCENE_API std::optional<ActionKind> actionFromName(std::string_view name);

struct Action
{
    ActionKind kind = ActionKind::Select;
    std::string attr;                        // ToggleAttr
    json values = json::array({0.0, 1.0});   // ToggleAttr: off, on
    std::string pose;                        // ApplyPose
    double weight = 1.0;                     // ApplyPose
    bool mirror = false;                     // ApplyPose
    std::string command;                     // RunCommand
    json call = json::object();              // RunCommand

    LILA_SCENE_API json toJson() const;
    LILA_SCENE_API static Action fromJson(const json& j);
};

struct Point
{
    double x = 0.0, y = 0.0;
};

struct Item
{
    std::string id;
    Shape shape = Shape::Rect;
    double x = 0.0, y = 0.0, w = 24.0, h = 24.0;
    std::vector<Point> points;
    std::string color = "#808080";
    std::string textColor;
    std::string label;
    std::string image;
    std::vector<std::string> targets;
    Action action;

    LILA_SCENE_API json toJson() const;
    LILA_SCENE_API static Item fromJson(const json& j);
    LILA_SCENE_API bool contains(double px, double py) const;
};

struct Page
{
    std::string name = "Main";
    std::string background;
    double backgroundOpacity = 1.0;
    double width = 400.0, height = 600.0;
    std::vector<Item> items;

    LILA_SCENE_API json toJson() const;
    LILA_SCENE_API static Page fromJson(const json& j);
    LILA_SCENE_API Item* item(const std::string& id);
    LILA_SCENE_API const Item* item(const std::string& id) const;
    /** Topmost item under the point (later items draw on top), or null. */
    LILA_SCENE_API const Item* itemAt(double x, double y) const;
    /** Items whose bounds intersect the rectangle. */
    LILA_SCENE_API std::vector<std::string> itemsIn(double x0, double y0, double x1, double y1) const;
};

struct Document
{
    std::string name;
    std::string character;
    std::string templateId;
    std::string nameSpace; // the default one; the panel picks per view
    std::vector<Page> pages;

    LILA_SCENE_API json toJson() const;
    /** Throws std::invalid_argument when j is not a picker document. */
    LILA_SCENE_API static Document fromJson(const json& j);
    LILA_SCENE_API Page* page(const std::string& name);
    LILA_SCENE_API const Page* page(const std::string& name) const;
    /** The item and the page holding it. */
    LILA_SCENE_API Item* item(const std::string& id, Page** pageOut = nullptr);
    LILA_SCENE_API const Item* item(const std::string& id) const;
    /** "item1", "item2", ... not used on any page. */
    LILA_SCENE_API std::string uniqueItemId(std::string_view stem = "item") const;
};

// Namespaces and targets

/** "a:b:hand_l" -> "a:b"; "" without one. */
LILA_SCENE_API std::string namespaceOf(std::string_view name);
/**
 * The node a target names under nameSpace: "ns:target" when ns is given, else the target as written, else the only
 * node in the scene whose name without namespace matches. kNoNode when none (or several, with no namespace) match.
 */
LILA_SCENE_API NodeId resolve(Graph& g, const std::string& target, const std::string& nameSpace);
LILA_SCENE_API std::vector<NodeId> resolveTargets(Graph& g, const Item& item, const std::string& nameSpace);
/** Namespaces in the scene holding at least one of the document's targets, sorted; "" stands for the root namespace. */
LILA_SCENE_API std::vector<std::string> namespacesFor(Graph& g, const Document& doc);

// Selection sync

enum class Highlight : uint8_t
{
    None,
    Partial, // some resolved targets selected
    Full,    // every resolved target selected
};
/** How selected each item looks given the scene selection. Items with no resolved targets are None. */
LILA_SCENE_API Highlight highlightOf(Graph& g, const Item& item, const std::string& nameSpace, const std::vector<NodeId>& selection);
/** The selection a click on items makes: their resolved targets, in item order, without repeats. */
LILA_SCENE_API std::vector<NodeId> selectionFor(Graph& g, const Page& page, const std::vector<std::string>& itemIds,
                                                const std::string& nameSpace);

// Editing helpers (pure: the commands wrap them)

/**
 * The opposite-side copy of an item: targets and label swap sides (hand_l -> hand_r), the shape reflects across the
 * vertical line x = axis, a pose action flips its mirror flag. The copy keeps the source's id; callers rename it.
 */
LILA_SCENE_API Item mirrored(const Item& item, double axis);
/** "left", "right", "top", "bottom", "hcenter" (centres on one vertical line), "vcenter". Throws on another mode. */
LILA_SCENE_API void align(std::vector<Item*>& items, std::string_view mode);
/** "horizontal" or "vertical": even gaps between the first and last item along that axis. */
LILA_SCENE_API void distribute(std::vector<Item*>& items, std::string_view axis);
LILA_SCENE_API double snap(double v, double grid);
/** "#rrggbb" for a sided name: blue for left, red for right, yellow for centre. */
LILA_SCENE_API std::string sideColor(const std::string& nodeName);

/**
 * A picker from a compact layout, the form rig templates carry:
 *   { "name", "character", "template", "grid": 1,
 *     "pages": [ { "name", "background", "width", "height",
 *                  "items": [ { "target" | "targets", "x", "y", "size": n | [w, h], "shape", "color", "label",
 *                               "action", "mirror": true } ] } ] }
 * or a flat { "items": [...] } for a single page. x, y are item centres; positions and sizes are multiplied by grid.
 * Items without a colour get sideColor of their first target; "mirror" also adds the opposite side's item reflected
 * across the page centre.
 */
LILA_SCENE_API Document generateFromLayout(const json& layout);

// Library: scene documents (undoable, saved with the scene) and template documents (registered by extensions)

LILA_SCENE_API std::vector<Document> scenePickers();
LILA_SCENE_API const Document* findScene(const std::string& name);
/** The scene document of that name, else the template one. */
LILA_SCENE_API std::optional<Document> find(const std::string& name);
/** Adds or replaces by name. Undoable. */
LILA_SCENE_API void store(const Document& doc);
/** Undoable. False when there is no such scene document. */
LILA_SCENE_API bool removeScene(const std::string& name);

/** Owner is the extension id, so unregisterOwned drops them when it unloads. Replaces one of the same name. */
LILA_SCENE_API void registerTemplatePicker(const Document& doc, const std::string& owner = {});
/** Owned by ctx and removed when the extension unloads. */
LILA_SCENE_API void registerTemplatePicker(ext::Context& ctx, const Document& doc);
LILA_SCENE_API void unregisterTemplatePicker(const std::string& name);
LILA_SCENE_API void unregisterOwned(const std::string& owner);
LILA_SCENE_API std::vector<Document> templatePickers();

LILA_SCENE_API Document readFile(const std::filesystem::path& file);
LILA_SCENE_API void writeFile(const Document& doc, const std::filesystem::path& file);

/** Saved with the scene as "pickers": [document, ...]. fromJson and clear skip undo. */
LILA_SCENE_API json sceneToJson();
LILA_SCENE_API void sceneFromJson(const json& j);
LILA_SCENE_API void clearScene();

/** Called after any library change, undo and redo included. */
LILA_SCENE_API int addListener(std::function<void()> fn);
LILA_SCENE_API void removeListener(int id);

/** createPicker, deletePicker, listPickers, editPicker, pickerSelect, pickerRun, exportPicker, importPicker. */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::picker
