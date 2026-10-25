#include "brush/BrushUi.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/Overlay.h"
#include "ui/Tools.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lila::brush::ui
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinRadius = 2.0, kMaxRadius = 2000.0;
constexpr const char* kOpenPref = "/brush/strokeOptionsOpen";

bool isViewport(QWidget* w) { return w && w->inherits("lila::ui::ViewportWidget"); }

int64_t nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

void stepRadius(BrushSettings& s, double factor)
{
    if (s.radiusUnit == RadiusUnit::World)
        s.worldRadius = std::clamp(s.worldRadius * factor, 1e-3, 1e6);
    else
        s.radius = std::clamp(s.radius * factor, kMinRadius, kMaxRadius);
}
} // namespace

// ------------------------------------------------------------------------------------------------ BrushControls

class BrushControls::Filter : public QObject
{
public:
    explicit Filter(BrushControls* c) : mControls(c) {}

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::TabletPress:
        case QEvent::TabletMove:
            mControls->tablet(static_cast<QTabletEvent*>(e)->pressure());
            return false;
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride: break;
        default: return false;
        }
        auto* k = static_cast<QKeyEvent*>(e);
        // Shift and Ctrl change what a stroke would do, which the tools' messages and cursors show.
        if (e->type() != QEvent::ShortcutOverride && (k->key() == Qt::Key_Shift || k->key() == Qt::Key_Control))
            lila::ui::viewportTools::requestRedraw();
        QWidget* focus = QApplication::focusWidget();
        if (obj != focus || !isViewport(focus))
            return false;
        const int key = k->key();
        const bool ours = key == Qt::Key_F || key == Qt::Key_B || key == Qt::Key_M ||
                          std::find(mControls->mExtraKeys.begin(), mControls->mExtraKeys.end(), key) != mControls->mExtraKeys.end();
        if (!ours || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)))
            return false;
        if (e->type() == QEvent::ShortcutOverride)
        {
            e->accept();
            return true;
        }
        if (k->isAutoRepeat())
            return true;
        return mControls->key(key, k->modifiers(), e->type() == QEvent::KeyPress);
    }

private:
    BrushControls* mControls;
};

BrushControls::BrushControls(SettingsRef settings, std::function<void()> changed, std::function<void()> finished)
    : mSettings(std::move(settings)), mChanged(std::move(changed)), mFinished(std::move(finished))
{
}

BrushControls::~BrushControls() { deactivate(); }

void BrushControls::activate(std::vector<int> extraKeys, std::function<bool(int, Qt::KeyboardModifiers, bool)> onKey)
{
    mExtraKeys = std::move(extraKeys);
    mOnKey = std::move(onKey);
    if (!mFilter)
    {
        mFilter = std::make_unique<Filter>(this);
        qApp->installEventFilter(mFilter.get());
    }
}

void BrushControls::deactivate()
{
    if (mFilter && qApp)
        qApp->removeEventFilter(mFilter.get());
    mFilter.reset();
    mArmed = mAdjusting = Adjust::None;
}

bool BrushControls::key(int k, Qt::KeyboardModifiers mods, bool down)
{
    Adjust kind = Adjust::None;
    if (k == Qt::Key_B || (k == Qt::Key_F && !(mods & Qt::ShiftModifier)))
        kind = Adjust::Radius;
    else if (k == Qt::Key_M || (k == Qt::Key_F && (mods & Qt::ShiftModifier)))
        kind = Adjust::Strength;
    if (kind == Adjust::None)
        return mOnKey ? mOnKey(k, mods, down) : false;
    if (down)
    {
        mArmed = kind;
        mArmedKey = k;
        mArmedUsed = false;
        return true;
    }
    if (k != mArmedKey)
        return true;
    // A tapped F still frames the selection, as everywhere else.
    if (k == Qt::Key_F && !mArmedUsed && mAdjusting == Adjust::None)
        QTimer::singleShot(0, [] { lila::ui::actions::run(json("viewport.frameSelected")); });
    mArmed = Adjust::None;
    mArmedKey = 0;
    return true;
}

void BrushControls::tablet(double pressure)
{
    mPressure = std::clamp(pressure, 0.0, 1.0);
    mPressureAt = nowMs();
}

double BrushControls::pressure() const { return mPressureAt && nowMs() - mPressureAt < 100 ? mPressure : 1.0; }

StrokeInput BrushControls::input(const QMouseEvent* e) const
{
    return {glm::dvec2(e->position().x(), e->position().y()), pressure()};
}

