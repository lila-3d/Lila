#include "ui/NodePicker.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/Transform.h"
#include "ui/Outliner.h"
#include "ui/PaintSelect.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ViewportTool.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

#include <algorithm>

namespace lila::ui
{
namespace
{
scene::Graph& graph() { return scene::Graph::current(); }

/** Completes the whole text, or in a list the name after the last comma. */
class NameCompleter : public QCompleter
{
public:
    NameCompleter(QAbstractItemModel* model, bool multiple, QLineEdit* line) : QCompleter(model, line), mMultiple(multiple), mLine(line) {}

    QStringList splitPath(const QString& path) const override { return {(mMultiple ? path.section(',', -1) : path).trimmed()}; }

    QString pathFromIndex(const QModelIndex& index) const override
    {
        QString name = QCompleter::pathFromIndex(index);
        if (!mMultiple)
            return name;
        const QString text = mLine->text();
        const qsizetype comma = text.lastIndexOf(',');
        return comma < 0 ? name : text.left(comma + 1) + " " + name;
    }

private:
    bool mMultiple;
    QLineEdit* mLine;
};

class PickTool;
PickTool* gPickTool = nullptr;

/** The eyedropper: takes the mesh clicked in a viewport into a NodePicker, then gives the previous tool back. */
class PickTool final : public ViewportTool
{
public:
    PickTool(NodePicker* picker, std::string previous) : mPicker(picker), mPrevious(std::move(previous)) {}
    ~PickTool() override
    {
        if (gPickTool == this)
            gPickTool = nullptr;
    }

    const char* name() const override { return "nodePick"; }
    NodePicker* picker() const { return mPicker; }

    void activate() override
    {
        gPickTool = this;
        if (mPicker)
            mPicker->setEyedropperOn(true);
    }
    void deactivate() override
    {
        viewportTools::setHoverHighlight(scene::kNoNode);
        if (mPicker)
            mPicker->setEyedropperOn(false);
        if (gPickTool == this)
            gPickTool = nullptr;
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() == Qt::RightButton || !mPicker)
        {
            finish();
            return true;
        }
        if (e->button() != Qt::LeftButton)
            return false;
        hover(e->position(), view);
        if (mHover != scene::kNoNode)
        {
            mPicker->pick(mHover);
            if (!mPicker->multiple())
                finish();
        }
        viewportTools::requestRedraw();
        return true;
    }
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        hover(e->position(), view);
        viewportTools::requestRedraw();
        return false;
    }
    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override { return e->button() == Qt::LeftButton || e->button() == Qt::RightButton; }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (e->key() != Qt::Key_Escape && e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter)
            return false;
        finish();
        return true;
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        ToolStatus status;
        status.tool = "Pick";
        if (mPicker)
        {
            status.subject = mPicker->subject();
            status.prompt = mPicker->multiple() ? "click meshes to add or remove them" : "click a mesh";
        }
        status.keys = {{"Esc", "done"}, {"RMB", "done"}};
        drawToolStatus(p, view, status);
        if (mHover != scene::kNoNode)
            if (const scene::Node* n = graph().node(mHover))
                p.label(mAt + QPointF(16.0, 28.0), qs(n->path()));
    }

    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    /** Ends the tool after the event that asked, since replacing the active tool destroys it. */
    void finish()
    {
        QTimer::singleShot(0, qApp, [self = this, previous = mPrevious] {
            if (viewportTools::active() != self)
                return;
            if (previous.empty() || !viewportTools::activate(previous))
                viewportTools::set(nullptr);
        });
    }

private:
    /** The mesh under the cursor that the picker takes, highlighted in the viewports. */
    void hover(QPointF at, const ViewInfo& view)
    {
        mAt = at;
        mHover = scene::kNoNode;
        if (mPicker)
            for (scene::NodeId id : paintSelect::objectsUnder(view, at, 0.0))
                if (mPicker->accepts(id))
                    mHover = id;
        viewportTools::setHoverHighlight(mHover);
    }

    QPointer<NodePicker> mPicker;
    std::string mPrevious;
    scene::NodeId mHover = scene::kNoNode;
    QPointF mAt;
};

