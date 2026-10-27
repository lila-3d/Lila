#include "ui/SimPreview.h"

#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/ViewportTool.h"

#include <QTimer>

#include <set>
#include <utility>

namespace lila::ui
{
namespace
{
std::set<scene::NodeId>& pending()
{
    static std::set<scene::NodeId> p;
    return p;
}

/** Restarted by every request, so it fires once edits have paused. */
QTimer& idleTimer()
{
    static QTimer* timer = [] {
        auto* t = new QTimer;
        t->setSingleShot(true);
        t->setInterval(200);
        QObject::connect(t, &QTimer::timeout, [] {
            scene::Graph& g = scene::Graph::current();
            for (scene::NodeId id : std::exchange(pending(), {}))
                if (g.node(id))
                    try
                    {
                        scene::sim::simulate(g, id, scene::sim::frameOf(g.time()));
                    }
                    catch (const std::exception&)
                    {
                    }
            viewportTools::requestRedraw();
        });
        return t;
    }();
    return *timer;
}
} // namespace

std::shared_ptr<const scene::sim::Frame> simPreviewFrame(scene::Graph& g, scene::NodeId node)
{
    const int64_t frame = scene::sim::frameOf(g.time());
    if (auto f = scene::sim::cached(g, node, frame))
        return f;
    if (scene::anim::playDirection() != 0 || scene::sim::extendable(g, node, frame, 2))
        return scene::sim::simulate(g, node, frame);
    pending().insert(node);
    idleTimer().start();
    return nullptr;
}
} // namespace lila::ui
