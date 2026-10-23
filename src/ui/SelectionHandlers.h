#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"
#include "core/Registry.h"
#include "scene/Plug.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <QPointF>
#include <QString>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Selection handlers: what an object-mode click in the viewport selects, and what hovering shades. With no handler
 * active, clicks select what they hit, as always. A mode makes one active by id (setActive), and from then on every
 * object-mode viewport click asks it first:
 *
 *  - redirect(hit) may name other nodes to select instead (a click on a skinned mesh's face selects the joint that
 *    dominates it). nullopt leaves the click alone; an empty list selects nothing (and clears, without modifiers).
 *    Shift/Ctrl apply to the redirected nodes as they would to the hit ones.
 *  - hover(hit) may return a Highlight: per-point or per-face weights on a mesh, drawn over it as a coloured wash
 *    (alpha = weight * color.a) while the cursor stays there. It is asked on the same throttled schedule as the
 *    preselection highlight.
 *
 * Names a handler returns then go through their joints' selectAs (a twist bone's skin selects the limb bone), see
 * applySelectAs. A right-button press asks the active handler's candidates for a marking menu of what to select.
 *
 * Marquee selections ask redirect once per node in the box, with no face or point. Component modes never use
 * handlers. Callbacks run on the main thread and may read the scene graph (already evaluated).
 *
 * Minimal handler:
 *
 *     selectionHandlers::add(ctx, {.id = "myExt.dominantJoint", .label = "Joint under mesh",
 *         .redirect = [](const selectionHandlers::Hit& h) -> std::optional<std::vector<std::string>> {
 *             if (h.face == selectionHandlers::kNone) return std::nullopt;
 *             return std::vector<std::string>{jointFor(h.node, h.face)};
 *         }});
 *     selectionHandlers::setActive("myExt.dominantJoint");
 */
namespace lila::ui::selectionHandlers
{
inline constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

/** What is under the cursor. On a mesh, face and point are ids in the mesh's evaluated (deformed) outMesh. */
struct Hit
{
    scene::NodeId node = scene::kNoNode; // kNoNode: empty space
    std::string name;                    // node's name, or what a viewport picker returned
    uint32_t face = kNone;               // mesh face under the cursor
    uint32_t point = kNone;              // the hit triangle's corner point nearest the cursor
    glm::dvec3 position{0.0};            // world-space hit on the surface (valid when face is)
    QPointF px;                          // cursor, widget pixels
    bool click = true;                   // false for a marquee member
};

struct Highlight
{
    enum class Domain : uint8_t
    {
        Point, // weights per point id, blended across faces
        Face,  // weights per face id, flat
    };
    scene::NodeId mesh = scene::kNoNode;
    Domain domain = Domain::Face;
    /** Indexed by point or face id of the mesh's outMesh; missing ids count as 0. */
    std::vector<float> weights;
    /** sRGB; alpha scales every weight. Data shading, so it may be coloured. */
    glm::vec4 color{0.62f, 0.38f, 0.95f, 0.55f};
};

/** One entry of the RMB candidate menu on a mesh: a node the click could mean. */
struct Candidate
{
    std::string name;
    QString label;
    std::string role; // redirect, weighted, parent, child, influence
    float weight = 0.0f;
};

struct Handler
{
    std::string id;
    QString label;
    std::function<std::optional<std::vector<std::string>>(const Hit&)> redirect;
    std::function<std::optional<Highlight>(const Hit&)> hover;
    /**
     * What a right-button press on the hit could select, for a marking menu to pick from (redirect target, the weighted
     * joint, parent, child...). Empty, or no callback, keeps the viewport's normal RMB menu. Asked of the active handler.
     */
    std::function<std::vector<Candidate>(const Hit&)> candidates;
    /**
     * Asked for every object-mode click whether or not it is active, before the active handler (registry order), for
     * stand-ins that always select what they stand for (physics proxy bodies select their joint). Only redirect runs.
     */
    bool always = false;
};

LILA_UI_API Registry<Handler>& registry();

/** Throws std::invalid_argument when the id is taken. */
LILA_UI_API const Handler& add(Handler handler);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the id is taken. */
LILA_UI_API const Handler* add(ext::Context& ctx, Handler handler);
LILA_UI_API bool remove(std::string_view id);
LILA_UI_API const Handler* find(std::string_view id);

/** Makes id the active handler; empty for none. False (no change) when id is not registered. */
LILA_UI_API bool setActive(std::string_view id);
/** The active handler's id, empty for none. */
LILA_UI_API const std::string& activeId();
/** The active handler, or null. */
LILA_UI_API const Handler* active();
/** Whether a click would be redirected by anything: an active handler or an always-on one. */
LILA_UI_API bool anyRedirect();

/**
 * Runs the active handler over a pick's hits: a hit whose redirect answers is replaced by the names it gave, the rest
 * keep their own name. Duplicates are dropped, first occurrence kept. Without a handler, just the hits' names.
 */
LILA_UI_API std::vector<std::string> redirect(const std::vector<Hit>& hits);

/** The active handler's candidates for a right-button press on hit; empty when it has none. */
LILA_UI_API std::vector<Candidate> candidates(const Hit& hit);

/**
 * The marking menu for candidates: the first in N, the next in S, W, E, then NE, NW, SE, SW and the overflow, each
 * selecting its node on release. fallback (a menu definition) becomes an overflow cascade "Object Menu",
 * so the viewport's own items stay reachable. Null for no candidates.
 */
LILA_UI_API nlohmann::json candidateMenu(const std::vector<Candidate>& candidates, const nlohmann::json& fallback = nullptr);

/**
 * Joint selectAs redirects (scene/SelectAs.h) apply to every redirected click unless bypassed: by the pref
 * general /rig/selectAsBypass (rig.selectAsToggle) or while a key bound to rig.selectAsBypassOn/Off is held.
 */
LILA_UI_API bool selectAsBypassed();
LILA_UI_API void setSelectAsHeld(bool held);
/** name through its selectAs chain, unless bypassed or it isn't a node with one. */
LILA_UI_API std::string applySelectAs(const std::string& name);

/** Called when the active handler changes or is removed. Returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);
} // namespace lila::ui::selectionHandlers