std::vector<std::string> splitTypes(const std::string& type)
{
    std::vector<std::string> out;
    for (const QString& t : QString::fromStdString(type).split('|', Qt::SkipEmptyParts))
        out.push_back(t.trimmed().toStdString());
    if (out.empty())
        out.push_back("node");
    return out;
}

bool showsInViewport(const scene::Graph& g, scene::NodeId id)
{
    const scene::Node* n = g.node(id);
    return !n || !n->type().isA("transform") || scene::effectivelyVisible(g, id);
}
} // namespace

NodePicker::NodePicker(std::string type, bool multiple, QWidget* parent) : QWidget(parent), mTypes(splitTypes(type)), mMultiple(multiple)
{
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);

    mLine = new QLineEdit(this);
    mLine->setAcceptDrops(true);
    mLine->installEventFilter(this);
    mWarning = mLine->addAction(icon("alert-triangle"), QLineEdit::TrailingPosition);
    mWarning->setVisible(false);
    h->addWidget(mLine, 1);

    mModel = new QStandardItemModel(this);
    mCompleter = new NameCompleter(mModel, mMultiple, mLine);
    mCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    mCompleter->setFilterMode(Qt::MatchContains);
    mCompleter->setCompletionMode(QCompleter::PopupCompletion);
    mCompleter->setMaxVisibleItems(16);
    mLine->setCompleter(mCompleter);
    connect(mCompleter, qOverload<const QString&>(&QCompleter::activated), this, [this](const QString&) {
        QTimer::singleShot(0, this, [this] { commit(); });
    });
    connect(mLine, &QLineEdit::editingFinished, this, [this] {
        if (!mCompleter->popup()->isVisible())
            commit();
    });
    connect(mLine, &QLineEdit::textEdited, this, [this] {
        if (mModel->rowCount() == 0)
            fillModel();
    });

    const QString what = typeText();
    mDrop = toolButton(this, "chevron-down", QString("Scene %1; hidden ones dimmed").arg(pluralText()));
    connect(mDrop, &QToolButton::clicked, this, &NodePicker::showDropdown);
    h->addWidget(mDrop);

    const bool meshes = std::ranges::any_of(mTypes, [](const std::string& t) { return t == "mesh" || t == "transform" || t == "node"; });
    if (meshes)
    {
        mDropper = toolButton(this, "color-picker",
                              mMultiple ? "Click meshes in a viewport to add or remove them; Esc when done" : "Click a mesh in a viewport");
        mDropper->setCheckable(true);
        connect(mDropper, &QToolButton::clicked, this, [this](bool on) {
            if (on)
                startEyedropper();
            else if (gPickTool && gPickTool->picker() == this)
                gPickTool->finish();
        });
        h->addWidget(mDropper);
    }

    mLoad = toolButton(this, "arrow-bar-to-left",
                       mMultiple ? QString("Load the selected %1; Shift adds them").arg(pluralText()) : QString("Load the selected %1").arg(what));
    connect(mLoad, &QToolButton::clicked, this, [this] { loadSelected(QApplication::keyboardModifiers() & Qt::ShiftModifier); });
    h->addWidget(mLoad);

    onMiddleClickReset(mLine, [this] { setNames({}, true); }, "none");
    mListener = graph().addListener([this](scene::Change c, scene::NodeId id) { onGraphChange(int(c), id); });
}

NodePicker::~NodePicker()
{
    graph().removeListener(mListener);
    if (gPickTool && gPickTool->picker() == this)
        viewportTools::set(nullptr);
}

QString NodePicker::typeText() const
{
    QStringList parts;
    for (const std::string& t : mTypes)
        parts << QString::fromStdString(t == "transform" ? "object" : t);
    return parts.join(" or ");
}

QString NodePicker::pluralText() const
{
    QStringList parts;
    for (const std::string& t : mTypes)
    {
        const QString one = QString::fromStdString(t == "transform" ? "object" : t);
        parts << (one.endsWith('h') || one.endsWith('s') ? one + "es" : one + "s");
    }
    return parts.join(" or ");
}

