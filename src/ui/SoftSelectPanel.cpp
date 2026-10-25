#include "ui/SoftSelectPanel.h"

#include "core/PrefStore.h"
#include "ui/Deletion.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"

#include <QActionGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui
{
namespace ss = scene::softSelect;

namespace
{
constexpr double kKeyHitPx = 7.0;

QPainterPath curvePath(const ss::Curve& c, const QRectF& r, int samples)
{
    QPainterPath path;
    for (int i = 0; i <= samples; ++i)
    {
        double t = double(i) / samples;
        QPointF p(r.left() + t * r.width(), r.bottom() - c.evaluate(t) * r.height());
        if (i == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    return path;
}

QIcon curveIcon(const ss::Curve& c)
{
    const QSize size(40, 26);
    const qreal dpr = 2.0;
    QPixmap pm(size * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(3, 3, size.width() - 6, size.height() - 6);
    p.setPen(QPen(themeShade(90), 1.0));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    QPainterPath line = curvePath(c, r, 48);
    QPainterPath fill = line;
    fill.lineTo(r.bottomRight());
    fill.lineTo(r.bottomLeft());
    fill.closeSubpath();
    p.fillPath(fill, themeShade(255, 36));
    p.setPen(QPen(themeShade(215), 1.4));
    p.drawPath(line);
    return QIcon(pm);
}

const char* interpLabel(ss::Interp i)
{
    switch (i)
    {
    case ss::Interp::Linear: return "Linear";
    case ss::Interp::Smooth: return "Smooth";
    case ss::Interp::Flat: return "Flat (step)";
    case ss::Interp::Spline: break;
    }
    return "Spline";
}

constexpr ss::Interp kInterps[] = {ss::Interp::Linear, ss::Interp::Smooth, ss::Interp::Spline, ss::Interp::Flat};
} // namespace

FalloffCurveEditor::FalloffCurveEditor(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(110);
    setToolTip("Falloff from the selection (left) to the radius (right).\nDrag keys, click to add, Delete or right-click a key to "
               "delete it, right-click to set its interpolation, double-click to reset.");
    deletion::claim(this, [this] {
        if (mHover >= 0 && mDrag < 0 && mCurve.keys().size() > 2)
        {
            mCurve.removeKey(mHover);
            mHover = -1;
            edited();
        }
    });
}

void FalloffCurveEditor::setCurve(const ss::Curve& c)
{
    mCurve = c;
    mDrag = -1;
    mHover = -1;
    update();
}

void FalloffCurveEditor::setDrawMode(bool on)
{
    if (mDrawMode == on)
        return;
    mDrawMode = on;
    mStroke.clear();
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
    if (onDrawModeChanged)
        onDrawModeChanged(on);
}

QRectF FalloffCurveEditor::plotRect() const
{
    return QRectF(rect()).adjusted(7, 7, -7, -7);
}

QPointF FalloffCurveEditor::toScreen(double pos, double value) const
{
    QRectF r = plotRect();
    return {r.left() + pos * r.width(), r.bottom() - value * r.height()};
}

glm::dvec2 FalloffCurveEditor::toCurve(const QPointF& p) const
{
    QRectF r = plotRect();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0)};
}

int FalloffCurveEditor::keyAt(const QPointF& p) const
{
    int best = -1;
    double bestD = kKeyHitPx;
    for (int i = 0; i < int(mCurve.keys().size()); ++i)
    {
        const ss::Key& k = mCurve.keys()[i];
        QPointF d = toScreen(k.pos, k.value) - p;
        if (double len = std::hypot(d.x(), d.y()); len <= bestD)
        {
            bestD = len;
            best = i;
        }
    }
    return best;
}

void FalloffCurveEditor::edited()
{
    update();
    if (onEdited)
        onEdited(mCurve);
}

void FalloffCurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), themeShade(34));
    QRectF r = plotRect();
    p.setPen(QPen(themeShade(56), 1.0));
    for (int i = 0; i <= 4; ++i)
    {
        double x = r.left() + r.width() * i / 4.0, y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    QPainterPath line = curvePath(mCurve, r, std::max(64, int(r.width())));
    QPainterPath fill = line;
    fill.lineTo(r.bottomRight());
    fill.lineTo(r.bottomLeft());
    fill.closeSubpath();
    p.fillPath(fill, themeShade(255, 28));
    p.setPen(QPen(themeShade(205), 1.5));
    p.drawPath(line);

    if (!mStroke.empty())
    {
        QPainterPath s;
        for (size_t i = 0; i < mStroke.size(); ++i)
        {
            QPointF q = toScreen(mStroke[i].x, mStroke[i].y);
            i == 0 ? s.moveTo(q) : s.lineTo(q);
        }
        p.setPen(QPen(themeShade(255, 170), 1.5, Qt::DashLine));
        p.drawPath(s);
    }

    if (!mDrawMode)
        for (int i = 0; i < int(mCurve.keys().size()); ++i)
        {
            const ss::Key& k = mCurve.keys()[i];
            QPointF c = toScreen(k.pos, k.value);
            bool hot = i == mDrag || i == mHover;
            p.setPen(QPen(themeShade(20), 1.0));
            p.setBrush(hot ? themeShade(255) : themeShade(170));
            double h = hot ? 4.0 : 3.0;
            // Shape tells the interpolation apart without colour: square linear/flat, round smooth/spline.
            if (k.interp == ss::Interp::Linear || k.interp == ss::Interp::Flat)
                p.drawRect(QRectF(c.x() - h, c.y() - h, 2 * h, 2 * h));
            else
                p.drawEllipse(c, h, h);
        }
    if (mDrawMode)
    {
        p.setPen(themeShade(140));
        p.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignRight, "Draw");
    }
}

