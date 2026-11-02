#include "ui/DrivePopover.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "rigmath/MathNodes.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::PlugType;

json runCommand(const char* name, const json& call) { return ops::execute(name, call, CallSource::UI); }

/** Scalar plugs (numbers and Vec3 components) can feed each other; a whole Vec3 or matrix only its own kind. */
bool compatible(const scene::AttrDef& src, int srcComponent, const scene::AttrDef& dst, int dstComponent)
{
    auto scalar = [](const scene::AttrDef& d, int c) {
        return (d.type == PlugType::Vec3 && c >= 0) || d.type == PlugType::Float || d.type == PlugType::Int || d.type == PlugType::Bool ||
               d.type == PlugType::Enum;
    };
    if (scalar(dst, dstComponent))
        return scalar(src, srcComponent);
    return src.type == dst.type && srcComponent < 0;
}

double numberAt(const std::string& plug)
{
    Graph& g = Graph::current();
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
} // namespace

DrivePopover::DrivePopover(std::string target, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint), mTarget(std::move(target))
{
    setObjectName("drivePopover");
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(360);
    installEventFilter(this);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(4);

    auto* top = new QHBoxLayout;
    auto* title = new QLabel(QString("<b>Drive %1</b>").arg(QString::fromStdString(mTarget).toHtmlEscaped()), this);
    top->addWidget(title, 1);
    auto* closer = new QToolButton(this);
    closer->setIcon(icon("x"));
    closer->setAutoRaise(true);
    closer->setToolTip("Close (Esc)");
    connect(closer, &QToolButton::clicked, this, &QWidget::close);
    top->addWidget(closer);
    v->addLayout(top);

    auto* sourceRow = new QHBoxLayout;
    mSource = new QLabel(this);
    mSource->setObjectName("driveSource");
    sourceRow->addWidget(mSource, 1);
    mJump = new QToolButton(this);
    mJump->setObjectName("driveJump");
    mJump->setIcon(icon("arrow-up-right"));
    mJump->setAutoRaise(true);
    mJump->setToolTip("Select the source");
    mBreak = new QToolButton(this);
    mBreak->setObjectName("driveBreak");
    mBreak->setIcon(icon("unlink"));
    mBreak->setAutoRaise(true);
    mBreak->setToolTip("Break the connection");
    sourceRow->addWidget(mJump);
    sourceRow->addWidget(mBreak);
    v->addLayout(sourceRow);
    connect(mBreak, &QToolButton::clicked, this, [this] { breakSource(); });
    connect(mJump, &QToolButton::clicked, this, [this] {
        Graph& g = Graph::current();
        if (auto src = g.source(g.plug(mTarget)))
            runCommand("select", {{"args", {g.node(src->node)->path()}}});
    });

    mTabs = new QTabBar(this);
    mTabs->setObjectName("driveTabs");
    for (const char* t : {"Connect", "Driven Key", "Expression", "Math"})
        mTabs->addTab(t);
    mTabs->setExpanding(false);
    v->addWidget(mTabs);
    mPages = new QStackedWidget(this);
    v->addWidget(mPages, 1);
    connect(mTabs, &QTabBar::currentChanged, mPages, &QStackedWidget::setCurrentIndex);

    // Connect: search, or pick in the scene.
    {
        auto* page = new QWidget(mPages);
        auto* pv = new QVBoxLayout(page);
        pv->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout;
        mSearch = new QLineEdit(page);
        mSearch->setObjectName("driveSearch");
        mSearch->setPlaceholderText("Source: node.attr");
        mSearch->setClearButtonEnabled(true);
        mSearch->installEventFilter(this);
        mPick = new QToolButton(page);
        mPick->setObjectName("drivePick");
        mPick->setIcon(icon("color-picker"));
        mPick->setCheckable(true);
        mPick->setToolTip("Pick: select an object to list its plugs, or highlight a channel in the Channel Box to connect it");
        row->addWidget(mSearch, 1);
        row->addWidget(mPick);
        pv->addLayout(row);
        mList = new QListWidget(page);
        mList->setObjectName("driveList");
        mList->setMinimumHeight(160);
        pv->addWidget(mList, 1);
        connect(mSearch, &QLineEdit::textChanged, this, [this] { refillSources(); });
        connect(mList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { connectFrom(item->text().toStdString()); });
        mPages->addWidget(page);
    }
    // Driven key: a driver and its keys, with the curve they make.
    {
        auto* page = new QWidget(mPages);
        auto* pv = new QVBoxLayout(page);
        pv->setContentsMargins(0, 0, 0, 0);
        mDriver = new QLineEdit(page);
        mDriver->setObjectName("driveDriver");
        mDriver->setPlaceholderText("Driver: node.attr");
        pv->addWidget(mDriver);
        auto* body = new QHBoxLayout;
        mKeys = new QTableWidget(0, 2, page);
        mKeys->setObjectName("driveKeys");
        mKeys->setHorizontalHeaderLabels({"Driver", "Driven"});
        mKeys->verticalHeader()->hide();
        mKeys->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        mKeys->setMaximumWidth(180);
        body->addWidget(mKeys);
        mCurve = new QLabel(page);
        mCurve->setObjectName("driveCurve");
        mCurve->setMinimumSize(150, 110);
        body->addWidget(mCurve, 1);
        pv->addLayout(body, 1);
        auto* buttons = new QHBoxLayout;
        auto* key = new QPushButton("Key Current", page);
        key->setObjectName("driveKeyCurrent");
        key->setToolTip("Add a key at the driver's and this plug's values now");
        auto* remove = new QPushButton("Remove Key", page);
        auto* apply = new QPushButton("Apply", page);
        apply->setObjectName("driveApplyKeys");
        apply->setDefault(true);
        buttons->addWidget(key);
        buttons->addWidget(remove);
        buttons->addStretch(1);
        buttons->addWidget(apply);
        pv->addLayout(buttons);
        connect(key, &QPushButton::clicked, this, [this] { keyCurrent(); });
        connect(remove, &QPushButton::clicked, this, [this] {
            mKeys->removeRow(mKeys->currentRow());
            drawCurve();
        });
        connect(apply, &QPushButton::clicked, this, [this] { applyDrivenKey(); });
        connect(mKeys, &QTableWidget::itemChanged, this, [this] { drawCurve(); });
        mPages->addWidget(page);
    }
    // Expression: Python syntax, compiled to rig math nodes where it can be.
    {
        auto* page = new QWidget(mPages);
        auto* pv = new QVBoxLayout(page);
        pv->setContentsMargins(0, 0, 0, 0);
        mExpression = new QLineEdit(page);
        mExpression->setObjectName("driveExpression");
        mExpression->setPlaceholderText("= ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)");
        mExpression->setFont(monoFont());
        pv->addWidget(mExpression);
        mExpressionNote = new QLabel(page);
        mExpressionNote->setObjectName("driveExpressionNote");
        mExpressionNote->setWordWrap(true);
        mExpressionNote->setStyleSheet(QString("color: %1;").arg(themeColor("dimText").name()));
        mExpressionNote->setText("Arithmetic, comparisons, if/else, math functions and other plugs by name compile to rig math nodes, "
                                 "which evaluate as fast as nodes and go to Unreal Control Rig. Anything else runs as Python.");
        pv->addWidget(mExpressionNote);
        pv->addStretch(1);
        auto* apply = new QPushButton("Apply", page);
        apply->setObjectName("driveApplyExpression");
        pv->addWidget(apply, 0, Qt::AlignRight);
        connect(apply, &QPushButton::clicked, this, [this] { applyExpression(); });
        connect(mExpression, &QLineEdit::returnPressed, this, [this] { applyExpression(); });
        mPages->addWidget(page);
    }
    // Math: a rig math node wired in.
    {
        auto* page = new QWidget(mPages);
        auto* pv = new QVBoxLayout(page);
        pv->setContentsMargins(0, 0, 0, 0);
        mMathSearch = new QLineEdit(page);
        mMathSearch->setObjectName("driveMathSearch");
        mMathSearch->setPlaceholderText("Search rig math: lerp, clamp, remap...");
        pv->addWidget(mMathSearch);
        mMathList = new QListWidget(page);
        mMathList->setObjectName("driveMathList");
        pv->addWidget(mMathList, 1);
        connect(mMathSearch, &QLineEdit::textChanged, this, [this] { refillMath(); });
        connect(mMathList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { addMath(item->data(Qt::UserRole).toString().toStdString()); });
        mPages->addWidget(page);
    }

    mStatus = new QLabel(this);
    mStatus->setObjectName("driveStatus");
    mStatus->setWordWrap(true);
    mStatus->setStyleSheet("color: #e0a050;");
    mStatus->hide();
    v->addWidget(mStatus);

    mGraphListener = Graph::current().addListener([this](scene::Change, scene::NodeId) {
        QMetaObject::invokeMethod(this, [this] {
            try
            {
                Graph::current().plug(mTarget);
            }
            catch (const std::exception&)
            {
                close();
                return;
            }
            refreshSource();
            if (mPick->isChecked())
                refillSources();
        }, Qt::QueuedConnection);
    });
    // Picking: a channel highlighted in the Channel Box on another node connects straight away.
    mViewListener = attrView::addListener([this] {
        if (!mPick->isChecked())
            return;
        const std::vector<scene::PlugRef>& hl = attrView::highlightedChannels();
        Graph& g = Graph::current();
        const scene::PlugRef me = g.plug(mTarget);
        if (hl.size() == 1 && hl[0].node != me.node)
        {
            const std::string plug = g.plugName(hl[0]);
            QMetaObject::invokeMethod(this, [this, plug] {
                mPick->setChecked(false);
                connectFrom(plug);
            }, Qt::QueuedConnection);
        }
    });
    refreshSource();
    refillSources();
    refillMath();
    drawCurve();
}

