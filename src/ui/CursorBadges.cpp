#include "ui/CursorBadges.h"

#include "core/PrefStore.h"
#include "ui/Hotkeys.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"
#include "ui/ViewportWidget.h"

#include <QStringList>

#include <algorithm>

namespace lila::ui::cursorBadges
{
namespace
{
constexpr double kIcon = 16.0;
constexpr double kGap = 4.0;
const QPointF kOffset(16.0, 20.0); // clear of the arrow cursor's tip and body

std::vector<Badge>& badges()
{
    static std::vector<Badge> list;
    return list;
}

struct Status
{
    const ViewportWidget* owner = nullptr;
    QString text;
    std::function<void(const QString&)> sink;
};

Status& status()
{
    static Status s;
    return s;
}

QString lineFor(const Badge& b)
{
    QStringList keys;
    if (!b.key.isEmpty())
        keys << b.key;
    for (const std::string& id : b.actions)
        if (QString k = Hotkeys::get().keyFor(id); !k.isEmpty() && !keys.contains(k))
            keys << k;
    return keys.isEmpty() ? b.message : b.message + " (" + keys.join(" / ") + ")";
}
} // namespace

void add(Badge badge)
{
    auto& list = badges();
    auto it = std::find_if(list.begin(), list.end(), [&](const Badge& b) { return b.id == badge.id; });
    if (it != list.end())
        *it = std::move(badge);
    else
        list.push_back(std::move(badge));
}

std::vector<const Badge*> active(const ViewportWidget& view)
{
    std::vector<const Badge*> out;
    if (!PrefStore::get().value<bool>("general", kEnabledPref, true))
        return out;
    for (const Badge& b : badges())
        if (b.active && b.active(view))
            out.push_back(&b);
    return out;
}

void draw(OverlayPainter& p, QPointF cursor, const std::vector<const Badge*>& list)
{
    if (list.empty())
        return;
    QPointF at = cursor + kOffset;
    QRectF all(at, QSizeF(list.size() * (kIcon + kGap), kIcon));
    glm::vec4 color = p.ink(0.88f, 1.0f, all);
    p.setShadow(true);
    double x = at.x();
    for (const Badge* b : list)
    {
        p.icon(QRectF(x, at.y(), kIcon, kIcon), b->icon, color);
        x += kIcon + kGap;
    }
    p.setShadow(false);
}

void publish(const ViewportWidget* view, const std::vector<const Badge*>& list)
{
    Status& s = status();
    if (list.empty() && s.owner != view)
        return;
    QStringList lines;
    for (const Badge* b : list)
        lines << lineFor(*b);
    QString text = lines.join("   ");
    s.owner = list.empty() ? nullptr : view;
    if (text == s.text)
        return;
    s.text = text;
    if (s.sink)
        s.sink(text);
}

void setStatusSink(std::function<void(const QString&)> shown)
{
    status().sink = std::move(shown);
}

void registerDefaults()
{
    auto snap = [](unsigned flag) { return [flag](const ViewportWidget&) { return (tools::snaps() & flag) != 0; }; };
    add({"snap.grid", "grid-pattern", snap(SnapGrid), "Grid snapping enabled", {"snap.gridOn"}});
    add({"snap.edge", "line", snap(SnapEdge), "Edge snapping enabled", {"snap.edgeOn"}});
    add({"snap.vertex", "point", snap(SnapVertex), "Vertex snapping enabled", {"snap.vertexOn"}});
    add({"snap.rotateHold", "angle", snap(SnapRotateHold), "Rotate snap toggled", {"snap.rotateHoldOn"}});
    add({"pivotEdit", "target", [](const ViewportWidget&) { return tools::pivotEdit(); }, "Pivot edit",
         {"tool.pivotEditOn", "tool.pivotEditToggle"}});
    add({"softSelect.adjust", "circle-dotted", [](const ViewportWidget&) { return tools::softSelectAdjusting(); },
         "Drag to set the soft select radius", {"softSelect.keyDown", "softSelect.adjustOn"}});
    add({"manipulator.precision", "feather", [](const ViewportWidget& v) { return v.manipulator().precisionDrag(); },
         "Precision drag", {}, "Shift"});
    add({"manipulator.ctrlPlane", "perspective", [](const ViewportWidget& v) { return v.manipulator().ctrlPlaneDrag(); },
         "Dragging in the plane of the other two axes", {}, "Ctrl"});
}
} // namespace lila::ui::cursorBadges
