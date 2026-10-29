#include "ui/ModifierStack.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/AttributeEditor.h"
#include "ui/Deletion.h"
#include "ui/NodeLookup.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/NumberExpr.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDockWidget>
#include <QDrag>
#include <QElapsedTimer>
#include <QDragEnterEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;
using scene::PlugRef;
using scene::PlugType;

constexpr const char* kMime = "application/x-lila-modifier";
constexpr std::string_view kSuffix = "Modifier";

QColor gray(int v) { return themeShade(v); }

std::map<std::string, QString>& iconTable()
{
    static std::map<std::string, QString> t = {
        {"mirror", "flip-vertical"},   {"subdivision", "cube-3d-sphere"}, {"bevel", "border-corners"},
        {"array", "box-multiple"},     {"triangulate", "triangles"},      {"weightedNormals", "arrows-diagonal"},
        {"solidify", "stack-2"},       {"splitRings", "columns-3"},       {"decimate", "chart-dots-3"},      {"autoBevel", "sparkles"},
        {"skinCluster", "bone"},       {"meshSource", "box"},             {"morph", "mood-smile"},
        {"ffd", "grid-dots"},          {"deltaMush", "droplet"},          {"wrap", "layers-intersect"},
        {"cage", "box"},               {"softMod", "focus-2"},            {"bendDeformer", "rainbow"},
        {"twistDeformer", "rotate-clockwise"}, {"taperDeformer", "triangle-inverted"}, {"squashDeformer", "arrows-vertical"},
        {"sineDeformer", "wave-sine"}, {"waveDeformer", "ripple"}, {"transferAttributes", "transfer"},
    };
    return t;
}

/** "bevelModifier" -> "bevel"; anything else unchanged. */
std::string kindOf(const std::string& typeName)
{
    if (typeName.size() > kSuffix.size() && typeName.ends_with(kSuffix))
        return typeName.substr(0, typeName.size() - kSuffix.size());
    return typeName;
}

/** "weightedNormals" -> "Weighted Normals". */
QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)) && !std::isupper(static_cast<unsigned char>(name[i - 1])))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

QString formatNumber(double v, bool integer)
{
    if (integer)
        return QString::number(std::llround(v));
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 3);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

double numericOf(const scene::PlugValue& v)
{
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    return 0.0;
}

json run(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

void setAttr(const PlugRef& p, const json& value)
{
    Graph& g = Graph::current();
    if (!g.node(p.node))
        return;
    json v = value;
    if (v.is_number() && p.attr >= 0)
    {
        // Typed and dragged values stay inside the attr's hard limits.
        const scene::AttrDef& def = g.attr(p);
        double d = v.get<double>();
        double c = std::clamp(d, def.minValue.value_or(-HUGE_VAL), def.maxValue.value_or(HUGE_VAL));
        if (c != d)
            v = v.is_number_integer() ? json(int64_t(std::llround(c))) : json(c);
    }
    run("setAttr", {{"args", {g.plugName(p), v}}});
}

std::string nameOf(NodeId id)
{
    scene::Node* n = Graph::current().node(id);
    return n ? n->path() : std::string();
}

struct Counts
{
    int points = 0;
    int faces = 0;
};

Counts countsOf(const scene::MeshPtr& m)
{
    Counts c;
    if (!m)
        return c;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        c.points += m->pointValid(p);
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        c.faces += m->faceValid(f);
    return c;
}

QToolButton* toolButton(const QString& iconName, const QString& tip, QWidget* parent, bool checkable = false)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setCheckable(checkable);
    b->setIconSize(QSize(16, 16));
    return b;
}

/** Keyed (or driven) look of a field, shared by every field type. */
enum class PlugState
{
    Free,
    Keyed,
    OnKey,
    Driven,
};

PlugState stateOf(Graph& g, const PlugRef& ref)
{
    NodeId curve = scene::anim::curveFor(g, ref);
    if (curve)
        return scene::anim::hasKeyAt(*g.node(curve), g.time()) ? PlugState::OnKey : PlugState::Keyed;
    if (g.source(ref) || (ref.component >= 0 && g.source(PlugRef{ref.node, ref.attr, -1})))
        return PlugState::Driven;
    return PlugState::Free;
}

/** The setAttr value that restores a plug to its default, or nullopt for one that doesn't reset. */
std::optional<json> defaultOf(const PlugRef& ref)
{
    Graph& g = Graph::current();
    if (!g.node(ref.node))
        return std::nullopt;
    return valueReset::plugDefault(g.attr(ref), ref.component);
}

QString defaultText(const PlugRef& ref)
{
    std::optional<json> def = defaultOf(ref);
    return def ? QString::fromStdString(valueReset::describe(*def, Graph::current().attr(ref).enumNames)) : QString();
}

/** Restores a plug's default through setAttr, unless it is driven. False when nothing was reset. */
bool resetPlug(const PlugRef& ref)
{
    Graph& g = Graph::current();
    std::optional<json> def = defaultOf(ref);
    if (!def || stateOf(g, ref) == PlugState::Driven)
        return false;
    setAttr(ref, *def);
    return true;
}

void addResetAction(QMenu* menu, const PlugRef& ref, QWidget* flash)
{
    if (!defaultOf(ref))
        return;
    menu->addSeparator();
    QPointer<QWidget> target = flash;
    menu->addAction("Reset to Default", [ref, target] {
        if (resetPlug(ref) && target)
            flashReset(target);
    })->setEnabled(stateOf(Graph::current(), ref) != PlugState::Driven);
}

/** A middle click on a row label resets its plug; the field it labels flashes. */
void resetOnMiddleClick(QLabel* label, const PlugRef& ref, QWidget* field)
{
    if (defaultOf(ref))
        onMiddleClickReset(label, [ref] { resetPlug(ref); }, defaultText(ref), field);
}

void addKeyActions(QMenu* menu, const PlugRef& ref)
{
    Graph& g = Graph::current();
    if (!g.node(ref.node) || !(g.attr(ref).flags & scene::AttrKeyable))
        return;
    std::string plug = g.plugName(ref);
    menu->addSeparator();
    menu->addAction(icon("keyframe"), "Set Key", [plug] { run("setKeyframe", {{"args", {plug}}}); });
    if (scene::anim::curveFor(g, ref))
        menu->addAction("Delete Keys", [plug] { run("cutKey", {{"args", {plug}}, {"clear", true}}); });
}