bool BrushControls::mousePress(const QMouseEvent* e)
{
    const bool right = e->button() == Qt::RightButton;
    if (right)
    {
        if (mArmed == Adjust::None && !lila::ui::tools::softSelectAdjusting())
            return false;
        lila::ui::tools::markSoftSelectAdjusted();
    }
    else if (!lila::ui::tools::brushRadiusDrag(e, mArmed != Adjust::None))
        return false;
    mAdjusting = right ? Adjust::Strength : mArmed != Adjust::None ? mArmed : Adjust::Radius;
    mArmedUsed = true;
    mStart = e->position();
    const BrushSettings& s = mSettings();
    if (e->button() == Qt::MiddleButton)
        mFrom = mAdjusting == Adjust::Strength ? 0.0 : s.radiusUnit == RadiusUnit::World ? 1e-3 : kMinRadius;
    else
        mFrom = mAdjusting == Adjust::Strength ? s.strength : s.radiusUnit == RadiusUnit::World ? s.worldRadius : s.radius;
    return true;
}

bool BrushControls::mouseMove(const QMouseEvent* e)
{
    if (mAdjusting == Adjust::None)
        return false;
    BrushSettings& s = mSettings();
    const double dx = e->position().x() - mStart.x();
    if (mAdjusting == Adjust::Strength)
        s.strength = std::clamp(mFrom + dx / 300.0, 0.0, 1.0);
    else if (s.radiusUnit == RadiusUnit::World)
        s.worldRadius = std::clamp(std::max(mFrom, 1e-3) * std::pow(2.0, dx / 100.0), 1e-3, 1e6);
    else
        s.radius = std::clamp(mFrom + dx, kMinRadius, kMaxRadius);
    if (mChanged)
        mChanged();
    return true;
}

bool BrushControls::mouseRelease(const QMouseEvent* e)
{
    if (mAdjusting == Adjust::None)
        return false;
    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton && e->button() != Qt::RightButton)
        return true;
    mAdjusting = Adjust::None;
    if (mFinished)
        mFinished();
    return true;
}

bool BrushControls::keyPress(const QKeyEvent* e)
{
    if (e->key() != Qt::Key_BracketLeft && e->key() != Qt::Key_BracketRight)
        return false;
    stepRadius(mSettings(), e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25);
    if (mChanged)
        mChanged();
    if (mFinished)
        mFinished();
    return true;
}

// ------------------------------------------------------------------------------------------------ cursor

glm::dvec3 screenRight(const lila::ui::ViewInfo& view, const glm::dmat4& world)
{
    const glm::dvec3 right = glm::dvec3(glm::inverse(view.view)[0]);
    const glm::dvec3 local = glm::dvec3(glm::inverse(world) * glm::dvec4(right, 0.0));
    return glm::length(local) > 0.0 ? glm::normalize(local) : glm::dvec3(1.0, 0.0, 0.0);
}

QPointF ringPosition(const Stroke& stroke, QPointF hover)
{
    if (!stroke.active() || stroke.sampler().mode() != StrokeMode::Freehand || stroke.settings().lazyRadius <= 0.0)
        return hover;
    const glm::dvec2 b = stroke.sampler().brush();
    return {b.x, b.y};
}

