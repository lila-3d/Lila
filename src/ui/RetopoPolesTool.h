#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "retopo/Retopo.h"
#include "scene/Plug.h"
#include "ui/ViewportTool.h"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace lila::ui
{
/**
 * Retopo Poles: shows where an auto quad remesh's edge flow turns (its poles) on the selected mesh, with a live preview
 * of the remesh, and lets you put them where you want them.
 *
 *   drag a pole          move it along the surface (its mirror follows under symmetry)
 *   Ctrl click           add a 3/5 pole pair at the cursor
 *   Ctrl Shift click     remove a pole and the nearest pole of the other kind, so the count stays balanced
 *   Ctrl wheel           change the face count
 *   R                    back to the automatic poles
 *   Enter                build the remesh with these poles (quadRemesh poles=...)
 *   Esc, Q/W/E/R         leave the tool
 *
 * Solving runs on a worker thread; the preview catches up when it finishes.
 */
class LILA_UI_API RetopoPolesTool : public ViewportTool
{
public:
    explicit RetopoPolesTool(scene::NodeId source);
    ~RetopoPolesTool() override;
    const char* name() const override { return "Retopo Poles"; }

    void activate() override;
    void deactivate() override;
    bool mousePress(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override;
    bool wheel(QWheelEvent* e, const ViewInfo& view) override;
    bool keyPress(QKeyEvent* e, const ViewInfo& view) override;
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override;

    /** Face count the preview and the build aim for; kept in the general prefs. */
    static int targetFaces();
    static void setTargetFaces(int faces);

private:
    /** What a solve hands back, in the source's object space. */
    struct Solved
    {
        uint64_t generation = 0;
        std::optional<std::vector<retopo::Pole>> poles; // set when the solve found them (automatic mode)
        std::vector<std::optional<int>> shellTotals;
        geo::Mesh preview;
        std::string error;
    };

    bool refresh();
    void solve(bool automatic);
    void poll();
    retopo::QuadRemeshOptions options() const;
    bool surfaceHit(QPointF px, const ViewInfo& view, glm::dvec3& out) const;
    bool visible(const glm::dvec3& p, const ViewInfo& view) const;
    int pick(QPointF px, const ViewInfo& view) const;
    /** The pole mirroring pole i across the symmetry plane, or -1. */
    int mirrorOf(int i) const;
    void addPair(QPointF px, const ViewInfo& view);
    void removePair(int i);
    void build();
    std::string status() const;

    scene::NodeId mSource = scene::kNoNode;
    scene::MeshPtr mMesh;
    glm::dmat4 mWorld{1.0};
    geo::MeshBvh mBvh; // world space
    int mSymmetryAxis = -1;

    std::vector<retopo::Pole> mPoles; // world space
    std::vector<std::optional<int>> mShellTotals;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> mPreviewEdges; // world space
    std::string mError;

    uint64_t mGeneration = 0;
    std::future<Solved> mJob;
    bool mQueued = false;
    bool mQueuedAutomatic = false;
    std::unique_ptr<QTimer> mTimer;

    int mHover = -1;
    int mDrag = -1;
    int mDragMirror = -1;
    std::string mCursorView;
};

/** Registers "retopo.poles". */
LILA_UI_API void registerRetopoPolesActions();
} // namespace lila::ui