void FalloffCurveEditor::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(e);
    if (mDrawMode)
    {
        mStroke = {toCurve(e->position())};
        update();
        return;
    }
    mDrag = keyAt(e->position());
    if (mDrag < 0)
    {
        glm::dvec2 at = toCurve(e->position());
        ss::Interp interp = ss::Interp::Spline;
        for (const ss::Key& k : mCurve.keys())
            if (k.pos <= at.x)
                interp = k.interp;
        mDrag = mCurve.addKey({at.x, at.y, interp});
        edited();
    }
    update();
}

void FalloffCurveEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (!mStroke.empty())
    {
        mStroke.push_back(toCurve(e->position()));
        update();
        return;
    }
    if (mDrag >= 0)
    {
        glm::dvec2 at = toCurve(e->position());
        mDrag = mCurve.moveKey(mDrag, at.x, at.y);
        edited();
        return;
    }
    int hover = mDrawMode ? -1 : keyAt(e->position());
    if (hover != mHover)
    {
        mHover = hover;
        update();
    }
}

void FalloffCurveEditor::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(e);
    if (!mStroke.empty())
    {
        std::vector<glm::dvec2> stroke = std::move(mStroke);
        mStroke.clear();
        // A click without a drag leaves the curve alone.
        if (stroke.size() >= 3)
        {
            // Tolerance in curve units, about two pixels.
            mCurve = ss::curveFromStroke(stroke, 2.0 / std::max(20.0, plotRect().height()));
            edited();
        }
        update();
        return;
    }
    mDrag = -1;
    update();
}

void FalloffCurveEditor::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    mDrag = -1;
    mStroke.clear();
    mCurve = ss::Curve();
    edited();
}

void FalloffCurveEditor::contextMenuEvent(QContextMenuEvent* e)
{
    mDrag = -1;
    QMenu menu(this);
    int key = mDrawMode ? -1 : keyAt(e->pos());
    if (key >= 0)
    {
        QAction* del = menu.addAction(icon("trash"), "Delete Key", [this, key] {
            mCurve.removeKey(key);
            mHover = -1;
            edited();
        });
        del->setEnabled(mCurve.keys().size() > 2);
        menu.addSection("Interpolation");
        auto* group = new QActionGroup(&menu);
        for (ss::Interp i : kInterps)
        {
            QAction* a = menu.addAction(interpLabel(i), [this, key, i] {
                mCurve.setInterp(key, i);
                edited();
            });
            a->setCheckable(true);
            a->setChecked(mCurve.keys()[key].interp == i);
            group->addAction(a);
        }
    }
    else
    {
        QAction* draw = menu.addAction(icon("scribble"), "Draw Curve", [this](bool on) { setDrawMode(on); });
        draw->setCheckable(true);
        draw->setChecked(mDrawMode);
        menu.addAction(icon("refresh"), "Reset", [this] {
            mCurve = ss::Curve();
            edited();
        });
        QMenu* all = menu.addMenu("Set All Keys To");
        for (ss::Interp i : kInterps)
            all->addAction(interpLabel(i), [this, i] {
                for (int k = 0; k < int(mCurve.keys().size()); ++k)
                    mCurve.setInterp(k, i);
                edited();
            });
    }
    menu.exec(e->globalPos());
}

void FalloffCurveEditor::leaveEvent(QEvent*)
{
    mHover = -1;
    update();
}