DrivePopover::~DrivePopover()
{
    Graph::current().removeListener(mGraphListener);
    attrView::removeListener(mViewListener);
}

void DrivePopover::fail(const QString& why)
{
    mStatus->setText(why);
    mStatus->show();
}

void DrivePopover::refreshSource()
{
    Graph& g = Graph::current();
    scene::PlugRef p;
    try
    {
        p = g.plug(mTarget);
    }
    catch (const std::exception&)
    {
        return;
    }
    std::optional<scene::PlugRef> src = g.source(p);
    if (!src && p.component >= 0)
        src = g.source({p.node, p.attr, -1, p.element});
    const bool keyed = scene::anim::curveFor(g, p) != scene::kNoNode;
    if (!src)
        mSource->setText("<span style='color: gray'>Not driven</span>");
    else
        mSource->setText(QString::fromUtf8("← ") + QString::fromStdString(g.plugName(*src)).toHtmlEscaped() + (keyed ? " (keys)" : ""));
    mJump->setEnabled(src.has_value());
    mBreak->setEnabled(src.has_value());
}

std::vector<std::string> DrivePopover::candidates(const QString& text) const
{
    Graph& g = Graph::current();
    std::vector<std::string> out;
    scene::PlugRef me;
    try
    {
        me = g.plug(mTarget);
    }
    catch (const std::exception&)
    {
        return out;
    }
    const scene::AttrDef& mine = g.attr(me);
    const QString q = text.trimmed();
    const QString nodePart = q.section('.', 0, 0), attrPart = q.contains('.') ? q.section('.', 1) : QString();
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (id == me.node || n->type().isA("animCurve") || n->type().isA("animCurveRQ"))
            continue;
        const QString name = QString::fromStdString(n->name());
        if (!q.isEmpty() && !name.contains(nodePart, Qt::CaseInsensitive) && !(attrPart.isEmpty() && q.size() > 2))
            continue;
        const bool nodeHit = q.isEmpty() || name.contains(nodePart, Qt::CaseInsensitive);
        for (const scene::AttrDef& d : n->type().attrs)
        {
            if ((d.flags & scene::AttrHidden) || (d.flags & scene::AttrArray) || !(d.flags & (scene::AttrKeyable | scene::AttrOutput | scene::AttrChannelBox | scene::AttrUser)))
                continue;
            const QString filter = nodeHit ? attrPart : q;
            auto add = [&](const std::string& attr, int component) {
                if (!compatible(d, component, mine, me.component))
                    return;
                if (!filter.isEmpty() && !QString::fromStdString(attr).contains(filter, Qt::CaseInsensitive))
                    return;
                out.push_back(n->path() + "." + attr);
            };
            if (d.type == PlugType::Vec3)
            {
                add(d.name, -1);
                for (int c = 0; c < 3; ++c)
                    add(d.childNames[size_t(c)], c);
            }
            else
                add(d.name, -1);
            if (out.size() >= 300)
                return out;
        }
    }
    return out;
}

