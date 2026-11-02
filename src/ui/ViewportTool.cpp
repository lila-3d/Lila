#include "ui/ViewportTool.h"

#include "core/Events.h"
#include "core/Log.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>

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

const QCursor& deleteCursor()
{
    static const QCursor cursor = [] {
        const int size = 24;
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(int(size * dpr), int(size * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing);
        for (auto [width, color] : {std::pair{4.5, QColor(20, 20, 20)}, std::pair{2.0, QColor(240, 240, 240)}})
        {
            painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(6, 6), QPointF(18, 18));
            painter.drawLine(QPointF(18, 6), QPointF(6, 18));
        }
        painter.end();
        return QCursor(pm, size / 2, size / 2);
    }();
    return cursor;
}

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

namespace
{
constexpr double kKeyLine = 17.0, kKeyGap = 12.0;

double widestKey(const std::vector<KeyHint>& hints)
{
    double w = 0.0;
    for (const KeyHint& h : hints)
        w = std::max(w, double(OverlayPainter::textWidth(h.keys)));
    return w;
}
} // namespace

double keyListHeight(size_t rows) { return kKeyLine * double(rows); }

double keyListWidth(const std::vector<KeyHint>& hints)
{
    double what = 0.0;
    for (const KeyHint& h : hints)
        what = std::max(what, double(OverlayPainter::textWidth(h.what)));
    return widestKey(hints) + kKeyGap + what;
}

void drawKeyList(OverlayPainter& p, QPointF bottomLeft, const std::vector<KeyHint>& hints, float alpha)
{
    const double keyWidth = widestKey(hints);
    double y = bottomLeft.y() - keyListHeight(hints.size()) + 4.0;
    const glm::vec4 key(0.3f, 0.75f, 1.0f, alpha), white(0.95f, 0.95f, 0.95f, alpha);
    p.setShadow(true);
    for (const KeyHint& h : hints)
    {
        p.text(QPointF(bottomLeft.x(), y), h.keys, key);
        p.text(QPointF(bottomLeft.x() + keyWidth + kKeyGap, y), h.what, white);
        y += kKeyLine;
    }
    p.setShadow(false);
}
} // namespace lila::ui

namespace lila::ui::viewportTools
{
namespace
{
std::unique_ptr<ViewportTool> gTool;
std::string gToolId; // registered id gTool came from
std::optional<glm::dvec3> gOrbitPoint;
std::map<int, std::function<void()>> gListeners;
int gNext = 1;

void install(std::unique_ptr<ViewportTool> tool, std::string id)
{
    if (gTool)
        gTool->deactivate();
    gOrbitPoint.reset();
    gTool = std::move(tool);
    gToolId = gTool ? std::move(id) : std::string();
    if (gTool)
        gTool->activate();
    requestRedraw();
    events::send({.type = events::Event::ViewportToolChanged, .name = gTool ? gTool->name() : std::string()});
}
} // namespace

Registry<ToolDef>& registry()
{
    static Registry<ToolDef>* r = [] {
        auto* reg = new Registry<ToolDef>();
        // The active tool's code may belong to whoever registered it, so it ends before its definition goes.
        reg->addListener([](RegistryChange change, const std::string& id, const std::string&) {
            if (change == RegistryChange::Removed && gTool && gToolId == id)
                install(nullptr, {});
        });
        return reg;
    }();
    return *r;
}

const ToolDef& add(ToolDef def)
{
    std::string id = def.id;
    return registry().add(std::move(id), std::move(def));
}

const ToolDef* add(ext::Context& ctx, ToolDef def)
{
    std::string id = def.id;
    try
    {
        ctx.add(registry(), "viewportTool", id, std::move(def));
        return registry().find(id);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: viewport tool: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id) { return registry().remove(id); }

bool activate(std::string_view id)
{
    const ToolDef* def = registry().find(id);
    if (!def || !def->create)
        return false;
    std::unique_ptr<ViewportTool> tool = def->create();
    if (!tool)
        return false;
    install(std::move(tool), std::string(id));
    return true;
}

const std::string& activeId() { return gToolId; }

void set(std::unique_ptr<ViewportTool> tool) { install(std::move(tool), {}); }

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

void setOrbitPoint(std::optional<glm::dvec3> point) { gOrbitPoint = point; }

std::optional<glm::dvec3> orbitPoint() { return gOrbitPoint; }
} // namespace lila::ui::viewportTools
