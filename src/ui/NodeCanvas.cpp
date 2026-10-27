#include "ui/NodeCanvas.h"

namespace lila::ui::canvas
{
NodeItem::NodeItem(NodeCanvas* canvas, const json& info) : mCanvas(canvas)
{
    name = info.value("name", "");
    type = info.value("type", "");
    category = info.value("category", "");
    label = qs(info.value("label", type));
    for (const json& s : info.value("inputs", json::array()))
        inputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), s.value("multi", false), s.value("linked", false),
                          s.value("value", json()), s.value("implicit", ""), s.value("doc", "")});
    for (const json& s : info.value("outputs", json::array()))
        outputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), false, false, json(), "", s.value("doc", "")});
    for (const json& p : info.value("props", json::array()))
        props.push_back({p.value("id", ""), p.value("value", "")});
    viewer = !canvas->backend->viewerType().empty() && type == canvas->backend->viewerType();
    thumbSize = canvas->backend->thumbnailSize(type);
    if (std::string sub = canvas->backend->subgroupNodeType(); !sub.empty() && type == sub)
        for (auto& [id, v] : props)
            if (id == "group")
                groupRef = v;
    if (!groupRef.empty())
        label = qs(groupRef);
    reroute = type == gn::kRerouteType;
    portal = type == gn::kSendType || type == gn::kReceiveType;
    problem = qs(info.value("problem", ""));
    if (portal)
        props.clear();
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    layoutRows();
    const json& pos = info["position"];
    setPos(pos[0].get<double>(), pos[1].get<double>());
    QString tip = label;
    if (!groupRef.empty())
        tip += "\nGroup node: double-click to open " + qs(groupRef) + ".";
    if (isViewer())
        tip += "\nClick the eye to preview in the viewport.";
    if (reroute)
        tip = "Reroute" + (outputs.empty() ? QString() : " (" + qs(outputs[0].type) + ")") + "\nDrag the middle to move it, the rim to wire it to more inputs.";
    setToolTip(tip);
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged)
        mCanvas->nodeMoved(this);
    return QGraphicsItem::itemChange(change, value);
}

void NodeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (reroute)
    {
        QColor c = outputs.empty() ? gray(165) : mCanvas->backend->socketStyle(outputs[0].type).color;
        p->setPen(isSelected() ? QPen(themeColor("nodeSelected"), 2.0) : QPen(gray(18), 1.2));
        p->setBrush(c);
        p->drawEllipse(QPointF(0.0, 0.0), kRerouteR, kRerouteR);
        return;
    }
    if (muted)
        p->setOpacity(p->opacity() * 0.45);
    QRectF body(0, 0, kNodeWidth, height());
    p->setPen(isSelected() ? QPen(themeColor("nodeSelected"), 1.6) : QPen(gray(20), 1.0));
    p->setBrush(themeColor("nodeBody"));
    p->drawRoundedRect(body, 6, 6);
    QPainterPath head;
    head.addRoundedRect(QRectF(0, 0, kNodeWidth, kHeader), 6, 6);
    if (!collapsed)
        head.addRect(QRectF(0, kHeader - 6, kNodeWidth, 6));
    p->setPen(Qt::NoPen);
    const GraphBackend& backend = *mCanvas->backend;
    p->setBrush(backend.headerColor(type, category));
    p->drawPath(head.simplified());
    p->setPen(gray(232));
    QFont f = p->font();
    f.setBold(true);
    p->setFont(f);
    p->drawText(QRectF(8, 0, kNodeWidth - 16 - (isViewer() || !problem.isEmpty() ? kHeader : 0.0), kHeader), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(shownTitle(), Qt::ElideRight, int(kNodeWidth - 16 - (problem.isEmpty() ? 0.0 : kHeader))));
    if (!problem.isEmpty())
    {
        QRectF b = badgeRect();
        p->setPen(Qt::NoPen);
        p->setBrush(themeColor("statusError"));
        p->drawEllipse(b);
        p->setPen(QPen(gray(15), 1.0));
        p->drawText(b, Qt::AlignCenter, "!");
    }
    f.setBold(false);
    p->setFont(f);
    if (isViewer())
    {
        QRectF eye = eyeRect();
        if (viewerActive)
        {
            p->setPen(Qt::NoPen);
            p->setBrush(gray(150));
            p->drawRoundedRect(eye, 3, 3);
        }
        QIcon ic = icon(viewerActive ? "eye" : "eye-off");
        p->drawPixmap(eye.adjusted(2, 2, -2, -2).toRect(), ic.pixmap(QSize(32, 32), viewerActive ? QIcon::Active : QIcon::Normal));
    }

    if (collapsed)
    {
        // One socket mark on each side that has links, where the wires meet.
        for (bool in : {true, false})
            for (const SocketInfo& s : in ? inputs : outputs)
                if (s.linked)
                {
                    drawSocket(*p, QPointF(in ? 0.0 : kNodeWidth, kHeader * 0.5), backend.socketStyle(s.type), false);
                    break;
                }
        return;
    }
    if (thumbSize > 0)
    {
        QRectF r((kNodeWidth - thumbSize) * 0.5, kHeader + 4.0, thumbSize, thumbSize);
        p->setPen(QPen(gray(20), 1.0));
        p->setBrush(gray(28));
        p->drawRect(r);
        if (!thumb.isNull())
            p->drawImage(r, thumb);
    }

    double y = top();
    for (int oi : shownOut)
    {
        size_t i = size_t(oi);
        p->setPen(gray(205));
        p->drawText(QRectF(8, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignRight, pretty(outputs[i].id));
        drawSocket(*p, QPointF(kNodeWidth, y + kRow * 0.5), backend.socketStyle(outputs[i].type), outputs[i].field);
        y += kRow;
    }
    for (size_t i = 0; i < props.size(); ++i, y += kRow)
    {
        QRectF r(8, y + 2, kNodeWidth - 16, kRow - 4);
        p->setPen(QPen(gray(40), 1));
        p->setBrush(gray(44));
        p->drawRoundedRect(r, 3, 3);
        p->setPen(gray(200));
        QString text = props[i].second.empty() ? pretty(props[i].first) : pretty(props[i].second);
        p->drawText(r.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    for (int ii : shownIn)
    {
        const SocketInfo& s = inputs[size_t(ii)];
        QString text = pretty(s.id);
        bool valueless = backend.valueless(s.type);
        if (!s.linked && !valueless)
        {
            QString v = !s.implicit.empty() && s.value.is_null() ? pretty(s.implicit) : valueText(s.value, s.type);
            if (!v.isEmpty())
                text += "  " + v;
        }
        p->setPen(s.linked || valueless ? gray(205) : gray(170));
        p->drawText(QRectF(12, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(text, Qt::ElideRight, int(kNodeWidth - 20)));
        drawSocket(*p, QPointF(0, y + kRow * 0.5), backend.socketStyle(s.type), s.field);
        if (s.multi)
        {
            p->setPen(QPen(gray(18), 1));
            p->drawLine(QPointF(-2, y + kRow * 0.5), QPointF(2, y + kRow * 0.5));
        }
        y += kRow;
    }
}

} // namespace lila::ui::canvas