/**
 * A number field bound to a plug. Click to type (expressions like `+=5` work, as in the channel box); drag sideways
 * to scrub (Ctrl slow, Shift fast), with the left or middle button. A middle click restores the default. Its row label
 * does all of this too.
 */
class ScrubEdit : public QLineEdit
{
public:
    ScrubEdit(PlugRef plug, bool integer, QWidget* parent) : QLineEdit(parent), mPlug(plug), mInt(integer)
    {
        mClock.start();
        setAlignment(Qt::AlignRight);
        setMinimumWidth(40);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setCursor(Qt::SizeHorCursor);
        // Not click focus: Qt would focus on press, so the press would place the caret instead of starting a scrub,
        // and the release would never select the text for typing over.
        setFocusPolicy(Qt::TabFocus);
        setAcceptDrops(false);
        setDoc({});
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QLineEdit::editingFinished, this, [this] { commitText(); });
        connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
            QMenu* menu = createStandardContextMenu();
            addResetAction(menu, mPlug, this);
            addKeyActions(menu, mPlug);
            menu->exec(mapToGlobal(at));
            delete menu;
        });
    }

    void attachLabel(QLabel* label)
    {
        label->setCursor(Qt::SizeHorCursor);
        label->installEventFilter(this);
        if (defaultOf(mPlug))
            label->setToolTip(label->toolTip() + (label->toolTip().isEmpty() ? "" : "\n") + resetHint(defaultText(mPlug)));
    }

    bool busy() const { return hasFocus() || mScrubbing; }

    void setDoc(const QString& doc, const QString& note = {})
    {
        mDoc = doc;
        QString tip = doc;
        if (!note.isEmpty())
            tip += (tip.isEmpty() ? "" : "\n") + note;
        tip += (tip.isEmpty() ? "" : "\n") + QString("Drag to scrub (Ctrl slow, Shift fast); click to type, +=5 works");
        if (defaultOf(mPlug))
            tip += "\n" + resetHint(defaultText(mPlug));
        setToolTip(tip);
    }

    void refresh(Graph& g)
    {
        PlugState state = stateOf(g, mPlug);
        mReadOnly = state == PlugState::Driven;
        setReadOnly(mReadOnly);
        if (state != mState)
        {
            mState = state;
            QPalette pal = palette();
            pal.setColor(QPalette::Base, state == PlugState::OnKey ? gray(84) : state == PlugState::Keyed ? gray(60) : gray(36));
            pal.setColor(QPalette::Text, mReadOnly ? gray(130) : gray(212));
            setPalette(pal);
            QFont f = font();
            f.setItalic(mReadOnly);
            setFont(f);
            const char* note = state == PlugState::Driven ? "Driven by a connection"
                               : state == PlugState::Keyed ? "Keyed"
                               : state == PlugState::OnKey ? "Key on this frame"
                                                           : "";
            setDoc(mDoc, note);
        }
        if (!busy())
            setText(formatNumber(numericOf(g.value(mPlug)), mInt));
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (!qobject_cast<QLabel*>(obj))
            return QLineEdit::eventFilter(obj, e);
        auto* me = static_cast<QMouseEvent*>(e);
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
            if (me->button() == Qt::LeftButton || me->button() == Qt::MiddleButton)
            {
                press(me);
                return true;
            }
            break;
        case QEvent::MouseMove:
            if (mPress)
            {
                move(me);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (mPress && me->button() == (mMiddle ? Qt::MiddleButton : Qt::LeftButton))
            {
                release(me);
                return true;
            }
            break;
        default: break;
        }
        return QLineEdit::eventFilter(obj, e);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (hasFocus() || (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton))
        {
            QLineEdit::mousePressEvent(e);
            return;
        }
        press(e);
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!mPress)
        {
            QLineEdit::mouseMoveEvent(e);
            return;
        }
        move(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (!mPress)
        {
            QLineEdit::mouseReleaseEvent(e);
            return;
        }
        if (e->button() != (mMiddle ? Qt::MiddleButton : Qt::LeftButton))
            return;
        bool middle = mMiddle;
        if (release(e) || middle || mReadOnly)
            return;
        setFocus(Qt::MouseFocusReason);
        selectAll();
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (mScrubbing && e->key() == Qt::Key_Escape)
        {
            mPress.reset();
            endScrub(false);
            return;
        }
        if (e->key() == Qt::Key_Escape)
        {
            setModified(false);
            clearFocus();
            refresh(Graph::current());
            return;
        }
        QLineEdit::keyPressEvent(e);
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            clearFocus();
    }

    void focusInEvent(QFocusEvent* e) override
    {
        QLineEdit::focusInEvent(e);
        setCursor(Qt::IBeamCursor);
    }

    void focusOutEvent(QFocusEvent* e) override
    {
        QLineEdit::focusOutEvent(e);
        setCursor(Qt::SizeHorCursor);
        refresh(Graph::current());
    }

