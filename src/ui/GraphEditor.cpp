#include "ui/GraphEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/AttrView.h"
#include "ui/Deletion.h"
#include "ui/GraphCurveView.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <cmath>

namespace lila::ui
{
using namespace graphed;
using scene::Graph;

namespace
{
GraphEditor*& currentGraphEditor()
{
    static GraphEditor* e = nullptr;
    return e;
}

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCheckable(checkable);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QFrame* separator(QWidget* parent)
{
    auto* s = new QFrame(parent);
    s->setFrameShape(QFrame::VLine);
    s->setFixedWidth(9);
    s->setStyleSheet(QString("color: %1;").arg(gray(60).name()));
    return s;
}

/** 0..3 from the tool row, then the views, pivots, quantities and rotation displays: names for prefs. */
constexpr const char* kViewNames[] = {"absolute", "stacked", "normalized"};
constexpr const char* kToolNames[] = {"move", "rotate", "scale"};
constexpr const char* kPivotNames[] = {"selection", "click", "currentFrame", "firstKey", "lastKey"};
constexpr const char* kQuantityNames[] = {"channels", "worldSpeed", "worldAngularSpeed"};
constexpr const char* kRotationNames[] = {"euler", "quaternion", "both"};

template <size_t N> int indexOf(const char* const (&names)[N], const std::string& name, int fallback)
{
    for (size_t i = 0; i < N; ++i)
        if (name == names[i])
            return int(i);
    return fallback;
}

QString unitOf(const scene::Node& curve)
{
    const std::string& t = curve.type().name;
    if (t == "animCurveTA" || t == "animCurveRQ")
        return QStringLiteral("°");
    if (t == "animCurveTL")
        return QStringLiteral("cm");
    return {};
}

/** "+=5", "-=5", "*=2", "/=2" or a plain number: the operator ('=' for plain) and the number. */
std::optional<std::pair<char, double>> parseEntry(QString text)
{
    text = text.trimmed();
    char op = '=';
    if (text.size() > 2 && text[1] == '=' && QString("+-*/").contains(text[0]))
    {
        op = char(text[0].toLatin1());
        text = text.mid(2).trimmed();
    }
    bool ok = false;
    double x = text.toDouble(&ok);
    if (!ok || (op == '/' && x == 0.0))
        return std::nullopt;
    return std::pair{op, x};
}

const char* const kTweenModes[] = {"neighbour", "ease", "pushPull", "smooth", "default"};
} // namespace

GraphEditor::GraphEditor(QWidget* parent) : QWidget(parent)
{
    currentGraphEditor() = this;
    setProperty("hotkeyContext", "graphEditor");
    deletion::claim(this, [this] { deleteSelectedKeys(); });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    mView = new CurveView(this);

    // Settings first, so the toolbar is built showing them.
    {
        PrefStore& prefs = PrefStore::get();
        ViewSettings& s = mView->settings;
        s.view = ViewMode(indexOf(kViewNames, prefs.value<std::string>("graphEditor", "/view", ""), 0));
        s.tool = Tool(indexOf(kToolNames, prefs.value<std::string>("graphEditor", "/tool", ""), 0));
        s.pivot = Pivot(indexOf(kPivotNames, prefs.value<std::string>("graphEditor", "/pivot", ""), 0));
        s.quantity = Quantity(indexOf(kQuantityNames, prefs.value<std::string>("graphEditor", "/quantity", ""), 0));
        s.rotation = RotationDisplay(indexOf(kRotationNames, prefs.value<std::string>("graphEditor", "/rotation", ""), 0));
        s.falloff = prefs.value<bool>("graphEditor", "/falloff", false);
        s.falloffFrames = std::clamp(prefs.value<double>("graphEditor", "/falloffFrames", 6.0), 0.5, 1000.0);
        s.speedStrip = prefs.value<bool>("graphEditor", "/speedStrip", false);
        s.accelStrip = prefs.value<bool>("graphEditor", "/accelerationStrip", false);
        s.targetSpeed = std::max(0.0, prefs.value<double>("graphEditor", "/targetSpeed", 0.0));
        s.showBuffer = prefs.value<bool>("graphEditor", "/showBuffer", true);
    }

    auto* bar = new QHBoxLayout();
    bar->setSpacing(2);

    const char* toolIcons[] = {"arrows-move", "rotate-2", "resize"};
    const QString toolTips[] = {"Move keys (W). Drag the box's edges to scale the selection.",
                                "Rotate tangents (E): turns each selected key's handles about the key.",
                                "Scale keys (R) about the pivot. Drag sideways for time, up and down for value."};
    for (int i = 0; i < 3; ++i)
    {
        mToolButtons[i] = toolButton(this, toolIcons[i], toolTips[i], true);
        connect(mToolButtons[i], &QToolButton::clicked, this, [this, i] { setTool(i); });
        bar->addWidget(mToolButtons[i]);
    }
    auto* pivot = toolButton(this, "focus-centered", "Scale pivot");
    pivot->setPopupMode(QToolButton::InstantPopup);
    auto* pivotMenu = new QMenu(pivot);
    auto* pivotGroup = new QActionGroup(pivotMenu);
    const char* pivotLabels[] = {"Selection Centre", "Click Point", "Current Frame", "First Key", "Last Key"};
    for (int i = 0; i < 5; ++i)
    {
        QAction* a = pivotMenu->addAction(pivotLabels[i], this, [this, i] {
            mView->settings.pivot = Pivot(i);
            saveSettings();
        });
        a->setCheckable(true);
        a->setChecked(int(mView->settings.pivot) == i);
        pivotGroup->addAction(a);
    }
    pivot->setMenu(pivotMenu);
    bar->addWidget(pivot);
    mFalloff = toolButton(this, "ripple", "Falloff (B): keys near the selection follow value changes, less with distance", true);
    connect(mFalloff, &QToolButton::clicked, this, [this] { toggleFalloff(); });
    bar->addWidget(mFalloff);
    mFalloffRadius = new QDoubleSpinBox(this);
    mFalloffRadius->setRange(0.5, 1000.0);
    mFalloffRadius->setDecimals(1);
    mFalloffRadius->setSuffix(" f");
    mFalloffRadius->setToolTip("Falloff radius in frames");
    mFalloffRadius->setFixedWidth(62);
    mFalloffRadius->setValue(mView->settings.falloffFrames);
    connect(mFalloffRadius, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        mView->settings.falloffFrames = v;
        saveSettings();
    });
    bar->addWidget(mFalloffRadius);
    bar->addWidget(separator(this));

