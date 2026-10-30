#include "ui/UvEditorInternal.h"

namespace lila::ui
{
bool UvView::insidePoly(const std::vector<QPointF>& poly, QPointF p)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y() > p.y()) != (poly[j].y() > p.y()) &&
            p.x() < (poly[j].x() - poly[i].x()) * (p.y() - poly[i].y()) / (poly[j].y() - poly[i].y()) + poly[i].x())
            in = !in;
    return in;
}

double UvView::segDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double l2 = d.x() * d.x() + d.y() * d.y();
    double t = l2 > 0.0 ? std::clamp(((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t - p;
    return std::hypot(q.x(), q.y());
}

ComponentMode UvView::pickMode() const
{
    ComponentMode m = scene::components::mode();
    return m == ComponentMode::Object || uvLive::islandSelect() ? ComponentMode::Face : m;
}

UvView::Hit UvView::faceAt(QPointF s) const
{
    Hit best;
    std::vector<QPointF> poly;
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
    {
        const MeshUv& mu = mMeshes[mi];
        for (size_t fi = 0; fi < mu.faces.size(); ++fi)
        {
            poly.clear();
            for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                poly.push_back(toScreen(mu.uv[k]));
            if (insidePoly(poly, s))
                best = {mi, mu.faces[fi]};
        }
    }
    return best;
}

UvView::Hit UvView::hitAt(QPointF s, ComponentMode mode) const
{
    Hit hit;
    double bestPx = mode == ComponentMode::Vertex ? 9.0 : 7.0;
    if (mode == ComponentMode::Vertex)
    {
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
            for (size_t k = 0; k < mMeshes[mi].uv.size(); ++k)
            {
                QPointF d = toScreen(mMeshes[mi].uv[k]) - s;
                double px = std::hypot(d.x(), d.y());
                if (px < bestPx)
                {
                    bestPx = px;
                    hit = {mi, mMeshes[mi].mesh->cornerPoint(mMeshes[mi].corner[k])};
                }
            }
    }
    else if (mode == ComponentMode::Edge)
    {
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
            for (const Segment& sg : mMeshes[mi].segments)
            {
                double px = segDistance(s, toScreen(mMeshes[mi].uv[sg.a]), toScreen(mMeshes[mi].uv[sg.b]));
                if (px < bestPx)
                {
                    bestPx = px;
                    hit = {mi, sg.edge};
                }
            }
    }
    else
        hit = faceAt(s);
    return hit;
}

scene::componentPick::Hover UvView::hoverAt(QPointF s) const
{
    ComponentMode mode = pickMode();
    Hit hit = hitAt(s, mode);
    if (hit.id == geo::kInvalid)
        return {};
    return {mode, mMeshes[hit.mesh].node, hit.id};
}

void UvView::setPreselect(const scene::componentPick::Hover& h)
{
    if (h == mPreselect)
        return;
    mPreselect = h;
    update();
}

void UvView::drawPreselect(QPainter& p)
{
    if (mPreselect.empty() || mPreselect.mode != pickMode())
        return;
    auto it = std::find_if(mMeshes.begin(), mMeshes.end(), [&](const MeshUv& m) { return m.node == mPreselect.node; });
    if (it == mMeshes.end() || it->corner.empty())
        return;
    size_t mi = size_t(it - mMeshes.begin());
    const MeshUv& mu = *it;
    const geo::Mesh& m = *mu.mesh;
    const QColor hot = gray(200);
    // With symmetry the mirror image lights up too, since a click selects both.
    Index mirrorId = geo::kInvalid;
    if (const geo::SymmetryMap* sym = symmetryOf(mu))
        mirrorId = mPreselect.mode == ComponentMode::Vertex ? sym->mirrorPoint(mPreselect.id)
                   : mPreselect.mode == ComponentMode::Edge ? sym->mirrorEdge(m, mPreselect.id)
                                                             : sym->mirrorFace(m, mPreselect.id);
    auto lit = [&](Index id) { return id == mPreselect.id || (mirrorId != geo::kInvalid && id == mirrorId); };
    if (mPreselect.mode == ComponentMode::Vertex)
    {
        p.setPen(QPen(gray(20), 1.0));
        p.setBrush(hot);
        for (size_t k = 0; k < mu.uv.size(); ++k)
            if (lit(m.cornerPoint(mu.corner[k])))
                p.drawRect(QRectF(toScreen(drawnUv(mi, k)) - QPointF(4.0, 4.0), QSizeF(8.0, 8.0)));
    }
    else if (mPreselect.mode == ComponentMode::Edge)
    {
        p.setPen(QPen(hot, 3.5, Qt::SolidLine, Qt::RoundCap));
        for (const Segment& sg : mu.segments)
            if (lit(sg.edge))
                p.drawLine(toScreen(drawnUv(mi, sg.a)), toScreen(drawnUv(mi, sg.b)));
    }
    else
    {
        auto hovered = std::find(mu.faces.begin(), mu.faces.end(), mPreselect.id);
        if (hovered == mu.faces.end())
            return;
        const size_t hf = size_t(hovered - mu.faces.begin());
        auto mirrored = mirrorId == geo::kInvalid ? mu.faces.end() : std::find(mu.faces.begin(), mu.faces.end(), mirrorId);
        const size_t hm = mirrored == mu.faces.end() ? hf : size_t(mirrored - mu.faces.begin());
        const bool island = uvLive::islandSelect();
        const std::vector<int>* islands = island ? &pickIslands(mi) : nullptr;
        p.setPen(QPen(hot, island ? 1.0 : 2.0));
        p.setBrush(gray(255, 40));
        for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            if (island ? (*islands)[fi] == (*islands)[hf] || (*islands)[fi] == (*islands)[hm] : fi == hf || fi == hm)
            {
                QPolygonF poly;
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    poly << toScreen(drawnUv(mi, k));
                p.drawPolygon(poly);
            }
    }
}

const std::vector<int>& UvView::pickIslands(size_t mi)
{
    MeshUv& mu = mMeshes[mi];
    if (mu.pickIsland.size() != mu.faces.size())
    {
        std::vector<int> faceIsland(mu.mesh->faceCapacity(), -1);
        std::vector<std::vector<Index>> islands = uv::islandFaces(*mu.mesh, {}, islandOptions(mu));
        for (size_t i = 0; i < islands.size(); ++i)
            for (Index f : islands[i])
                faceIsland[f] = int(i);
        mu.pickIsland.resize(mu.faces.size());
        for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            mu.pickIsland[fi] = faceIsland[mu.faces[fi]];
    }
    return mu.pickIsland;
}

void UvView::pickAt(QPointF s, Qt::KeyboardModifiers mods)
{
    if (uvLive::islandSelect())
    {
        selectIslandAt(s, mods);
        return;
    }
    ComponentMode mode = pickMode();
    Hit hit = hitAt(s, mode);
    std::map<scene::NodeId, std::vector<Index>> hits;
    if (hit.id != geo::kInvalid)
        hits[mMeshes[hit.mesh].node].push_back(hit.id);
    apply(mode, hits, mods);
}

void UvView::pickRect(QRectF r, Qt::KeyboardModifiers mods)
{
    ComponentMode mode = pickMode();
    std::map<scene::NodeId, std::vector<Index>> hits;
    for (const MeshUv& mu : mMeshes)
    {
        if (mu.corner.empty())
            continue;
        const geo::Mesh& m = *mu.mesh;
        std::vector<Index>& out = hits[mu.node];
        if (mode == ComponentMode::Vertex)
        {
            for (size_t k = 0; k < mu.uv.size(); ++k)
                if (r.contains(toScreen(mu.uv[k])))
                    out.push_back(m.cornerPoint(mu.corner[k]));
        }
        else if (mode == ComponentMode::Edge)
        {
            for (const Segment& sg : mu.segments)
                if (r.contains(toScreen(mu.uv[sg.a])) && r.contains(toScreen(mu.uv[sg.b])))
                    out.push_back(sg.edge);
        }
        else if (uvLive::islandSelect())
        {
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    if (r.contains(toScreen(mu.uv[k])))
                    {
                        out.push_back(mu.faces[fi]);
                        break;
                    }
            out = uv::islandsOfFaces(m, out, islandOptions(mu));
        }
        else
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            {
                glm::dvec2 c(0.0);
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    c += mu.uv[k];
                c /= double(mu.faceStart[fi + 1] - mu.faceStart[fi]);
                if (r.contains(toScreen(c)))
                    out.push_back(mu.faces[fi]);
            }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        if (out.empty())
            hits.erase(mu.node);
    }
    apply(mode, hits, mods);
}

uv::ChartOptions UvView::islandOptions(const MeshUv& mu)
{
    uv::ChartOptions co;
    co.source = uv::IslandSource::Uvs;
    co.uvSet = mu.set;
    return co;
}

void UvView::selectIslandAt(QPointF s, Qt::KeyboardModifiers mods)
{
    Hit hit = faceAt(s);
    std::map<scene::NodeId, std::vector<Index>> hits;
    if (hit.id != geo::kInvalid)
    {
        const MeshUv& mu = mMeshes[hit.mesh];
        const Index seed[] = {hit.id};
        hits[mu.node] = uv::islandsOfFaces(*mu.mesh, seed, islandOptions(mu));
    }
    // A click on empty space in island mode clears, like the other modes; a double-click elsewhere does nothing.
    if (!hits.empty() || uvLive::islandSelect())
        apply(ComponentMode::Face, hits, mods);
}

void UvView::selectLoopAt(QPointF s, Qt::KeyboardModifiers mods)
{
    Hit hit = hitAt(s, ComponentMode::Edge);
    if (hit.id == geo::kInvalid)
        return;
    const MeshUv& mu = mMeshes[hit.mesh];
    const scene::isolation::Visibility vis = scene::isolation::activeVisibility(mu.node, *mu.mesh);
    apply(ComponentMode::Edge, {{mu.node, scene::isolation::clipLoop(*mu.mesh, vis, geo::edgeLoop(*mu.mesh, hit.id), hit.id, ComponentMode::Edge, false)}},
          mods);
}

std::optional<bool> UvView::selectedUnder(QPointF s) const
{
    ComponentMode mode = pickMode();
    Hit hit = hitAt(s, mode);
    if (hit.id == geo::kInvalid)
        return std::nullopt;
    const scene::components::Ids& ids = scene::components::selected(mMeshes[hit.mesh].node, mode);
    return std::ranges::find(ids, hit.id) != ids.end();
}

std::map<scene::NodeId, scene::components::Ids> UvView::componentsUnder(QPointF s, double radiusPx)
{
    ComponentMode mode = pickMode();
    std::map<scene::NodeId, scene::components::Ids> out;
    std::vector<QPointF> poly;
    for (const MeshUv& mu : mMeshes)
    {
        if (mu.corner.empty())
            continue;
        const geo::Mesh& m = *mu.mesh;
        scene::components::Ids ids;
        if (mode == ComponentMode::Vertex)
        {
            for (size_t k = 0; k < mu.uv.size(); ++k)
                if (QPointF d = toScreen(mu.uv[k]) - s; std::hypot(d.x(), d.y()) <= radiusPx)
                    ids.push_back(m.cornerPoint(mu.corner[k]));
        }
        else if (mode == ComponentMode::Edge)
        {
            for (const Segment& sg : mu.segments)
                if (segDistance(s, toScreen(mu.uv[sg.a]), toScreen(mu.uv[sg.b])) <= radiusPx)
                    ids.push_back(sg.edge);
        }
        else
            for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            {
                poly.clear();
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    poly.push_back(toScreen(mu.uv[k]));
                bool near = insidePoly(poly, s);
                for (size_t i = 0; i < poly.size() && !near; ++i)
                    near = segDistance(s, poly[i], poly[(i + 1) % poly.size()]) <= radiusPx;
                if (near)
                    ids.push_back(mu.faces[fi]);
            }
        if (!ids.empty() && mode == ComponentMode::Face && uvLive::islandSelect())
            ids = uv::islandsOfFaces(m, ids, islandOptions(mu));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        if (!ids.empty())
            out[mu.node] = std::move(ids);
    }
    return out;
}

void UvView::drawTabBadges(QPainter& p)
{
    ViewportState st;
    st.cursor = mMousePos;
    if (mTabStroke.active())
        st.tabSelects = mTabStroke.op() != paintSelect::Stroke::Op::Remove;
    else if (mTabHeld && mCursorInside && tabSelectable())
    {
        Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
        st.tabSelects =
            tabSelect::modeAtPress(selectedUnder(mMousePos), mods & Qt::ShiftModifier, mods & Qt::ControlModifier) == tabSelect::Mode::Select;
    }
    std::vector<const cursorBadges::Badge*> badges;
    if (st.tabSelects)
        for (const cursorBadges::Badge* b : cursorBadges::active(st))
            if (b->id.starts_with("tabSelect."))
                badges.push_back(b);
    cursorBadges::draw(p, mMousePos, badges);
    cursorBadges::publish(this, badges);
}

void UvView::tabPaint(QPointF a, QPointF b)
{
    const double r = paintSelect::tabRadius(pickMode());
    QPointF d = b - a;
    double len = std::hypot(d.x(), d.y());
    int steps = std::max(1, int(std::ceil(len / std::max(1.0, r * 0.5))));
    for (int i = len > 0.0 ? 1 : 0; i <= steps; ++i)
        mTabStroke.touch(componentsUnder(a + d * (double(i) / steps), r));
    update();
}

void UvView::apply(ComponentMode mode, const std::map<scene::NodeId, std::vector<Index>>& hits, Qt::KeyboardModifiers mods)
{
    scene::Graph& g = scene::Graph::current();
    bool shift = mods & Qt::ShiftModifier, ctrl = mods & Qt::ControlModifier;
    const char* flag = mode == ComponentMode::Vertex ? "vertices" : mode == ComponentMode::Edge ? "edges" : "faces";
    UndoChunk chunk("Select UVs");
    if (hits.empty() && !shift && !ctrl)
    {
        for (const MeshUv& mu : mMeshes)
            if (g.node(mu.node))
            {
                safeRun("selectComponents", {{"args", {mu.name}}, {flag, json::array()}}, CallSource::UI);
                break;
            }
        return;
    }
    bool first = true;
    for (auto& [id, picked] : hits)
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        const std::vector<Index> ids = withMirror(id, mode, picked);
        json call = {{"args", {n->path()}}, {flag, ids}};
        if (shift && ctrl)
            call["add"] = true;
        else if (ctrl)
            call["deselect"] = true;
        else if (shift)
            call["toggle"] = true;
        else if (!first)
            call["add"] = true;
        first = false;
        safeRun("selectComponents", call, CallSource::UI);
    }
}
} // namespace lila::ui
