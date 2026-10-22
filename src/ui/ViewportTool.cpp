#include "ui/ViewportTool.h"

#include <algorithm>
#include <map>

namespace lila::ui
{
namespace
{
struct QueuedMessage
{
    QString text;
    glm::vec4 color;
};
std::vector<QueuedMessage> gMessages;
} // namespace

void drawToolMessage(OverlayPainter&, const ViewInfo&, const QString& text, const glm::vec4& color)
{
    gMessages.push_back({text, color});
}

void flushToolMessages(OverlayPainter& p, const ViewInfo& view, const std::vector<QRectF>& occupied)
{
    double y = 24.0;
    for (const QueuedMessage& m : gMessages)
    {
        const double w = OverlayPainter::textWidth(m.text);
        const double x = std::max(12.0, (view.width - w) / 2.0);
        for (bool moved = true; moved;)
        {
            moved = false;
            QRectF r(x - 6.0, y - 16.0, w + 12.0, 22.0);
            for (const QRectF& o : occupied)
                if (o.intersects(r))
                {
                    y = o.bottom() + 20.0;
                    moved = true;
                }
        }
        p.label(QPointF(x, y), m.text, m.color);
        y += 22.0;
    }
    gMessages.clear();
}
} // namespace lila::ui

namespace lila::ui::viewportTools
{
namespace
{
std::unique_ptr<ViewportTool> gTool;
std::map<int, std::function<void()>> gListeners;
int gNext = 1;
} // namespace

void set(std::unique_ptr<ViewportTool> tool)
{
    if (gTool)
        gTool->deactivate();
    gTool = std::move(tool);
    if (gTool)
        gTool->activate();
    requestRedraw();
}

ViewportTool* active() { return gTool.get(); }

void requestRedraw()
{
    auto copy = gListeners;
    for (auto& [id, fn] : copy)
        fn();
}

int addListener(std::function<void()> fn)
{
    gListeners[gNext] = std::move(fn);
    return gNext++;
}

void removeListener(int id) { gListeners.erase(id); }
} // namespace lila::ui::viewportTools