    for (const char* t : {"auto", "spline", "linear", "flat", "step", "clamped"})
    {
        auto* b = toolButton(this, QString("curve-tangent-%1").arg(t).toUtf8().constData(),
                             QString("%1 tangents on the selected keys").arg(niceName(t)));
        connect(b, &QToolButton::clicked, this, [this, t] { applyTangent(t); });
        bar->addWidget(b);
    }
    auto* breakB = toolButton(this, "unlink", "Break tangents: in and out handles move apart");
    connect(breakB, &QToolButton::clicked, this, [this] { lockTangents(false); });
    bar->addWidget(breakB);
    auto* unify = toolButton(this, "link", "Unify tangents: in and out handles move together");
    connect(unify, &QToolButton::clicked, this, [this] { lockTangents(true); });
    bar->addWidget(unify);
    auto* infinity = toolButton(this, "infinity", "Pre and post infinity: how the curve continues before its first key and after its last");
    infinity->setPopupMode(QToolButton::InstantPopup);
    auto* infMenu = new QMenu(infinity);
    for (bool pre : {true, false})
    {
        QMenu* sub = infMenu->addMenu(pre ? "Pre Infinity" : "Post Infinity");
        for (const char* m : {"constant", "linear", "cycle", "cycleRelative", "oscillate"})
            sub->addAction(QString(m) == "cycleRelative" ? QString("Cycle with Offset") : niceName(m), [this, pre, m] { applyInfinity(pre, m); });
    }
    infinity->setMenu(infMenu);
    bar->addWidget(infinity);
    auto* loop = toolButton(this, "repeat", "Make the last key a loop key: it mirrors the first key and the curve cycles after it");
    connect(loop, &QToolButton::clicked, this, [this] { mView->loopKeys(true); });
    bar->addWidget(loop);
    bar->addWidget(separator(this));