private:
    void press(QMouseEvent* e)
    {
        mPress = e->globalPosition().toPoint();
        mMiddle = e->button() == Qt::MiddleButton;
        if (mMiddle)
            mMidClick.press(mPress->x(), mPress->y(), mClock.elapsed());
    }

    void move(QMouseEvent* e)
    {
        QPoint pos = e->globalPosition().toPoint();
        if (mMiddle)
        {
            mMidClick.move(pos.x(), pos.y());
            if (!mMidClick.dragging())
                return;
        }
        drag(pos, e->modifiers());
    }

    /** Ends a press: commits a scrub, or resets on a middle click. True when the press did either. */
    bool release(QMouseEvent* e)
    {
        mPress.reset();
        QPoint pos = e->globalPosition().toPoint();
        if (mMiddle && mMidClick.release(pos.x(), pos.y(), mClock.elapsed()) && !mScrubbing)
        {
            if (!mReadOnly && resetPlug(mPlug))
                flashReset(this);
            return true;
        }
        if (!mScrubbing)
            return false;
        endScrub(true);
        return true;
    }

    void drag(QPoint pos, Qt::KeyboardModifiers mods)
    {
        if (mReadOnly)
            return;
        if (!mScrubbing)
        {
            if (std::abs(pos.x() - mPress->x()) < 3)
                return;
            Graph& g = Graph::current();
            if (!g.node(mPlug.node))
                return;
            mScrubbing = true;
            mSaved = g.value(mPlug);
            mAccum = 0.0;
            mLast = pos;
        }
        double step = (mods & Qt::ControlModifier) ? 0.01 : (mods & Qt::ShiftModifier) ? 1.0 : 0.1;
        mAccum += (pos.x() - mLast.x()) * step;
        mLast = pos;
        // Held at the limits so dragging back moves the value straight away.
        const scene::AttrDef& def = Graph::current().attr(mPlug);
        double base = numericOf(mSaved);
        mAccum = std::clamp(base + mAccum, def.minValue.value_or(-HUGE_VAL), def.maxValue.value_or(HUGE_VAL)) - base;
        // Live preview without undo; endScrub records the one real setAttr.
        withoutUndo([this](Graph& g) { g.setValue(mPlug, scrubbed()); });
        setText(formatNumber(numericOf(scrubbed()), mInt));
    }

    void endScrub(bool commit)
    {
        mScrubbing = false;
        scene::PlugValue final = scrubbed();
        withoutUndo([this](Graph& g) { g.setValue(mPlug, mSaved); });
        if (commit && mAccum != 0.0)
        {
            double v = numericOf(final);
            setAttr(mPlug, mInt ? json(int64_t(std::llround(v))) : json(std::round(v * 1e4) / 1e4));
        }
        refresh(Graph::current());
    }

    scene::PlugValue scrubbed() const
    {
        double v = numericOf(mSaved) + mAccum;
        if (std::holds_alternative<int64_t>(mSaved))
            return int64_t(std::llround(v));
        return v;
    }

    template <class F> void withoutUndo(F&& fn)
    {
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        try
        {
            fn(Graph::current());
        }
        catch (const std::exception&)
        {
        }
        u.setEnabled(was);
    }

    void commitText()
    {
        if (!isModified())
            return;
        setModified(false);
        Graph& g = Graph::current();
        if (!g.node(mPlug.node))
            return;
        std::optional<double> v = evalExpression(text(), numericOf(g.value(mPlug)));
        if (!v)
        {
            log::warn("modifier stack: can't read '{}'", text().toStdString());
            refresh(g);
            return;
        }
        setAttr(mPlug, mInt ? json(int64_t(std::llround(*v))) : json(*v));
    }

    PlugRef mPlug;
    QString mDoc;
    bool mInt = false;
    bool mReadOnly = false;
    PlugState mState = PlugState::Free;
    std::optional<QPoint> mPress;
    bool mMiddle = false;
    valueReset::ClickTracker mMidClick;
    QElapsedTimer mClock;
    bool mScrubbing = false;
    QPoint mLast;
    double mAccum = 0.0;
    scene::PlugValue mSaved;
};

/** Starts a drag of the modifier when its grip is pulled. */
class GripFilter : public QObject
{
public:
    GripFilter(QWidget* card, std::string name, QObject* parent) : QObject(parent), mCard(card), mName(std::move(name)) {}

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        auto* me = static_cast<QMouseEvent*>(e);
        if (e->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton)
        {
            mPress = me->globalPosition().toPoint();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease)
            mPress.reset();
        else if (e->type() == QEvent::MouseMove && mPress &&
                 (me->globalPosition().toPoint() - *mPress).manhattanLength() >= QApplication::startDragDistance())
        {
            mPress.reset();
            auto* drag = new QDrag(mCard);
            auto* data = new QMimeData;
            data->setData(kMime, QByteArray::fromStdString(mName));
            drag->setMimeData(data);
            QPixmap pm = mCard->grab();
            if (pm.height() > 120)
                pm = pm.copy(0, 0, pm.width(), 120);
            drag->setPixmap(pm);
            drag->setHotSpot(QPoint(12, 12));
            drag->exec(Qt::MoveAction);
            return true;
        }
        return false;
    }

private:
    QWidget* mCard;
    std::string mName;
    std::optional<QPoint> mPress;
};
} // namespace

/** The column of cards; takes modifier drops and draws where one would land. */
class ModifierCardList : public QWidget
{
public:
    explicit ModifierCardList(ModifierStackWidget* owner) : QWidget(owner), mOwner(owner) { setAcceptDrops(true); }

    /** Modifier cards in stack order, for working out drop positions. */
    std::vector<QWidget*> modCards;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMime))
            e->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kMime))
            return;
        e->acceptProposedAction();
        int index = indexAt(e->position().toPoint().y());
        if (index != mDrop)
        {
            mDrop = index;
            update();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        mDrop = -1;
        update();
    }

    void dropEvent(QDropEvent* e) override
    {
        int to = indexAt(e->position().toPoint().y());
        mDrop = -1;
        update();
        if (!e->mimeData()->hasFormat(kMime))
            return;
        e->acceptProposedAction();
        std::string name = e->mimeData()->data(kMime).toStdString();
        // Defer so the drag finishes before the cards are rebuilt under it.
        QTimer::singleShot(0, mOwner, [owner = mOwner, name, to] { owner->dropModifier(name, to); });
    }

    void paintEvent(QPaintEvent*) override
    {
        if (mDrop < 0 || modCards.empty())
            return;
        int y = mDrop < int(modCards.size()) ? modCards[mDrop]->geometry().top() - 3 : modCards.back()->geometry().bottom() + 3;
        QPainter p(this);
        p.fillRect(QRect(4, y - 1, width() - 8, 2), gray(200));
    }

private:
    /** Insertion slot for a y: 0 above the first modifier card, size() below the last. */
    int indexAt(int y) const
    {
        for (int i = 0; i < int(modCards.size()); ++i)
            if (y < modCards[i]->geometry().center().y())
                return i;
        return int(modCards.size());
    }

    ModifierStackWidget* mOwner;
    int mDrop = -1;
};

void setModifierIcon(const std::string& kindOrType, const QString& tablerName)
{
    iconTable()[kindOf(kindOrType)] = tablerName;
}

QString modifierIconName(const std::string& kindOrType)
{
    auto& t = iconTable();
    auto it = t.find(kindOf(kindOrType));
    if (it != t.end())
        return it->second;
    if (kindOrType.rfind("gn_", 0) == 0)
        return "topology-star-3";
    if (kindOrType.rfind("poly", 0) == 0)
        return "cube";
    return "adjustments";
}

