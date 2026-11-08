#include "ui/UvEditorInternal.h"

namespace lila::ui
{
void UvView::drawPins(QPainter& p)
{
    p.setPen(QPen(gray(10), 1.5));
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
    {
        const MeshUv& mu = mMeshes[mi];
        if (mu.pinned.size() != mu.uv.size())
            continue;
        for (size_t k = 0; k < mu.uv.size(); ++k)
            if (mu.pinned[k])
            {
                QPointF c = toScreen(drawnUv(mi, k));
                p.setBrush(gray(250));
                p.drawEllipse(c, 4.5, 4.5);
                p.setBrush(gray(10));
                p.drawEllipse(c, 1.2, 1.2);
            }
    }
}

QColor UvView::stretchColor(double v, bool twoColour)
{
    v = std::clamp(v, -1.0, 1.0);
    if (!twoColour)
    {
        int g = int(std::lround(128.0 + 105.0 * v));
        return QColor(g, g, g, 225);
    }
    auto mix = [](QColor a, QColor b, double t) {
        return QColor(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                      int(a.blue() + (b.blue() - a.blue()) * t), 225);
    };
    return v < 0.0 ? mix(QColor(128, 128, 128), QColor(45, 70, 150), -v) : mix(QColor(128, 128, 128), QColor(240, 165, 95), v);
}

std::vector<double> UvView::faceStretch(size_t mi)
{
    MeshUv& mu = mMeshes[mi];
    const geo::Mesh& m = *mu.mesh;
    const size_t nf = mu.faces.size();
    if (mu.island.size() != nf)
    {
        std::vector<uint8_t> seams = uv::seamFlags(m);
        uv::ChartOptions co;
        co.source = uv::IslandSource::Uvs;
        co.uvSet = mu.set;
        co.extraSeams = seams;
        std::vector<int> faceIsland(m.faceCapacity(), 0);
        std::vector<std::vector<Index>> islands = uv::islandFaces(m, {}, co);
        for (size_t i = 0; i < islands.size(); ++i)
            for (Index f : islands[i])
                faceIsland[f] = int(i);
        mu.island.resize(nf);
        mu.area3.resize(nf);
        mu.islandRatio.assign(islands.size(), 0.0);
        for (size_t fi = 0; fi < nf; ++fi)
        {
            mu.island[fi] = faceIsland[mu.faces[fi]];
            glm::dvec3 sum(0.0), p0 = m.position(m.cornerPoint(mu.corner[mu.faceStart[fi]]));
            for (uint32_t k = mu.faceStart[fi] + 1; k + 1 < mu.faceStart[fi + 1]; ++k)
                sum += glm::cross(m.position(m.cornerPoint(mu.corner[k])) - p0, m.position(m.cornerPoint(mu.corner[k + 1])) - p0);
            mu.area3[fi] = 0.5 * glm::length(sum);
        }
    }
    std::vector<double> auv(nf, 0.0), sumUv(mu.islandRatio.size(), 0.0), sum3(mu.islandRatio.size(), 0.0);
    for (size_t fi = 0; fi < nf; ++fi)
    {
        double a = 0.0;
        for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
        {
            uint32_t k2 = k + 1 < mu.faceStart[fi + 1] ? k + 1 : mu.faceStart[fi];
            glm::dvec2 p0 = drawnUv(mi, k), p1 = drawnUv(mi, k2);
            a += p0.x * p1.y - p1.x * p0.y;
        }
        auv[fi] = 0.5 * a;
        sumUv[size_t(mu.island[fi])] += auv[fi];
        sum3[size_t(mu.island[fi])] += mu.area3[fi];
    }
    std::vector<double> out(nf, 0.0);
    for (size_t fi = 0; fi < nf; ++fi)
    {
        size_t i = size_t(mu.island[fi]);
        double sign = sumUv[i] < 0.0 ? -1.0 : 1.0, ratio = sum3[i] > 0.0 ? std::abs(sumUv[i]) / sum3[i] : 0.0;
        double a = auv[fi] * sign;
        if (mu.area3[fi] <= 0.0 || ratio <= 0.0)
            continue;
        out[fi] = a <= 0.0 ? -1.0 : std::clamp(std::log2(a / mu.area3[fi] / ratio), -1.0, 1.0);
    }
    return out;
}

void UvView::togglePinAt(QPointF s)
{
    sync();
    double best = 12.0;
    size_t bm = 0, bk = SIZE_MAX;
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        for (size_t k = 0; k < mMeshes[mi].uv.size(); ++k)
        {
            QPointF d = toScreen(drawnUv(mi, k)) - s;
            if (double px = std::hypot(d.x(), d.y()); px < best)
            {
                best = px;
                bm = mi;
                bk = k;
            }
        }
    if (bk == SIZE_MAX)
        return;
    uv::live::Session* sess = uv::live::get(mMeshes[bm].node);
    if (!sess)
        return;
    auto [i, v] = sess->wedge(mMeshes[bm].corner[bk]);
    if (i < 0)
        return;
    bool wake = uvLive::liveMode();
    sess->begin("UV Pin");
    sess->solver().edit([i, v, wake](std::vector<uv::LiveIsland>& is) {
        if (size_t(i) >= is.size())
            return;
        is[size_t(i)].setPinned(v, !is[size_t(i)].pinned(v));
        if (wake)
            is[size_t(i)].wake();
    });
    sess->setStroking(false);
    uvLive::ensureDriver();
}

std::vector<size_t> UvView::partners(size_t mi, size_t si) const
{
    std::vector<size_t> out;
    const MeshUv& mu = mMeshes[mi];
    for (size_t k = 0; k < mu.segments.size(); ++k)
        if (k != si && mu.segments[k].edge == mu.segments[si].edge)
            out.push_back(k);
    return out;
}

bool UvView::armSewDrag(QPointF s)
{
    if (pickMode() != ComponentMode::Edge)
        return false;
    double best = 7.0;
    bool found = false;
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        for (size_t si = 0; si < mMeshes[mi].segments.size(); ++si)
        {
            const Segment& sg = mMeshes[mi].segments[si];
            if (!sg.seam)
                continue;
            double d = segDistance(s, toScreen(mMeshes[mi].uv[sg.a]), toScreen(mMeshes[mi].uv[sg.b]));
            if (d < best && !partners(mi, si).empty())
            {
                best = d;
                mSew = {true, false, mi, si};
                found = true;
            }
        }
    return found;
}

QPointF UvView::segmentMid(size_t mi, size_t si) const
{
    const Segment& sg = mMeshes[mi].segments[si];
    return (toScreen(drawnUv(mi, sg.a)) + toScreen(drawnUv(mi, sg.b))) * 0.5;
}

void UvView::drawSewDrag(QPainter& p)
{
    // Hovering a split edge links its two sides, so the partner is easy to find.
    if (!mSew.dragging && mPreselect.mode == ComponentMode::Edge && !mPreselect.empty())
        for (size_t mi = 0; mi < mMeshes.size(); ++mi)
        {
            if (mMeshes[mi].node != mPreselect.node)
                continue;
            std::vector<size_t> sides;
            for (size_t si = 0; si < mMeshes[mi].segments.size(); ++si)
                if (mMeshes[mi].segments[si].edge == mPreselect.id)
                    sides.push_back(si);
            if (sides.size() == 2)
            {
                QPointF a = segmentMid(mi, sides[0]), b = segmentMid(mi, sides[1]);
                if (std::hypot(a.x() - b.x(), a.y() - b.y()) > 2.0)
                {
                    p.setPen(QPen(gray(200), 1.0, Qt::DashLine));
                    p.drawLine(a, b);
                }
            }
        }
    if (!mSew.dragging)
        return;
    const MeshUv& mu = mMeshes[mSew.mesh];
    for (size_t k : partners(mSew.mesh, mSew.segment))
    {
        const Segment& sg = mu.segments[k];
        QPointF a = toScreen(drawnUv(mSew.mesh, sg.a)), b = toScreen(drawnUv(mSew.mesh, sg.b));
        bool near = segDistance(mMousePos, a, b) <= 25.0;
        p.setPen(QPen(gray(255), near ? 5.0 : 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
    }
    p.setPen(QPen(gray(235), 1.5, Qt::DashLine));
    p.drawLine(segmentMid(mSew.mesh, mSew.segment), mMousePos);
}

void UvView::finishSewDrag(QPointF s)
{
    if (mSew.mesh >= mMeshes.size())
        return;
    const MeshUv& mu = mMeshes[mSew.mesh];
    bool hit = false;
    for (size_t k : partners(mSew.mesh, mSew.segment))
    {
        const Segment& sg = mu.segments[k];
        hit |= segDistance(s, toScreen(mu.uv[sg.a]), toScreen(mu.uv[sg.b])) <= 25.0;
    }
    if (!hit)
        return;
    Index edge = mu.segments[mSew.segment].edge;
    scene::NodeId node = mu.node;
    std::vector<Index> faces;
    mu.mesh->forEachEdgeCorner(edge, [&](Index c) { faces.push_back(mu.mesh->cornerFace(c)); });
    safeRun("uvSew", {{"args", {mu.name}}, {"edges", {edge}}, {"relax", uvLive::liveMode()}}, CallSource::UI);
    uvLive::settleAfter(node, faces, "UV Live Settle");
}

bool UvView::armLiveDrag(QPointF s)
{
    ComponentMode mode = pickMode();
    Hit hit = hitAt(s, mode);
    if (hit.id == geo::kInvalid)
        return false;
    mLiveDrag = {};
    mLiveDrag.armed = true;
    mLiveDrag.mesh = hit.mesh;
    mLiveDrag.mode = mode;
    mLiveDrag.id = hit.id;
    mLiveDrag.island = (mMods & Qt::ShiftModifier) != 0;
    mLiveDrag.pressUv = toUv(s);
    return true;
}

void UvView::startLiveDrag()
{
    LiveDrag& d = mLiveDrag;
    d.started = true;
    if (d.mesh >= mMeshes.size())
        return;
    const MeshUv& mu = mMeshes[d.mesh];
    uv::live::Session* s = uv::live::get(mu.node);
    if (!s)
        return;
    d.node = mu.node;
    const geo::Mesh& m = *mu.mesh;
    std::vector<uint8_t> use(mu.corner.size(), 0);
    if (d.mode == ComponentMode::Vertex)
    {
        const Index grabbed[] = {d.id};
        for (Index c : geo::uvVertexCorners(m, mu.set, grabbed))
            if (c < mu.entryOf.size() && mu.entryOf[c] != ~0u)
                use[mu.entryOf[c]] = 1;
    }
    else if (d.mode == ComponentMode::Edge)
    {
        double best = 1e30;
        const Segment* pick = nullptr;
        for (const Segment& sg : mu.segments)
            if (sg.edge == d.id)
                if (double px = segDistance(mPressPos, toScreen(mu.uv[sg.a]), toScreen(mu.uv[sg.b])); px < best)
                {
                    best = px;
                    pick = &sg;
                }
        if (pick)
            use[pick->a] = use[pick->b] = 1;
    }
    else
        for (size_t fi = 0; fi < mu.faces.size(); ++fi)
            if (mu.faces[fi] == d.id)
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    use[k] = 1;
    // With symmetry the grabbed side drives and its partners follow it mirrored, so only that side is held.
    const uv::UvMirror* mirror = mirrorOf(mu);
    int grabSide = 0;
    if (mirror)
    {
        std::vector<Index> grabbed;
        for (size_t k = 0; k < use.size(); ++k)
            if (use[k])
                grabbed.push_back(mu.corner[k]);
        grabSide = uv::uvMirrorDrive(*mirror, grabbed);
    }
    // Grabbing part of the selection drags the whole selection.
    std::vector<uint8_t> sel = selectedEntries(mu);
    bool inSelection = false;
    for (size_t k = 0; k < use.size(); ++k)
        inSelection |= use[k] && sel[k];
    if (inSelection)
        for (size_t k = 0; k < use.size(); ++k)
            use[k] |= sel[k];
    if (mirror)
        for (size_t k = 0; k < use.size(); ++k)
            if (mirror->side[mu.corner[k]] == -grabSide)
                use[k] = 0;
    std::set<int> islandSet;
    for (size_t k = 0; k < use.size(); ++k)
        if (use[k])
            islandSet.insert(s->wedge(mu.corner[k]).first);
    std::set<std::pair<int, Index>> seen;
    for (size_t k = 0; k < mu.corner.size(); ++k)
    {
        auto w = s->wedge(mu.corner[k]);
        bool take = use[k] || (d.island && islandSet.count(w.first));
        if (w.first < 0 || !take || !seen.insert(w).second)
            continue;
        d.wedges.emplace_back(w.first, w.second, mu.uv[k]);
    }
    d.islands.assign(islandSet.begin(), islandSet.end());
    std::erase(d.islands, -1);
    if (d.wedges.empty())
        return;
    s->begin("UV Live Drag");
    s->setStroking(true);
    updateLiveDrag();
    uvLive::ensureDriver();
}

void UvView::updateLiveDrag()
{
    uv::live::Session* s = uv::live::get(mLiveDrag.node, false);
    if (!s || mLiveDrag.wedges.empty())
        return;
    glm::dvec2 delta = toUv(mMousePos) - mLiveDrag.pressUv;
    s->solver().edit([w = mLiveDrag.wedges, islands = mLiveDrag.islands, delta](std::vector<uv::LiveIsland>& is) {
        for (auto& [i, v, start] : w)
            if (size_t(i) < is.size())
                is[size_t(i)].setTarget(v, start + delta, 1.0);
        for (int i : islands)
            if (size_t(i) < is.size())
                is[size_t(i)].wake();
    });
}

void UvView::endLiveDrag(Qt::KeyboardModifiers mods)
{
    uv::live::Session* s = uv::live::get(mLiveDrag.node, false);
    if (!s)
        return;
    bool pin = mods & Qt::ControlModifier;
    s->solver().edit([w = mLiveDrag.wedges, islands = mLiveDrag.islands, pin](std::vector<uv::LiveIsland>& is) {
        if (pin)
            for (auto& [i, v, start] : w)
                if (size_t(i) < is.size())
                    is[size_t(i)].setPinned(v, true);
        for (int i : islands)
            if (size_t(i) < is.size())
            {
                is[size_t(i)].clearTargets();
                is[size_t(i)].wake();
            }
    });
    s->setStroking(false);
}

bool UvView::brushKey(QKeyEvent* e, bool down)
{
    int k = e->key();
    if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight)
    {
        if (down)
        {
            uvLive::brush().radius = std::clamp(uvLive::brush().radius * (k == Qt::Key_BracketLeft ? 0.8 : 1.25), 2.0, 2000.0);
            uvLive::settingsChanged();
            update();
        }
        return true;
    }
    Adjust kind = Adjust::None;
    if (k == Qt::Key_B || (k == Qt::Key_F && !(e->modifiers() & Qt::ShiftModifier)))
        kind = Adjust::Radius;
    else if (k == Qt::Key_M || (k == Qt::Key_F && (e->modifiers() & Qt::ShiftModifier)))
        kind = Adjust::Strength;
    if (kind == Adjust::None)
        return false;
    if (e->isAutoRepeat())
        return true;
    if (down)
    {
        mArmed = kind;
        mArmedKey = k;
        mArmedUsed = false;
        return true;
    }
    if (k != mArmedKey)
        return true;
    // A tapped F still frames the selection.
    if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
        frameSelection();
    mArmed = Adjust::None;
    mArmedKey = 0;
    return true;
}

void UvView::brushPress(QMouseEvent* e)
{
    if (mArmed != Adjust::None)
    {
        mAdjusting = mArmed;
        mArmedUsed = true;
        mAdjustStart = e->position();
        mAdjustFrom = mAdjusting == Adjust::Radius ? uvLive::brush().radius : uvLive::brush().strength;
        return;
    }
    sync();
    BrushStroke& b = mStroke;
    b = {};
    b.active = true;
    b.brush = (e->modifiers() & Qt::ShiftModifier) ? uv::LiveBrush::Relax : uvLive::brush().brush;
    b.invert = (e->modifiers() & Qt::ControlModifier) != 0;
    b.last = e->position();
    b.lastUv = b.fromUv = toUv(e->position());
    std::string label = "UV " + std::string(uv::liveBrushName(b.brush)) + " Brush";
    for (const MeshUv& mu : mMeshes)
        if (!mu.corner.empty())
            if (uv::live::Session* s = uv::live::get(mu.node))
            {
                s->begin(label);
                s->setStroking(true);
                b.nodes.push_back(mu.node);
            }
    uvLive::ensureDriver();
    // The pin brushes wait for a drag, so a click can toggle one pin instead.
    if (b.brush != uv::LiveBrush::Pin && b.brush != uv::LiveBrush::Unpin)
    {
        dabAt(e->position(), true);
        b.started = true;
    }
}

void UvView::brushMove(QMouseEvent* e)
{
    QPointF pos = e->position();
    if (mAdjusting != Adjust::None)
    {
        double dx = pos.x() - mAdjustStart.x();
        if (mAdjusting == Adjust::Radius)
            uvLive::brush().radius = std::clamp(mAdjustFrom + dx, 2.0, 2000.0);
        else
            uvLive::brush().strength = std::clamp(mAdjustFrom + dx / 300.0, 0.0, 1.0);
        uvLive::settingsChanged();
        return;
    }
    BrushStroke& b = mStroke;
    if (!b.active)
        return;
    if (!b.started)
    {
        QPointF d = pos - mPressPos;
        if (std::abs(d.x()) + std::abs(d.y()) < 3.0)
            return;
        dabAt(b.last, true);
        b.started = true;
    }
    if (b.brush == uv::LiveBrush::Grab)
    {
        dabAt(pos, false);
        b.last = pos;
        return;
    }
    QPointF d = pos - b.last;
    double len = std::hypot(d.x(), d.y()), step = std::max(2.0, uvLive::brush().radius * 0.15);
    int count = int(len / step);
    for (int k = 1; k <= count; ++k)
        dabAt(b.last + d * (k * step / len), false);
    if (count > 0)
        b.last += d * (count * step / len);
}

void UvView::dabAt(QPointF px, bool first)
{
    BrushStroke& b = mStroke;
    uv::LiveDab d;
    d.brush = b.brush;
    d.center = toUv(px);
    d.radius = uvLive::brush().radius / mZoom;
    d.strength = uvLive::brush().strength;
    d.invert = b.invert;
    d.delta = d.center - b.lastUv;
    d.lineFrom = b.fromUv;
    d.first = first;
    b.lastUv = d.center;
    bool wake = uvLive::liveMode();
    for (scene::NodeId node : b.nodes)
        if (uv::live::Session* s = uv::live::get(node, false))
            s->solver().edit([d, wake](std::vector<uv::LiveIsland>& is) {
                for (uv::LiveIsland& i : is)
                    if (i.applyDab(d) && wake)
                        i.wake();
            });
}

void UvView::brushRelease(QMouseEvent* e)
{
    if (mAdjusting != Adjust::None)
    {
        mAdjusting = Adjust::None;
        return;
    }
    BrushStroke b = std::move(mStroke);
    mStroke = {};
    if (!b.active)
        return;
    bool wake = uvLive::liveMode();
    for (scene::NodeId node : b.nodes)
        if (uv::live::Session* s = uv::live::get(node, false))
        {
            if (b.brush == uv::LiveBrush::Grab)
                s->solver().edit([wake](std::vector<uv::LiveIsland>& is) {
                    for (uv::LiveIsland& i : is)
                        if (i.hasTargets())
                        {
                            i.clearTargets();
                            if (wake)
                                i.wake();
                        }
                });
            s->setStroking(false);
        }
    if (!b.started)
        togglePinAt(e->position());
}

void UvView::drawBrush(QPainter& p)
{
    if (mTool != Tool::Brush)
        return;
    QPointF c = mAdjusting != Adjust::None ? mAdjustStart : mMousePos;
    double r = uvLive::brush().radius;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(gray(240), 1.3));
    p.drawEllipse(c, r, r);
    if (mAdjusting == Adjust::Strength)
    {
        p.setPen(QPen(gray(160), 1.0));
        p.drawEllipse(c, r * uvLive::brush().strength, r * uvLive::brush().strength);
    }
}
} // namespace lila::ui