    mTweenMode = new QComboBox(this);
    mTweenMode->addItems({"Neighbour", "Ease", "Push/Pull", "Smooth", "Default"});
    mTweenMode->setItemData(0, "Towards the key before (left) or after (right) the selection", Qt::ToolTipRole);
    mTweenMode->setItemData(1, "Onto an ease-in (left) or ease-out (right) between the neighbouring keys", Qt::ToolTipRole);
    mTweenMode->setItemData(2, "Onto (left) or away from (right) the line between the neighbouring keys", Qt::ToolTipRole);
    mTweenMode->setItemData(3, "Towards (right) or away from (left) the average of each key's neighbours", Qt::ToolTipRole);
    mTweenMode->setItemData(4, "Towards (right) or away from (left) the channel's default value", Qt::ToolTipRole);
    mTweenMode->setFocusPolicy(Qt::NoFocus);
    mTweenMode->setCurrentIndex(indexOf(kTweenModes, PrefStore::get().value<std::string>("graphEditor", "/tween", ""), 0));
    mTweenMode->setToolTip("Tween: what the slider blends the selected keys towards");
    connect(mTweenMode, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
    bar->addWidget(mTweenMode);
    mTween = new QSlider(Qt::Horizontal, this);
    mTween->setRange(-100, 100);
    mTween->setFixedWidth(110);
    mTween->setFocusPolicy(Qt::NoFocus);
    mTween->setToolTip("Drag to blend the selected keys; it springs back to the middle on release");
    connect(mTween, &QSlider::sliderPressed, this, [this] { mView->beginTween(anim::BlendMode(mTweenMode->currentIndex())); });
    connect(mTween, &QSlider::valueChanged, this, [this](int v) { mView->setTween(v / 100.0); });
    connect(mTween, &QSlider::sliderReleased, this, [this] {
        mView->endTween();
        QSignalBlocker block(mTween);
        mTween->setValue(0);
    });
    bar->addWidget(mTween);
    bar->addWidget(separator(this));

    auto* curveMenuButton = toolButton(this, "adjustments-horizontal", "Filters, modifiers and buffer curves");
    curveMenuButton->setPopupMode(QToolButton::InstantPopup);
    auto* curveMenu = new QMenu(curveMenuButton);
    connect(curveMenu, &QMenu::aboutToShow, this, [this, curveMenu] {
        curveMenu->clear();
        fillCurveMenu(curveMenu);
    });
    curveMenuButton->setMenu(curveMenu);
    bar->addWidget(curveMenuButton);

    const char* viewIcons[] = {"chart-line", "layout-rows", "arrows-diff"};
    const QString viewTips[] = {"Absolute: curves at their values", "Stacked: each curve in a band of its own, scaled to fit",
                                "Normalized: every curve scaled to -1..1 on top of each other"};
    for (int i = 0; i < 3; ++i)
    {
        mViewButtons[i] = toolButton(this, viewIcons[i], viewTips[i], true);
        connect(mViewButtons[i], &QToolButton::clicked, this, [this, i] { setViewMode(i); });
        bar->addWidget(mViewButtons[i]);
    }

    auto* motion = toolButton(this, "activity", "Speed and acceleration strips under the curves");
    motion->setPopupMode(QToolButton::InstantPopup);
    auto* motionMenu = new QMenu(motion);
    QAction* speed = motionMenu->addAction("Speed");
    speed->setCheckable(true);
    speed->setChecked(mView->settings.speedStrip);
    connect(speed, &QAction::toggled, this, [this](bool on) {
        mView->settings.speedStrip = on;
        saveSettings();
        mView->update();
    });
    QAction* accel = motionMenu->addAction("Acceleration");
    accel->setCheckable(true);
    accel->setChecked(mView->settings.accelStrip);
    connect(accel, &QAction::toggled, this, [this](bool on) {
        mView->settings.accelStrip = on;
        saveSettings();
        mView->update();
    });
    motionMenu->addSection("Of");
    auto* quantityGroup = new QActionGroup(motionMenu);
    const char* quantityLabels[] = {"Curves", "World Speed (selected objects)", "World Angular Speed (selected objects)"};
    const char* quantityTips[] = {"Each shown curve's rate of change, in its own units per second",
                                  "How fast each selected object moves through the world, parent motion included, cm/s",
                                  "How fast each selected object turns in the world, parent motion included, degrees/s"};
    for (int i = 0; i < 3; ++i)
    {
        QAction* a = motionMenu->addAction(quantityLabels[i], this, [this, i] {
            mView->settings.quantity = Quantity(i);
            mView->invalidateMotion();
            saveSettings();
            mView->update();
        });
        a->setToolTip(quantityTips[i]);
        a->setCheckable(true);
        a->setChecked(int(mView->settings.quantity) == i);
        quantityGroup->addAction(a);
    }
    motionMenu->setToolTipsVisible(true);
    motionMenu->addSeparator();
    auto* targetRow = new QWidget(motionMenu);
    auto* targetLayout = new QHBoxLayout(targetRow);
    targetLayout->setContentsMargins(24, 2, 8, 2);
    targetLayout->addWidget(new QLabel("Target Speed", targetRow));
    auto* target = new QDoubleSpinBox(targetRow);
    target->setRange(0.0, 1e6);
    target->setDecimals(1);
    target->setSpecialValueText("None");
    target->setToolTip("A reference line on the speed strip, in its units (a locomotion speed, say)");
    target->setValue(mView->settings.targetSpeed);
    connect(target, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        mView->settings.targetSpeed = v;
        saveSettings();
        mView->update();
    });
    targetLayout->addWidget(target);
    auto* targetAction = new QWidgetAction(motionMenu);
    targetAction->setDefaultWidget(targetRow);
    motionMenu->addAction(targetAction);
    motion->setMenu(motionMenu);
    bar->addWidget(motion);

    auto* rotation = toolButton(this, "rotate-3d", "Rotation curves as Euler angles, quaternion components (read only), or both");
    rotation->setPopupMode(QToolButton::InstantPopup);
    auto* rotationMenu = new QMenu(rotation);
    auto* rotationGroup = new QActionGroup(rotationMenu);
    const char* rotationLabels[] = {"Euler", "Quaternion", "Euler and Quaternion"};
    for (int i = 0; i < 3; ++i)
    {
        QAction* a = rotationMenu->addAction(rotationLabels[i], this, [this, i] {
            mView->settings.rotation = RotationDisplay(i);
            mView->sel.clear();
            saveSettings();
            rebuild();
        });
        a->setCheckable(true);
        a->setChecked(int(mView->settings.rotation) == i);
        rotationGroup->addAction(a);
    }
    rotation->setMenu(rotationMenu);
    bar->addWidget(rotation);