void drawCursor(lila::ui::OverlayPainter& p, const lila::ui::ViewInfo& view, const BrushSettings& s, QPointF at,
                const std::optional<CursorSurface>& surface, const CursorOptions& o)
{
    const glm::vec4 rim(0.95f, 0.95f, 0.95f, 1.0f), dim(0.6f, 0.6f, 0.6f, 0.8f);
    if (o.stroke && o.stroke->active())
    {
        const StrokeSampler& sm = o.stroke->sampler();
        const QPointF cursor(sm.cursor().x, sm.cursor().y);
        if (sm.mode() == StrokeMode::Line)
        {
            const QPointF start(sm.start().x, sm.start().y);
            p.line(start, cursor, 1.5f, rim);
            p.disc(start, 3.0f, rim);
        }
        else if (s.lazyRadius > 0.0)
        {
            p.line(at, cursor, 1.0f, dim);
            p.disc(cursor, 2.5f, dim);
        }
    }
    const double roundness = std::clamp(s.roundness, 0.01, 1.0);
    const double angle = s.angle * kPi / 180.0;
    if (surface)
    {
        const double r = s.radiusUnit == RadiusUnit::World ? s.worldRadius : s.radius * view.unitsPerPixel(surface->point);
        const glm::dvec3 n = glm::length(surface->normal) > 0.0 ? glm::normalize(surface->normal) : glm::dvec3(0, 1, 0);
        // The ellipse's long axis follows the tip angle from screen +x, laid into the surface.
        glm::dvec3 right = glm::dvec3(glm::inverse(view.view)[0]);
        right -= n * glm::dot(n, right);
        if (glm::length(right) < 1e-9)
            right = glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
        right = glm::normalize(right);
        const glm::dvec3 u = right * std::cos(angle) + glm::cross(n, right) * std::sin(angle);
        const glm::dvec3 v = glm::cross(n, u);
        auto ring = [&](double radius, double squeeze, int segs) {
            std::vector<QPointF> pts;
            for (int k = 0; k < segs; ++k)
            {
                const double a = 2.0 * kPi * k / segs;
                if (auto q = view.project(surface->point + (u * std::cos(a) + v * (std::sin(a) * squeeze)) * radius))
                    pts.push_back(*q);
            }
            return pts;
        };
        p.polyline(ring(r, 1.0, 64), 1.5f, rim, true);
        if (roundness < 1.0)
            p.polyline(ring(r, roundness, 48), 1.0f, dim, true);
        if (o.strengthRing)
            p.polyline(ring(r * s.strength, 1.0, 48), 1.0f, dim, true);
        if (o.normalTick)
        {
            auto c = view.project(surface->point), tip = view.project(surface->point + n * r * 0.35);
            if (c && tip)
                p.line(*c, *tip, 1.5f, rim);
        }
        return;
    }
    // Off the surface a world radius has no depth to be seen at, so the ring shows the screen radius.
    std::vector<QPointF> pts;
    for (int k = 0; k < 64; ++k)
    {
        const double a = 2.0 * kPi * k / 64;
        pts.push_back(at + QPointF(std::cos(a), std::sin(a)) * s.radius);
    }
    p.polyline(pts, 1.0f, dim, true);
}

// ------------------------------------------------------------------------------------------------ StrokeOptions