void DrivePopover::refillSources()
{
    QString text = mSearch->text();
    Graph& g = Graph::current();
    // Picking: the newly selected object's plugs, when the search doesn't name one.
    if (mPick->isChecked() && text.isEmpty() && !g.selection().empty())
        if (const scene::Node* n = g.node(g.selection().back()); n && n->id() != g.plug(mTarget).node)
            text = QString::fromStdString(n->name()) + ".";
    mList->clear();
    for (const std::string& c : candidates(text))
        mList->addItem(QString::fromStdString(c));
    if (mList->count())
        mList->setCurrentRow(0);
}

void DrivePopover::refillMath()
{
    mMathList->clear();
    const QString q = mMathSearch->text().trimmed();
    for (const rigmath::Function& f : rigmath::functions())
        for (const rigmath::Overload& o : f.overloads)
        {
            const QString type = QString::fromStdString(o.nodeType);
            if (!q.isEmpty() && !type.contains(q, Qt::CaseInsensitive) && !QString::fromStdString(f.family).contains(q, Qt::CaseInsensitive))
                continue;
            auto* item = new QListWidgetItem(type + "   " + QString::fromStdString(f.family), mMathList);
            item->setData(Qt::UserRole, type);
            item->setToolTip(QString::fromStdString(f.doc));
        }
    if (mMathList->count())
        mMathList->setCurrentRow(0);
}