ModifierStackWidget::ModifierStackWidget(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    mHeader = new QWidget(this);
    auto* top = new QHBoxLayout(mHeader);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(4);
    mAdd = new QToolButton(mHeader);
    mAdd->setText("Add Modifier");
    mAdd->setToolTip("Add a modifier to the top of the selected meshes' stacks. Hover a kind to see what it does.");
    mAdd->setIcon(icon("plus"));
    mAdd->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mAdd->setPopupMode(QToolButton::InstantPopup);
    mAdd->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* addMenu = new QMenu(mAdd);
    fillAddModifierMenu(addMenu);
    mAdd->setMenu(addMenu);
    top->addWidget(mAdd, 1);
    mMore = toolButton("dots-vertical", "Stack actions", mHeader);
    mMore->setPopupMode(QToolButton::InstantPopup);
    auto* more = new QMenu(mMore);
    more->addAction(icon("check"), "Apply All", this, [this] {
        if (mMesh)
            run("applyAllModifiers", {{"args", {nameOf(mMesh)}}});
    })->setToolTip("Bake every modifier into the base mesh");
    more->addAction(icon("stack-pop"), "Bake Modifier Stack", this, [this] {
        if (mMesh)
            run("bakeModifierStack", {{"args", {nameOf(mMesh)}}});
    })->setToolTip("Bake the generator and every modifier into the mesh itself");
    more->addSeparator();
    more->addAction(icon("chevron-down"), "Expand All", this, [this] { setCollapsed(scene::kNoNode, false, true); });
    more->addAction(icon("chevron-right"), "Collapse All", this, [this] { setCollapsed(scene::kNoNode, true, true); });
    more->setToolTipsVisible(true);
    mMore->setMenu(more);
    top->addWidget(mMore);
    outer->addWidget(mHeader);

    mTitle = new QLabel(this);
    QFont tf = mTitle->font();
    tf.setWeight(QFont::DemiBold);
    mTitle->setFont(tf);
    outer->addWidget(mTitle);

    mScroll = new QScrollArea(this);
    mScroll->setWidgetResizable(true);
    mScroll->setFrameShape(QFrame::NoFrame);
    mList = new ModifierCardList(this);
    mListLayout = new QVBoxLayout(mList);
    mListLayout->setContentsMargins(0, 0, 0, 0);
    mListLayout->setSpacing(6);
    mScroll->setWidget(mList);
    outer->addWidget(mScroll, 1);

    mFooter = new QLabel(this);
    QPalette fp = mFooter->palette();
    fp.setColor(QPalette::WindowText, gray(150));
    mFooter->setPalette(fp);
    outer->addWidget(mFooter);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(0);
    connect(mRefresh, &QTimer::timeout, this, &ModifierStackWidget::refresh);
    mListener = Graph::current().addListener([this](scene::Change, NodeId) { scheduleRefresh(); });
    // Clicking a card focuses the panel, so Delete then goes to the card rather than the viewport.
    setFocusPolicy(Qt::ClickFocus);
    deletion::claim(this, [this] { removeCardModifier(); });
    refresh();
}

void ModifierStackWidget::removeCardModifier()
{
    for (QWidget* from : {QApplication::focusWidget(), QApplication::widgetAt(QCursor::pos())})
        for (QWidget* w = from; w && w != this; w = w->parentWidget())
            for (auto& [id, card] : mCards)
                if (card.first == w)
                {
                    if (scene::Node* n = Graph::current().node(id); n && scene::modifiers::isModifier(*n))
                        run("removeModifier", {{"args", {nameOf(id)}}});
                    return;
                }
}

ModifierStackWidget::~ModifierStackWidget()
{
    Graph::current().removeListener(mListener);
}

void ModifierStackWidget::setMesh(NodeId mesh)
{
    mPinned = mesh;
    refresh();
}

void ModifierStackWidget::setHeaderVisible(bool on)
{
    mHeader->setVisible(on);
}

void ModifierStackWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    scheduleRefresh();
}

void ModifierStackWidget::scheduleRefresh()
{
    mRefresh->start();
}

NodeId ModifierStackWidget::resolveMesh() const
{
    Graph& g = Graph::current();
    if (mPinned)
        return g.node(mPinned) ? mPinned : scene::kNoNode;
    const auto& sel = g.selection();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
    {
        scene::Node* n = g.node(*it);
        if (!n)
            continue;
        // An instance's stack is its root's, which every member shares.
        if (n->type().isA("mesh"))
            return scene::instancing::root(g, n->id());
        if (scene::modifiers::isModifier(*n) || dynamic_cast<scene::GeneratorNode*>(n) || dynamic_cast<scene::MeshSourceNode*>(n))
            if (NodeId m = scene::modifiers::meshOf(g, n->id()))
                return m;
    }
    return scene::kNoNode;
}

std::vector<uint64_t> ModifierStackWidget::signature(NodeId mesh) const
{
    Graph& g = Graph::current();
    std::vector<uint64_t> sig{mesh};
    if (!mesh)
        return sig;
    NodeId src = scene::modifiers::stackSource(g, mesh);
    auto add = [&](NodeId id) {
        sig.push_back(id);
        // A geometry-node modifier's type changes when its group's interface does.
        if (scene::Node* n = g.node(id))
            sig.push_back(uint64_t(reinterpret_cast<uintptr_t>(&n->type())));
    };
    add(src);
    for (NodeId id : scene::modifiers::stack(g, mesh))
        add(id);
    return sig;
}

void ModifierStackWidget::refresh()
{
    LILA_ZONE();

    // Hidden (closed, or a tab behind another): showEvent catches up.
    if (!isVisible())
        return;
    NodeId mesh = resolveMesh();
    std::vector<uint64_t> sig = signature(mesh);
    if (mBuilt && mesh == mMesh && sig == mSignature)
    {
        updateValues();
        return;
    }
    bool changedMesh = mesh != mMesh;
    mMesh = mesh;
    mSignature = std::move(sig);
    rebuild();
    if (changedMesh)
        Q_EMIT meshChanged(mMesh);
}