StrokeOptions::StrokeOptions(QWidget* parent, SettingsRef settings, std::function<void()> changed)
    : QWidget(parent), mSettings(std::move(settings)), mChanged(std::move(changed))
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    auto* header = new QToolButton(this);
    header->setText("Stroke");
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setAutoRaise(true);
    header->setToolTip("Pressure, stabiliser, line strokes, jitter, tip shape and radial symmetry, shared by every brush");
    col->addWidget(header);
    mBody = new QWidget(this);
    col->addWidget(mBody);
    auto* form = new QFormLayout(mBody);
    form->setContentsMargins(8, 0, 0, 0);

    auto spin = [&](double lo, double hi, double step, int decimals, const char* tip, const QString& suffix = {}) {
        auto* s = new QDoubleSpinBox(mBody);
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        s->setToolTip(tip);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { edited(); });
        return s;
    };
    auto check = [&](const char* label, const char* tip) {
        auto* c = new QCheckBox(label, mBody);
        c->setToolTip(tip);
        connect(c, &QCheckBox::toggled, this, [this] { edited(); });
        return c;
    };

    auto* unitRow = new QHBoxLayout;
    mUnit = new QComboBox(mBody);
    mUnit->addItem("Screen", "screen");
    mUnit->addItem("World", "world");
    mUnit->setToolTip("Screen: the radius stays the same size on screen however far the surface is. World: a fixed size on the surface.");
    mWorldRadius = spin(0.001, 100000.0, 0.5, 3, "Brush radius on the surface, in scene units", " cm");
    unitRow->addWidget(mUnit);
    unitRow->addWidget(mWorldRadius, 1);
    form->addRow("Radius in", unitRow);
    connect(mUnit, &QComboBox::activated, this, [this] { edited(); });

    mFlow = spin(0.0, 1.0, 0.05, 3, "How much each dab adds; strokes that build up do so more slowly below 1");
    form->addRow("Flow", mFlow);

    auto* pressRow = new QHBoxLayout;
    const char* names[] = {"Size", "Opacity", "Flow"};
    for (int k = 0; k < 3; ++k)
    {
        mPressure[k] = check(names[k], "Pen pressure scales this");
        pressRow->addWidget(mPressure[k]);
    }
    pressRow->addStretch(1);
    form->addRow("Pressure", pressRow);
    auto* curveRow = new QHBoxLayout;
    mPressureFloor = spin(0.0, 1.0, 0.05, 2, "What the lightest touch still gives", "");
    mPressureFloor->setPrefix("min ");
    mPressureExp = spin(0.1, 5.0, 0.1, 2, "Pressure curve: below 1 a light touch already reaches far, above 1 it takes a firm press");
    mPressureExp->setPrefix("curve ");
    curveRow->addWidget(mPressureFloor);
    curveRow->addWidget(mPressureExp);
    form->addRow("", curveRow);

    mMode = new QComboBox(mBody);
    mMode->addItem("Freehand", "freehand");
    mMode->addItem("Line", "line");
    mMode->setToolTip("Line: drag from where the line starts to where it ends; the dabs go down on release");
    form->addRow("Stroke", mMode);
    connect(mMode, &QComboBox::activated, this, [this] { edited(); });

    auto* lazyRow = new QHBoxLayout;
    mLazy = spin(0.0, 500.0, 5.0, 0, "Lazy mouse: the brush trails the cursor on a string this long, so strokes come out steady", " px");
    mSmoothing = spin(0.0, 0.95, 0.05, 2, "Eases each move toward the cursor instead of jumping to it");
    mSmoothing->setPrefix("smooth ");
    lazyRow->addWidget(mLazy);
    lazyRow->addWidget(mSmoothing);
    form->addRow("Stabiliser", lazyRow);

    auto* jitterRow = new QHBoxLayout;
    const char* jitterNames[] = {"pos ", "size ", "opac ", "angle "};
    const char* jitterTips[] = {"Scatter each dab by up to this fraction of the radius", "Shrink each dab by up to this fraction",
                                "Lighten each dab by up to this fraction", "Turn each dab's tip by up to this many degrees"};
    for (int k = 0; k < 4; ++k)
    {
        mJitter[k] = spin(0.0, k == 3 ? 180.0 : k == 0 ? 4.0 : 1.0, k == 3 ? 5.0 : 0.05, k == 3 ? 0 : 2, jitterTips[k]);
        mJitter[k]->setPrefix(jitterNames[k]);
        jitterRow->addWidget(mJitter[k]);
    }
    form->addRow("Jitter", jitterRow);

    auto* tipRow = new QHBoxLayout;
    mAngle = spin(-360.0, 360.0, 5.0, 0, "Tip angle, counterclockwise on screen", " deg");
    mRoundness = spin(0.01, 1.0, 0.05, 2, "1 is round; lower squeezes the tip into an ellipse across its angle");
    mRoundness->setPrefix("round ");
    mFollow = check("Follow", "The tip angle turns with the direction of the stroke");
    tipRow->addWidget(mAngle);
    tipRow->addWidget(mRoundness);
    tipRow->addWidget(mFollow);
    form->addRow("Tip", tipRow);

    auto* alphaRow = new QHBoxLayout;
    mAlpha = new QLineEdit(mBody);
    mAlpha->setPlaceholderText("none");
    mAlpha->setToolTip("A grayscale image that shapes the tip (white paints, black doesn't)");
    auto* browse = new QToolButton(mBody);
    browse->setIcon(lila::ui::icon("folder-open"));
    browse->setToolTip("Choose an alpha image");
    auto* clear = new QToolButton(mBody);
    clear->setIcon(lila::ui::icon("x"));
    clear->setToolTip("No alpha");
    alphaRow->addWidget(mAlpha, 1);
    alphaRow->addWidget(browse);
    alphaRow->addWidget(clear);
    form->addRow("Alpha", alphaRow);
    connect(mAlpha, &QLineEdit::editingFinished, this, [this] { edited(); });
    connect(browse, &QToolButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(this, "Brush Alpha", mAlpha->text(), "Images (*.png *.exr *.tga *.tif *.tiff *.psd)");
        if (f.isEmpty())
            return;
        mAlpha->setText(f);
        edited();
    });
    connect(clear, &QToolButton::clicked, this, [this] {
        mAlpha->clear();
        edited();
    });

    auto* radialRow = new QHBoxLayout;
    mRadialCount = new QSpinBox(mBody);
    mRadialCount->setRange(1, 64);
    mRadialCount->setToolTip("Copies of every dab turned evenly about the object's axis; 1 is off");
    mRadialAxis = new QComboBox(mBody);
    mRadialAxis->addItems({"X", "Y", "Z"});
    mRadialAxis->setToolTip("The object axis the copies turn about, through the object's pivot origin");
    radialRow->addWidget(mRadialCount);
    radialRow->addWidget(mRadialAxis);
    radialRow->addStretch(1);
    form->addRow("Radial", radialRow);
    connect(mRadialCount, &QSpinBox::valueChanged, this, [this] { edited(); });
    connect(mRadialAxis, &QComboBox::activated, this, [this] { edited(); });

    const bool open = PrefStore::get().value<bool>("general", kOpenPref, false);
    mBody->setVisible(open);
    header->setIcon(lila::ui::icon(open ? "chevron-down" : "chevron-right"));
    connect(header, &QToolButton::clicked, this, [this, header] {
        const bool show = !mBody->isVisible();
        mBody->setVisible(show);
        header->setIcon(lila::ui::icon(show ? "chevron-down" : "chevron-right"));
        PrefStore::get().set("general", kOpenPref, show);
    });
    sync();
}

