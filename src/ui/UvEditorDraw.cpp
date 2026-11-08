#include "ui/UvEditorInternal.h"
#include "ui/ViewportDisplay.h"

namespace lila::ui
{
void UvView::drawBackground(QPainter& p)
{
    const bool udim = mUdim && !mPackPreview;
    int tilesU = udim ? 10 : 1, tilesV = udim ? 2 : 1;
    for (int tv = 0; tv < tilesV; ++tv)
        for (int tu = 0; tu < tilesU; ++tu)
        {
            QRectF tile(toScreen({double(tu), double(tv + 1)}), toScreen({double(tu + 1), double(tv)}));
            p.fillRect(tile, tu == 0 && tv == 0 ? gray(50) : gray(45));
            if (mDisplay == Display::Checker)
            {
                const int n = uvLive::checkerTiling();
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                        if ((x + y) % 2)
                            p.fillRect(QRectF(toScreen({tu + double(x) / n, tv + double(y + 1) / n}),
                                              toScreen({tu + double(x + 1) / n, tv + double(y) / n})),
                                       gray(64));
            }
            if (udim)
            {
                p.setPen(gray(110));
                p.drawText(tile.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop, QString::number(1001 + tu + 10 * tv));
            }
        }
    if (!mPackPreview)
        mTexPaint.drawBackground(p, rect(), [this](const glm::dvec2& uv) { return toScreen(uv); }, uvOf(), mUdim);
    // Tenths grid inside the tiles, tile borders brighter.
    p.setPen(QPen(gray(58), 0));
    for (int i = 1; i < 10 * tilesU; ++i)
        if (i % 10)
            p.drawLine(toScreen({i / 10.0, 0.0}), toScreen({i / 10.0, double(tilesV)}));
    for (int i = 1; i < 10 * tilesV; ++i)
        if (i % 10)
            p.drawLine(toScreen({0.0, i / 10.0}), toScreen({double(tilesU), i / 10.0}));
    p.setPen(QPen(gray(96), 0));
    for (int i = 0; i <= tilesU; ++i)
        p.drawLine(toScreen({double(i), 0.0}), toScreen({double(i), double(tilesV)}));
    for (int i = 0; i <= tilesV; ++i)
        p.drawLine(toScreen({0.0, double(i)}), toScreen({double(tilesU), double(i)}));
}

void UvView::drawFaces(QPainter& p)
{
    ComponentMode mode = scene::components::mode();
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
    {
        MeshUv& mu = mMeshes[mi];
        if (mu.corner.empty())
            continue;
        if (mDisplay == Display::Distortion && !mu.distortionValid)
        {
            std::vector<uv::FaceDistortion> d = uv::faceDistortion(*mu.mesh, mu.set);
            mu.distortion.resize(mu.faces.size());
            for (size_t i = 0; i < mu.faces.size(); ++i)
                mu.distortion[i] = uv::distortionValue(d[mu.faces[i]]);
            mu.distortionValid = true;
        }
        std::unordered_set<Index> selFaces;
        if (mode == ComponentMode::Face)
            for (uint32_t f : scene::components::selected(mu.node, ComponentMode::Face))
                selFaces.insert(f);
        const bool shaded = mDisplay == Display::Stretch || mDisplay == Display::Islands;
        std::vector<double> stretch;
        if (shaded)
            stretch = faceStretch(mi);
        const bool twoColour = uvLive::twoColourHeatmap();
        p.setPen(Qt::NoPen);
        QPolygonF poly;
        for (size_t fi = 0; fi < mu.faces.size(); ++fi)
        {
            bool sel = selFaces.count(mu.faces[fi]) > 0;
            if (shaded && fi < mu.island.size())
            {
                poly.clear();
                for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                    poly << toScreen(drawnUv(mi, k));
                p.setBrush(mDisplay == Display::Stretch ? stretchColor(stretch[fi], twoColour)
                                                        : gray(70 + (mu.island[fi] * 53) % 120, 210));
                p.drawPolygon(poly);
            }
            if (mDisplay != Display::Distortion && !sel)
                continue;
            poly.clear();
            for (uint32_t k = mu.faceStart[fi]; k < mu.faceStart[fi + 1]; ++k)
                poly << toScreen(drawnUv(mi, k));
            if (mDisplay == Display::Distortion)
            {
                const float t = std::clamp(mu.distortion[fi], 0.0f, 1.0f);
                if (viewportDisplay::monotone())
                {
                    int v = 58 + int(t * 170.0f);
                    p.setBrush(QColor(v, v, v, 220));
                }
                else
                    p.setBrush(QColor(235 - int(t * 15.0f), 235 - int(t * 195.0f), 235 - int(t * 195.0f), 220));
                p.drawPolygon(poly);
            }
            if (sel)
            {
                p.setBrush(gray(200, 90));
                p.drawPolygon(poly);
            }
        }
    }
}

void UvView::drawEdges(QPainter& p)
{
    ComponentMode mode = scene::components::mode();
    QList<QLineF> plain, seams, selected;
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
    {
        const MeshUv& mu = mMeshes[mi];
        std::unordered_set<Index> selEdges;
        if (mode == ComponentMode::Edge)
            for (uint32_t e : scene::components::selected(mu.node, ComponentMode::Edge))
                selEdges.insert(e);
        for (const Segment& s : mu.segments)
        {
            QLineF l(toScreen(drawnUv(mi, s.a)), toScreen(drawnUv(mi, s.b)));
            if (selEdges.count(s.edge))
                selected.push_back(l);
            else if (s.seam)
                seams.push_back(l);
            else
                plain.push_back(l);
        }
    }
    p.setPen(QPen(gray(128), 1.0));
    p.drawLines(plain);
    p.setPen(QPen(gray(232), 1.6));
    p.drawLines(seams);
    p.setPen(QPen(gray(255), 2.6));
    p.drawLines(selected);
}

void UvView::drawPoints(QPainter& p)
{
    ComponentMode mode = scene::components::mode();
    if (mode != ComponentMode::Vertex && mode != ComponentMode::Edge && mode != ComponentMode::Face)
        return;
    for (size_t mi = 0; mi < mMeshes.size(); ++mi)
    {
        const MeshUv& mu = mMeshes[mi];
        std::vector<uint8_t> mask = selectedEntries(mu);
        if (mode == ComponentMode::Vertex && mu.corner.size() < 200000)
        {
            p.setPen(Qt::NoPen);
            p.setBrush(gray(170));
            for (size_t i = 0; i < mu.uv.size(); ++i)
                if (!mask[i])
                    p.drawRect(QRectF(toScreen(drawnUv(mi, i)) - QPointF(1.5, 1.5), QSizeF(3.0, 3.0)));
        }
        p.setPen(QPen(gray(20), 1.0));
        p.setBrush(gray(255));
        for (size_t i = 0; i < mu.uv.size(); ++i)
            if (mask[i])
                p.drawRect(QRectF(toScreen(drawnUv(mi, i)) - QPointF(2.5, 2.5), QSizeF(5.0, 5.0)));
    }
}

glm::dvec2 UvView::currentPivot() const
{
    glm::dvec2 lo, hi;
    return selectionBounds(lo, hi, true) ? (lo + hi) * 0.5 : glm::dvec2(0.5);
}

void UvView::drawManipulator(QPainter& p)
{
    if (mTool == Tool::Select || mTool == Tool::Brush || (mDrag == Handle::None && !hasSelection()))
        return;
    QPointF c = pivotScreen();
    auto col = [&](Handle h, int base) { return gray((mHover == h || mDrag == h) ? 255 : base); };
    const double len = 64.0;
    if (mTool == Tool::Rotate)
    {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(col(Handle::Ring, 200), 2.0));
        p.drawEllipse(c, len * 0.8, len * 0.8);
        if (mDrag == Handle::Ring)
        {
            p.setPen(QPen(gray(255), 1.0, Qt::DashLine));
            p.drawLine(c, c + QPointF(std::cos(mDragAngle), -std::sin(mDragAngle)) * len * 0.8);
        }
    }
    else
    {
        for (auto [h, dir, base] : {std::tuple{Handle::U, QPointF(1, 0), 220}, std::tuple{Handle::V, QPointF(0, -1), 160}})
        {
            QPointF tip = c + dir * len;
            p.setPen(QPen(col(h, base), 2.0));
            p.drawLine(c, tip);
            p.setPen(Qt::NoPen);
            p.setBrush(col(h, base));
            if (mTool == Tool::Move)
            {
                QPointF n(-dir.y(), dir.x());
                p.drawPolygon(QPolygonF() << tip + dir * 10.0 << tip + n * 5.0 << tip - n * 5.0);
            }
            else
                p.drawRect(QRectF(tip - QPointF(5, 5), QSizeF(10, 10)));
            p.setPen(gray(200));
            p.drawText(tip + dir * 14.0 + QPointF(-4, 4), h == Handle::U ? "U" : "V");
        }
    }
    p.setPen(QPen(gray(20), 1.0));
    p.setBrush(col(Handle::Free, 235));
    p.drawRect(QRectF(c - QPointF(6, 6), QSizeF(12, 12)));
}