    bar->addStretch(1);
    bar->addWidget(new QLabel("Frame", this));
    mKeyTime = new QLineEdit(this);
    mKeyTime->setFixedWidth(60);
    mKeyTime->setToolTip("Selected key's frame. +=, -=, *= or /= shift or scale several keys (scaling is about the first one).");
    bar->addWidget(mKeyTime);
    bar->addWidget(new QLabel("Value", this));
    mKeyValue = new QLineEdit(this);
    mKeyValue->setFixedWidth(70);
    mKeyValue->setToolTip("Selected keys' value. +=, -=, *= or /= change each key from its own value.");
    bar->addWidget(mKeyValue);
    mValueUnit = new QLabel(this);
    mValueUnit->setMinimumWidth(16);
    bar->addWidget(mValueUnit);
    auto* frameAll = toolButton(this, "zoom-scan", "Frame All");
    connect(frameAll, &QToolButton::clicked, this, [this] { frame(false); });
    bar->addWidget(frameAll);
    layout->addLayout(bar);
    connect(mKeyTime, &QLineEdit::editingFinished, this, [this] { editKeyField(true); });
    connect(mKeyValue, &QLineEdit::editingFinished, this, [this] { editKeyField(false); });

    auto* split = new QSplitter(Qt::Horizontal, this);
    mChannels = new QTreeWidget(split);
    mChannels->setHeaderHidden(true);
    mChannels->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mChannels->setIconSize(QSize(28, 12));
    mChannels->setFocusPolicy(Qt::ClickFocus);
    split->addWidget(mChannels);
    split->addWidget(mView);
    split->setStretchFactor(1, 1);
    split->setSizes({170, 600});
    layout->addWidget(split, 1);
    connect(mChannels, &QTreeWidget::itemSelectionChanged, this, [this] { applyVisibility(); });
    mSyncListener = attrView::addListener([this] { applyVisibility(); });

    mListener = Graph::current().addListener([this](scene::Change c, NodeId id) {
        mView->invalidateMotion();
        if (c == scene::Change::Value)
        {
            if ((mView->curves.contains(id) || mView->rotations.contains(id) ||
                 std::any_of(mView->channels.begin(), mView->channels.end(), [&](const Channel& ch) { return realCurve(ch.curve) == id; })) &&
                !mCurvesQueued)
            {
                mCurvesQueued = true;
                QTimer::singleShot(0, this, [this] {
                    mCurvesQueued = false;
                    reloadCurves();
                });
            }
            else if (mView->settings.quantity != Quantity::Channels && (mView->settings.speedStrip || mView->settings.accelStrip))
                mView->update();
            return;
        }
        scheduleRebuild();
    });
    onTimeChanged(this, [this] { mView->update(); });
    syncToolbar();
    rebuild();
}

GraphEditor::~GraphEditor()
{
    attrView::removeListener(mSyncListener);
    Graph::current().removeListener(mListener);
    if (currentGraphEditor() == this)
        currentGraphEditor() = nullptr;
}

GraphEditor* GraphEditor::instance()
{
    return currentGraphEditor();
}

void GraphEditor::focusInEvent(QFocusEvent* e)
{
    currentGraphEditor() = this;
    QWidget::focusInEvent(e);
}

void GraphEditor::frame(bool selectedOnly)
{
    mView->frame(selectedOnly);
}

void GraphEditor::setTool(int tool)
{
    mView->settings.tool = Tool(std::clamp(tool, 0, 2));
    syncToolbar();
    saveSettings();
    mView->update();
}

void GraphEditor::toolKeyDown(int tool)
{
    mView->toolKeyDown(Tool(std::clamp(tool, 0, 2)), PrefStore::get().value<bool>("graphEditor", "/holdToDrag", true));
    syncToolbar();
    saveSettings();
}

void GraphEditor::toolKeyUp()
{
    mView->toolKeyUp();
}

void GraphEditor::falloffKeyDown()
{
    mView->falloffKeyDown();
}

void GraphEditor::falloffKeyUp()
{
    if (!mView->falloffKeyUp())
        toggleFalloff();
}

void GraphEditor::toggleFalloff()
{
    mView->settings.falloff = !mView->settings.falloff;
    syncToolbar();
    saveSettings();
}

void GraphEditor::setViewMode(int mode)
{
    mView->settings.view = ViewMode(std::clamp(mode, 0, 2));
    mView->updateMaps();
    syncToolbar();
    saveSettings();
    mView->frame(false);
}

void GraphEditor::syncToolbar()
{
    for (int i = 0; i < 3; ++i)
    {
        mToolButtons[i]->setChecked(int(mView->settings.tool) == i);
        mViewButtons[i]->setChecked(int(mView->settings.view) == i);
    }
    mFalloff->setChecked(mView->settings.falloff);
    mFalloffRadius->setEnabled(mView->settings.falloff);
    if (std::abs(mFalloffRadius->value() - mView->settings.falloffFrames) > 1e-9)
    {
        QSignalBlocker block(mFalloffRadius);
        mFalloffRadius->setValue(mView->settings.falloffFrames);
    }
}

