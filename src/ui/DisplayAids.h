#pragma once

#include "core/Export.h"

#include "geo/Attribute.h"
#include "render/FrameState.h"
#include "scene/Plug.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QMenu;
class QWidgetAction;

namespace lila::geo
{
class Mesh;
}

namespace lila::ui
{
class ViewportWidget;
}

/**
 * Maya's mesh display aids, per viewport panel: two-sided lighting, a back-face tint for spotting reversed normals,
 * face and vertex normals, tangents, bounding boxes, component and UV ids, and hard, soft, border and non-manifold edge
 * highlights. Each panel keeps its choices in the general prefs under /viewport/aids/panel<N>/, like the HUD.
 */
namespace lila::ui::displayAids
{
enum class Aid : uint8_t
{
    TwoSidedLighting,
    BackfaceTint,
    FaceNormals,
    VertexNormals,
    Tangents,
    BoundingBoxes,
    VertexIds,
    EdgeIds,
    FaceIds,
    UvIds,
    HardEdges,
    SoftEdges,
    BorderEdges,
    NonManifold,
    Count
};

struct Info
{
    Aid aid;
    const char* id; // "faceNormals"; its action is "viewport.aid.<id>"
    const char* label;
    const char* doc;
    bool fallback; // on in a panel that never chose
};

/** Every aid, in menu order. */
LILA_UI_API const std::vector<Info>& all();
LILA_UI_API const Info& info(Aid a);
LILA_UI_API const Info* find(std::string_view id);

LILA_UI_API std::string prefKey(int panel, Aid a);
LILA_UI_API bool on(int panel, Aid a);
LILA_UI_API void setOn(int panel, Aid a, bool on);

/** Length of drawn normals, tangents and binormals in cm (general prefs, shared by every panel). */
inline constexpr const char* kNormalLength = "/viewport/aids/normalLength";
inline constexpr double kDefaultNormalLength = 5.0;
LILA_UI_API double normalLength();

/** Most id labels a view draws at once, nearest the camera first. */
inline constexpr size_t kMaxLabels = 1500;
/** Most normal, tangent and edge highlight segments a view draws at once; larger meshes draw every n-th. */
inline constexpr size_t kMaxSegments = 400000;

/** Check state of a "viewport.aid.*" action for a view, nullopt for any other id. */
LILA_UI_API std::optional<bool> actionState(const std::string& id, const ViewportWidget* view);

/** The aids as checkable rows acting on view's panel, with the normal length as a field. */
LILA_UI_API void fillMenu(QMenu* menu, ViewportWidget* view);
/** The normal length field as a menu row. */
LILA_UI_API QWidgetAction* normalLengthRow(QMenu* menu);

/** Registers "viewport.aid.<id>" per aid, each toggling it in active()'s panel. */
LILA_UI_API void registerActions(std::function<ViewportWidget*()> active);

/** Two-sided lighting and the back-face tint onto a frame, for panel. */
LILA_UI_API void apply(render::FrameState& s, int panel);

/** The tint back faces get: display-encoded rgb, alpha the mix. Monotone keeps it gray. */
LILA_UI_API glm::vec4 backfaceTint(bool monotone);

struct MeshEntry
{
    scene::MeshPtr mesh;
    glm::dmat4 world{1.0};
};

/** Whether any aid that draw() handles is on in panel, so callers can skip collecting meshes. */
LILA_UI_API bool anyDrawn(int panel);

/** The drawn aids for the shown meshes: lines into the frame, id labels through the painter. */
LILA_UI_API void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, int panel, const std::vector<MeshEntry>& meshes,
                      double dpr);

/** A mesh's edges by kind (ids into its edges or points). Border and non-manifold edges are never hard or soft. */
struct EdgeClasses
{
    std::vector<geo::Index> hard;
    std::vector<geo::Index> soft;
    std::vector<geo::Index> border;
    std::vector<geo::Index> nonManifold;
    /** Points where separate fans of faces meet (bowties), or on a non-manifold edge. */
    std::vector<geo::Index> nonManifoldPoints;
};
LILA_UI_API EdgeClasses classifyEdges(const geo::Mesh& mesh);

/**
 * Shared UV ids for a uv set, as Maya numbers them: corners at one point with the same UV share an id. ids gets one per
 * corner capacity (kInvalid for dead corners or without the set); returns the first corner of each id.
 */
LILA_UI_API std::vector<geo::Index> uvIds(const geo::Mesh& mesh, std::vector<geo::Index>& ids, int uvSet = 0);

/** Every n-th of count items so at most budget remain; 1 when they all fit. */
LILA_UI_API size_t strideFor(size_t count, size_t budget);

/** Keeps the cap nearest of whatever it is offered, without holding the rest. */
template <class T> class Nearest
{
public:
    explicit Nearest(size_t cap) : mCap(cap) {}

    /** Whether something at depth would still be kept, so callers can skip the work of offering it. */
    bool wants(float depth) const { return mHeap.size() < mCap || (mCap > 0 && depth < mHeap.front().first); }

    void offer(float depth, T item)
    {
        if (!wants(depth))
            return;
        if (mHeap.size() == mCap)
        {
            std::pop_heap(mHeap.begin(), mHeap.end(), farther);
            mHeap.pop_back();
        }
        mHeap.emplace_back(depth, std::move(item));
        std::push_heap(mHeap.begin(), mHeap.end(), farther);
    }

    /** Nearest first; leaves the set empty. */
    std::vector<T> take()
    {
        std::sort_heap(mHeap.begin(), mHeap.end(), farther);
        std::vector<T> out;
        out.reserve(mHeap.size());
        for (auto& [depth, item] : mHeap)
            out.push_back(std::move(item));
        mHeap.clear();
        return out;
    }

private:
    static bool farther(const std::pair<float, T>& a, const std::pair<float, T>& b) { return a.first < b.first; }

    size_t mCap;
    std::vector<std::pair<float, T>> mHeap; // max-heap on depth: the farthest kept sits in front
};
} // namespace lila::ui::displayAids
