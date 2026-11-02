#include "ui/AttributeEditor.h"
#include "ui/AttrAuthoring.h"
#include "ui/AttrView.h"
#include "ui/ChoiceTips.h"
#include "ui/DrivePopover.h"
#include "ui/DocsUi.h"
#include "core/PrefStore.h"
#include "scene/AttrComponents.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "ui/ColorPopup.h"
#include "ui/Deletion.h"
#include "ui/FontPicker.h"
#include "ui/Icons.h"
#include "ui/ImagePicker.h"
#include "ui/Issues.h"
#include "ui/CodeEdit.h"
#include "ui/MultilineEdit.h"
#include "ui/NumberExpr.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"
#include "ui/FileDialog.h"

#include <QGuiApplication>
#include <QApplication>
#include <QDrag>
#include <QButtonGroup>
#include <QRadioButton>
#include <QMimeData>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <cmath>
#include <filesystem>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::PlugType;
namespace dx = scene::diagnostics;

std::vector<AttributeEditor*> gEditors;
/** An issue to scroll to once an editor shows its object. */
uint64_t gPendingReveal = 0;

/**
 * A row label that elides instead of holding its full width, so a narrow editor squeezes names before fields. An issue
 * on its attribute puts a badge before the name.
 */
class AttrRowLabel final : public QLabel
{
public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {fontMetrics().horizontalAdvance("Wwwww"), QLabel::minimumSizeHint().height()}; }
    void setMark(QPixmap mark)
    {
        mMark = std::move(mark);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QRect r = contentsRect();
        const int gap = mMark.isNull() ? 0 : mMark.width() + 4;
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, r.width() - gap);
        if (!mMark.isNull())
        {
            const int textWidth = fontMetrics().horizontalAdvance(shown);
            const int x = (alignment() & Qt::AlignRight) ? r.right() - textWidth - gap + 1 : r.left();
            p.drawPixmap(x, r.center().y() - mMark.height() / 2, mMark);
            if (!(alignment() & Qt::AlignRight))
                r.setLeft(r.left() + gap);
        }
        style()->drawItemText(&p, r, int(alignment()), palette(), isEnabled(), shown, foregroundRole());
    }

private:
    QPixmap mMark;
};

constexpr const char* kAttrMime = "application/x-lila-attr";
constexpr const char* kHeadingMime = "application/x-lila-heading";
constexpr const char* kPlugMime = "application/x-lila-plug";

/** Drag, drop, double-click and hover hooks on one widget, for the editor's rows and headings. */
class RowGestures final : public QObject
{
public:
    explicit RowGestures(QWidget* on) : QObject(on) { on->installEventFilter(this); }

    std::function<void()> doubleClicked;
    /** Starts a drag of this mime type and text from a press and move; empty for none. */
    QString dragMime, dragText;
    std::function<bool(const QMimeData*)> accepts;
    std::function<void(const QMimeData*)> dropped;
    /** Shown only while the pointer is over the widget. */
    std::vector<QWidget*> reveal;

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        auto* w = static_cast<QWidget*>(o);
        switch (e->type())
        {
        case QEvent::Enter:
        case QEvent::Leave:
            for (QWidget* r : reveal)
                r->setVisible(e->type() == QEvent::Enter);
            break;
        case QEvent::MouseButtonDblClick:
            if (doubleClicked && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
            {
                doubleClicked();
                return true;
            }
            break;
        case QEvent::MouseButtonPress:
            mPress = static_cast<QMouseEvent*>(e)->pos();
            mPressed = static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton && !dragMime.isEmpty();
            break;
        case QEvent::MouseMove:
            if (mPressed && (static_cast<QMouseEvent*>(e)->pos() - mPress).manhattanLength() >= QApplication::startDragDistance())
            {
                mPressed = false;
                auto* data = new QMimeData;
                data->setData(dragMime, dragText.toUtf8());
                auto* drag = new QDrag(w);
                drag->setMimeData(data);
                drag->setPixmap(w->grab());
                drag->exec(Qt::MoveAction);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease: mPressed = false; break;
        case QEvent::DragEnter:
        case QEvent::DragMove:
        {
            auto* d = static_cast<QDropEvent*>(e);
            if (accepts && accepts(d->mimeData()))
            {
                d->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop:
        {
            auto* d = static_cast<QDropEvent*>(e);
            if (accepts && dropped && accepts(d->mimeData()))
            {
                d->acceptProposedAction();
                dropped(d->mimeData());
                return true;
            }
            break;
        }
        default: break;
        }
        return false;
    }

private:
    QPoint mPress;
    bool mPressed = false;
};

std::string mimeText(const QMimeData* d, const char* format) { return QString::fromUtf8(d->data(format)).toStdString(); }

/** Marks a label or section header with the worst of the issues and lists them all in its tooltip. */
void markIssues(QWidget* w, const std::vector<const dx::Issue*>& list)
{
    std::optional<issues::Worst> worst = issues::worst(list);
    if (!worst)
        return;
    w->setToolTip(w->toolTip() + (w->toolTip().isEmpty() ? "" : "\n\n") + issues::lines(list));
    if (auto* label = dynamic_cast<AttrRowLabel*>(w))
        label->setMark(issues::badge(worst->severity, worst->blocking, label->fontMetrics().height() - 4));
    else
        w->setStyleSheet(w->styleSheet() + QString(" color: %1;").arg(issues::color(worst->severity).name()));
}

/** A number a scrub drives: its plug, its component of a vector (-1 for a scalar) and the attr name setAttr takes. */
struct ScrubTarget
{
    scene::PlugRef ref;
    int component = -1;
    std::string attr;
    std::optional<double> minValue, maxValue;
    bool isInt = false;
};

double numberOf(const scene::PlugValue& v, int component)
{
    if (auto* v3 = std::get_if<glm::dvec3>(&v); v3 && component >= 0)
        return (*v3)[component];
    if (auto* f = std::get_if<double>(&v))
        return *f;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return 0.0;
}

/**
 * Channel box style virtual slider: a drag steps its targets 0.1 per pixel (Shift 1, Ctrl 0.01) live, and the release
 * commits them through setAttr as one undo step. The middle button drags.
 */
class NumberScrub final : public QObject
{
public:
    using Commit = std::function<void(const std::string& attr, const json& value)>;

    NumberScrub(QWidget* on, std::vector<ScrubTarget> targets, Commit commit)
        : QObject(on), mTargets(std::move(targets)), mCommit(std::move(commit))
    {
        on->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
        {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::MiddleButton)
                begin(me->globalPosition().toPoint());
            // Not consumed: a middle click without a drag still resets.
            return false;
        }
        case QEvent::MouseMove:
            return mActive && move(static_cast<QMouseEvent*>(e));
        case QEvent::MouseButtonRelease: return mActive && end();
        default: return false;
        }
    }

private:
    void begin(QPoint at)
    {
        Graph& g = Graph::current();
        mSaved.clear();
        for (const ScrubTarget& t : mTargets)
            if (g.node(t.ref.node) && !g.isLocked(t.ref))
                mSaved.push_back({t, g.value(t.ref)});
        mActive = !mSaved.empty();
        mMoved = false;
        mStart = mLast = at;
        mAccum = 0.0;
    }

    bool move(QMouseEvent* me)
    {
        // The release can go missing (focus stolen mid-drag); a move with no button held ends the drag.
        if (me->buttons() == Qt::NoButton)
            return end();
        const QPoint at = me->globalPosition().toPoint();
        const int dx = at.x() - mLast.x();
        mLast = at;
        if (!mMoved && std::abs(at.x() - mStart.x()) < valueReset::kMaxTravel)
            return false;
        if (!mMoved)
        {
            UndoStack::get().openChunk("Drag Attribute");
            mMoved = true;
        }
        const Qt::KeyboardModifiers mods = me->modifiers();
        mAccum += dx * ((mods & Qt::ControlModifier) ? 0.01 : (mods & Qt::ShiftModifier) ? 1.0 : 0.1);
        Graph& g = Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const auto& [t, start] : mSaved)
            if (g.node(t.ref.node))
                g.setValue(t.ref, withComponent(g.value(t.ref), t, valueFor(t, start)));
        u.setEnabled(was);
        return true;
    }

    bool end()
    {
        mActive = false;
        if (!mMoved)
            return false;
        Graph& g = Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const auto& [t, start] : mSaved)
            if (g.node(t.ref.node))
                g.setValue(t.ref, start);
        u.setEnabled(was);
        // Inside the chunk opened when the drag started, so the drag undoes as one step.
        for (const auto& [t, start] : mSaved)
        {
            const double v = valueFor(t, start);
            mCommit(t.attr, t.isInt ? json(int64_t(std::llround(v))) : json(std::round(v * 1e4) / 1e4));
        }
        UndoStack::get().closeChunk();
        mMoved = false;
        return true;
    }

    double valueFor(const ScrubTarget& t, const scene::PlugValue& start) const
    {
        double v = numberOf(start, t.component) + mAccum;
        if (t.minValue)
            v = std::max(v, *t.minValue);
        if (t.maxValue)
            v = std::min(v, *t.maxValue);
        return t.isInt ? std::round(v) : v;
    }

    static scene::PlugValue withComponent(scene::PlugValue current, const ScrubTarget& t, double v)
    {
        if (auto* v3 = std::get_if<glm::dvec3>(&current); v3 && t.component >= 0)
            (*v3)[t.component] = v;
        else if (std::holds_alternative<int64_t>(current))
            current = int64_t(std::llround(v));
        else
            current = v;
        return current;
    }

    std::vector<ScrubTarget> mTargets;
    std::vector<std::pair<ScrubTarget, scene::PlugValue>> mSaved;
    Commit mCommit;
    bool mActive = false;
    bool mMoved = false;
    QPoint mStart, mLast;
    double mAccum = 0.0;
};

QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

QString fmt(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 4);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

/** Folder a file browser opens in for a stored path. */
QString browseStart(const std::string& stored)
{
    namespace fs = std::filesystem;
    fs::path p = paths::resolvePath(stored);
    std::error_code ec;
    if (!stored.empty() && fs::is_directory(p.parent_path(), ec))
        return QString::fromStdU16String(p.parent_path().u16string());
    return Workspace::get().isOpen() ? QString::fromStdU16String(Workspace::get().root().u16string()) : QString();
}

/** Collapsible section header, Maya frameLayout style, in the color of the worst issue on its component. */
QWidget* section(const QString& title, QWidget* content, const std::vector<const dx::Issue*>& marks = {})
{
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton;
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(Qt::DownArrow);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QString style = "text-align: left; font-weight: 600; padding: 3px;";
    if (std::optional<issues::Worst> worst = issues::worst(marks))
    {
        style += QString(" color: %1;").arg(issues::color(worst->severity).name());
        header->setToolTip(issues::lines(marks));
    }
    header->setStyleSheet("QToolButton { " + style + " }");
    QObject::connect(header, &QToolButton::toggled, content, [header, content](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    v->addWidget(header);
    v->addWidget(content);
    return box;
}

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

std::vector<attributeEditor::Section*> extraSections()
{
    return attributeEditor::registry().allInOrder();
}
} // namespace

Registry<attributeEditor::AttrFilter>& attributeEditor::filters()
{
    static Registry<AttrFilter>& r = []() -> Registry<AttrFilter>& {
        static Registry<AttrFilter> made;
        using scene::lights::Kind;
        made.put("lila.light",
                 {.typeName = "light",
                  .triggers = {"lightType"},
                  .shows = [](const scene::Node& n, const std::string& attr) {
                      const Kind k = scene::lights::settings(n).kind;
                      const bool spot = k == Kind::Spot, sized = k == Kind::Area || k == Kind::Disc;
                      if (attr == "innerConeAngle" || attr == "outerConeAngle")
                          return spot;
                      if (attr == "width")
                          return sized;
                      if (attr == "height")
                          return k == Kind::Area;
                      if (attr == "sourceRadius")
                          return k == Kind::Point || spot || k == Kind::Directional;
                      if (attr == "range")
                          return k != Kind::Directional && k != Kind::Dome;
                      if (attr == "texture")
                          return k == Kind::Dome || k == Kind::Point || spot;
                      return true;
                  }},
                 kLilaOwner);
        return made;
    }();
    return r;
}

Registry<attributeEditor::Section>& attributeEditor::registry()
{
    static Registry<Section> r;
    return r;
}

void attributeEditor::addSection(std::string typeName, std::string title, SectionFactory fn, std::string owner)
{
    std::string id = typeName + "/" + title;
    registry().put(std::move(id), {std::move(typeName), std::move(title), std::move(fn)}, std::move(owner));
}

uint64_t attributeEditor::addSection(ext::Context& ctx, std::string typeName, std::string title, SectionFactory fn)
{
    std::string id = typeName + "/" + title;
    if (const std::string* owner = registry().ownerOf(id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "Attribute Editor section '" + id + "' already belongs to " + *owner);
        return 0;
    }
    addSection(std::move(typeName), std::move(title), std::move(fn), ctx.owner());
    return ctx.track("attributeEditorSection:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

void attributeEditor::revealIssue(uint64_t id)
{
    gPendingReveal = id;
    for (AttributeEditor* e : gEditors)
        if (e->reveal(id))
            gPendingReveal = 0;
}

bool attributeEditor::removeSection(const std::string& typeName, const std::string& title)
{
    return registry().remove(typeName + "/" + title);
}

std::vector<std::pair<std::string, QWidget*>> attributeEditor::buildSections(scene::NodeId id)
{
    std::vector<std::pair<std::string, QWidget*>> out;
    scene::Node* n = Graph::current().node(id);
    if (!n)
        return out;
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
                out.emplace_back(extra->title, w);
    return out;
}

AttributeEditor::AttributeEditor(QWidget* parent) : QWidget(parent)
{
    deletion::claimScene(this);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    setProperty("lilaMultiEditPanel", true);
    attrView::installKeyWatch();
    auto* top = new QHBoxLayout;
    mType = new QLabel(this);
    mType->setStyleSheet("color: #9a9a9a;");
    mName = new QLineEdit(this);
    top->addWidget(new QLabel("Name", this));
    top->addWidget(mName, 1);
    top->addWidget(mType);
    outer->addLayout(top);
    auto* find = new QHBoxLayout;
    mSearch = new QLineEdit(this);
    mSearch->setObjectName("attrSearch");
    mSearch->setPlaceholderText("Search attributes");
    mSearch->setClearButtonEnabled(true);
    mSearch->addAction(icon("search"), QLineEdit::LeadingPosition);
    connect(mSearch, &QLineEdit::textChanged, this, [this] { applySearch(); });
    mAdd = new QToolButton(this);
    mAdd->setObjectName("attrAdd");
    mAdd->setIcon(icon("plus"));
    mAdd->setAutoRaise(true);
    mAdd->setToolTip("Add an attribute or component");
    connect(mAdd, &QToolButton::clicked, this, [this] { openAdd(mAdd); });
    find->addWidget(mSearch, 1);
    find->addWidget(mAdd);
    outer->addLayout(find);
    mChips = new QWidget(this);
    mChips->setObjectName("aeChips");
    new QHBoxLayout(mChips);
    mChips->layout()->setContentsMargins(0, 0, 0, 0);
    mChips->layout()->setSpacing(3);
    outer->addWidget(mChips);
    mHint = new QFrame(this);
    mHint->setObjectName("multiEditHint");
    mHint->setStyleSheet(QString("#multiEditHint { background: %1; border-radius: 3px; }").arg(themeColor("altBase").name()));
    auto* hintRow = new QHBoxLayout(mHint);
    hintRow->setContentsMargins(6, 3, 6, 3);
    mHintText = new QLabel(mHint);
    mHintText->setObjectName("multiEditHintText");
    mHintText->setWordWrap(true);
    auto* hintOff = new QLabel("<a href=\"off\">Turn off this hint</a>", mHint);
    hintOff->setObjectName("multiEditHintOff");
    connect(hintOff, &QLabel::linkActivated, this, [this] {
        attrView::setHintEnabled(false);
        mHint->hide();
    });
    hintRow->addWidget(mHintText, 1);
    hintRow->addWidget(hintOff);
    mHint->hide();
    outer->addWidget(mHint);
    mViewSig = viewSignature();
    mViewListener = attrView::addListener([this] {
        const std::string sig = viewSignature();
        if (sig != mViewSig)
        {
            mViewSig = sig;
            mRebuild = true;
        }
        refreshMultiState();
        scheduleRefresh();
    });
    connect(mName, &QLineEdit::editingFinished, this, [this] {
        scene::Node* n = Graph::current().node(mNode);
        if (!n || mName->text().isEmpty() || mName->text().toStdString() == n->name())
            return;
        try
        {
            ops::execute("rename", {{"args", {n->path(), mName->text().toStdString()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });

    auto* scroll = mScroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->installEventFilter(this);
    mBody = new QWidget;
    mBodyLayout = new QVBoxLayout(mBody);
    mBodyLayout->setContentsMargins(0, 0, 0, 0);
    mBodyLayout->setSpacing(6);
    scroll->setWidget(mBody);
    outer->addWidget(scroll, 1);

    mSectionListener = attributeEditor::registry().addListener([this](RegistryChange, const std::string&, const std::string&) {
        mRebuild = true;
        scheduleRefresh();
    });
    mListener = Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        // An array element's change may add or drop a row, so element edits rebuild too.
        const scene::PlugRef* plug = Graph::current().changedPlug();
        if (c != scene::Change::Value || (plug && plug->element >= 0) || (plug && plug->node == mNode && mTriggers.contains(plug->attr)))
            mRebuild = true;
        scheduleRefresh();
    });
    mIssueListener = dx::addListener([this] {
        if (mHadIssues || !dx::issuesOf(mNode).empty())
        {
            mRebuild = true;
            scheduleRefresh();
        }
    });
    gEditors.push_back(this);
    rebuild();
}

AttributeEditor::~AttributeEditor()
{
    std::erase(gEditors, this);
    attrView::removeListener(mViewListener);
    dx::removeListener(mIssueListener);
    Graph::current().removeListener(mListener);
    attributeEditor::registry().removeListener(mSectionListener);
}

bool AttributeEditor::reveal(uint64_t issue)
{
    const dx::Issue* i = dx::find(issue);
    if (!i || i->node != mNode || mNode == scene::kNoNode)
        return false;
    QWidget* to = nullptr;
    if (!i->attr.empty())
        if (auto it = mAttrWidgets.find(i->attr); it != mAttrWidgets.end())
            to = it->second;
    if (!to && !i->component.empty())
        if (auto it = mComponentWidgets.find(QString::fromStdString(i->component).toLower().toStdString()); it != mComponentWidgets.end())
            to = it->second;
    if (!to)
        to = mBody->findChild<QWidget*>("issueList");
    if (!to)
        return false;
    // After the layout settles: a fresh rebuild's widgets have no geometry yet.
    QTimer::singleShot(0, this, [this, to = QPointer<QWidget>(to)] {
        if (to)
            mScroll->ensureWidgetVisible(to, 0, 40);
    });
    return true;
}

QWidget* AttributeEditor::issueList()
{
    const std::vector<const dx::Issue*> list = dx::issuesOf(mNode);
    if (list.empty())
        return nullptr;
    auto* box = new QWidget;
    box->setObjectName("issueList");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 2, 4, 4);
    v->setSpacing(2);
    for (const dx::Issue* i : list)
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* badge = new QLabel;
        const int h = fontMetrics().height() - 2;
        badge->setPixmap(issues::badge(i->severity, i->blocking(), h));
        badge->setFixedSize(h, h);
        badge->setToolTip(QString(dx::severityName(i->severity)) + (i->blocking() ? " (blocking)" : ""));
        row->addWidget(badge, 0, Qt::AlignTop);
        const QString at = issues::where(*i);
        const QString message = QString::fromStdString(i->message).toHtmlEscaped();
        auto* text = new QLabel(at.isEmpty() ? message
                                             : QString("<a href=\"#\" style=\"color: %1;\">%2</a>: %3")
                                                   .arg(issues::color(i->severity).name(), at.toHtmlEscaped(), message));
        text->setWordWrap(true);
        text->setTextFormat(Qt::RichText);
        std::string tip = "From " + i->source;
        if (i->blocking())
        {
            tip += "\nBlocks";
            for (const std::string& c : i->blocks)
                tip += " " + c;
        }
        text->setToolTip(QString::fromStdString(tip));
        const uint64_t id = i->id;
        connect(text, &QLabel::linkActivated, this, [this, id](const QString&) { reveal(id); });
        row->addWidget(text, 1);
        if (!i->fix.empty())
        {
            auto* fix = new QToolButton;
            fix->setText("Fix");
            fix->setAutoRaise(true);
            fix->setToolTip(QString::fromStdString(i->fix));
            connect(fix, &QToolButton::clicked, this, [id] {
                try
                {
                    ops::execute("fixIssue", {{"args", {id}}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
            row->addWidget(fix, 0, Qt::AlignTop);
        }
        v->addLayout(row);
    }
    return box;
}

void AttributeEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    // While a drag edits the scene every frame, the fields catch up ten times a second instead of every frame.
    QTimer::singleShot(QGuiApplication::mouseButtons() != Qt::NoButton ? 100 : 0, this, [this] {
        mQueued = false;
        if (mRebuild || shownNode() != mNode || Graph::current().selection() != mLastSelection)
            rebuild();
        else
            updateValues();
        mRebuild = false;
    });
}

void AttributeEditor::set(const std::string& attr, const json& value)
{
    Graph& g = Graph::current();
    scene::Node* n = g.node(mNode);
    if (!n)
        return;
    const std::vector<scene::NodeId> all = holders(attr);
    const bool many = all.size() > 1 && attrView::multiEditHeld();
    try
    {
        UndoChunk chunk("Set " + attr);
        for (scene::NodeId id : many ? all : std::vector<scene::NodeId>{mNode})
            ops::execute("setAttr", {{"args", {g.node(id)->path() + "." + attr, value}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        updateValues();
    }
    const std::string name = g.attr(g.plug(n->path() + "." + attr)).name;
    if (many)
    {
        attrView::noteMultiEditUsed();
        mBadgeCounts[name] = int(all.size());
        if (auto it = mBadges.find(name); it != mBadges.end())
        {
            it->second->setText(QString("\u00d7%1").arg(all.size()));
            it->second->show();
        }
        refreshMultiState();
    }
    else if (all.size() > 1 && attrView::hintEnabled())
        showHint(QString("Changed %1 only. Hold Alt to change all %2.").arg(QString::fromStdString(n->name())).arg(all.size()));
}

std::vector<scene::NodeId> AttributeEditor::holders(const std::string& attr) const
{
    Graph& g = Graph::current();
    std::vector<scene::NodeId> out;
    if (mNode != attrView::lead())
        return {mNode};
    for (scene::NodeId id : attrView::selectionLeadFirst())
        if (scene::Node* n = g.node(id))
            try
            {
                g.plug(n->path() + "." + attr);
                out.push_back(id);
            }
            catch (const std::exception&)
            {
            }
    return out;
}

bool AttributeEditor::mixed(const scene::PlugRef& ref) const
{
    if (!attrView::multiEditHeld() || Graph::current().selection().size() < 2)
        return false;
    Graph& g = Graph::current();
    const std::string plug = g.plugName(ref);
    const std::string attr = plug.substr(plug.find('.') + 1);
    const std::vector<scene::NodeId> all = holders(attr);
    if (all.size() < 2)
        return false;
    const scene::PlugValue first = g.value(g.plug(g.node(all[0])->path() + "." + attr));
    for (size_t i = 1; i < all.size(); ++i)
        if (!(g.value(g.plug(g.node(all[i])->path() + "." + attr)) == first))
            return true;
    return false;
}

void AttributeEditor::addElement(const std::vector<int>& attrs, int element)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    try
    {
        for (int a : attrs)
        {
            const scene::AttrDef& def = n->type().attrs[size_t(a)];
            std::string plug = Graph::current().plugName({mNode, a, -1, element});
            ops::execute("setAttr", {{"args", {plug, scene::toJson(def.defaultValue)}}}, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void AttributeEditor::runOnNode(const char* command, json call)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    call["args"] = {n->path()};
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void AttributeEditor::removeCategory(const std::string& category) { runOnNode("deleteAttr", {{"category", category}}); }

void AttributeEditor::removeElement(const std::string& attr)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    // A grouped child's element goes as a whole: group[3], every child at index 3.
    std::string plug = attr;
    if (size_t close = plug.find("]."); close != std::string::npos)
        plug = plug.substr(0, close + 1);
    try
    {
        ops::execute("removeMultiInstance", {{"args", {n->path() + "." + plug}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

QWidget* AttributeEditor::colorSwatch(const scene::AttrDef& def, scene::NodeId id, int index, bool readOnly)
{
    // A data display: the swatch shows the stored linear colour in sRGB.
    auto* b = new QToolButton;
    b->setMinimumWidth(36);
    b->setEnabled(!readOnly);
    std::string attr = def.name;
    const QString title = def.niceName.empty() ? niceName(def.name) : QString::fromStdString(def.niceName);
    auto current = [id, index] {
        const scene::PlugValue& v = Graph::current().value({id, index});
        auto* c = std::get_if<glm::dvec3>(&v);
        return c ? *c : glm::dvec3(0.0);
    };
    mUpdaters.push_back([b, current] {
        glm::dvec3 c = current();
        QColor q = QColor::fromRgbF(float(toSrgb(c.x)), float(toSrgb(c.y)), float(toSrgb(c.z)));
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #141414; }").arg(q.name()));
    });
    connect(b, &QToolButton::clicked, this, [b, attr, title, id, index, current] {
        const scene::Node* n = Graph::current().node(id);
        if (!n)
            return;
        const glm::dvec3 start = current();
        const std::string plug = n->path() + "." + attr;
        // Previews with undo off; closing restores the start and sets the result once, as one undo step.
        auto preview = [id, index](const glm::dvec3& c) {
            Graph& g = Graph::current();
            if (!g.node(id))
                return;
            UndoStack& u = UndoStack::get();
            const bool was = u.enabled();
            u.setEnabled(false);
            g.setValue({id, index}, c);
            u.setEnabled(was);
        };
        colorPopup::Options o;
        o.value.color = start;
        o.title = title;
        o.changed = [preview](const colorPopup::Value& v) { preview(v.color); };
        o.closed = [preview, start, plug](const colorPopup::Value& v, bool accepted) {
            preview(start);
            if (!accepted || v.color == start)
                return;
            try
            {
                ops::execute("setAttr", {{"args", {plug, json::array({v.color.x, v.color.y, v.color.z})}}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        };
        colorPopup::show(colorPopup::placeFor(b), std::move(o));
    });
    b->setMaximumWidth(48);
    if (readOnly)
        return b;
    return colorPopup::withEyedropper(b, [this, attr](const glm::dvec3& c) { set(attr, json::array({c.x, c.y, c.z})); });
}

void AttributeEditor::rebuild()
{
    LILA_ZONE();

    mUpdaters.clear();
    while (QLayoutItem* item = mBodyLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    Graph& g = Graph::current();
    if (g.selection() != mLastSelection)
    {
        mLastSelection = g.selection();
        mBadgeCounts.clear();
        if (!mPinned)
            mFocus = scene::kNoNode;
    }
    mBadges.clear();
    mNode = shownNode();
    if (!mNavigating && mNode != scene::kNoNode && (mHistoryAt < 0 || mHistory[size_t(mHistoryAt)] != mNode))
    {
        mHistory.resize(size_t(mHistoryAt + 1));
        mHistory.push_back(mNode);
        mHistoryAt = int(mHistory.size()) - 1;
    }
    scene::Node* n = g.node(mNode);
    rebuildHeader();
    mName->setText(n ? QString::fromStdString(n->name()) : QString());
    mName->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    mName->setEnabled(n != nullptr);
    mType->setText(n ? QString::fromStdString(n->type().name) : QString());
    mType->setToolTip(n ? QString::fromStdString(n->type().doc) : QString());
    mAttrWidgets.clear();
    mComponentWidgets.clear();
    mSearchRows.clear();
    mSections.clear();
    mAdd->setEnabled(n != nullptr);
    const std::vector<const dx::Issue*> nodeIssues = dx::issuesOf(mNode);
    mHadIssues = !nodeIssues.empty();
    if (!n)
    {
        mBodyLayout->addStretch(1);
        mBodyLayout->addWidget(emptyStateLabel("Select an object to see its attributes.", this));
        mBodyLayout->addStretch(2);
        return;
    }
    if (QWidget* list = issueList())
        mBodyLayout->addWidget(section("Issues", list));
    /** The issues on a component as a whole: a type, a category or a section by its title. */
    auto componentIssues = [&](std::string_view typeName, std::string_view category, const QString& title) {
        std::vector<const dx::Issue*> out;
        for (const dx::Issue* i : nodeIssues)
            if (i->attr.empty() && (dx::componentMatches(i->component, typeName, category, {}) ||
                                    (!i->component.empty() && QString::fromStdString(i->component).compare(title, Qt::CaseInsensitive) == 0)))
                out.push_back(i);
        return out;
    };
    auto noteComponent = [&](const std::string& name, QWidget* w) {
        if (!name.empty())
            mComponentWidgets.emplace(QString::fromStdString(name).toLower().toStdString(), w);
    };

    // One section per type in the inheritance chain, base first.
    std::vector<const scene::NodeType*> chain;
    for (const scene::NodeType* t = &n->type(); t; t = t->base)
        chain.insert(chain.begin(), t);

    scene::NodeId id = mNode;
    std::vector<const attributeEditor::AttrFilter*> filters;
    mTriggers.clear();
    for (const attributeEditor::AttrFilter* f : attributeEditor::filters().all())
        if (n->type().isA(f->typeName))
        {
            filters.push_back(f);
            for (const std::string& t : f->triggers)
                if (int index = n->type().attrIndex(t); index >= 0)
                    mTriggers.insert(index);
        }
    /** Adds one attr's row to form; false when the attr shows no editor. */
    auto addRow = [&](QFormLayout* form, const scene::AttrDef& def, int element, QString labelText) -> bool {
        if (def.flags & scene::AttrHidden)
            return false;
        for (const attributeEditor::AttrFilter* f : filters)
            if (f->shows && !f->shows(*n, def.name))
                return false;
        int index = n->type().attrIndex(def.name);
        const scene::PlugRef ref{id, index, -1, element};
        bool readOnly = !(def.flags & scene::AttrInput) || (element >= 0 && g.source(ref));
        std::string attr = def.name;
        if (element >= 0)
        {
            std::string plug = g.plugName(ref);
            attr = plug.substr(plug.find('.') + 1);
        }
        QWidget* field = nullptr;
        std::vector<ScrubTarget> scrubbed;
        const NumberScrub::Commit commit = [this](const std::string& a, const json& v) { set(a, v); };

        auto numberEdit = [&](int component) {
            auto* e = new QLineEdit;
            e->setAlignment(Qt::AlignRight);
            e->setReadOnly(readOnly);
            e->setMinimumWidth(e->fontMetrics().horizontalAdvance("-0.000") + 8);
            e->setMaximumWidth(e->fontMetrics().horizontalAdvance("-00000.0000") + 10);
            e->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            std::string target = component >= 0 ? def.childNames[component] : attr;
            if (!readOnly)
            {
                scrubbed.push_back({ref, component, target, def.minValue, def.maxValue, def.type == PlugType::Int});
                new NumberScrub(e, {scrubbed.back()}, commit);
                if (std::optional<json> d = valueReset::plugDefault(def, component))
                    onMiddleClickReset(e, [this, target, v = *d] { set(target, v); }, QString::fromStdString(valueReset::describe(*d)));
            }
            connect(e, &QLineEdit::editingFinished, this, [this, e, target, def, ref, component] {
                if (!e->isModified())
                    return;
                e->setModified(false);
                double current = 0.0;
                if (Graph::current().node(ref.node))
                {
                    const scene::PlugValue& pv = Graph::current().value(ref);
                    if (auto* v3 = std::get_if<glm::dvec3>(&pv); v3 && component >= 0)
                        current = (*v3)[component];
                    else if (auto* f = std::get_if<double>(&pv))
                        current = *f;
                    else if (auto* i = std::get_if<int64_t>(&pv))
                        current = double(*i);
                }
                std::optional<double> v = evalExpression(e->text(), current);
                if (!v)
                {
                    updateValues();
                    return;
                }
                if (def.minValue)
                    v = std::max(*v, *def.minValue);
                if (def.maxValue)
                    v = std::min(*v, *def.maxValue);
                set(target, def.type == PlugType::Int ? json(int64_t(std::llround(*v))) : json(*v));
            });
            mUpdaters.push_back([e, ref, component, owner = this] {
                // Only typing holds a value back; a field focused by a middle-drag still follows it.
                if (e->hasFocus() && e->isModified())
                    return;
                scene::Node* node = Graph::current().node(ref.node);
                if (!node)
                    return;
                if (owner->mixed(ref))
                {
                    e->setText("--");
                    return;
                }
                const scene::PlugValue& v = Graph::current().value(ref);
                double d = 0.0;
                if (auto* v3 = std::get_if<glm::dvec3>(&v))
                    d = (*v3)[component];
                else if (auto* f = std::get_if<double>(&v))
                    d = *f;
                else if (auto* i = std::get_if<int64_t>(&v))
                    d = double(*i);
                e->setText(attrView::formatNumber(d));
            });
            return e;
        };

        switch (def.type)
        {
        case PlugType::Float:
        case PlugType::Int: field = numberEdit(-1); break;
        case PlugType::Vec3:
        {
            field = new QWidget;
            auto* h = new QHBoxLayout(field);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            int widest = 0;
            for (int c = 0; c < 3; ++c)
            {
                QLineEdit* e = numberEdit(c);
                widest += e->maximumWidth() + h->spacing();
                h->addWidget(e);
            }
            if ((def.flags & scene::AttrColor) && element < 0)
            {
                QWidget* swatch = colorSwatch(def, id, index, readOnly);
                widest += swatch->sizeHint().width() + h->spacing();
                h->addWidget(swatch);
            }
            field->setMaximumWidth(widest);
            break;
        }
        case PlugType::Bool:
        {
            auto* cb = new QCheckBox;
            cb->setEnabled(!readOnly);
            connect(cb, &QCheckBox::clicked, this, [this, attr](bool on) { set(attr, on); });
            mUpdaters.push_back([cb, ref, owner = this] {
                QSignalBlocker block(cb);
                if (owner->mixed(ref))
                    cb->setCheckState(Qt::PartiallyChecked);
                else
                    cb->setChecked(std::get<bool>(Graph::current().value(ref)));
            });
            field = cb;
            break;
        }
        case PlugType::Enum:
        {
            auto* combo = new QComboBox;
            for (const std::string& e : def.enumNames)
                combo->addItem(QString::fromStdString(e));
            combo->setEnabled(!readOnly);
            connect(combo, &QComboBox::activated, this, [this, attr](int i) { set(attr, int64_t(i)); });
            mUpdaters.push_back([combo, ref, owner = this] {
                QSignalBlocker block(combo);
                combo->setPlaceholderText("--");
                combo->setCurrentIndex(owner->mixed(ref) ? -1 : int(std::get<int64_t>(Graph::current().value(ref))));
                choiceTips::refresh(combo);
            });
            field = combo;
            break;
        }
        case PlugType::String:
        {
            if ((def.flags & scene::AttrFont) && !readOnly)
            {
                auto* picker = new FontPicker;
                connect(picker, &FontPicker::chosen, this, [this, attr](const QString& font) { set(attr, font.toStdString()); });
                mUpdaters.push_back([picker, ref] { picker->setValue(std::get<std::string>(Graph::current().value(ref))); });
                field = picker;
                break;
            }
            if ((def.flags & scene::AttrImage) && !readOnly)
            {
                auto* picker = new ImagePicker(imagePicker::libraryFor(n->type(), def.name), niceName(def.name));
                connect(picker, &ImagePicker::chosen, this, [this, attr](const QString& v) { set(attr, v.toStdString()); });
                mUpdaters.push_back([picker, ref] { picker->setValue(std::get<std::string>(Graph::current().value(ref))); });
                field = picker;
                break;
            }
            if (def.flags & scene::AttrCode)
            {
                auto* box = new CodeEdit;
                box->setReadOnly(readOnly);
                connect(box, &CodeEdit::applied, this, [this, attr](const QString& text) { set(attr, text.toStdString()); });
                mUpdaters.push_back([box, ref] { box->showText(std::get<std::string>(Graph::current().value(ref))); });
                field = box;
                break;
            }
            if (def.flags & scene::AttrMultiline)
            {
                auto* box = new MultilineEdit;
                box->setReadOnly(readOnly);
                connect(box, &MultilineEdit::committed, this, [this, attr](const QString& text) { set(attr, text.toStdString()); });
                mUpdaters.push_back([box, ref] { box->showText(std::get<std::string>(Graph::current().value(ref))); });
                field = box;
                break;
            }
            auto* e = new QLineEdit;
            e->setReadOnly(readOnly);
            connect(e, &QLineEdit::editingFinished, this, [this, e, attr] {
                if (e->isModified())
                {
                    e->setModified(false);
                    set(attr, e->text().toStdString());
                }
            });
            mUpdaters.push_back([e, ref] {
                if (!e->hasFocus())
                    e->setText(QString::fromStdString(std::get<std::string>(Graph::current().value(ref))));
            });
            field = e;
            if ((def.flags & scene::AttrFile) && !readOnly)
            {
                field = new QWidget;
                auto* h = new QHBoxLayout(field);
                h->setContentsMargins(0, 0, 0, 0);
                h->setSpacing(3);
                auto* browse = new QToolButton;
                browse->setIcon(icon("folder-open"));
                browse->setToolTip("Browse...");
                connect(browse, &QToolButton::clicked, this, [this, e, attr, label = niceName(def.name)] {
                    QString f = ui::fileDialog::getOpenFileName(this, label, browseStart(e->text().toStdString()),
                                                             "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.webp);;All Files (*)");
                    if (!f.isEmpty())
                        set(attr, paths::storedPath(std::filesystem::path(f.toStdU16String())));
                });
                h->addWidget(e, 1);
                h->addWidget(browse);
            }
            break;
        }
        case PlugType::Matrix:
        {
            // Collapsed to a one-line summary: the full 4x4 is rarely read and would push the editable rows apart.
            auto* box = new QWidget;
            auto* v = new QVBoxLayout(box);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(2);
            auto* toggle = new QToolButton(box);
            toggle->setCheckable(true);
            toggle->setAutoRaise(true);
            toggle->setArrowType(Qt::RightArrow);
            toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            auto* label = new QLabel(box);
            label->setFont(monoFont());
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->hide();
            QObject::connect(toggle, &QToolButton::toggled, label, [toggle, label](bool on) {
                label->setVisible(on);
                toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            });
            v->addWidget(toggle, 0, Qt::AlignLeft);
            v->addWidget(label);
            mUpdaters.push_back([label, toggle, ref] {
                const glm::dmat4 m = std::get<glm::dmat4>(Graph::current().value(ref));
                toggle->setText(m == glm::dmat4(1.0) ? "Identity" : "4 x 4");
                QString cells[4][4];
                qsizetype width = 1;
                // Rows of the matrix as written on paper (column-major storage).
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        width = std::max(width, (cells[r][c] = fmt(m[c][r])).size());
                QString text;
                for (int r = 0; r < 4; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                        text += QString("%1").arg(cells[r][c], int(width) + (c ? 2 : 0));
                    if (r < 3)
                        text += "\n";
                }
                label->setText(text);
            });
            field = box;
            break;
        }
        }
        if (!field)
            return false;
        field->setToolTip(QString::fromStdString(def.doc));
        if (def.type == PlugType::Enum)
            if (auto* combo = qobject_cast<QComboBox*>(field))
                choiceTips::apply(combo, choiceTips::forAttr(def));
        if (element >= 0)
        {
            // An element's row ends in a button that removes it.
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            h->addWidget(field, 1);
            auto* remove = new QToolButton;
            remove->setIcon(icon("x"));
            remove->setToolTip("Remove this element");
            remove->setAutoRaise(true);
            connect(remove, &QToolButton::clicked, this, [this, attr] { removeElement(attr); });
            h->addWidget(remove);
            field->setToolTip(QString::fromStdString(def.doc));
            field = row;
        }
        auto* label = new AttrRowLabel(!labelText.isEmpty() ? labelText : attrView::label(def));
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QWidget::customContextMenuRequested, this,
                [this, label, attr, ref, def, element, readOnly, keyable = bool(def.flags & scene::AttrKeyable), user = bool(def.flags & scene::AttrUser)](const QPoint& at) {
            QMenu menu;
            addCopyActions(&menu, QString::fromStdString(attr), [this, ref] {
                return Graph::current().node(mNode) ? scene::toJson(Graph::current().value(ref)) : json();
            });
            if (user)
            {
                menu.addSeparator();
                menu.addAction(icon("pencil"), "Edit Attribute...", [this, attr, label] { openEdit(attr, label); });
                menu.addAction("Delete Attribute", [this, attr] { runOnNode("deleteAttr", {{"attribute", attr}}); });
            }
            if (!readOnly && element < 0 && def.type != PlugType::String && def.type != PlugType::Mesh && def.type != PlugType::Data)
                menu.addAction(icon("link"), "Drive...", [this, attr, label] {
                    if (scene::Node* node = Graph::current().node(mNode))
                        drivePopover::show(node->path() + "." + attr, label);
                });
            if (keyable)
            {
                menu.addSeparator();
                menu.addAction("Set Key", [this, attr] {
                    scene::Node* node = Graph::current().node(mNode);
                    if (!node)
                        return;
                    try
                    {
                        ops::execute("setKeyframe", {{"args", {node->path() + "." + attr}}}, CallSource::UI);
                    }
                    catch (const CommandError& e)
                    {
                        log::error("{}", e.what());
                    }
                });
            }
            menu.exec(label->mapToGlobal(at));
        });
        label->setToolTip(QString::fromStdString(def.name + (def.shortName.empty() ? "" : " (" + def.shortName + ")") +
                                                 (def.doc.empty() ? "" : "\n" + def.doc)));
        if (readOnly)
            label->setStyleSheet("color: #8a8a8a;");
        else if (std::optional<json> d = valueReset::plugDefault(def))
            onMiddleClickReset(label, [this, attr, v = *d] { set(attr, v); },
                               QString::fromStdString(valueReset::describe(*d, def.enumNames)), field);
        if (!scrubbed.empty())
            new NumberScrub(label, std::move(scrubbed), commit);
        std::vector<const dx::Issue*> rowIssues;
        for (const dx::Issue* i : nodeIssues)
            if (dx::attrMatches(*i, attr) || (i->attr.empty() && dx::componentMatches(i->component, {}, {}, def.name)))
                rowIssues.push_back(i);
        markIssues(label, rowIssues);
        mAttrWidgets.emplace(attr, label);
        mAttrWidgets.emplace(def.name, label);
        if ((def.flags & scene::AttrUser) && element < 0)
        {
            // Added attributes: a pencil to edit and a grip to drag, shown while the row is hovered.
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(2);
            h->addWidget(field, 1);
            auto* pencil = new QToolButton(row);
            pencil->setObjectName("editAttr");
            pencil->setIcon(icon("pencil"));
            pencil->setAutoRaise(true);
            pencil->setToolTip("Edit this attribute");
            auto* grip = new QLabel(row);
            grip->setPixmap(icon("grip-vertical").pixmap(14, 14));
            grip->setToolTip("Drag onto another attribute or heading to move it");
            grip->setCursor(Qt::OpenHandCursor);
            h->addWidget(pencil);
            h->addWidget(grip);
            pencil->hide();
            grip->hide();
            connect(pencil, &QToolButton::clicked, this, [this, attr, pencil] { openEdit(attr, pencil); });
            auto* hover = new RowGestures(row);
            hover->reveal = {pencil, grip};
            auto* drag = new RowGestures(grip);
            drag->dragMime = kAttrMime;
            drag->dragText = QString::fromStdString(attr);
            auto* drop = new RowGestures(label);
            label->setAcceptDrops(true);
            drop->doubleClicked = [this, attr, label] { openEdit(attr, label); };
            drop->accepts = [attr](const QMimeData* d) {
                return (d->hasFormat(kAttrMime) && mimeText(d, kAttrMime) != attr) || d->hasFormat(kHeadingMime);
            };
            drop->dropped = [this, attr, category = def.category](const QMimeData* d) {
                if (d->hasFormat(kAttrMime))
                    dropAttr(mimeText(d, kAttrMime), attr, category);
                else
                    dropHeading(mimeText(d, kHeadingMime), category);
            };
            field = row;
        }
        if (element < 0 && !readOnly && (def.flags & scene::AttrInput))
        {
            // A plug dragged from the Channel Box connects here; with Alt it becomes a driven key.
            auto* plugDrop = new RowGestures(label);
            label->setAcceptDrops(true);
            plugDrop->accepts = [](const QMimeData* d) { return d->hasFormat(kPlugMime); };
            plugDrop->dropped = [this, attr](const QMimeData* d) {
                if (scene::Node* node = Graph::current().node(mNode))
                    dropPlug(mimeText(d, kPlugMime), node->path() + "." + attr);
            };
        }
        if (element < 0)
        {
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            h->addWidget(field, 1);
            // What drives it, inline: a link that shows the source here.
            std::optional<scene::PlugRef> src = g.source(ref);
            for (int c = 0; !src && def.type == PlugType::Vec3 && c < 3; ++c)
                src = g.source({ref.node, ref.attr, c, -1});
            if (const scene::Node* from = src ? g.node(src->node) : nullptr; from && !from->type().isA("animCurve") && !from->type().isA("animCurveRQ"))
            {
                auto* link = new QLabel(QString("<a href=\"x\">\u2190 %1</a>").arg(QString::fromStdString(g.plugName(*src)).toHtmlEscaped()), row);
                link->setObjectName("aeSourceLink");
                link->setToolTip("Driven by this plug: click to show its node here");
                connect(link, &QLabel::linkActivated, this, [this, id = src->node] { focusNode(id); });
                h->addWidget(link);
            }
            auto* badge = new QLabel(row);
            badge->setObjectName("multiBadge");
            const QColor accent = themeColor("accent");
            badge->setStyleSheet(QString("QLabel { color: %1; background: rgba(%2, %3, %4, 51); border-radius: 6px; padding: 0 4px; "
                                         "font-size: 10px; font-weight: 600; }")
                                     .arg(accent.name())
                                     .arg(accent.red())
                                     .arg(accent.green())
                                     .arg(accent.blue()));
            badge->setProperty("lilaRawColors", true);
            if (auto it = mBadgeCounts.find(def.name); it != mBadgeCounts.end())
                badge->setText(QString("\u00d7%1").arg(it->second));
            else
                badge->hide();
            badge->setToolTip("Changed on this many objects");
            badge->setAccessibleName("Changed on several objects");
            mBadges[def.name] = badge;
            h->addWidget(badge);
            field = row;
        }
        mSearchRows.push_back({form, label, def});
        form->addRow(label, field);
        return true;
    };
    /** Rows for an array: one per element and an Add button; a grouped array shows each element's children together. */
    std::set<std::string> groupsShown;
    auto addAttrRows = [&](QFormLayout* form, const scene::AttrDef& def) -> int {
        if (!(def.flags & scene::AttrArray))
            return addRow(form, def, -1, {}) ? 1 : 0;
        if (def.flags & scene::AttrHidden)
            return 0;
        const size_t dot = def.name.find('.');
        const int index = n->type().attrIndex(def.name);
        std::vector<int> children = {index};
        std::vector<int> indices;
        std::string addPlug = def.name;
        if (dot != std::string::npos)
        {
            const std::string group = def.name.substr(0, dot);
            if (!groupsShown.insert(group).second)
                return 0;
            children = n->type().groupChildren(group);
            indices = g.groupIndices(id, group);
            addPlug = group;
        }
        else
            for (const scene::PlugRef& e : g.elements({id, index}))
                indices.push_back(e.element);
        auto* heading = new QLabel(niceName(addPlug));
        heading->setStyleSheet("color: #c8c8c8; padding-top: 4px;");
        heading->setToolTip(QString::fromStdString(def.doc));
        form->addRow(heading);
        int added = 1;
        for (int i : indices)
        {
            if (dot == std::string::npos)
                added += addRow(form, def, i, QString("[%1]").arg(i)) ? 1 : 0;
            else
                for (int c : children)
                {
                    const scene::AttrDef& cd = n->type().attrs[size_t(c)];
                    QString child = niceName(cd.name.substr(dot + 1));
                    added += addRow(form, cd, i, QString("[%1] %2").arg(i).arg(child)) ? 1 : 0;
                }
        }
        // Only arrays that hold values can grow here; connection-only ones grow by connecting.
        bool addable = true;
        for (int c : children)
            addable = addable && (n->type().attrs[size_t(c)].flags & scene::AttrStorable) && (n->type().attrs[size_t(c)].flags & scene::AttrInput);
        if (addable)
        {
            auto* add = new QToolButton;
            add->setIcon(icon("plus"));
            add->setText("Add");
            add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            add->setToolTip("Add an element after the last one");
            const int next = indices.empty() ? 0 : indices.back() + 1;
            connect(add, &QToolButton::clicked, this, [this, children, next] { addElement(children, next); });
            form->addRow(QString(), add);
        }
        return added;
    };
    auto newForm = [](QWidget* content) {
        auto* form = new QFormLayout(content);
        form->setLabelAlignment(Qt::AlignRight);
        form->setContentsMargins(8, 2, 4, 2);
        form->setSpacing(3);
        return form;
    };
    auto subHeading = [&](QFormLayout* form, const std::string& title, std::function<void()> remove = {}) -> QLabel* {
        auto* l = new QLabel(QString::fromStdString(title));
        l->setStyleSheet("color: #c8c8c8; font-weight: 600; padding-top: 4px; border-bottom: 1px solid #474747;");
        markIssues(l, componentIssues({}, title, QString::fromStdString(title)));
        noteComponent(title, l);
        if (!remove)
        {
            form->addRow(l);
            return l;
        }
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(l, 1);
        auto* plus = new QToolButton;
        plus->setObjectName("headingAdd");
        plus->setIcon(icon("plus"));
        plus->setAutoRaise(true);
        plus->setToolTip(QString::fromStdString("Add an attribute under " + title));
        connect(plus, &QToolButton::clicked, this, [this, plus, title] { openAdd(plus, title); });
        h->addWidget(plus);
        auto* x = new QToolButton;
        x->setIcon(icon("x"));
        x->setAutoRaise(true);
        x->setToolTip(QString::fromStdString("Remove " + title + " and its attributes"));
        connect(x, &QToolButton::clicked, this, std::move(remove));
        h->addWidget(x);
        form->addRow(row);
        return l;
    };

    // Attrs with a category show together under its heading in a Parameters section, in attr order.
    std::vector<const scene::AttrDef*> parameters;
    for (const scene::NodeType* t : chain)
        for (const scene::AttrDef& def : t->ownAttrs)
            if (!def.category.empty() || (!def.niceName.empty() && !(def.flags & scene::AttrHidden)))
                parameters.push_back(&def);
    if (!parameters.empty())
    {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        std::vector<std::string> categories{""};
        for (const scene::AttrDef* d : parameters)
            if (std::find(categories.begin(), categories.end(), d->category) == categories.end())
                categories.push_back(d->category);
        int added = 0;
        for (const std::string& c : categories)
        {
            bool headed = c.empty();
            for (const scene::AttrDef* d : parameters)
            {
                if (d->category != c)
                    continue;
                if (!headed)
                {
                    subHeading(form, c);
                    headed = true;
                }
                added += addAttrRows(form, *d);
            }
        }
        if (added)
            mBodyLayout->addWidget(section("Parameters", content));
        else
            delete content;
    }

    for (const scene::NodeType* t : chain)
    {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        int added = 0;
        for (const scene::AttrDef& def : t->ownAttrs)
        {
            if (std::find(parameters.begin(), parameters.end(), &def) != parameters.end())
                continue;
            added += addAttrRows(form, def);
        }
        if (added && !t->doc.empty())
        {
            auto* about = new QLabel(QString::fromStdString(t->doc));
            about->setWordWrap(true);
            about->setStyleSheet("color: #9a9a9a;");
            form->insertRow(0, about);
        }
        if (added)
        {
            const QString title = niceName(t->name) + " Attributes";
            QWidget* box = section(title, content, componentIssues(t->name, {}, title));
            noteComponent(t->name, box);
            mBodyLayout->addWidget(box);
        }
        else
            delete content;
    }
    {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        int added = 0;
        std::vector<std::string> categories{""};
        for (const scene::AttrDef& def : n->type().attrs)
            if ((def.flags & scene::AttrUser) && std::find(categories.begin(), categories.end(), def.category) == categories.end())
                categories.push_back(def.category);
        for (const std::string& c : categories)
        {
            bool headed = c.empty();
            for (const scene::AttrDef& def : n->type().attrs)
            {
                if (!(def.flags & scene::AttrUser) || def.category != c)
                    continue;
                if (!headed)
                {
                    QLabel* heading = subHeading(form, c, [this, c] { removeCategory(c); });
                    heading->setObjectName("componentHeading");
                    heading->setContextMenuPolicy(Qt::CustomContextMenu);
                    connect(heading, &QWidget::customContextMenuRequested, this, [this, heading, c](const QPoint& at) { headingMenu(c, heading, at); });
                    heading->setAcceptDrops(true);
                    heading->setCursor(Qt::OpenHandCursor);
                    auto* gestures = new RowGestures(heading);
                    gestures->dragMime = kHeadingMime;
                    gestures->dragText = QString::fromStdString(c);
                    gestures->accepts = [c](const QMimeData* d) {
                        return d->hasFormat(kAttrMime) || (d->hasFormat(kHeadingMime) && mimeText(d, kHeadingMime) != c);
                    };
                    gestures->dropped = [this, c](const QMimeData* d) {
                        if (d->hasFormat(kAttrMime))
                            dropAttr(mimeText(d, kAttrMime), {}, c);
                        else
                            dropHeading(mimeText(d, kHeadingMime), c);
                    };
                    headed = true;
                }
                added += addAttrRows(form, def);
            }
        }
        if (added)
        {
            auto* add = new QToolButton;
            add->setObjectName("extraAdd");
            add->setIcon(icon("plus"));
            add->setText("Add Attribute");
            add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            add->setAutoRaise(true);
            connect(add, &QToolButton::clicked, this, [this, add] { openAdd(add); });
            form->addRow(QString(), add);
            mBodyLayout->addWidget(section("Extra Attributes", content));
        }
        else
            delete content;
    }
    {
        // Drives: what this object's plugs feed, each a link to the node it drives.
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        int added = 0;
        for (const auto& [dst, src] : g.connections())
        {
            if (src.node != id || dst.node == id)
                continue;
            const scene::Node* to = g.node(dst.node);
            if (!to || to->type().isA("animCurve") || to->type().isA("animCurveRQ"))
                continue;
            const std::string from = g.plugName(src);
            auto* link = new QLabel(QString("<a href=\"x\">\u2192 %1</a>").arg(QString::fromStdString(g.plugName(dst)).toHtmlEscaped()));
            link->setObjectName("aeDrivesLink");
            connect(link, &QLabel::linkActivated, this, [this, nodeId = dst.node] { focusNode(nodeId); });
            form->addRow(QString::fromStdString(from.substr(from.find('.') + 1)), link);
            ++added;
        }
        if (added)
            mBodyLayout->addWidget(section("Drives", content));
        else
            delete content;
    }
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
            {
                const QString title = QString::fromStdString(extra->title);
                QWidget* box = section(title, w, componentIssues({}, {}, title));
                noteComponent(extra->title, box);
                mBodyLayout->addWidget(box);
            }
    for (int i = 0; i < mBodyLayout->count(); ++i)
        if (QWidget* box = mBodyLayout->itemAt(i)->widget())
            if (auto* header = box->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
            {
                mSections.emplace_back(box, header->text());
                rememberCollapse(box, n->type().registered().name + ":" + header->text().toStdString());
                auto* help = new QToolButton(header);
                help->setObjectName("sectionHelp");
                help->setIcon(icon("help-circle"));
                help->setAutoRaise(true);
                help->setToolTip("Open the docs for " + QString::fromStdString(n->type().registered().name));
                auto* hl = new QHBoxLayout(header);
                hl->setContentsMargins(0, 0, 2, 0);
                hl->addStretch(1);
                hl->addWidget(help);
                connect(help, &QToolButton::clicked, this, [page = attrView::helpPageFor(n->type())] { docs::open(page); });
            }
    mBodyLayout->addStretch(1);
    updateValues();
    fitLabels();
    applySearch();
    if (gPendingReveal && reveal(gPendingReveal))
        gPendingReveal = 0;
}

std::vector<scene::NodeId> AttributeEditor::targets() const
{
    if (mNode == scene::kNoNode)
        return {};
    if (attrView::multiEditHeld() && mNode == attrView::lead() && Graph::current().selection().size() > 1)
        return attrView::selectionLeadFirst();
    return {mNode};
}

scene::NodeId AttributeEditor::shownNode() const
{
    Graph& g = Graph::current();
    if (mPinned && g.node(mNode))
        return mNode;
    if (mFocus != scene::kNoNode && g.node(mFocus))
        return mFocus;
    return attrView::lead();
}

std::string AttributeEditor::viewSignature() const
{
    std::string sig = std::string(attrView::nameModeName(attrView::nameMode())) + "|" + attrView::currentView() + "|" +
                      std::to_string(attrView::precision()) + "|" + (attrView::showNamespaces() ? "ns" : "");
    for (const attrView::View& v : attrView::views())
        if (v.name == attrView::currentView())
            for (const auto& [type, titles] : v.hidden)
                for (const std::string& t : titles)
                    sig += "|" + type + "/" + t;
    return sig;
}

void AttributeEditor::focusNode(scene::NodeId id)
{
    if (!Graph::current().node(id))
        return;
    mFocus = id;
    mPinned = false;
    mRebuild = true;
    scheduleRefresh();
}

void AttributeEditor::rebuildHeader()
{
    auto* row = static_cast<QHBoxLayout*>(mChips->layout());
    while (QLayoutItem* item = row->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    Graph& g = Graph::current();
    auto tool = [&](const char* name, const char* iconName, const QString& tip) {
        auto* b = new QToolButton(mChips);
        b->setObjectName(name);
        b->setIcon(icon(iconName));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };
    QToolButton* back = tool("focusBack", "arrow-left", "Back to the previous object shown");
    QToolButton* forward = tool("focusForward", "arrow-right", "Forward");
    // Drop history entries whose nodes are gone.
    back->setEnabled(mHistoryAt > 0);
    forward->setEnabled(mHistoryAt >= 0 && mHistoryAt + 1 < int(mHistory.size()));
    auto step = [this](int delta) {
        int at = mHistoryAt + delta;
        while (at >= 0 && at < int(mHistory.size()) && !Graph::current().node(mHistory[size_t(at)]))
            at += delta;
        if (at < 0 || at >= int(mHistory.size()))
            return;
        mHistoryAt = at;
        mFocus = mHistory[size_t(at)];
        mNavigating = true;
        rebuild();
        mNavigating = false;
    };
    connect(back, &QToolButton::clicked, this, [step] { step(-1); });
    connect(forward, &QToolButton::clicked, this, [step] { step(1); });

    // The selected objects, lead first; a click makes one the lead without touching the selection.
    const std::vector<scene::NodeId> sel = attrView::selectionLeadFirst();
    const QColor accent = themeColor("accent");
    for (size_t i = 0; i < sel.size() && i < 4; ++i)
    {
        const scene::Node* n = g.node(sel[i]);
        auto* chip = new QToolButton(mChips);
        chip->setObjectName("selectionChip");
        chip->setText(QString::fromStdString(n->name()));
        chip->setToolTip(i == 0 ? "The lead object: edits go here" : "Click to make this the lead object");
        chip->setCheckable(true);
        chip->setChecked(sel[i] == mNode);
        chip->setAutoRaise(true);
        chip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        if (i == 0)
            chip->setStyleSheet("QToolButton { font-weight: 600; }");
        connect(chip, &QToolButton::clicked, this, [this, id = sel[i]] {
            mFocus = scene::kNoNode;
            mPinned = false;
            attrView::setLead(id);
            mRebuild = true;
            scheduleRefresh();
        });
        row->addWidget(chip);
    }
    if (sel.size() > 4)
    {
        auto* more = new QLabel(QString("+%1").arg(sel.size() - 4), mChips);
        more->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
        row->addWidget(more);
    }
    mMultiPill = new QLabel(mChips);
    mMultiPill->setObjectName("multiEditPill");
    mMultiPill->setProperty("lilaRawColors", true);
    mMultiPill->setStyleSheet(QString("QLabel { color: %1; border: 1px solid %1; border-radius: 3px; padding: 1px 6px; }").arg(accent.name()));
    mMultiPill->hide();
    row->addWidget(mMultiPill);

    // Related nodes: what feeds the shown one, and the deformers and modifiers it feeds.
    if (const scene::Node* shown = g.node(mNode))
    {
        std::vector<scene::NodeId> related;
        for (const auto& [dst, src] : g.connections())
        {
            scene::NodeId other = scene::kNoNode;
            if (dst.node == mNode)
                other = src.node;
            else if (src.node == mNode)
                if (const scene::Node* d = g.node(dst.node); d && (d->traits() & (scene::Node::kDeformer | scene::Node::kModifier)))
                    other = dst.node;
            const scene::Node* o = g.node(other);
            if (!o || other == mNode || o->type().isA("animCurve") || o->type().isA("animCurveRQ") ||
                std::find(related.begin(), related.end(), other) != related.end())
                continue;
            related.push_back(other);
        }
        (void)shown;
        if (!related.empty())
        {
            auto* sep = new QLabel(QString::fromUtf8("·"), mChips);
            sep->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
            row->addWidget(sep);
        }
        for (size_t i = 0; i < related.size() && i < 4; ++i)
        {
            const scene::Node* o = g.node(related[i]);
            auto* chip = new QToolButton(mChips);
            chip->setObjectName("relatedChip");
            chip->setText(QString::fromStdString(o->name()));
            chip->setIcon(icon(QString::fromStdString(scene::types::iconFor(o->type(), "box"))));
            chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            chip->setToolTip(QString::fromStdString(o->type().registered().name) + ": show it here");
            chip->setAutoRaise(true);
            connect(chip, &QToolButton::clicked, this, [this, id = related[i]] { focusNode(id); });
            row->addWidget(chip);
        }
    }
    row->addStretch(1);
    QToolButton* pin = tool("aePin", mPinned ? "pinned" : "pin", mPinned ? "Unpin: follow the selection again" : "Pin: keep showing this object");
    pin->setCheckable(true);
    pin->setChecked(mPinned);
    connect(pin, &QToolButton::toggled, this, [this](bool on) {
        mPinned = on;
        if (!on)
            mFocus = scene::kNoNode;
        mRebuild = true;
        scheduleRefresh();
    });
    QToolButton* show = tool("aeShow", "adjustments", "Show: sections, name mode and saved views");
    connect(show, &QToolButton::clicked, this, [this, show] { showViewPopover(show); });
    mChips->setVisible(true);
    refreshMultiState();
}

void AttributeEditor::refreshMultiState()
{
    if (!mMultiPill)
        return;
    const size_t count = Graph::current().selection().size();
    const bool armed = count > 1 && mNode == attrView::lead() && (attrView::multiEditHeld() || !mBadgeCounts.empty());
    mMultiPill->setText(QString("Editing %1 objects").arg(count));
    mMultiPill->setVisible(armed);
    if (armed)
        mHint->hide();
}

void AttributeEditor::showHint(const QString& text)
{
    mHintText->setText(text);
    mHint->show();
    QTimer::singleShot(5000, mHint, [hint = QPointer<QWidget>(mHint), text, label = QPointer<QLabel>(mHintText)] {
        if (hint && label && label->text() == text)
            hint->hide();
    });
}

void AttributeEditor::showViewPopover(QWidget* anchor)
{
    scene::Node* n = Graph::current().node(mNode);
    auto* pop = new QFrame(window(), Qt::Popup);
    pop->setObjectName("aeViewPopover");
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(pop);
    v->setContentsMargins(8, 8, 8, 8);
    v->addWidget(new QLabel("<b>Names</b>", pop));
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(pop);
    for (attrView::NameMode m : {attrView::NameMode::Nice, attrView::NameMode::Long, attrView::NameMode::Short})
    {
        auto* r = new QRadioButton(m == attrView::NameMode::Nice ? "Nice" : m == attrView::NameMode::Long ? "Long" : "Short", pop);
        r->setObjectName(QString("nameMode_") + attrView::nameModeName(m));
        r->setChecked(attrView::nameMode() == m);
        group->addButton(r);
        connect(r, &QRadioButton::toggled, pop, [m](bool on) {
            if (on)
                attrView::setNameMode(m);
        });
        modes->addWidget(r);
    }
    v->addLayout(modes);
    v->addWidget(new QLabel("<b>View</b>", pop));
    auto* views = new QComboBox(pop);
    views->setObjectName("aeViews");
    for (const attrView::View& view : attrView::views())
        views->addItem(QString::fromStdString(view.name) + (view.source == "studio" ? "  (studio)" : ""), QString::fromStdString(view.name));
    views->setCurrentIndex(std::max(0, views->findData(QString::fromStdString(attrView::currentView()))));
    connect(views, &QComboBox::activated, pop, [views](int i) { attrView::setCurrentView(views->itemData(i).toString().toStdString()); });
    v->addWidget(views);
    auto* saveRow = new QHBoxLayout;
    auto* name = new QLineEdit(pop);
    name->setObjectName("aeViewName");
    name->setPlaceholderText("Save view as...");
    auto* studio = new QCheckBox("Studio", pop);
    studio->setEnabled(!PrefStore::get().studioDirs().empty());
    connect(name, &QLineEdit::returnPressed, pop, [name, studio, pop] {
        if (attrView::saveView(name->text().trimmed().toStdString(), studio->isChecked()))
            pop->close();
    });
    saveRow->addWidget(name, 1);
    saveRow->addWidget(studio);
    v->addLayout(saveRow);
    if (n)
    {
        v->addWidget(new QLabel("<b>Sections</b>", pop));
        const std::string type = n->type().registered().name;
        for (const auto& [box, title] : mSections)
        {
            if (title == "Issues")
                continue;
            auto* cb = new QCheckBox(title, pop);
            cb->setObjectName("sectionToggle");
            cb->setChecked(attrView::sectionShown(type, title));
            connect(cb, &QCheckBox::toggled, pop, [type, title](bool on) { attrView::setSectionShown(type, title, on); });
            v->addWidget(cb);
        }
    }
    pop->adjustSize();
    pop->move(anchor->mapToGlobal(QPoint(anchor->width() - pop->width(), anchor->height())));
    pop->show();
}


void AttributeEditor::openAdd(QWidget* anchor, std::string category)
{
    if (mNode != scene::kNoNode)
        attrAuthoring::showAdd(targets(), anchor, std::move(category));
}

void AttributeEditor::openEdit(const std::string& attr, QWidget* anchor)
{
    if (mNode != scene::kNoNode)
        attrAuthoring::showEdit(targets(), attr, anchor);
}

namespace
{
double valueOfPlug(Graph& g, const std::string& plug)
{
    const scene::PlugRef p = g.plug(plug);
    const scene::PlugValue v = g.value(p);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    if (auto* v3 = std::get_if<glm::dvec3>(&v))
        return (*v3)[std::max(0, p.component)];
    return 0.0;
}

json aeRunLogged(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return json();
    }
}
} // namespace

void AttributeEditor::headingMenu(const std::string& category, QWidget* on, QPoint at)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    const std::string path = n->path();
    QMenu menu;
    menu.addAction(icon("plus"), "Add Attribute Here...", [this, on, category] { openAdd(on, category); });
    menu.addSeparator();
    auto save = [path, category](bool studio) {
        json file = aeRunLogged("saveComponent", {{"args", {path}}, {"category", category}, {"studio", studio}});
        if (file.is_string())
            log::info("Saved component {} to {}", category, file.get<std::string>());
    };
    menu.addAction(icon("device-floppy"), "Save as Component", [save] { save(false); });
    QAction* studio = menu.addAction("Save to Studio Library", [save] { save(true); });
    studio->setEnabled(!PrefStore::get().studioDirs().empty());
    QMenu* unreal = menu.addMenu("Unreal Export");
    const json current = aeRunLogged("componentExport", {{"args", {path}}, {"component", category}, {"query", true}});
    const std::string mode = current.is_object() ? current.value("mode", "assetUserData") : "assetUserData";
    for (auto [id, label] : {std::pair{"assetUserData", "Asset User Data"}, {"actorTags", "Actor Tags"}, {"component", "Actor Component..."},
                             {"dataAsset", "Data Asset..."}, {"custom", "Custom Exporter..."}, {"none", "Don't Export"}})
    {
        QAction* a = unreal->addAction(label);
        a->setCheckable(true);
        a->setChecked(mode == id);
        const std::string modeId = id;
        connect(a, &QAction::triggered, this, [path, category, modeId, on, current] {
            json call = {{"args", {path}}, {"component", category}, {"mode", modeId}};
            if (modeId != "component" && modeId != "dataAsset" && modeId != "custom")
            {
                aeRunLogged("componentExport", call);
                return;
            }
            // The class or exporter name, typed in place.
            auto* box = new QLineEdit(on->window());
            box->setWindowFlags(Qt::Popup);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setMinimumWidth(320);
            box->setPlaceholderText(modeId == "custom" ? "Exporter name" : "/Script/Module.ClassName or /Game/Path/Class");
            box->setText(QString::fromStdString(current.is_object() ? current.value("class", "") : ""));
            box->move(on->mapToGlobal(QPoint(0, on->height())));
            QObject::connect(box, &QLineEdit::returnPressed, box, [box, call]() mutable {
                call["unrealClass"] = box->text().trimmed().toStdString();
                aeRunLogged("componentExport", call);
                box->close();
            });
            box->show();
            box->setFocus();
        });
    }
    menu.addSeparator();
    menu.addAction(icon("x"), "Remove Component", [this, category] { removeCategory(category); });
    menu.exec(on->mapToGlobal(at));
}

void AttributeEditor::dropAttr(const std::string& attr, const std::string& before, const std::string& category)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n || attr == before)
        return;
    const int from = n->type().attrIndex(attr);
    if (from < 0)
        return;
    UndoChunk chunk("Move Attribute");
    const std::string plug = n->path() + "." + attr;
    if (n->type().attrs[size_t(from)].category != category)
        aeRunLogged("editAttr", {{"args", {plug}}, {"category", category}});
    if (!before.empty())
    {
        aeRunLogged("reorderAttr", {{"args", {plug}}, {"before", before}});
        return;
    }
    // Under a heading: after its last attribute.
    std::string last;
    for (const scene::AttrDef& d : n->type().attrs)
        if ((d.flags & scene::AttrUser) && d.category == category && d.name != attr)
            last = d.name;
    if (!last.empty())
        aeRunLogged("reorderAttr", {{"args", {plug}}, {"after", last}});
}

void AttributeEditor::dropHeading(const std::string& category, const std::string& before)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n || category == before)
        return;
    std::vector<std::string> moving;
    std::string first;
    for (const scene::AttrDef& d : n->type().attrs)
        if (d.flags & scene::AttrUser)
        {
            if (d.category == category)
                moving.push_back(d.name);
            else if (d.category == before && first.empty())
                first = d.name;
        }
    if (moving.empty() || first.empty())
        return;
    UndoChunk chunk("Move Component");
    for (const std::string& a : moving)
        aeRunLogged("reorderAttr", {{"args", {n->path() + "." + a}}, {"before", first}});
}

void AttributeEditor::dropPlug(const std::string& source, const std::string& target)
{
    if (source.empty() || source == target)
        return;
    if (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
    {
        attrView::noteMultiEditUsed();
        Graph& g = Graph::current();
        try
        {
            const double d = valueOfPlug(g, source), v = valueOfPlug(g, target);
            aeRunLogged("driveChannel", {{"args", {target}}, {"driver", source}, {"keys", json::array({json::array({d, v}), json::array({d + 1.0, v + 1.0})})},
                                        {"force", true}});
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
        return;
    }
    aeRunLogged("connectAttr", {{"args", {source, target}}, {"force", true}});
}

void AttributeEditor::applySearch()
{
    const QString q = mSearch->text().trimmed();
    std::set<QWidget*> hit;
    for (const SearchRow& r : mSearchRows)
    {
        const bool show = attrAuthoring::matches(r.def, q);
        r.form->setRowVisible(r.label, show);
        if (show && r.form->parentWidget())
            hit.insert(r.form->parentWidget()->parentWidget());
    }
    for (const auto& [box, title] : mSections)
    {
        const scene::Node* shown = Graph::current().node(mNode);
        const bool inView = !shown || title == "Issues" || attrView::sectionShown(shown->type().registered().name, title);
        const bool show = inView && (q.isEmpty() || hit.contains(box) || title.contains(q, Qt::CaseInsensitive));
        box->setVisible(show);
        if (q.isEmpty() || !show)
            continue;
        // Searching opens the sections it finds rows in, without remembering them open.
        if (auto* header = box->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly))
        {
            QSignalBlocker block(header);
            header->setChecked(true);
            header->setArrowType(Qt::DownArrow);
            if (QLayoutItem* item = box->layout()->itemAt(1); item && item->widget())
                item->widget()->setVisible(true);
        }
    }
}

void AttributeEditor::rememberCollapse(QWidget* section, const std::string& key)
{
    auto* header = section->findChild<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
    if (!header)
        return;
    std::string safe = key;
    for (char& c : safe)
        if (c == '/' || c == '~')
            c = '_';
    const std::string pointer = "/collapsed/" + safe;
    if (PrefStore::get().value("attributeEditor", pointer, false))
        header->setChecked(false);
    connect(header, &QToolButton::toggled, this, [this, pointer](bool on) {
        if (mSearch->text().trimmed().isEmpty())
            PrefStore::get().set("attributeEditor", pointer, !on);
    });
}

void AttributeEditor::fitLabels()
{
    for (QFormLayout* form : mBody->findChildren<QFormLayout*>())
    {
        std::vector<AttrRowLabel*> labels;
        int longest = 0, shortest = 0, fieldMin = 0;
        for (int r = 0; r < form->rowCount(); ++r)
        {
            QLayoutItem* l = form->itemAt(r, QFormLayout::LabelRole);
            QLayoutItem* f = form->itemAt(r, QFormLayout::FieldRole);
            auto* label = l ? dynamic_cast<AttrRowLabel*>(l->widget()) : nullptr;
            if (!label || !f)
                continue;
            labels.push_back(label);
            longest = std::max(longest, label->QLabel::sizeHint().width());
            shortest = std::max(shortest, label->minimumSizeHint().width());
            fieldMin = std::max(fieldMin, f->minimumSize().width());
        }
        if (labels.empty() || !form->parentWidget())
            continue;
        const QMargins m = form->contentsMargins();
        const int avail = mScroll->viewport()->width() - form->parentWidget()->mapTo(mBody, QPoint()).x() - m.left() - m.right() -
                          form->horizontalSpacing();
        const int width = std::clamp(avail - fieldMin, shortest, std::max(shortest, longest));
        for (AttrRowLabel* label : labels)
            if (label->maximumWidth() != width)
                label->setMaximumWidth(width);
    }
}

bool AttributeEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mScroll->viewport() && event->type() == QEvent::Resize)
        fitLabels();
    return QWidget::eventFilter(watched, event);
}

void AttributeEditor::updateValues()
{
    LILA_ZONE();

    Graph::current().evaluate();
    if (!Graph::current().node(mNode))
        return;
    for (auto& fn : mUpdaters)
        fn();
}
} // namespace lila::ui