void NodePicker::setPlaceholder(const QString& text) { mLine->setPlaceholderText(text); }

bool NodePicker::event(QEvent* e)
{
    // The line's middle-click reset filter shows the line's own tooltip, so it carries the picker's.
    // ToolTipChange never reaches changeEvent.
    if (e->type() == QEvent::ToolTipChange)
        mLine->setToolTip(toolTip());
    return QWidget::event(e);
}

bool NodePicker::accepts(scene::NodeId id) const
{
    const scene::Node* n = graph().node(id);
    if (!n)
        return false;
    for (const std::string& t : mTypes)
        if (t == "node" || (n->type().isA(t) && !(t == "mesh" && n->type().isA("lattice"))))
            return true;
    return false;
}

std::vector<std::string> NodePicker::names() const
{
    std::vector<std::string> out;
    const QString text = mLine->text();
    for (const QString& part : mMultiple ? text.split(',', Qt::SkipEmptyParts) : QStringList{text})
        if (QString t = part.trimmed(); !t.isEmpty())
            out.push_back(t.toStdString());
    return out;
}

nlohmann::json NodePicker::value() const
{
    std::vector<std::string> n = names();
    if (mMultiple)
        return n;
    return n.empty() ? std::string() : n.front();
}

void NodePicker::setValue(const nlohmann::json& value)
{
    std::vector<std::string> n;
    if (value.is_string() && !value.get<std::string>().empty())
        n.push_back(value.get<std::string>());
    else if (value.is_array())
        for (const nlohmann::json& e : value)
            if (e.is_string())
                n.push_back(e.get<std::string>());
    setNames(n, false);
}

void NodePicker::setNames(const std::vector<std::string>& names, bool emit)
{
    QStringList parts;
    for (const std::string& n : names)
        parts << QString::fromStdString(n);
    mLine->setText(parts.join(", "));
    validate();
    if (emit)
        Q_EMIT edited(value());
}

void NodePicker::commit()
{
    std::vector<std::string> n = names();
    setNames(n, true);
}

void NodePicker::validate()
{
    mIds.clear();
    QStringList problems;
    scene::Graph& g = graph();
    for (const std::string& name : names())
    {
        scene::Node* n = nullptr;
        try
        {
            n = g.find(name);
        }
        catch (const CommandError& e)
        {
            problems << QString::fromUtf8(e.what());
            mIds.push_back(scene::kNoNode);
            continue;
        }
        if (!n)
            problems << QString("No node named %1").arg(qs(name));
        else if (!accepts(n->id()))
            problems << QString("%1 is not a %2").arg(qs(name), typeText());
        mIds.push_back(n ? n->id() : scene::kNoNode);
    }
    mWarning->setVisible(!problems.isEmpty());
    mWarning->setToolTip(problems.join('\n'));
}

void NodePicker::onGraphChange(int change, scene::NodeId id)
{
    auto c = scene::Change(change);
    if (c == scene::Change::Value || c == scene::Change::Selection || c == scene::Change::Connection)
        return;
    mModel->clear();
    if (c == scene::Change::Renamed && std::ranges::find(mIds, id) != mIds.end())
    {
        std::vector<std::string> n = names();
        for (size_t i = 0; i < n.size() && i < mIds.size(); ++i)
            if (mIds[i] == id)
                n[i] = graph().path(id);
        setNames(n, true);
        return;
    }
    validate();
}

void NodePicker::fillModel()
{
    scene::Graph& g = graph();
    struct Entry
    {
        QString path;
        bool shown;
    };
    std::vector<Entry> entries;
    for (scene::NodeId id : g.nodes())
        if (accepts(id))
            entries.push_back({qs(g.path(id)), showsInViewport(g, id)});
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        if (a.shown != b.shown)
            return a.shown;
        return QString::compare(a.path, b.path, Qt::CaseInsensitive) < 0;
    });
    mModel->clear();
    const QColor dim = themeColor("dimText");
    for (const Entry& e : entries)
    {
        auto* item = new QStandardItem(e.path);
        if (!e.shown)
        {
            item->setForeground(dim);
            item->setToolTip("Hidden");
        }
        mModel->appendRow(item);
    }
}