SoftSelectPanel::SoftSelectPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 0, 2, 2);
    v->setSpacing(3);

    mPresetBox = new QWidget(this);
    mPresets = new FlowLayout(2);
    mPresets->setContentsMargins(0, 0, 0, 0);
    mPresetBox->setLayout(mPresets);
    for (const ss::Preset& p : ss::builtinPresets())
        mPresets->addWidget(thumbnail(p, mPresetBox, false));
    v->addWidget(mPresetBox);

    mEditor = new FalloffCurveEditor(this);
    mEditor->onEdited = [this](const ss::Curve& c) {
        ss::setCurrentCurve(c);
        markCurrentPreset();
    };
    v->addWidget(mEditor);

    auto* tools = new QWidget(this);
    auto* h = new QHBoxLayout(tools);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto tool = [&](const char* ic, const QString& tip) {
        auto* b = new QToolButton(tools);
        b->setIcon(icon(ic));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        h->addWidget(b);
        return b;
    };
    mDraw = tool("scribble", "Draw the falloff freehand");
    mDraw->setCheckable(true);
    connect(mDraw, &QToolButton::toggled, this, [this](bool on) { mEditor->setDrawMode(on); });
    mEditor->onDrawModeChanged = [this](bool on) {
        QSignalBlocker block(mDraw);
        mDraw->setChecked(on);
    };
    connect(tool("refresh", "Reset to the default smooth falloff"), &QToolButton::clicked, this, [] { ss::setCurrentCurve(ss::Curve()); });
    connect(tool("device-floppy", "Save the curve as a preset"), &QToolButton::clicked, this, [this] {
        bool ok = false;
        QString name = QInputDialog::getText(this, "Save Falloff Preset", "Name:", QLineEdit::Normal, "", &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        if (ss::findBuiltin(name.toStdString()))
            name += " (custom)";
        ss::saveUserPreset(name.toStdString(), mEditor->curve());
    });
    h->addStretch(1);
    auto* distance = new QLabel("Distance", tools);
    h->addWidget(distance);
    mMode = new QComboBox(tools);
    onMiddleClickReset(distance, [] { PrefStore::get().set("general", ss::kFalloffMode, std::string("volume")); }, "Volume", mMode);
    mMode->addItem("Volume", "volume");
    mMode->addItem("Surface", "surface");
    mMode->setToolTip("Measure the falloff straight through space, or along the mesh's edges");
    connect(mMode, &QComboBox::activated, this,
            [this](int) { PrefStore::get().set("general", ss::kFalloffMode, mMode->currentData().toString().toStdString()); });
    h->addWidget(mMode);
    v->addWidget(tools);

    mUserBox = new QWidget(this);
    mUser = new FlowLayout(2);
    mUser->setContentsMargins(0, 0, 0, 0);
    mUserBox->setLayout(mUser);
    v->addWidget(mUserBox);

    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(this, [this] { syncFromPrefs(); }, Qt::QueuedConnection);
    });
    syncFromPrefs();
}

SoftSelectPanel::~SoftSelectPanel()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* SoftSelectPanel::thumbnail(const ss::Preset& p, QWidget* parent, bool user)
{
    auto* b = new QToolButton(parent);
    b->setIcon(curveIcon(p.curve));
    b->setIconSize(QSize(40, 26));
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolTip(QString::fromStdString(p.name) + (user ? "\nRight-click to rename or delete" : ""));
    ss::Curve curve = p.curve;
    connect(b, &QToolButton::clicked, this, [this, curve] {
        ss::setCurrentCurve(curve);
        mEditor->setCurve(curve);
        markCurrentPreset();
    });
    if (user)
    {
        std::string name = p.name;
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QToolButton::customContextMenuRequested, this, [this, b, name](const QPoint& at) {
            QMenu menu;
            menu.addAction(icon("edit"), "Rename...", [this, name] {
                bool ok = false;
                QString to = QInputDialog::getText(this, "Rename Falloff Preset", "Name:", QLineEdit::Normal, QString::fromStdString(name), &ok)
                                 .trimmed();
                if (ok && !to.isEmpty() && !ss::findBuiltin(to.toStdString()))
                    ss::renameUserPreset(name, to.toStdString());
            });
            menu.addAction(icon("device-floppy"), "Overwrite With Current Curve", [this, name] { ss::saveUserPreset(name, mEditor->curve()); });
            menu.addAction(icon("trash"), "Delete", [name] { ss::deleteUserPreset(name); });
            menu.exec(b->mapToGlobal(at));
        });
    }
    mThumbs.push_back({b, curve});
    return b;
}

void SoftSelectPanel::rebuildUserPresets()
{
    std::erase_if(mThumbs, [this](const auto& t) { return t.first->parentWidget() == mUserBox; });
    while (QLayoutItem* item = mUser->takeAt(0))
    {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    std::vector<ss::Preset> presets = ss::userPresets();
    for (const ss::Preset& p : presets)
        mUser->addWidget(thumbnail(p, mUserBox, true));
    mUserBox->setVisible(!presets.empty());
}

void SoftSelectPanel::syncFromPrefs()
{
    PrefStore& prefs = PrefStore::get();
    json users = prefs.value("general", ss::kUserPresets, json::array());
    if (users != mUserJson)
    {
        mUserJson = users;
        rebuildUserPresets();
    }
    if (!mEditor->editing())
    {
        ss::Curve c = ss::currentCurve();
        if (!(c == mEditor->curve()))
            mEditor->setCurve(c);
    }
    {
        QSignalBlocker block(mMode);
        mMode->setCurrentIndex(ss::surfaceFalloff() ? 1 : 0);
    }
    markCurrentPreset();
}

void SoftSelectPanel::markCurrentPreset()
{
    bool marked = false;
    for (auto& [b, c] : mThumbs)
    {
        // Only one lights up even when a saved preset duplicates a built-in.
        bool on = !marked && c == mEditor->curve();
        marked |= on;
        QSignalBlocker block(b);
        b->setChecked(on);
    }
}
} // namespace lila::ui