void UvView::drawStacks(QPainter& p)
{
    if (!mShowStacks || mPackPreview)
        return;
    std::vector<StackBadges> next;
    for (const MeshUv& mu : mMeshes)
    {
        if (!mu.mesh)
            continue;
        auto old = std::find_if(mStacks.begin(), mStacks.end(), [&](const StackBadges& s) { return s.mesh == mu.mesh && s.set == mu.set; });
        if (old != mStacks.end())
        {
            next.push_back(std::move(*old));
            continue;
        }
        StackBadges sb{mu.mesh, mu.set, {}};
        std::span<const glm::vec2> uv = mu.mesh->cornerAttrs().get<glm::vec2>(mu.set);
        for (const auto& stack : uv::stackedIslands(*mu.mesh, mu.set))
        {
            glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
            for (Index f : stack.front())
                mu.mesh->forEachFaceCorner(f, [&](Index c) {
                    lo = glm::min(lo, glm::dvec2(uv[c]));
                    hi = glm::max(hi, glm::dvec2(uv[c]));
                });
            sb.badges.emplace_back((lo + hi) * 0.5, int(stack.size()));
        }
        next.push_back(std::move(sb));
    }
    mStacks = std::move(next);
    QFont f = p.font();
    f.setBold(true);
    p.setFont(f);
    for (const StackBadges& sb : mStacks)
        for (const auto& [at, count] : sb.badges)
        {
            QPointF c = toScreen(at);
            QRectF r(c.x() - 14, c.y() - 9, 28, 18);
            p.setPen(QPen(gray(20), 1.0));
            p.setBrush(gray(225));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(gray(20));
            p.drawText(r, Qt::AlignCenter, QString("x%1").arg(count));
        }
    p.setBrush(Qt::NoBrush);
    f.setBold(false);
    p.setFont(f);
}