void GraphEditor::saveSettings()
{
    const ViewSettings& s = mView->settings;
    PrefStore& prefs = PrefStore::get();
    prefs.set("graphEditor", "/view", kViewNames[int(s.view)]);
    prefs.set("graphEditor", "/tool", kToolNames[int(s.tool)]);
    prefs.set("graphEditor", "/pivot", kPivotNames[int(s.pivot)]);
    prefs.set("graphEditor", "/quantity", kQuantityNames[int(s.quantity)]);
    prefs.set("graphEditor", "/rotation", kRotationNames[int(s.rotation)]);
    prefs.set("graphEditor", "/falloff", s.falloff);
    prefs.set("graphEditor", "/falloffFrames", s.falloffFrames);
    prefs.set("graphEditor", "/speedStrip", s.speedStrip);
    prefs.set("graphEditor", "/accelerationStrip", s.accelStrip);
    prefs.set("graphEditor", "/targetSpeed", s.targetSpeed);
    prefs.set("graphEditor", "/showBuffer", s.showBuffer);
    if (mTweenMode)
        prefs.set("graphEditor", "/tween", kTweenModes[std::clamp(mTweenMode->currentIndex(), 0, 4)]);
}

void GraphEditor::scheduleRebuild()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        rebuild();
    });
}

void GraphEditor::rebuild()
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<std::string> keep;
    for (QTreeWidgetItem* item : mChannels->selectedItems())
        keep.push_back(item->data(0, Qt::UserRole + 1).toString().toStdString());

    std::vector<Channel> channels;
    QSignalBlocker block(mChannels);
    mChannels->clear();
    const RotationDisplay rotationDisplay = mView->settings.rotation;
    for (NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        std::vector<anim::AnimatedPlug> plugs = anim::animatedPlugs(g, id);
        if (plugs.empty())
            continue;
        auto* top = new QTreeWidgetItem(mChannels, {QString::fromStdString(n->name())});
        top->setData(0, Qt::UserRole + 1, QString::fromStdString(n->fullPath()));
        top->setToolTip(0, QString::fromStdString(n->fullPath()));
        QFont f = top->font(0);
        f.setWeight(QFont::DemiBold);
        top->setFont(0, f);
        auto addChannel = [&](const anim::AnimatedPlug& ap, NodeId cid, const QString& label, int axis) {
            Channel ch;
            ch.plug = ap.plug;
            ch.curve = cid;
            scene::Node* curveNode = g.node(ap.curve);
            ch.curveName = curveNode->path();
            ch.label = label;
            ch.unit = slotOf(cid) >= 3 ? QString() : unitOf(*curveNode);
            ch.axis = axis;
            ch.modifiers = anim::modifiersOf(*curveNode);
            // Neighbouring channels never share a look: gray level and dash both cycle, at different periods.
            static constexpr int kLevels[] = {210, 150, 180, 120};
            static constexpr Qt::PenStyle kDashes[] = {Qt::SolidLine, Qt::DashLine, Qt::DotLine};
            ch.level = kLevels[channels.size() % 4];
            ch.dash = kDashes[channels.size() % 3];
            QString text = ch.label;
            if (!ch.modifiers.empty())
            {
                QStringList names;
                for (const anim::Modifier& m : ch.modifiers)
                    names << anim::modifierTypeName(m.type);
                text += "  ·  " + names.join(", ");
            }
            auto* item = new QTreeWidgetItem(top, {text});
            item->setIcon(0, [&] {
                QPixmap pm(28, 12);
                pm.fill(Qt::transparent);
                QPainter p(&pm);
                p.setPen(QPen(channelColor(ch, ch.level), 2, ch.dash, Qt::FlatCap));
                p.drawLine(2, 6, 26, 6);
                return QIcon(pm);
            }());
            if (!ch.modifiers.empty())
                item->setToolTip(0, "Modifiers change this curve's output: " + text.section("  ·  ", 1));
            item->setData(0, Qt::UserRole, int(channels.size()));
            item->setData(0, Qt::UserRole + 1, QString::fromStdString(g.plugName(ap.plug)) + ch.label);
            channels.push_back(std::move(ch));
        };
        for (const anim::AnimatedPlug& ap : plugs)
        {
            const scene::AttrDef& a = g.attr(ap.plug);
            bool rotation = anim::isRotationCurve(*g.node(ap.curve));
            std::string attr = ap.plug.component >= 0 ? a.childNames[ap.plug.component] : a.name;
            if (!rotation || rotationDisplay != RotationDisplay::Quaternion)
                addChannel(ap, rotation ? slotId(ap.curve, ap.plug.component) : ap.curve, niceName(attr), [&] {
                    if (ap.plug.component < 0 || a.childNames.size() != 3 || a.childNames[size_t(ap.plug.component)].empty())
                        return -1;
                    char last = char(std::toupper(static_cast<unsigned char>(a.childNames[size_t(ap.plug.component)].back())));
                    return last == 'X' + ap.plug.component ? ap.plug.component : -1;
                }());
            if (rotation && rotationDisplay != RotationDisplay::Euler && ap.plug.component == 2)
                for (int s = 3; s < 7; ++s)
                    addChannel(ap, slotId(ap.curve, s), niceName(a.name) + " " + QChar("WXYZ"[s - 3]), -1);
        }
        top->setExpanded(true);
    }
    for (QTreeWidgetItemIterator it(mChannels); *it; ++it)
        if (std::find(keep.begin(), keep.end(), (*it)->data(0, Qt::UserRole + 1).toString().toStdString()) != keep.end())
            (*it)->setSelected(true);

    bool wasEmpty = mView->channels.empty();
    mView->channels = std::move(channels);
    mView->invalidateMotion();
    reloadCurves();
    applyVisibility();
    if (wasEmpty && !mView->channels.empty() && mView->isVisible())
    {
        mView->mFramed = true;
        mView->frame(false);
    }
}