void ModifierStackWidget::rebuild()
{
    LILA_ZONE();

    int scroll = mScroll->verticalScrollBar()->value();
    mBuilt = true;
    mUpdaters.clear();
    mCards.clear();
    mList->modCards.clear();
    while (QLayoutItem* item = mListLayout->takeAt(0))
    {
        if (QWidget* w = item->widget())
        {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }

    Graph& g = Graph::current();
    g.evaluate();
    mAdd->setEnabled(mMesh != scene::kNoNode);
    mMore->setEnabled(mMesh != scene::kNoNode);
    if (!mMesh)
    {
        mTitle->setText("Select a mesh to see its modifiers");
        mFooter->clear();
        mListLayout->addStretch(1);
        return;
    }
    mTitle->setText(QString::fromStdString(nameOf(mMesh)));
    mUpdaters.push_back([this] { mTitle->setText(QString::fromStdString(nameOf(mMesh))); });

    mListLayout->addWidget(buildSourceCard(scene::modifiers::stackSource(g, mMesh)));
    std::vector<NodeId> mods = scene::modifiers::stack(g, mMesh);
    for (int i = 0; i < int(mods.size()); ++i)
    {
        QWidget* card = buildModifierCard(mods[i], i, int(mods.size()));
        mList->modCards.push_back(card);
        mListLayout->addWidget(card);
    }
    if (mods.empty())
    {
        auto* hint = new QLabel("No modifiers. Add one above, or from Mesh > Add Modifier.", mList);
        hint->setWordWrap(true);
        hint->setAlignment(Qt::AlignCenter);
        QPalette hp = hint->palette();
        hp.setColor(QPalette::WindowText, gray(130));
        hint->setPalette(hp);
        hint->setContentsMargins(8, 12, 8, 12);
        mListLayout->addWidget(hint);
    }
    mListLayout->addStretch(1);

    mUpdaters.push_back([this] {
        Graph& gr = Graph::current();
        scene::Node* m = gr.node(mMesh);
        if (!m)
            return;
        Counts base = countsOf(scene::modifiers::cage(gr, mMesh));
        Counts result = countsOf(m->as<scene::MeshPtr>(scene::MeshNode::kOutMesh));
        mFooter->setText(QString("Base %1 verts, %2 faces    Result %3 verts, %4 faces")
                             .arg(base.points).arg(base.faces).arg(result.points).arg(result.faces));
    });
    updateValues();
    QTimer::singleShot(0, this, [this, scroll] { mScroll->verticalScrollBar()->setValue(scroll); });
}

void ModifierStackWidget::updateValues()
{
    LILA_ZONE();

    Graph::current().evaluate();
    for (auto& fn : mUpdaters)
        fn();
}

void ModifierStackWidget::setCollapsed(NodeId id, bool collapsed, bool all)
{
    for (auto& [cid, card] : mCards)
    {
        if (!all && cid != id)
            continue;
        if (collapsed)
            mCollapsed.insert(cid);
        else
            mCollapsed.erase(cid);
        card.second->setVisible(!collapsed);
        if (auto* chevron = card.first->findChild<QToolButton*>("chevron"))
            chevron->setIcon(icon(collapsed ? "chevron-right" : "chevron-down"));
    }
}

QWidget* ModifierStackWidget::buildHeaderFor(QWidget* card, QWidget* body, NodeId id, bool isModifier, int index, int count)
{
    Graph& g = Graph::current();
    scene::Node* n = g.node(id);
    auto* header = new QWidget(card);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(2, 2, 2, 2);
    h->setSpacing(2);

    auto* chevron = toolButton(mCollapsed.contains(id) ? "chevron-right" : "chevron-down", "Show or hide the settings (Ctrl: all cards)", header);
    chevron->setObjectName("chevron");
    connect(chevron, &QToolButton::clicked, this, [this, id] {
        bool collapse = !mCollapsed.contains(id);
        setCollapsed(id, collapse, QApplication::keyboardModifiers() & Qt::ControlModifier);
    });
    h->addWidget(chevron);

    if (isModifier)
    {
        auto* grip = new QLabel(header);
        grip->setPixmap(icon("grip-vertical").pixmap(14, 14));
        grip->setCursor(Qt::OpenHandCursor);
        grip->setToolTip("Drag to reorder");
        grip->installEventFilter(new GripFilter(card, n->path(), grip));
        h->addWidget(grip);
    }

    auto* typeIcon = new QLabel(header);
    typeIcon->setPixmap(icon(modifierIconName(n->type().name)).pixmap(16, 16));
    QString typeDoc = QString::fromStdString(n->type().doc);
    QString kindName = niceName(kindOf(n->type().name));
    typeIcon->setToolTip(kindName + "  (" + QString::fromStdString(n->type().name) + ")" + (typeDoc.isEmpty() ? "" : "\n" + typeDoc));
    h->addWidget(typeIcon);

    auto* name = new QLineEdit(QString::fromStdString(n->name()), header);
    name->setFrame(false);
    name->setAcceptDrops(false);
    name->setToolTip(kindName + (typeDoc.isEmpty() ? "" : "\n" + typeDoc) + "\nClick to rename.");
    QPalette np = name->palette();
    np.setColor(QPalette::Base, QColor(0, 0, 0, 0));
    name->setPalette(np);
    QFont nf = name->font();
    nf.setWeight(QFont::DemiBold);
    name->setFont(nf);
    connect(name, &QLineEdit::editingFinished, this, [name, id] {
        std::string now = nameOf(id);
        std::string wanted = name->text().trimmed().toStdString();
        if (!wanted.empty() && wanted != now)
            run("rename", {{"args", {now, wanted}}});
        name->setText(QString::fromStdString(nameOf(id)));
    });
    h->addWidget(name, 1);
    mUpdaters.push_back([name, id] {
        if (!name->hasFocus())
            name->setText(QString::fromStdString(nameOf(id)));
    });

    if (!isModifier)
        return header;

    auto* mn = static_cast<scene::ModifierNode*>(n);
    auto* error = new QLabel(header);
    error->setPixmap(icon("alert-triangle").pixmap(16, 16));
    error->hide();
    h->addWidget(error);

    auto* viewport = toolButton("device-desktop", "Show in viewport", header, true);
    PlugRef enabledPlug{id, mn->enabledAttr, -1};
    connect(viewport, &QToolButton::clicked, this, [enabledPlug](bool on) { setAttr(enabledPlug, on); });
    h->addWidget(viewport);

    int exportAttr = n->type().attrIndex("exportEnabled");
    QToolButton* exporter = nullptr;
    if (exportAttr >= 0)
    {
        exporter = toolButton("file-export", "Apply on export", header, true);
        connect(exporter, &QToolButton::clicked, this, [id, exportAttr](bool on) { setAttr({id, exportAttr, -1}, on); });
        h->addWidget(exporter);
    }

    auto* apply = toolButton("check", "Apply: bake this modifier (and those before it) into the base mesh", header);
    connect(apply, &QToolButton::clicked, this, [id] { run("applyModifier", {{"args", {nameOf(id)}}}); });
    h->addWidget(apply);

    auto* menuButton = toolButton("dots-vertical", "More", header);
    menuButton->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(menuButton);
    menu->addAction(icon("check"), "Apply", this, [id] { run("applyModifier", {{"args", {nameOf(id)}}}); });
    menu->addAction(icon("copy"), "Duplicate", this, [id] { run("duplicateModifier", {{"args", {nameOf(id)}}}); });
    menu->addSeparator();
    QAction* up = menu->addAction(icon("arrow-up"), "Move Up", this, [id] { run("moveModifier", {{"args", {nameOf(id)}}, {"by", -1}}); });
    QAction* down = menu->addAction(icon("arrow-down"), "Move Down", this, [id] { run("moveModifier", {{"args", {nameOf(id)}}, {"by", 1}}); });
    QAction* first = menu->addAction("Move to First", this, [id, index] { run("moveModifier", {{"args", {nameOf(id)}}, {"by", -index}}); });
    QAction* last = menu->addAction("Move to Last", this, [id, index, count] { run("moveModifier", {{"args", {nameOf(id)}}, {"by", count - 1 - index}}); });
    up->setEnabled(index > 0);
    first->setEnabled(index > 0);
    down->setEnabled(index < count - 1);
    last->setEnabled(index < count - 1);
    menu->addSeparator();
    menu->addAction(icon("focus-2"), "Select Node", this, [id] { run("select", {{"args", {nameOf(id)}}}); });
    menu->addAction(icon("trash"), "Delete", this, [id] { run("removeModifier", {{"args", {nameOf(id)}}}); });
    menuButton->setMenu(menu);
    h->addWidget(menuButton);

    auto* remove = toolButton("x", "Delete this modifier", header);
    connect(remove, &QToolButton::clicked, this, [id] { run("removeModifier", {{"args", {nameOf(id)}}}); });
    h->addWidget(remove);

    auto* errorText = body->findChild<QLabel*>("errorText");
    mUpdaters.push_back([id, viewport, exporter, exportAttr, error, errorText, name] {
        scene::Node* node = Graph::current().node(id);
        auto* m = dynamic_cast<scene::ModifierNode*>(node);
        if (!m)
            return;
        bool on = m->as<bool>(m->enabledAttr);
        viewport->setChecked(on);
        viewport->setIcon(icon(on ? "device-desktop" : "device-desktop-off"));
        if (exporter)
        {
            bool ex = m->as<bool>(exportAttr);
            exporter->setChecked(ex);
        }
        QPalette p = name->palette();
        p.setColor(QPalette::Text, on ? gray(220) : gray(120));
        name->setPalette(p);
        QString msg = QString::fromStdString(m->error());
        error->setVisible(!msg.isEmpty());
        error->setToolTip(msg);
        if (errorText)
        {
            errorText->setVisible(!msg.isEmpty());
            errorText->setText("Failed, passing its input through: " + msg);
        }
    });
    return header;
}

QWidget* ModifierStackWidget::buildMeshInputRow(const PlugRef& ref, QWidget* parent)
{
    // Only the transfer modifier's source is picked here; other mesh inputs are wired by their own tools.
    if (Graph::current().node(ref.node)->type().name != "transferAttributesModifier")
        return nullptr;
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto* name = new QLabel(row);
    h->addWidget(name, 1);
    auto* pick = toolButton("arrow-big-left-lines", "Use the selected mesh as the source (transferSource)", row);
    h->addWidget(pick);
    NodeId mod = ref.node;
    connect(pick, &QToolButton::clicked, this, [mod] {
        Graph& g = Graph::current();
        NodeId target = scene::modifiers::meshOf(g, mod);
        for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
            if (scene::Node* n = g.node(*it); n && n->type().isA("mesh") && n->id() != target)
            {
                run("transferSource", {{"args", {nameOf(mod), n->path()}}});
                return;
            }
        log::error("Transfer source: select the source mesh (not the one being modified), then click again");
    });
    mUpdaters.push_back([name, ref] {
        Graph& g = Graph::current();
        auto src = g.source(ref);
        scene::Node* n = src ? g.node(src->node) : nullptr;
        name->setText(n ? QString::fromStdString(n->name()) : QString("(none)"));
        name->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    });
    return row;
}

QWidget* ModifierStackWidget::buildArrayCenterRow(NodeId mod, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    auto* name = new QLabel(row);
    h->addWidget(name, 1);
    auto* pick = toolButton("arrow-big-left-lines", "Centre on the selected object's transform (arrayCenter)", row);
    auto* pivot = toolButton("focus-centered", "Centre on this mesh's pivot (arrayCenter pivot=True)", row);
    auto* clear = toolButton("unlink", "Back to the object's origin", row);
    h->addWidget(pick);
    h->addWidget(pivot);
    h->addWidget(clear);
    connect(pick, &QToolButton::clicked, this, [mod] {
        Graph& g = Graph::current();
        NodeId target = scene::modifiers::meshOf(g, mod);
        for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
            if (scene::Node* n = g.node(*it); n && n->type().isA("transform") && n->id() != target)
            {
                run("arrayCenter", {{"args", {nameOf(mod), n->path()}}});
                return;
            }
        log::error("Array centre: select the object to centre on (not the arrayed mesh), then click again");
    });
    connect(pivot, &QToolButton::clicked, this, [mod] { run("arrayCenter", {{"args", {nameOf(mod)}}, {"pivot", true}}); });
    connect(clear, &QToolButton::clicked, this, [mod] { run("arrayCenter", {{"args", {nameOf(mod)}}}); });
    mUpdaters.push_back([name, mod] {
        Graph& g = Graph::current();
        scene::Node* m = g.node(mod);
        if (!m)
            return;
        auto src = g.source({mod, m->type().attrIndex("centerMatrix"), -1});
        scene::Node* n = src ? g.node(src->node) : nullptr;
        name->setText(n ? QString::fromStdString(n->name()) : QString("(none)"));
        name->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    });
    return row;
}

QWidget* ModifierStackWidget::buildParams(NodeId id, bool isModifier)
{
    Graph& g = Graph::current();
    scene::Node* n = g.node(id);
    auto* box = new QWidget;
    auto* form = new QFormLayout(box);
    form->setLabelAlignment(Qt::AlignRight);
    form->setContentsMargins(8, 2, 6, 4);
    form->setSpacing(3);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    int rows = 0;
    const auto& attrs = n->type().attrs;
    // Attrs keep their order; a category gets its heading the first time one of its attrs shows.
    std::vector<int> order;
    std::vector<std::string> categories{""};
    for (int i = 0; i < int(attrs.size()); ++i)
        if (std::find(categories.begin(), categories.end(), attrs[i].category) == categories.end())
            categories.push_back(attrs[i].category);
    for (const std::string& c : categories)
        for (int i = 0; i < int(attrs.size()); ++i)
            if (attrs[i].category == c)
                order.push_back(i);
    std::string heading;
    for (int i : order)
    {
        const scene::AttrDef& def = attrs[i];
        if ((def.flags & (scene::AttrHidden | scene::AttrArray)) || !(def.flags & scene::AttrInput))
            continue;
        if (def.category != heading)
        {
            heading = def.category;
            auto* h = new QLabel(QString::fromStdString(heading), box);
            h->setStyleSheet("color: #c8c8c8; font-weight: 600; padding-top: 4px; border-bottom: 1px solid #474747;");
            form->addRow(h);
        }
        if (def.type == PlugType::Mesh)
        {
            if (QWidget* row = buildMeshInputRow({id, i, -1}, box))
            {
                auto* label = new QLabel(niceName(def.name == "sourceMesh" ? "source" : def.name));
                label->setToolTip(QString::fromStdString(def.doc));
                form->addRow(label, row);
                ++rows;
            }
            continue;
        }
        if (def.type == PlugType::Data || def.type == PlugType::Matrix)
            continue;
        if (isModifier && (def.name == "enabled" || def.name == "exportEnabled"))
            continue;
        QString doc = QString::fromStdString(def.doc);
        auto* label = new QLabel(def.niceName.empty() ? niceName(def.name) : QString::fromStdString(def.niceName));
        label->setToolTip(QString::fromStdString(def.name + (def.doc.empty() ? "" : "\n" + def.doc)));
        QWidget* field = nullptr;
        PlugRef ref{id, i, -1};
        switch (def.type)
        {
        case PlugType::Float:
        case PlugType::Int:
        {
            auto* e = new ScrubEdit(ref, def.type == PlugType::Int, box);
            e->setDoc(doc);
            e->attachLabel(label);
            mUpdaters.push_back([e] { e->refresh(Graph::current()); });
            field = e;
            break;
        }
        case PlugType::Vec3:
        {
            field = new QWidget(box);
            auto* h = new QHBoxLayout(field);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            for (int c = 0; c < 3; ++c)
            {
                auto* e = new ScrubEdit({id, i, c}, false, field);
                e->setDoc(QString::fromStdString(def.childNames[c]) + (doc.isEmpty() ? "" : ": " + doc));
                h->addWidget(e);
                mUpdaters.push_back([e] { e->refresh(Graph::current()); });
            }
            resetOnMiddleClick(label, ref, field);
            break;
        }
        case PlugType::Bool:
        {
            auto* cb = new QCheckBox(box);
            connect(cb, &QCheckBox::clicked, this, [ref](bool on) { setAttr(ref, on); });
            cb->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(cb, &QWidget::customContextMenuRequested, cb, [cb, ref](const QPoint& at) {
                QMenu menu;
                addResetAction(&menu, ref, cb);
                addKeyActions(&menu, ref);
                if (!menu.isEmpty())
                    menu.exec(cb->mapToGlobal(at));
            });
            mUpdaters.push_back([cb, ref] {
                Graph& gr = Graph::current();
                QSignalBlocker block(cb);
                cb->setChecked(std::get<bool>(gr.value(ref)));
                cb->setEnabled(stateOf(gr, ref) != PlugState::Driven);
            });
            field = cb;
            resetOnMiddleClick(label, ref, field);
            break;
        }
        case PlugType::Enum:
        {
            auto* combo = new QComboBox(box);
            for (const std::string& e : def.enumNames)
                combo->addItem(niceName(e));
            connect(combo, &QComboBox::activated, this, [ref](int k) { setAttr(ref, int64_t(k)); });
            mUpdaters.push_back([combo, ref] {
                Graph& gr = Graph::current();
                QSignalBlocker block(combo);
                combo->setCurrentIndex(int(std::get<int64_t>(gr.value(ref))));
                combo->setEnabled(stateOf(gr, ref) != PlugState::Driven);
            });
            field = combo;
            resetOnMiddleClick(label, ref, field);
            break;
        }
        case PlugType::String:
        {
            auto* e = new QLineEdit(box);
            e->setAcceptDrops(false);
            connect(e, &QLineEdit::editingFinished, this, [e, ref] {
                if (!e->isModified())
                    return;
                e->setModified(false);
                setAttr(ref, e->text().toStdString());
            });
            mUpdaters.push_back([e, ref] {
                if (!e->hasFocus())
                    e->setText(QString::fromStdString(std::get<std::string>(Graph::current().value(ref))));
            });
            field = e;
            resetOnMiddleClick(label, ref, field);
            break;
        }
        default: break;
        }
        if (!field)
        {
            delete label;
            continue;
        }
        if (!doc.isEmpty() && field->toolTip().isEmpty())
            field->setToolTip(doc);
        form->addRow(label, field);
        ++rows;
        if (n->type().name == "arrayModifier" && def.name == "center")
        {
            auto* objectLabel = new QLabel("Center Object");
            objectLabel->setToolTip("The object whose transform the copies turn about when center is object.");
            form->addRow(objectLabel, buildArrayCenterRow(id, box));
            ++rows;
        }
    }
    if (!rows)
    {
        delete box;
        return nullptr;
    }
    return box;
}

QWidget* ModifierStackWidget::buildSourceCard(NodeId source)
{
    Graph& g = Graph::current();
    auto* card = new QFrame(mList);
    card->setObjectName("modCard");
    card->setStyleSheet("#modCard { background: #2c2c2c; border: 1px solid #3a3a3a; border-radius: 4px; }");
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* body = new QWidget(card);
    auto* bv = new QVBoxLayout(body);
    bv->setContentsMargins(0, 0, 0, 0);
    bv->setSpacing(2);

    scene::Node* src = g.node(source);
    bool generator = src && dynamic_cast<scene::GeneratorNode*>(src);
    if (generator)
    {
        v->addWidget(buildHeaderFor(card, body, source, false, 0, 0));
        if (QWidget* params = buildParams(source, false))
            bv->addWidget(params);
    }
    else
    {
        // The base mesh: a meshSource under the stack, or the mesh's own stored inMesh.
        auto* header = new QWidget(card);
        auto* h = new QHBoxLayout(header);
        h->setContentsMargins(8, 4, 4, 4);
        h->setSpacing(6);
        auto* ic = new QLabel(header);
        ic->setPixmap(icon("box").pixmap(16, 16));
        h->addWidget(ic);
        auto* title = new QLabel(src ? "Base Mesh" : "Base Mesh (stored on the mesh)", header);
        QFont f = title->font();
        f.setWeight(QFont::DemiBold);
        title->setFont(f);
        title->setToolTip("Direct component edits land here, under every modifier.");
        h->addWidget(title, 1);
        v->addWidget(header);
    }
    v->addWidget(body);
    body->setVisible(!mCollapsed.contains(source));
    mCards[source] = {card, body};
    return card;
}

QWidget* ModifierStackWidget::buildModifierCard(NodeId mod, int index, int count)
{
    auto* card = new QFrame(mList);
    card->setObjectName("modCard");
    card->setStyleSheet("#modCard { background: #353535; border: 1px solid #424242; border-radius: 4px; }");
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* body = new QWidget(card);
    auto* bv = new QVBoxLayout(body);
    bv->setContentsMargins(0, 0, 0, 4);
    bv->setSpacing(2);
    auto* errorText = new QLabel(body);
    errorText->setObjectName("errorText");
    errorText->setWordWrap(true);
    errorText->setContentsMargins(10, 2, 8, 2);
    QFont ef = errorText->font();
    ef.setWeight(QFont::DemiBold);
    errorText->setFont(ef);
    errorText->hide();
    bv->addWidget(errorText);
    if (QWidget* params = buildParams(mod, true))
        bv->addWidget(params);
    for (auto& [title, w] : attributeEditor::buildSections(mod))
    {
        auto* caption = new QLabel(QString::fromStdString(title), body);
        caption->setContentsMargins(10, 4, 4, 0);
        QPalette cp = caption->palette();
        cp.setColor(QPalette::WindowText, gray(150));
        caption->setPalette(cp);
        bv->addWidget(caption);
        w->setParent(body);
        w->setContentsMargins(6, 0, 6, 0);
        bv->addWidget(w);
    }

    v->addWidget(buildHeaderFor(card, body, mod, true, index, count));
    v->addWidget(body);
    body->setVisible(!mCollapsed.contains(mod));
    mCards[mod] = {card, body};
    return card;
}

void ModifierStackWidget::dropModifier(const std::string& name, int to)
{
    Graph& g = Graph::current();
    scene::Node* n = findNode(name);
    if (!n || !mMesh)
        return;
    std::vector<NodeId> mods = scene::modifiers::stack(g, mMesh);
    auto it = std::find(mods.begin(), mods.end(), n->id());
    if (it == mods.end())
        return;
    int from = int(it - mods.begin());
    if (to > from)
        --to;
    if (to != from)
        run("moveModifier", {{"args", {name}}, {"by", to - from}});
}

void fillAddModifierMenu(QMenu* menu, bool stackRow)
{
    constexpr const char* kRow = "lilaModifierRow";
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, stackRow] {
        for (QAction* a : menu->actions())
            if (a->property(kRow).toBool())
                delete a;
        QAction* anchor = menu->actions().value(0);
        auto add = [&](QAction* a) {
            a->setProperty(kRow, true);
            menu->insertAction(anchor, a);
            return a;
        };
        if (!stackRow)
            add(new QAction("Add", menu))->setSeparator(true);
        bool any = false;
        for (const std::string& kind : scene::modifiers::kinds())
        {
            QAction* a = add(new QAction(icon(modifierIconName(kind)), niceName(kind), menu));
            any = true;
            std::string id = "modifier." + kind;
            a->setData(QString::fromStdString(id));
            std::string doc = scene::modifiers::kindDoc(kind);
            std::string tip = actions::find(id) ? actions::tooltip(id) : niceName(kind).toStdString() + (doc.empty() ? "" : "\n" + doc);
            if (const scene::NodeType* t = scene::types::find(kind + std::string(kSuffix)); t && !t->owner.empty())
                tip += "\nFrom " + t->owner;
            a->setToolTip(QString::fromStdString(tip));
            a->setStatusTip(QString::fromStdString(doc));
            QObject::connect(a, &QAction::triggered, menu, [kind] {
                Graph& g = Graph::current();
                json meshes = json::array();
                for (NodeId id : g.selection())
                    if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                        meshes.push_back(n->path());
                if (meshes.empty())
                {
                    log::error("Add Modifier: select a mesh");
                    return;
                }
                UndoChunk chunk("Add " + kind + " Modifier");
                for (const json& m : meshes)
                    run("addModifier", {{"args", {m}}, {"kind", kind}});
                showModifierStack();
            });
        }
        if (!any)
            add(new QAction("No modifier kinds registered", menu))->setEnabled(false);
        if (stackRow)
        {
            QAction* sep = add(new QAction(menu));
            sep->setSeparator(true);
            QAction* stack = add(new QAction(icon("stack-2"), "Modifier Stack...", menu));
            QObject::connect(stack, &QAction::triggered, menu, [] { showModifierStack(); });
        }
    });
}