void UvView::drawHud(QPainter& p)
{
    p.setPen(gray(200));
    QString tool = mTool == Tool::Select ? "Select (Q)" : mTool == Tool::Move ? "Move (W)" : mTool == Tool::Rotate ? "Rotate (E)" : "Scale (R)";
    if (mTool == Tool::Brush)
    {
        std::string_view b = uv::liveBrushName(uvLive::brush().brush);
        tool = QString("Brush %1  radius %2  strength %3")
                   .arg(QString::fromUtf8(b.data(), qsizetype(b.size())))
                   .arg(int(std::lround(uvLive::brush().radius)))
                   .arg(uvLive::brush().strength, 0, 'f', 2);
    }
    if (uvLive::liveMode())
        tool += "   Live";
    QString meshes;
    bool missing = false;
    for (const MeshUv& mu : mMeshes)
    {
        meshes += (meshes.isEmpty() ? "" : ", ") + QString::fromStdString(mu.name);
        missing |= mu.corner.empty();
    }
    if (meshes.isEmpty())
        meshes = "select a mesh";
    QString text = QString::fromStdString(uv::currentUvSet()) + (mPackPreview ? mPackFailed ? "   Pack Preview failed: real UVs   " : "   Pack Preview (view only)   " : "   ") + meshes + "   " + tool;
    for (const MeshUv& mu : mMeshes)
        if (scene::Node* n = scene::Graph::current().node(mu.node); n && uv::uvSymmetryAxis(*n) >= 0)
        {
            text += QString("   Symmetry %1").arg(QChar('X' + uv::uvSymmetryAxis(*n)));
            break;
        }
    if (missing)
        text += "   (no uvs in this set)";
    p.drawText(QRectF(8, 4, width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, text);
    // Preview positions are not real uvs.
    if (mPackPreview)
        return;
    glm::dvec2 at = toUv(mMousePos);
    p.setPen(gray(140));
    p.drawText(QRectF(8, height() - 22, width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QString("u %1  v %2").arg(at.x, 0, 'f', 3).arg(at.y, 0, 'f', 3));
}

UvView::Handle UvView::handleAt(QPointF s) const
{
    if (mTool == Tool::Select || mTool == Tool::Brush || !hasSelection())
        return Handle::None;
    QPointF c = pivotScreen();
    QPointF d = s - c;
    const double len = 64.0;
    if (std::abs(d.x()) <= 8 && std::abs(d.y()) <= 8)
        return Handle::Free;
    if (mTool == Tool::Rotate)
    {
        double r = std::hypot(d.x(), d.y());
        return std::abs(r - len * 0.8) < 8.0 ? Handle::Ring : Handle::None;
    }
    if (d.x() > 8 && d.x() < len + 14 && std::abs(d.y()) < 7)
        return Handle::U;
    if (-d.y() > 8 && -d.y() < len + 14 && std::abs(d.x()) < 7)
        return Handle::V;
    return Handle::None;
}

void UvView::beginDrag(Handle h)
{
    mDrag = h;
    mAxisLock = 0;
    mPivot = currentPivot();
    mDragMove = glm::dvec2(0.0);
    mDragScale = glm::dvec2(1.0);
    mDragAngle = 0.0;
    mDragMask.clear();
    mDragFollow.clear();
    for (const MeshUv& mu : mMeshes)
    {
        mDragMask.push_back(selectedEntries(mu));
        mDragFollow.push_back(dragFollow(mu, mDragMask.back()));
    }
    mPressUv = toUv(mPressPos);
}

std::vector<std::pair<uint32_t, glm::dmat2>> UvView::dragFollow(const MeshUv& mu, const std::vector<uint8_t>& mask) const
{
    std::vector<std::pair<uint32_t, glm::dmat2>> out;
    const uv::UvMirror* mirror = mirrorOf(mu);
    if (!mirror)
        return out;
    std::vector<Index> moved;
    for (size_t k = 0; k < mask.size(); ++k)
        if (mask[k])
            moved.push_back(mu.corner[k]);
    out.assign(mu.corner.size(), {~0u, glm::dmat2(1.0)});
    for (const uv::UvFollow& r : uv::uvFollowRules(*mirror, moved, uv::uvMirrorDrive(*mirror, moved)))
        if (r.corner < mu.entryOf.size() && r.source < mu.entryOf.size() && mu.entryOf[r.corner] != ~0u && mu.entryOf[r.source] != ~0u)
            out[mu.entryOf[r.corner]] = {mu.entryOf[r.source], r.linear};
    return out;
}

void UvView::updateDrag(Qt::KeyboardModifiers mods)
{
    glm::dvec2 now = toUv(mMousePos), d = now - mPressUv;
    Tool t = mTool == Tool::Select ? Tool::Move : mTool;
    // Shift on a free move or scale locks to whichever axis the drag first heads along, until release.
    QPointF px = mMousePos - mPressPos;
    if (mDrag != Handle::U && mDrag != Handle::V && (mods & Qt::ShiftModifier) && !mAxisLock && std::hypot(px.x(), px.y()) > 4.0)
        mAxisLock = std::abs(px.x()) >= std::abs(px.y()) ? 1 : 2;
    if (!(mods & Qt::ShiftModifier))
        mAxisLock = 0;
    const bool lockU = mDrag == Handle::U || mAxisLock == 1, lockV = mDrag == Handle::V || mAxisLock == 2;
    if (t == Tool::Move)
    {
        mDragMove = lockU ? glm::dvec2(d.x, 0.0) : lockV ? glm::dvec2(0.0, d.y) : d;
        if (mods & Qt::ControlModifier)
        {
            // Snap to sixty-fourths of the tile.
            mDragMove = glm::round(mDragMove * 64.0) / 64.0;
        }
    }
    else if (t == Tool::Rotate)
    {
        glm::dvec2 a = mPressUv - mPivot, b = now - mPivot;
        // The centre handle and middle drags turn by horizontal travel: the press can sit on the pivot.
        if (mDrag == Handle::Free)
            mDragAngle = -glm::radians((mMousePos - mPressPos).x() * 0.5);
        else
            mDragAngle = std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));
        if (mods & Qt::ShiftModifier)
            mDragAngle = std::round(mDragAngle / glm::radians(15.0)) * glm::radians(15.0);
    }
    else
    {
        double su = std::exp(px.x() / 120.0), sv = std::exp(-px.y() / 120.0);
        if (lockU)
            mDragScale = {su, 1.0};
        else if (lockV)
            mDragScale = {1.0, sv};
        else
        {
            double s = std::exp((px.x() - px.y()) / 160.0);
            mDragScale = {s, s};
        }
    }
}

void UvView::commitDrag()
{
    Handle h = mDrag;
    mDrag = Handle::None;
    mDragMask.clear();
    mDragFollow.clear();
    (void)h;
    json call = {{"pivot", {mPivot.x, mPivot.y}}};
    bool any = false;
    if (glm::length(mDragMove) > 0.0)
    {
        call["translate"] = {mDragMove.x, mDragMove.y};
        any = true;
    }
    if (std::abs(mDragAngle) > 1e-9)
    {
        call["rotate"] = glm::degrees(mDragAngle);
        any = true;
    }
    if (mDragScale != glm::dvec2(1.0))
    {
        call["scale"] = {mDragScale.x, mDragScale.y};
        any = true;
    }
    if (any)
        safeRun("uvTransform", call, CallSource::UI);
    update();
}
} // namespace lila::ui