void GraphEditor::reloadCurves()
{
    Graph& g = Graph::current();
    mView->curves.clear();
    mView->rotations.clear();
    mView->loopCurves.clear();
    for (Channel& ch : mView->channels)
    {
        scene::Node* n = g.node(realCurve(ch.curve));
        if (!n)
            continue;
        ch.modifiers = anim::modifiersOf(*n);
        if (anim::curveLoops(*n))
            mView->loopCurves.insert(ch.curve);
        try
        {
            int slot = slotOf(ch.curve);
            if (slot < 0)
            {
                mView->curves[ch.curve] = anim::curveOf(*n);
                continue;
            }
            auto it = mView->rotations.find(n->id());
            if (it == mView->rotations.end())
                it = mView->rotations.emplace(n->id(), anim::rotationCurveOf(*n).looped()).first;
            mView->curves[ch.curve] = derivedChannel(it->second, slot);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", n->path(), e.what());
        }
    }
    mView->dropMissing();
    mView->updateMaps();
    refreshKeyFields();
    mView->update();
}

void GraphEditor::applyVisibility()
{
    std::vector<int> picked;
    for (QTreeWidgetItem* item : mChannels->selectedItems())
    {
        if (item->childCount() == 0)
            picked.push_back(item->data(0, Qt::UserRole).toInt());
        for (int c = 0; c < item->childCount(); ++c)
            picked.push_back(item->child(c)->data(0, Qt::UserRole).toInt());
    }
    // Synced to the Channel Box: its highlighted channels, unless channels are picked here.
    const std::vector<scene::PlugRef>& synced = attrView::highlightedChannels();
    const bool sync = picked.empty() && attrView::syncGraphEditor() && !synced.empty();
    for (int i = 0; i < int(mView->channels.size()); ++i)
        mView->channels[i].shown = sync ? std::find(synced.begin(), synced.end(), mView->channels[i].plug) != synced.end()
                                        : picked.empty() || std::find(picked.begin(), picked.end(), i) != picked.end();
    mView->dropMissing();
    mView->updateMaps();
    if (mView->settings.view != ViewMode::Absolute)
        mView->frame(false);
    refreshKeyFields();
    mView->update();
}

void GraphEditor::applyTangent(const char* type)
{
    bool step = std::string_view(type) == "step";
    UndoChunk chunk("Key Tangents");
    if (!mView->sel.empty())
    {
        for (const KeySel& k : mView->sel)
            if (std::string name = mView->commandTarget(k.curve); !name.empty())
            {
                json call = {{"args", {name}}, {"time", k.time}, {"outTangentType", type}};
                if (!step)
                    call["inTangentType"] = type;
                runUnechoed("keyTangent", call);
            }
    }
    else
        for (NodeId c : mView->targetCurves())
            if (std::string name = mView->commandTarget(c); !name.empty())
            {
                json call = {{"args", {name}}, {"outTangentType", type}};
                if (!step)
                    call["inTangentType"] = type;
                runUnechoed("keyTangent", call);
            }
    log::write(LogLevel::Echo, std::string("# tangents: ") + type + "\n");
}

void GraphEditor::lockTangents(bool lock)
{
    if (mView->sel.empty())
    {
        log::warn("{} tangents: select keys first", lock ? "Unify" : "Break");
        return;
    }
    {
        UndoChunk chunk(lock ? "Unify Tangents" : "Break Tangents");
        for (const KeySel& k : mView->sel)
            if (std::string name = mView->commandTarget(k.curve); !name.empty())
                runUnechoed("keyTangent", {{"args", {name}}, {"time", k.time}, {"lock", lock}});
    }
    log::write(LogLevel::Echo, std::string("# tangents ") + (lock ? "unified\n" : "broken\n"));
}

