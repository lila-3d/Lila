#include "ui/NodeCanvas.h"

namespace lila::ui
{
int NodeCanvas::tieAt(QPointF sp) const
{
    for (TieItem* t : mTieItems)
        if (t->shape().contains(t->mapFromScene(sp)))
            return t->index;
    return -1;
}

void NodeCanvas::hoverTie(int t)
{
    if (t == mHotTie)
        return;
    for (TieItem* item : mTieItems)
    {
        item->hot = item->index == t;
        item->update();
    }
    mHotTie = t;
    if (!mZipKey)
    {
        if (t >= 0)
            viewport()->setCursor(Qt::OpenHandCursor);
        else
            viewport()->unsetCursor();
    }
}

void NodeCanvas::layoutTies()
{
    std::map<LinkKey, LinkItem*> byKey;
    for (LinkItem* l : mLinks)
    {
        l->passes.clear();
        byKey[l->key()] = l;
    }
    struct Pass
    {
        double t; // where along the wire the tie sits
        QPointF entry, exit;
    };
    std::map<LinkItem*, std::vector<Pass>> passes;
    for (int i = 0; i < int(mTies.size()); ++i)
    {
        const TieView& tie = mTies[i];
        QPointF d = tieDirection(tie.angle), across(-d.y(), d.x());
        std::vector<std::pair<double, LinkItem*>> order;
        for (const LinkKey& k : tie.links)
            if (auto it = byKey.find(k); it != byKey.end() && it->second->isVisible())
            {
                LinkItem* l = it->second;
                QPointF mid = (l->from->socketPos(false, l->out) + l->to->socketPos(true, l->in)) * 0.5;
                order.push_back({dot(mid - tie.pos, across), l});
            }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t s = 0; s < order.size(); ++s)
        {
            LinkItem* l = order[s].second;
            QPointF a = l->from->socketPos(false, l->out), b = l->to->socketPos(true, l->in);
            QPointF run = dot(b - a, d) < 0.0 ? -d : d;
            QPointF c = tie.pos + across * ((double(s) - double(order.size() - 1) * 0.5) * kTieSpacing);
            QPointF ab = b - a;
            double t = dot(tie.pos - a, ab) / std::max(1e-9, dot(ab, ab));
            passes[l].push_back({t, c - run * (kTieRun * 0.5), c + run * (kTieRun * 0.5)});
        }
        if (i < int(mTieItems.size()))
        {
            TieItem* item = mTieItems[i];
            item->setCount(int(order.size()));
            item->color = tieColor(tie.color);
            item->setPos(tie.pos);
            item->setRotation(tie.angle);
            item->setVisible(order.size() >= 2);
            item->update();
        }
    }
    for (auto& [l, list] : passes)
    {
        std::sort(list.begin(), list.end(), [](const Pass& a, const Pass& b) { return a.t < b.t; });
        for (const Pass& p : list)
            l->passes.push_back({p.entry, p.exit});
    }
    for (LinkItem* l : mLinks)
        l->update();
}

void NodeCanvas::dragTie(QPointF sp, Qt::KeyboardModifiers mods)
{
    TieView& tie = mTies[mTieDrag];
    if (mTieRotate)
    {
        QPointF v = sp - tie.pos;
        if (std::hypot(v.x(), v.y()) < 4.0)
            return;
        double a = qRadiansToDegrees(std::atan2(v.y(), v.x()));
        // Ctrl turns freely; otherwise it steps by 15 degrees.
        tie.angle = (mods & Qt::ControlModifier) ? a : std::round(a / 15.0) * 15.0;
    }
    else
        tie.pos = sp - mTieGrab;
    layoutTies();
}

std::vector<LinkKey> NodeCanvas::linksCrossing(QPointF a, QPointF b) const
{
    std::vector<LinkKey> out;
    QLineF stroke(a, b);
    for (LinkItem* l : mLinks)
    {
        if (!l->isVisible())
            continue;
        const QPainterPath& path = l->path();
        for (int k = 0; k < 48; ++k)
            if (QLineF(path.pointAtPercent(k / 48.0), path.pointAtPercent((k + 1) / 48.0)).intersects(stroke) == QLineF::BoundedIntersection)
            {
                out.push_back(l->key());
                break;
            }
    }
    return out;
}

void NodeCanvas::finishTieStroke(QPointF end)
{
    mTieStroke = false;
    removeTemp();
    if (!mZipKey)
        viewport()->unsetCursor();
    std::vector<LinkKey> crossed = linksCrossing(mTieStart, end);
    std::vector<TieView> next = mTies;
    if (mTieTarget >= 0 && mTieTarget < int(next.size()))
    {
        TieView& tie = next[mTieTarget];
        size_t before = tie.links.size();
        for (const LinkKey& k : crossed)
            if (std::find(tie.links.begin(), tie.links.end(), k) == tie.links.end())
                tie.links.push_back(k);
        if (tie.links.size() != before)
            commitTies(next);
        return;
    }
    if (crossed.size() < 2)
    {
        QToolTip::showText(QCursor::pos(), "A zip tie needs two or more wires: drag across them while holding the key.", this);
        return;
    }
    QPointF v = end - mTieStart;
    QPointF dir(-v.y(), v.x());
    QPointF flow(0.0, 0.0);
    for (LinkItem* l : mLinks)
        if (std::find(crossed.begin(), crossed.end(), l->key()) != crossed.end())
        {
            QPointF f = l->to->socketPos(true, l->in) - l->from->socketPos(false, l->out);
            flow += f / std::max(1e-9, std::hypot(f.x(), f.y()));
        }
    if (dot(dir, flow) < 0.0)
        dir = -dir;
    double angle = qRadiansToDegrees(std::atan2(dir.y(), dir.x()));
    angle = std::round(angle / 15.0) * 15.0;
    next.push_back({crossed, (mTieStart + end) * 0.5, angle == -0.0 ? 0.0 : angle, {}});
    commitTies(next);
}

void NodeCanvas::tieMenu(int t, QPoint global)
{
    QMenu menu(this);
    fillPaletteMenu(menu.addMenu("Colour"), [this, t](std::string id) {
        std::vector<TieView> next = mTies;
        next[t].color = std::move(id);
        commitTies(next);
    });
    menu.addAction(QString("Rotate 90%1").arg(QChar(0xb0)), this, [this, t] {
        std::vector<TieView> next = mTies;
        next[t].angle = std::fmod(next[t].angle + 90.0, 360.0);
        commitTies(next);
    });
    menu.addAction("Level", this, [this, t] {
        std::vector<TieView> next = mTies;
        next[t].angle = 0.0;
        commitTies(next);
    });
    QMenu* remove = menu.addMenu("Remove Wire");
    for (size_t i = 0; i < mTies[t].links.size(); ++i)
        remove->addAction(linkLabel(mTies[t].links[i]), this, [this, t, i] {
            std::vector<TieView> next = mTies;
            next[t].links.erase(next[t].links.begin() + std::ptrdiff_t(i));
            if (next[t].links.size() < 2)
                next.erase(next.begin() + t);
            commitTies(next);
        });
    menu.addSeparator();
    menu.addAction("Delete Tie", this, [this, t] {
        std::vector<TieView> next = mTies;
        next.erase(next.begin() + t);
        commitTies(next);
    });
    menu.exec(global);
}
} // namespace lila::ui
