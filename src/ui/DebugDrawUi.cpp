#include "ui/DebugDrawUi.h"

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "debug/DebugDraw.h"
#include "ui/Actions.h"
#include "ui/Overlay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kShowKey = "/debugDraw/show";

/** The painter takes sRGB; debug colors are linear. */
glm::vec4 srgb(const glm::vec4& c)
{
    auto f = [](float x) { return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f; };
    return {f(std::min(c.r, 1.0f)), f(std::min(c.g, 1.0f)), f(std::min(c.b, 1.0f)), c.a};
}

std::atomic<bool> gRedrawPosted{false};

void redrawSoon()
{
    if (!gRedrawPosted.exchange(true))
        mainq::post([] {
            gRedrawPosted = false;
            viewportTools::requestRedraw();
        });
}

/** Redraws the views when the next draw expires, so it leaves the screen without waiting for other activity. */
void scheduleExpiry()
{
    static QTimer* timer = [] {
        auto* t = new QTimer(QCoreApplication::instance());
        t->setSingleShot(true);
        QObject::connect(t, &QTimer::timeout, [] { viewportTools::requestRedraw(); });
        return t;
    }();
    const double next = debugDraw::nextExpiry();
    if (!std::isfinite(next))
    {
        timer->stop();
        return;
    }
    const int ms = int(std::clamp((next - debugDraw::now()) * 1000.0 + 1.0, 0.0, 3'600'000.0));
    if (!timer->isActive() || timer->remainingTime() > ms)
        timer->start(ms);
}

/** The render copies of a snapshot, made once per change and shared by every view's frames. */
struct RenderCopy
{
    uint64_t generation = 0;
    std::shared_ptr<const std::vector<render::LineBatch>> lines;
    std::shared_ptr<const std::vector<render::LineVertex>> tris;
};

const RenderCopy& renderCopy(const debugDraw::Snapshot& snap)
{
    static RenderCopy copy;
    if (copy.generation == snap.generation && copy.lines)
        return copy;
    auto lines = std::make_shared<std::vector<render::LineBatch>>();
    lines->reserve(snap.lines.size());
    for (const debugDraw::LineBatch& b : snap.lines)
    {
        render::LineBatch& out = lines->emplace_back();
        out.width = b.width;
        out.onTop = b.onTop;
        out.vertices.reserve(b.vertices.size());
        for (const debugDraw::Vertex& v : b.vertices)
            out.vertices.push_back({v.pos, v.color});
    }
    auto tris = std::make_shared<std::vector<render::LineVertex>>();
    tris->reserve(snap.triangles.size());
    for (const debugDraw::Vertex& v : snap.triangles)
        tris->push_back({v.pos, v.color});
    copy = {snap.generation, std::move(lines), std::move(tris)};
    return copy;
}

/** The snapshot to draw now, its lines and triangles already in s; null when there is nothing. */
std::shared_ptr<const debugDraw::Snapshot> addLines(render::FrameState& s, bool inView)
{
    if (!debugDrawShown())
        return nullptr;
    std::shared_ptr<const debugDraw::Snapshot> snap = debugDraw::snapshot();
    if (inView)
        scheduleExpiry();
    if (snap->empty())
        return nullptr;
    const RenderCopy& copy = renderCopy(*snap);
    s.debugLines = copy.lines;
    s.debugTris = copy.tris;
    return snap;
}

void drawDebug(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)
{
    LILA_ZONE();
    std::shared_ptr<const debugDraw::Snapshot> snap = addLines(s, true);
    if (!snap)
        return;

    const QRectF bounds(-64.0, -64.0, view.width + 128.0, view.height + 128.0);
    for (const debugDraw::ScreenPoint& pt : snap->points)
        if (std::optional<QPointF> at = view.project(glm::dvec3(pt.pos)); at && bounds.contains(*at))
        {
            const double h = pt.size * 0.5;
            p.rect(QRectF(at->x() - h, at->y() - h, pt.size, pt.size), srgb(pt.color));
        }
    p.setShadow(true);
    for (const debugDraw::ScreenText& t : snap->texts)
        if (std::optional<QPointF> at = view.project(glm::dvec3(t.pos)); at && bounds.contains(*at))
            p.text(*at, QString::fromUtf8(t.text.data(), qsizetype(t.text.size())), srgb(t.color), OverlayPainter::Center);
    p.setShadow(false);
}
} // namespace

void addDebugDraw(render::FrameState& s)
{
    addLines(s, false);
}

bool debugDrawShown()
{
    return PrefStore::get().value<bool>("general", kShowKey, true);
}

void registerDebugDrawUi()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    viewportHooks::addDrawer("lila.debugDraw", drawDebug);
    debugDraw::setChangedHook(redrawSoon);
    actions::add({.id = "viewport.toggleDebugDraw",
                  .label = "Debug Drawing",
                  .category = "View",
                  .run =
                      [] {
                          PrefStore::get().set("general", kShowKey, !debugDrawShown());
                          viewportTools::requestRedraw();
                      },
                  .doc = "Shows or hides what scripts and extensions draw with lila.debug_draw, in every view.",
                  .python = "lila.prefs.set(\"general\", \"/debugDraw/show\", False)",
                  .checked = [] { return debugDrawShown(); }});
    actions::add({.id = "viewport.clearDebugDraw",
                  .label = "Clear Debug Drawing",
                  .category = "View",
                  .run = [] { debugDraw::flush(); },
                  .doc = "Removes everything scripts and extensions drew with lila.debug_draw, persistent channels included.",
                  .python = "lila.debug_draw.flush_debug_draw()"});
}
} // namespace lila::ui
