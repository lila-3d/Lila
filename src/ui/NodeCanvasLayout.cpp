#include "ui/NodeCanvas.h"

#include <climits>
#include <numeric>

namespace lila::ui
{
namespace
{
/** A node's box on the canvas: the body for nodes, the dot for reroutes. */
QRectF boxOf(const NodeItem* n)
{
    if (n->reroute)
        return QRectF(n->pos() - QPointF(NodeItem::kRerouteR, NodeItem::kRerouteR), QSizeF(2 * NodeItem::kRerouteR, 2 * NodeItem::kRerouteR));
    return QRectF(n->pos(), QSizeF(kNodeWidth, n->height()));
}
} // namespace

std::vector<NodeItem*> NodeCanvas::selectedNodes() const
{
    std::vector<NodeItem*> out;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i))
            out.push_back(n);
    return out;
}

void NodeCanvas::moveTo(const std::map<NodeItem*, QPointF>& to, const char* label)
{
    json positions = json::object();
    for (auto& [n, p] : to)
        if (QLineF(n->pos(), p).length() > 0.01)
            positions[n->name] = {p.x(), p.y()};
    if (positions.empty())
        return;
    UndoChunk chunk(label);
    runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
}

void NodeCanvas::align(Align a)
{
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.size() < 2)
        return;
    QRectF all;
    double cx = 0.0, cy = 0.0;
    for (NodeItem* n : nodes)
    {
        QRectF r = boxOf(n);
        all = all.united(r);
        cx += r.center().x();
        cy += r.center().y();
    }
    cx /= double(nodes.size());
    cy /= double(nodes.size());
    std::map<NodeItem*, QPointF> to;
    for (NodeItem* n : nodes)
    {
        QRectF r = boxOf(n);
        QPointF shift(0.0, 0.0);
        switch (a)
        {
        case Align::Left: shift.setX(all.left() - r.left()); break;
        case Align::Center: shift.setX(cx - r.center().x()); break;
        case Align::Right: shift.setX(all.right() - r.right()); break;
        case Align::Top: shift.setY(all.top() - r.top()); break;
        case Align::Middle: shift.setY(cy - r.center().y()); break;
        case Align::Bottom: shift.setY(all.bottom() - r.bottom()); break;
        }
        to[n] = n->pos() + shift;
    }
    moveTo(to, "Align Nodes");
}

void NodeCanvas::distribute(bool horizontal)
{
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.size() < 3)
        return;
    auto lead = [horizontal](const QRectF& r) { return horizontal ? r.left() : r.top(); };
    auto extent = [horizontal](const QRectF& r) { return horizontal ? r.width() : r.height(); };
    std::sort(nodes.begin(), nodes.end(), [&](NodeItem* a, NodeItem* b) { return lead(boxOf(a)) < lead(boxOf(b)); });
    QRectF first = boxOf(nodes.front()), last = boxOf(nodes.back());
    double span = lead(last) + extent(last) - lead(first);
    double used = 0.0;
    for (NodeItem* n : nodes)
        used += extent(boxOf(n));
    double gap = (span - used) / double(nodes.size() - 1);
    std::map<NodeItem*, QPointF> to;
    double at = lead(first);
    for (NodeItem* n : nodes)
    {
        QRectF r = boxOf(n);
        double d = at - lead(r);
        to[n] = n->pos() + (horizontal ? QPointF(d, 0.0) : QPointF(0.0, d));
        at += extent(r) + gap;
    }
    moveTo(to, "Distribute Nodes");
}