void StrokeOptions::sync()
{
    const BrushSettings& s = mSettings();
    std::vector<QSignalBlocker> blocks;
    blocks.reserve(24);
    for (QObject* o : std::initializer_list<QObject*>{mUnit, mWorldRadius, mFlow, mPressure[0], mPressure[1], mPressure[2], mPressureFloor,
                                                      mPressureExp, mMode, mLazy, mSmoothing, mJitter[0], mJitter[1], mJitter[2], mJitter[3],
                                                      mAngle, mFollow, mRoundness, mAlpha, mRadialCount, mRadialAxis})
        blocks.emplace_back(o);
    mUnit->setCurrentIndex(s.radiusUnit == RadiusUnit::World ? 1 : 0);
    mWorldRadius->setValue(s.worldRadius);
    mWorldRadius->setEnabled(s.radiusUnit == RadiusUnit::World);
    mFlow->setValue(s.flow);
    mPressure[0]->setChecked(s.pressureSize.enabled);
    mPressure[1]->setChecked(s.pressureOpacity.enabled);
    mPressure[2]->setChecked(s.pressureFlow.enabled);
    mPressureFloor->setValue(s.pressureSize.floor);
    mPressureExp->setValue(s.pressureSize.exponent);
    mMode->setCurrentIndex(s.mode == StrokeMode::Line ? 1 : 0);
    mLazy->setValue(s.lazyRadius);
    mSmoothing->setValue(s.smoothing);
    mJitter[0]->setValue(s.jitter.position);
    mJitter[1]->setValue(s.jitter.size);
    mJitter[2]->setValue(s.jitter.opacity);
    mJitter[3]->setValue(s.jitter.angle);
    mAngle->setValue(s.angle);
    mFollow->setChecked(s.followStroke);
    mRoundness->setValue(s.roundness);
    mAlpha->setText(QString::fromStdString(s.alphaPath));
    mRadialCount->setValue(s.radial.count);
    mRadialAxis->setCurrentIndex(s.radial.axis);
}

void StrokeOptions::edited()
{
    BrushSettings& s = mSettings();
    s.radiusUnit = mUnit->currentIndex() == 1 ? RadiusUnit::World : RadiusUnit::Screen;
    s.worldRadius = mWorldRadius->value();
    s.flow = mFlow->value();
    PressureCurve* curves[] = {&s.pressureSize, &s.pressureOpacity, &s.pressureFlow};
    for (int k = 0; k < 3; ++k)
    {
        curves[k]->enabled = mPressure[k]->isChecked();
        curves[k]->floor = mPressureFloor->value();
        curves[k]->exponent = mPressureExp->value();
    }
    s.mode = mMode->currentIndex() == 1 ? StrokeMode::Line : StrokeMode::Freehand;
    s.lazyRadius = mLazy->value();
    s.smoothing = mSmoothing->value();
    s.jitter = {mJitter[0]->value(), mJitter[1]->value(), mJitter[2]->value(), mJitter[3]->value()};
    s.angle = mAngle->value();
    s.followStroke = mFollow->isChecked();
    s.roundness = mRoundness->value();
    const std::string alpha = mAlpha->text().trimmed().toStdString();
    if (alpha != s.alphaPath)
    {
        s.alphaPath = alpha;
        s.alpha.reset();
    }
    s.radial = {mRadialCount->value(), mRadialAxis->currentIndex()};
    mWorldRadius->setEnabled(s.radiusUnit == RadiusUnit::World);
    if (mChanged)
        mChanged();
}
} // namespace lila::brush::ui