void GraphEditor::applyInfinity(bool pre, const char* mode)
{
    Graph& g = Graph::current();
    json names = json::array();
    for (NodeId c : mView->targetCurves())
        if (scene::Node* n = g.node(realCurve(c)))
            if (std::find(names.begin(), names.end(), n->path()) == names.end())
                names.push_back(n->path());
    if (names.empty())
        return;
    try
    {
        ops::execute("setInfinity", {{"args", names}, {pre ? "preInfinite" : "postInfinite", mode}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void GraphEditor::deleteSelectedKeys()
{
    if (mView->sel.empty())
        return;
    std::vector<KeySel> doomed = mView->sel;
    mView->sel.clear();
    {
        UndoChunk chunk("Delete Keys");
        for (const KeySel& k : doomed)
            if (std::string name = mView->commandTarget(k.curve); !name.empty())
                runUnechoed("cutKey", {{"args", {name}}, {"time", k.time}, {"clear", true}});
    }
    log::write(LogLevel::Echo, "# deleted " + std::to_string(doomed.size()) + " keys\n");
    refreshKeyFields();
}

void GraphEditor::runOnKeys(const std::string& command, const json& call)
{
    json full = call;
    if (!mView->sel.empty())
        full["keys"] = mView->keysArgument();
    else
        full["args"] = mView->targetNames();
    if (full.contains("args") && full["args"].empty())
    {
        log::warn("{}: no curves in the graph editor", command);
        return;
    }
    actions::runWithOptions(command, full);
}

void GraphEditor::runOnCurves(const std::string& command, const json& call)
{
    Graph& g = Graph::current();
    json names = json::array();
    for (NodeId c : mView->targetCurves())
    {
        std::string target = slotOf(c) < 0 ? mView->commandTarget(c) : (g.node(realCurve(c)) ? g.node(realCurve(c))->path() : "");
        if (!target.empty() && std::find(names.begin(), names.end(), target) == names.end())
            names.push_back(target);
    }
    if (names.empty())
    {
        log::warn("{}: no curves in the graph editor", command);
        return;
    }
    json full = call;
    full["args"] = names;
    actions::runWithOptions(command, full);
}

void GraphEditor::snapshotBuffer()
{
    Graph& g = Graph::current();
    int count = 0;
    for (NodeId c : mView->targetCurves())
    {
        scene::Node* n = g.node(realCurve(c));
        if (!n)
            continue;
        Buffer b;
        b.rotation = anim::isRotationCurve(*n);
        if (b.rotation)
            b.rotationCurve = anim::rotationCurveOf(*n);
        else
            b.curve = anim::curveOf(*n);
        mView->buffers[n->id()] = std::move(b);
        ++count;
    }
    log::info("Buffer: snapshot of {} curve{}", count, count == 1 ? "" : "s");
    mView->update();
}

void GraphEditor::swapBuffer()
{
    Graph& g = Graph::current();
    int count = 0;
    {
        UndoChunk chunk("Swap Buffer Curves");
        std::set<NodeId> done;
        for (NodeId c : mView->targetCurves())
        {
            const NodeId id = realCurve(c);
            auto it = mView->buffers.find(id);
            scene::Node* n = g.node(id);
            if (!n || it == mView->buffers.end() || !done.insert(id).second)
                continue;
            Buffer now;
            now.rotation = anim::isRotationCurve(*n);
            if (now.rotation != it->second.rotation)
                continue;
            if (now.rotation)
            {
                now.rotationCurve = anim::rotationCurveOf(*n);
                anim::setRotationCurve(g, id, it->second.rotationCurve);
            }
            else
            {
                now.curve = anim::curveOf(*n);
                anim::setCurve(g, id, it->second.curve);
            }
            it->second = std::move(now);
            ++count;
        }
    }
    if (count == 0)
        log::warn("Swap Buffer: no snapshot of these curves yet (Snapshot Buffer first)");
    else
        log::write(LogLevel::Echo, "# swapped " + std::to_string(count) + " buffer curves\n");
}

void GraphEditor::fillCurveMenu(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    menu->addSection("Filters");
    for (const char* id : {"graphEditor.butterworth", "graphEditor.gaussian", "graphEditor.simplify", "graphEditor.resample", "graphEditor.eulerFilter"})
        menus::addActionRow(menu, id);
    menu->addSection("Modifiers");
    for (const char* id : {"graphEditor.noise", "graphEditor.stepped", "graphEditor.limits", "graphEditor.removeModifiers"})
        menus::addActionRow(menu, id);
    menu->addSection("Buffer");
    menus::addActionRow(menu, "graphEditor.snapshotBuffer");
    menus::addActionRow(menu, "graphEditor.swapBuffer");
    QAction* show = menu->addAction("Show Buffer Curves");
    show->setCheckable(true);
    show->setChecked(mView->settings.showBuffer);
    connect(show, &QAction::toggled, this, [this](bool on) {
        mView->settings.showBuffer = on;
        saveSettings();
        mView->update();
    });
}

void GraphEditor::fillContextMenu(QMenu* menu)
{
    QMenu* tangents = menu->addMenu("Tangents");
    for (const char* t : {"auto", "spline", "linear", "flat", "step", "clamped"})
        tangents->addAction(icon(QString("curve-tangent-%1").arg(t)), niceName(t), [this, t] { applyTangent(t); });
    tangents->addSeparator();
    tangents->addAction(icon("unlink"), "Break", [this] { lockTangents(false); });
    tangents->addAction(icon("link"), "Unify", [this] { lockTangents(true); });
    tangents->addSeparator();
    tangents->addAction("Weighted", [this] {
        UndoChunk chunk("Weighted Tangents");
        for (NodeId c : mView->targetCurves())
            if (std::string name = mView->commandTarget(c); !name.empty())
                runUnechoed("keyTangent", {{"args", {name}}, {"weightedTangents", true}});
    });
    tangents->addAction("Non-Weighted", [this] {
        UndoChunk chunk("Non-Weighted Tangents");
        for (NodeId c : mView->targetCurves())
            if (std::string name = mView->commandTarget(c); !name.empty())
                runUnechoed("keyTangent", {{"args", {name}}, {"weightedTangents", false}});
    });
    for (bool pre : {true, false})
    {
        QMenu* inf = menu->addMenu(pre ? "Pre Infinity" : "Post Infinity");
        for (const char* m : {"constant", "linear", "cycle", "cycleRelative", "oscillate"})
            inf->addAction(QString(m) == "cycleRelative" ? QString("Cycle with Offset") : niceName(m), [this, pre, m] { applyInfinity(pre, m); });
    }
    menu->addAction("Loop Key", [this] { mView->loopKeys(true); });
    menu->addAction("Clear Loop Key", [this] { mView->loopKeys(false); });
    fillCurveMenu(menu->addMenu("Curves"));
    menu->addSeparator();
    menu->addAction("Delete Keys", [this] { deleteSelectedKeys(); })->setEnabled(!mView->sel.empty());
    menu->addAction("Frame Selection", [this] { frame(true); });
    menu->addAction("Frame All", [this] { frame(false); });
}

void GraphEditor::refreshKeyFields()
{
    const auto& sel = mView->sel;
    std::optional<double> t, v;
    bool sameT = true, sameV = true;
    QString unit;
    bool sameUnit = true;
    for (const KeySel& k : sel)
    {
        auto it = mView->curves.find(k.curve);
        if (it == mView->curves.end())
            continue;
        int i = it->second.find(k.time);
        if (i < 0)
            continue;
        double value = it->second.keys[size_t(i)].value;
        if (t && std::abs(*t - k.time) > kEps)
            sameT = false;
        if (v && std::abs(*v - value) > 1e-9)
            sameV = false;
        if (const Channel* ch = mView->channelOf(k.curve))
        {
            if (t && ch->unit != unit)
                sameUnit = false;
            unit = ch->unit;
        }
        t = k.time;
        v = value;
    }
    mKeyTime->setEnabled(!sel.empty());
    mKeyValue->setEnabled(!sel.empty());
    mKeyTime->setText(t && sameT ? formatValue(anim::frameOf(*t)) : QString());
    mKeyValue->setText(v && sameV ? formatValue(*v) : QString());
    mKeyTime->setPlaceholderText(t && !sameT ? "mixed" : QString());
    mKeyValue->setPlaceholderText(v && !sameV ? "mixed" : QString());
    mValueUnit->setText(sameUnit ? unit : QString());
}

void GraphEditor::editKeyField(bool time)
{
    QLineEdit* field = time ? mKeyTime : mKeyValue;
    auto entry = parseEntry(field->text());
    if (!entry || mView->sel.empty())
        return;
    auto [op, x] = *entry;
    if (time)
    {
        std::vector<double> times;
        for (const KeySel& k : mView->sel)
            times.push_back(k.time);
        const double first = *std::min_element(times.begin(), times.end());
        const bool allSame = std::all_of(times.begin(), times.end(), [&](double s) { return std::abs(s - first) <= kEps; });
        json call = {{"keys", mView->keysArgument()}};
        switch (op)
        {
        case '=':
            if (!allSame)
            {
                log::warn("Frame: the selected keys are at different frames; use += or -= to shift them");
                refreshKeyFields();
                return;
            }
            call["timeOffset"] = anim::secondsOf(x) - first;
            break;
        case '+': call["timeOffset"] = anim::secondsOf(x) - anim::secondsOf(0.0); break;
        case '-': call["timeOffset"] = anim::secondsOf(0.0) - anim::secondsOf(x); break;
        case '*':
        case '/':
            call["timeScale"] = op == '*' ? x : 1.0 / x;
            call["timePivot"] = first;
            call["snap"] = true;
            break;
        }
        try
        {
            ops::execute("transformKeys", call, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
            return;
        }
        anim::KeyTransform xf{.timeOffset = call.value("timeOffset", 0.0), .timeScale = call.value("timeScale", 1.0), .timePivot = first,
                              .snap = call.contains("snap")};
        for (KeySel& k : mView->sel)
            k.time = xf.time(k.time, anim::playback().fps);
        refreshKeyFields();
        return;
    }
    UndoChunk chunk("Key Values");
    for (const KeySel& k : mView->sel)
    {
        std::string name = mView->commandTarget(k.curve);
        auto it = mView->curves.find(k.curve);
        if (name.empty() || it == mView->curves.end())
            continue;
        const int i = it->second.find(k.time);
        if (i < 0)
            continue;
        const double v = it->second.keys[size_t(i)].value;
        const double to = op == '=' ? x : op == '+' ? v + x : op == '-' ? v - x : op == '*' ? v * x : v / x;
        runUnechoed("keyframe", {{"args", {name}}, {"time", k.time}, {"valueChange", to}});
    }
}
} // namespace lila::ui