void NodeCanvas::straighten()
{
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.empty())
        return;
    std::set<NodeItem*> picked(nodes.begin(), nodes.end());
    // Upstream first, so each node lines up with where its source ends up.
    std::map<NodeItem*, int> depth;
    std::function<int(NodeItem*, int)> depthOf = [&](NodeItem* n, int guard) -> int {
        if (auto it = depth.find(n); it != depth.end())
            return it->second;
        int d = 0;
        if (guard < 256)
            for (LinkItem* l : mLinks)
                if (l->to == n && picked.contains(l->from))
                    d = std::max(d, depthOf(l->from, guard + 1) + 1);
        depth[n] = d;
        return d;
    };
    for (NodeItem* n : nodes)
        depthOf(n, 0);
    std::sort(nodes.begin(), nodes.end(), [&](NodeItem* a, NodeItem* b) { return depth[a] < depth[b]; });
    std::map<NodeItem*, QPointF> to;
    auto posOf = [&](NodeItem* n) { return to.contains(n) ? to[n] : n->pos(); };
    for (NodeItem* n : nodes)
    {
        // The first wire into it, from a selected node if any; a lone node lines up with whatever feeds it.
        LinkItem* in = nullptr;
        for (LinkItem* l : mLinks)
            if (l->to == n && l->from != n && (!in || (picked.contains(l->from) && !picked.contains(in->from))))
                in = l;
        if (in && (nodes.size() == 1 || picked.contains(in->from)))
        {
            double src = in->from->socketPos(false, in->out).y() + (posOf(in->from) - in->from->pos()).y();
            double dst = n->socketPos(true, in->in).y();
            to[n] = posOf(n) + QPointF(0.0, src - dst);
            continue;
        }
        if (nodes.size() == 1)
            for (LinkItem* l : mLinks)
                if (l->from == n)
                {
                    to[n] = n->pos() + QPointF(0.0, l->to->socketPos(true, l->in).y() - n->socketPos(false, l->out).y());
                    break;
                }
    }
    moveTo(to, "Straighten Links");
}
void NodeCanvas::autoLayout()
{
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.size() < 2)
    {
        nodes.clear();
        for (auto& [name, n] : mNodes)
            nodes.push_back(n);
    }
    if (nodes.size() < 2)
        return;
    std::set<NodeItem*> in(nodes.begin(), nodes.end());
    std::map<NodeItem*, std::vector<NodeItem*>> preds, succs;
    for (LinkItem* l : mLinks)
        if (in.contains(l->from) && in.contains(l->to) && l->from != l->to)
        {
            preds[l->to].push_back(l->from);
            succs[l->from].push_back(l->to);
        }

    // Columns by longest path from the sources, then sources pulled up to sit just before their first reader.
    std::map<NodeItem*, int> rank;
    std::function<int(NodeItem*, int)> rankOf = [&](NodeItem* n, int guard) -> int {
        if (auto it = rank.find(n); it != rank.end())
            return it->second;
        int r = 0;
        if (guard < 512)
            for (NodeItem* p : preds[n])
                r = std::max(r, rankOf(p, guard + 1) + 1);
        rank[n] = r;
        return r;
    };
    for (NodeItem* n : nodes)
        rankOf(n, 0);
    for (NodeItem* n : nodes)
        if (preds[n].empty() && !succs[n].empty())
        {
            int lowest = INT_MAX;
            for (NodeItem* s : succs[n])
                lowest = std::min(lowest, rank[s]);
            rank[n] = std::max(rank[n], lowest - 1);
        }

    int columns = 0;
    for (auto& [n, r] : rank)
        columns = std::max(columns, r + 1);
    std::vector<std::vector<NodeItem*>> cols(static_cast<size_t>(columns));
    for (NodeItem* n : nodes)
        cols[size_t(rank[n])].push_back(n);
    for (auto& col : cols)
        std::sort(col.begin(), col.end(), [](NodeItem* a, NodeItem* b) { return a->pos().y() < b->pos().y(); });

    // A few barycentre sweeps: each node moves toward the middle of its neighbours in the column it was sorted against.
    std::map<NodeItem*, double> slot;
    auto number = [&] {
        for (auto& col : cols)
            for (size_t i = 0; i < col.size(); ++i)
                slot[col[i]] = double(i);
    };
    number();
    for (int sweep = 0; sweep < 6; ++sweep)
    {
        bool down = sweep % 2 == 0;
        for (int c = down ? 1 : columns - 2; down ? c < columns : c >= 0; c += down ? 1 : -1)
        {
            std::map<NodeItem*, double> key;
            for (NodeItem* n : cols[size_t(c)])
            {
                const std::vector<NodeItem*>& near = down ? preds[n] : succs[n];
                if (near.empty())
                {
                    key[n] = slot[n];
                    continue;
                }
                double sum = 0.0;
                for (NodeItem* m : near)
                    sum += slot[m];
                key[n] = sum / double(near.size());
            }
            std::stable_sort(cols[size_t(c)].begin(), cols[size_t(c)].end(), [&](NodeItem* a, NodeItem* b) { return key[a] < key[b]; });
            number();
        }
    }

    QRectF before;
    for (NodeItem* n : nodes)
        before = before.united(n->sceneBoundingRect());
    constexpr double kColumnGap = 80.0, kRowGap = 30.0;
    std::vector<double> heights;
    double tallest = 0.0;
    for (auto& col : cols)
    {
        double h = 0.0;
        for (NodeItem* n : col)
            h += (n->reroute ? 2 * NodeItem::kRerouteR : n->height()) + kRowGap;
        heights.push_back(h - kRowGap);
        tallest = std::max(tallest, h - kRowGap);
    }
    std::map<NodeItem*, QPointF> to;
    double x = before.left();
    for (size_t c = 0; c < cols.size(); ++c)
    {
        // Columns centre on the tallest, so wires between them stay short.
        double y = before.top() + (tallest - heights[c]) * 0.5;
        double width = 0.0;
        for (NodeItem* n : cols[c])
        {
            bool dot = n->reroute;
            double h = dot ? 2 * NodeItem::kRerouteR : n->height();
            to[n] = dot ? QPointF(x + kNodeWidth * 0.5, y + NodeItem::kRerouteR) : QPointF(x, y);
            y += h + kRowGap;
            width = std::max(width, dot ? 2 * NodeItem::kRerouteR : kNodeWidth);
        }
        x += std::max(width, kNodeWidth) + kColumnGap;
    }

    UndoChunk chunk("Auto Layout");
    moveTo(to, "Auto Layout");
    // Frames follow their members.
    std::vector<gn::GFrame> frames = mFrames;
    bool framesMoved = false;
    for (gn::GFrame& f : frames)
    {
        QRectF r;
        for (const std::string& m : f.nodes)
            if (auto it = mNodes.find(m); it != mNodes.end() && to.contains(it->second))
            {
                NodeItem* n = it->second;
                r = r.united(QRectF(to[n] - (n->reroute ? QPointF(NodeItem::kRerouteR, NodeItem::kRerouteR) : QPointF()),
                                    n->reroute ? QSizeF(2 * NodeItem::kRerouteR, 2 * NodeItem::kRerouteR) : QSizeF(kNodeWidth, n->height())));
            }
        if (r.isNull() || f.collapsed)
            continue;
        r.adjust(-30, -30 - FrameItem::kTitle, 30, 30);
        f.position = {r.x(), r.y()};
        f.size = {r.width(), r.height()};
        framesMoved = true;
    }
    if (framesMoved && backend->supports("SetFrames"))
        commitFrames(frames);
}
} // namespace lila::ui