bool DrivePopover::connectFrom(const std::string& source)
{
    try
    {
        runCommand("connectAttr", {{"args", {source, mTarget}}, {"force", true}});
    }
    catch (const CommandError& e)
    {
        fail(e.what());
        return false;
    }
    Q_EMIT committed();
    close();
    return true;
}

bool DrivePopover::breakSource()
{
    Graph& g = Graph::current();
    const scene::PlugRef p = g.plug(mTarget);
    try
    {
        if (scene::anim::curveFor(g, p))
            runCommand("cutKey", {{"args", {mTarget}}, {"clear", true}});
        else if (g.source(p))
            runCommand("disconnectAttr", {{"args", {mTarget}}});
        else if (p.component >= 0 && g.source({p.node, p.attr, -1, p.element}))
            runCommand("disconnectAttr", {{"args", {g.plugName({p.node, p.attr, -1, p.element})}}});
    }
    catch (const CommandError& e)
    {
        fail(e.what());
        return false;
    }
    refreshSource();
    return true;
}

void DrivePopover::keyCurrent()
{
    const std::string driver = mDriver->text().trimmed().toStdString();
    double d = 0.0;
    try
    {
        d = numberAt(driver);
    }
    catch (const std::exception&)
    {
        fail("Name the driver plug first, as node.attr.");
        return;
    }
    const double v = numberAt(mTarget);
    QSignalBlocker block(mKeys);
    const int r = mKeys->rowCount();
    mKeys->insertRow(r);
    mKeys->setItem(r, 0, new QTableWidgetItem(attrView::formatNumber(d)));
    mKeys->setItem(r, 1, new QTableWidgetItem(attrView::formatNumber(v)));
    drawCurve();
}

bool DrivePopover::applyDrivenKey()
{
    const std::string driver = mDriver->text().trimmed().toStdString();
    if (driver.empty())
    {
        fail("Name the driver plug, as node.attr.");
        return false;
    }
    json keys = json::array();
    for (int r = 0; r < mKeys->rowCount(); ++r)
    {
        bool ok1 = false, ok2 = false;
        const double d = mKeys->item(r, 0) ? mKeys->item(r, 0)->text().toDouble(&ok1) : 0.0;
        const double v = mKeys->item(r, 1) ? mKeys->item(r, 1)->text().toDouble(&ok2) : 0.0;
        if (!ok1 || !ok2)
        {
            fail(QString("Key %1 isn't two numbers.").arg(r + 1));
            return false;
        }
        keys.push_back(json::array({d, v}));
    }
    json call = {{"args", {mTarget}}, {"driver", driver}, {"force", true}};
    if (!keys.empty())
        call["keys"] = keys;
    try
    {
        runCommand("driveChannel", call);
    }
    catch (const CommandError& e)
    {
        fail(e.what());
        return false;
    }
    Q_EMIT committed();
    close();
    return true;
}

bool DrivePopover::applyExpression()
{
    QString text = mExpression->text().trimmed();
    if (text.startsWith('='))
        text = text.mid(1).trimmed();
    if (text.isEmpty())
    {
        fail("Type an expression.");
        return false;
    }
    try
    {
        runCommand("expression", {{"args", {mTarget}}, {"string", text.toStdString()}});
    }
    catch (const CommandError& e)
    {
        fail(e.what());
        return false;
    }
    Q_EMIT committed();
    close();
    return true;
}