void NodePicker::showDropdown()
{
    fillModel();
    if (mModel->rowCount() == 0)
    {
        QToolTip::showText(mDrop->mapToGlobal(QPoint(0, mDrop->height())), QString("No %1 in the scene").arg(pluralText()), mDrop);
        return;
    }
    if (mMultiple)
    {
        QString text = mLine->text().trimmed();
        if (!text.isEmpty() && !text.endsWith(','))
            mLine->setText(text + ", ");
    }
    mLine->setFocus();
    mCompleter->setCompletionPrefix(QString());
    mCompleter->complete(mLine->rect());
}

void NodePicker::loadSelected(bool add)
{
    scene::Graph& g = graph();
    std::vector<scene::NodeId> found;
    for (scene::NodeId id : g.selection())
        if (accepts(id))
            found.push_back(id);
    if (found.empty())
        for (scene::NodeId id : g.selection())
            for (scene::NodeId d : g.descendants(id))
                if (accepts(d) && std::ranges::find(found, d) == found.end())
                    found.push_back(d);
    if (found.empty())
    {
        QToolTip::showText(mLoad->mapToGlobal(QPoint(0, mLoad->height())), QString("Select a %1 first").arg(typeText()), mLoad);
        return;
    }
    if (!mMultiple)
    {
        setNames({g.path(found.back())}, true);
        return;
    }
    std::vector<std::string> n = add ? names() : std::vector<std::string>{};
    for (scene::NodeId id : found)
        if (std::string p = g.path(id); std::ranges::find(n, p) == n.end())
            n.push_back(p);
    setNames(n, true);
}

void NodePicker::pick(scene::NodeId id)
{
    std::string p = graph().path(id);
    if (!mMultiple)
    {
        setNames({p}, true);
        return;
    }
    std::vector<std::string> n = names();
    if (auto it = std::ranges::find(n, p); it != n.end())
        n.erase(it);
    else
        n.push_back(p);
    setNames(n, true);
}

void NodePicker::setEyedropperOn(bool on)
{
    if (mDropper)
        mDropper->setChecked(on);
}

void NodePicker::startEyedropper()
{
    viewportTools::set(std::make_unique<PickTool>(this, viewportTools::activeId()));
}

void NodePicker::dropNames(const QStringList& dropped)
{
    scene::Graph& g = graph();
    std::vector<std::string> n = mMultiple ? names() : std::vector<std::string>{};
    for (const QString& name : dropped)
    {
        scene::Node* node = nullptr;
        try
        {
            node = g.find(name.toStdString());
        }
        catch (const CommandError&)
        {
        }
        if (!node || !accepts(node->id()))
            continue;
        std::string p = g.path(node->id());
        if (!mMultiple)
        {
            setNames({p}, true);
            return;
        }
        if (std::ranges::find(n, p) == n.end())
            n.push_back(p);
    }
    if (mMultiple)
        setNames(n, true);
}

bool NodePicker::eventFilter(QObject* obj, QEvent* e)
{
    if (obj != mLine)
        return QWidget::eventFilter(obj, e);
    switch (e->type())
    {
    case QEvent::DragEnter:
    case QEvent::DragMove:
    {
        auto* d = static_cast<QDropEvent*>(e);
        if (!d->mimeData()->hasFormat(kNodeNamesMime))
            break;
        d->setDropAction(Qt::LinkAction);
        d->accept();
        return true;
    }
    case QEvent::Drop:
    {
        auto* d = static_cast<QDropEvent*>(e);
        if (!d->mimeData()->hasFormat(kNodeNamesMime))
            break;
        d->setDropAction(Qt::LinkAction);
        d->accept();
        dropNames(QString::fromUtf8(d->mimeData()->data(kNodeNamesMime)).split('\n', Qt::SkipEmptyParts));
        return true;
    }
    default: break;
    }
    return QWidget::eventFilter(obj, e);
}
} // namespace lila::ui