void showModifierStack()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = w->findChild<QDockWidget*>(kModifierStackDock))
        {
            d->show();
            d->raise();
            flashLocate(d);
            return;
        }
}

void registerModifierStackActions()
{
    actions::add({.id = "window.modifierStack", .label = "Modifier Stack", .category = "Windows", .run = [] {
                      for (QWidget* w : QApplication::topLevelWidgets())
                          if (auto* d = w->findChild<QDockWidget*>(kModifierStackDock))
                          {
                              toggleDock(d);
                              return;
                          }
                  },
                  .doc = "The selected mesh's modifiers as cards: add, reorder, toggle, apply and edit them.",
                  .python = "lila.ui.run_action(\"window.modifierStack\")"});
    auto onMeshes = [](const char* command) {
        return [command] {
            json meshes = json::array();
            Graph& g = Graph::current();
            for (NodeId id : g.selection())
                if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                    meshes.push_back(n->path());
            if (meshes.empty())
                log::error("select a mesh");
            else
                run(command, {{"args", meshes}});
        };
    };
    actions::add({.id = "modifier.applyAll", .label = "Apply All Modifiers", .category = "Modifiers", .run = onMeshes("applyAllModifiers"),
                  .doc = "Bake every modifier of the selected meshes into their base mesh.", .python = "applyAllModifiers()"});
}
} // namespace lila::ui