std::string DrivePopover::addMath(const std::string& nodeType)
{
    Graph& g = Graph::current();
    const scene::NodeType* t = scene::types::find(nodeType);
    if (!t)
    {
        fail("No math node " + QString::fromStdString(nodeType));
        return {};
    }
    const scene::PlugRef me = g.plug(mTarget);
    const scene::AttrDef& mine = g.attr(me);
    std::string out;
    try
    {
        UndoChunk chunk("Drive with " + nodeType);
        const json created = runCommand("createNode", {{"args", {nodeType}}});
        const std::string node = created.is_array() ? created[0].get<std::string>() : created.get<std::string>();
        const scene::Node* n = g.find(node);
        for (const scene::AttrDef& d : n->type().attrs)
            if ((d.flags & scene::AttrOutput) && compatible(d, -1, mine, me.component))
            {
                runCommand("connectAttr", {{"args", {node + "." + d.name, mTarget}}, {"force", true}});
                out = node;
                break;
            }
        if (out.empty())
            throw CommandError(nodeType + " has no output that fits " + mTarget);
    }
    catch (const CommandError& e)
    {
        fail(e.what());
        return {};
    }
    Q_EMIT committed();
    close();
    return out;
}

void DrivePopover::drawCurve()
{
    std::vector<std::pair<double, double>> pts;
    for (int r = 0; r < mKeys->rowCount(); ++r)
        if (mKeys->item(r, 0) && mKeys->item(r, 1))
            pts.emplace_back(mKeys->item(r, 0)->text().toDouble(), mKeys->item(r, 1)->text().toDouble());
    std::sort(pts.begin(), pts.end());
    const QSize size = mCurve->size().expandedTo(QSize(150, 110));
    QPixmap pm(size);
    pm.fill(themeColor("base"));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(themeColor("altBase"), 1));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    if (pts.empty())
    {
        p.setPen(themeColor("dimText"));
        p.drawText(pm.rect(), Qt::AlignCenter, "No keys");
        mCurve->setPixmap(pm);
        return;
    }
    double x0 = pts.front().first, x1 = pts.back().first, y0 = pts[0].second, y1 = pts[0].second;
    for (auto [x, y] : pts)
    {
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    if (x1 - x0 < 1e-9)
        x1 = x0 + 1.0;
    if (y1 - y0 < 1e-9)
        y1 = y0 + 1.0;
    auto map = [&](double x, double y) {
        return QPointF(8 + (x - x0) / (x1 - x0) * (size.width() - 16), size.height() - 8 - (y - y0) / (y1 - y0) * (size.height() - 16));
    };
    QPainterPath path(map(pts[0].first, pts[0].second));
    for (size_t i = 1; i < pts.size(); ++i)
        path.lineTo(map(pts[i].first, pts[i].second));
    p.setPen(QPen(themeColor("text"), 1.5));
    p.drawPath(path);
    p.setBrush(themeColor("accent"));
    p.setPen(Qt::NoPen);
    for (auto [x, y] : pts)
        p.drawEllipse(map(x, y), 3, 3);
    mCurve->setPixmap(pm);
}

bool DrivePopover::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto* k = static_cast<QKeyEvent*>(event);
        if (k->key() == Qt::Key_Escape)
        {
            close();
            return true;
        }
        if (watched == mSearch && (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up))
        {
            QApplication::sendEvent(mList, event);
            return true;
        }
        if (watched == mSearch && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter))
        {
            if (QListWidgetItem* item = mList->currentItem())
                connectFrom(item->text().toStdString());
            else
                connectFrom(mSearch->text().trimmed().toStdString());
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

namespace drivePopover
{
DrivePopover* show(const std::string& target, QWidget* anchor)
{
    auto* p = new DrivePopover(target, anchor ? anchor->window() : nullptr);
    p->setAttribute(Qt::WA_DeleteOnClose);
    p->adjustSize();
    QPoint at = anchor ? anchor->mapToGlobal(QPoint(anchor->width(), 0)) : QCursor::pos();
    if (QScreen* s = QGuiApplication::screenAt(at))
    {
        const QRect r = s->availableGeometry();
        at.setX(std::clamp(at.x(), r.left(), std::max(r.left(), r.right() - p->width())));
        at.setY(std::clamp(at.y(), r.top(), std::max(r.top(), r.bottom() - p->height())));
    }
    p->move(at);
    p->show();
    p->findChild<QLineEdit*>("driveSearch")->setFocus();
    return p;
}
} // namespace drivePopover
} // namespace lila::ui
