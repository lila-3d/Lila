#include "ui/NodeEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "gn/Gn.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/AnimatedSend.h"
#include "ui/DockSidebar.h"
#include "ui/GnViewerDraw.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <array>
#include <cctype>
#include <cmath>
#include <utility>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace lila::ui
{
using json = nlohmann::json;

namespace
{
QColor gray(int v, int alpha = 255) { return themeShade(v, alpha); }

constexpr double kNodeWidth = 170.0;
constexpr double kHeader = 24.0;
constexpr double kRow = 20.0;
constexpr double kSocketR = 5.0;

std::function<void()>& showHook()
{
    static std::function<void()> hook;
    return hook;
}

std::vector<NodeEditor*>& editors()
{
    static std::vector<NodeEditor*> list;
    return list;
}

NodeEditor*& lastFocused()
{
    static NodeEditor* e = nullptr;
    return e;
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

/** "sizeX" -> "Size X". */
QString pretty(const std::string& id)
{
    QString out;
    for (size_t i = 0; i < id.size(); ++i)
    {
        char c = id[i];
        bool upper = c >= 'A' && c <= 'Z';
        bool digit = c >= '0' && c <= '9';
        bool prevDigit = i > 0 && id[i - 1] >= '0' && id[i - 1] <= '9';
        if (i == 0)
            out += QChar(c).toUpper();
        else if (upper || (digit && !prevDigit))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

/** "Spike Length" -> "spikeLength": the parameter name for a typed display name, empty when nothing usable is left. */
std::string identifierFrom(const QString& text)
{
    std::string out;
    bool up = false;
    for (QChar ch : text)
    {
        char c = ch.toLatin1();
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!alnum)
        {
            up = !out.empty();
            continue;
        }
        if (out.empty() && c >= '0' && c <= '9')
            out += '_';
        out += out.empty() ? char(std::tolower(c)) : up ? char(std::toupper(c)) : c;
        up = false;
    }
    return out;
}

/** Runs a gn command from the UI; failures go to the log rather than a dialog. */
json runCmd(const char* name, json call)
{
    try
    {
        // Queries run quietly; edits echo to the script editor like any UI action.
        bool query = std::string_view(name) == "gnGroupInfo" || std::string_view(name) == "gnNodeTypes";
        return ops::execute(name, call, query ? CallSource::Script : CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

bool hasPlug(const std::string& path)
{
    try
    {
        return scene::Graph::current().plug(path).attr >= 0;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
}

/** Nonzero while a side-panel field sets a value, which then doesn't rebuild the panel under the field. */
int gFieldEdits = 0;

void setFromField(json call)
{
    ++gFieldEdits;
    runCmd("gnSetValue", std::move(call));
    --gFieldEdits;
}

/** The theme's colour per socket type: lightness only in Monotone, where the shape tells the rest. */
QColor socketColor(const std::string& type)
{
    if (type == "geometry")
        return themeColor("socketGeometry");
    if (type == "float")
        return themeColor("socketFloat");
    if (type == "int")
        return themeColor("socketInt");
    if (type == "bool")
        return themeColor("socketBool");
    if (type == "string")
        return themeColor("socketString");
    if (type == "color")
        return themeColor("socketColor");
    return themeColor(type == "rotation" ? "socketRotation" : "socketVector");
}

/** The header colour for a node type's category. */
QColor headerColor(const std::string& type, std::string_view c)
{
    if (type == "GroupInput" || type == "GroupOutput")
        return themeColor("nodeGroupIo");
    static const std::pair<std::string_view, const char*> kTokens[] = {
        {"Geometry", "nodeGeometry"}, {"Mesh", "nodeMesh"},           {"Mesh Primitives", "nodeMesh"}, {"Curve", "nodeCurve"},
        {"Curve Primitives", "nodeCurve"}, {"Point", "nodePoint"},    {"Instances", "nodeInstances"}, {"Attribute", "nodeAttribute"},
        {"Input", "nodeInput"},       {"Output", "nodeOutput"},       {"Utilities", "nodeUtilities"}, {"Color", "nodeColor"},
        {"Texture", "nodeTexture"},   {"Group", "nodeGroupIo"},
    };
    for (auto& [name, token] : kTokens)
        if (c == name)
            return themeColor(token);
    return themeColor("nodeHeader");
}

/**
 * Socket shapes: fields are diamonds (like Blender); otherwise geometry and float are circles, int and bool squares,
 * vectors circles with a dark dot, rotations rings, strings pills.
 */
void drawSocket(QPainter& p, QPointF c, const std::string& type, bool field)
{
    double r = type == "geometry" ? kSocketR + 1.0 : kSocketR;
    p.setPen(QPen(gray(18), 1.2));
    p.setBrush(socketColor(type));
    if (field)
    {
        QPolygonF d;
        d << c + QPointF(0, -r - 1) << c + QPointF(r + 1, 0) << c + QPointF(0, r + 1) << c + QPointF(-r - 1, 0);
        p.drawPolygon(d);
    }
    else if (type == "int" || type == "bool")
        p.drawRect(QRectF(c.x() - r + 0.5, c.y() - r + 0.5, 2 * r - 1, 2 * r - 1));
    else if (type == "string")
        p.drawRoundedRect(QRectF(c.x() - r - 2, c.y() - r + 1.5, 2 * r + 4, 2 * r - 3), 3, 3);
    else
        p.drawEllipse(c, r, r);
    if (type == "vector")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(30));
        p.drawEllipse(c, 1.8, 1.8);
    }
    else if (type == "rotation")
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(45));
        p.drawEllipse(c, r * 0.5, r * 0.5);
    }
    else if (type == "color")
    {
        p.setPen(QPen(gray(45), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r * 0.55, r * 0.55);
    }
}

QString valueText(const json& v, const std::string& type)
{
    auto num = [](double d) {
        QString s = QString::number(d, 'f', 3);
        while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
            s.chop(1);
        return s;
    };
    if (v.is_null())
        return {};
    if (type == "bool")
        return v.is_boolean() ? (v.get<bool>() ? "on" : "off") : (v.is_number() && v.get<double>() != 0.0 ? "on" : "off");
    if (v.is_array() && (v.size() == 3 || v.size() == 4) && v[0].is_number())
    {
        QString t = num(v[0].get<double>()) + ", " + num(v[1].get<double>()) + ", " + num(v[2].get<double>());
        return v.size() == 4 ? t + ", " + num(v[3].get<double>()) : t;
    }
    if (v.is_number())
        return num(v.get<double>());
    if (v.is_string())
        return qs(v.get<std::string>());
    return {};
}

struct SocketInfo
{
    std::string id;
    std::string type;
    bool field = false;
    bool multi = false;
    bool linked = false;
    json value;
    std::string implicit;
    std::string doc;
};

class NodeItem final : public QGraphicsItem
{
public:
    NodeItem(NodeCanvas* canvas, const json& info);

    std::string name;
    std::string type;
    std::string category;
    QString label;
    std::vector<SocketInfo> inputs, outputs;
    std::vector<std::pair<std::string, std::string>> props;
    bool viewerActive = false;
    std::string groupRef; // the group a Group node runs

    bool isViewer() const { return type == "Viewer"; }
    /** The Viewer's eye toggle, in item coordinates. */
    QRectF eyeRect() const { return QRectF(kNodeWidth - kHeader, 2, kHeader - 4, kHeader - 4); }
    /** Socket under a scene point as a tooltip: id, type, field, doc. Empty when none. */
    QString socketTip(QPointF scenePos) const
    {
        bool input = false;
        int i = socketAt(scenePos, input);
        if (i < 0)
            return {};
        const SocketInfo& s = input ? inputs[i] : outputs[i];
        QString t = qs(s.id) + ": " + qs(s.type) + (s.field ? " (field)" : "") + (s.multi ? ", takes several links" : "");
        if (!s.implicit.empty())
            t += "\nUnlinked, it reads the " + qs(s.implicit) + ".";
        if (!s.doc.empty())
            t += "\n" + qs(s.doc);
        return t;
    }

    QRectF boundingRect() const override { return QRectF(-2, -2, kNodeWidth + 4, height() + 4); }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;

    double height() const { return kHeader + kRow * double(outputs.size() + props.size() + inputs.size()) + 6.0; }
    /** Socket centre in scene coordinates. */
    QPointF socketPos(bool input, int index) const
    {
        double row = input ? double(outputs.size() + props.size() + index) : double(index);
        return mapToScene(QPointF(input ? 0.0 : kNodeWidth, kHeader + kRow * row + kRow * 0.5));
    }
    /** Socket under a scene point, -1 for none. */
    int socketAt(QPointF scenePos, bool& input) const
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            bool in = pass == 1;
            const auto& list = in ? inputs : outputs;
            for (int i = 0; i < int(list.size()); ++i)
                if (QLineF(socketPos(in, i), scenePos).length() <= kSocketR + 4.0)
                {
                    input = in;
                    return i;
                }
        }
        return -1;
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

private:
    NodeCanvas* mCanvas;
};

class LinkItem final : public QGraphicsPathItem
{
public:
    LinkItem(NodeItem* from, int out, NodeItem* to, int in) : from(from), out(out), to(to), in(in)
    {
        setZValue(-1);
        bool field = from->outputs[out].field;
        QPen pen(gray(165), 2.0);
        if (field)
            pen.setStyle(Qt::DashLine);
        setPen(pen);
        update();
    }

    void update()
    {
        QPointF a = from->socketPos(false, out), b = to->socketPos(true, in);
        double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
        QPainterPath path(a);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        setPath(path);
    }

    NodeItem* from;
    int out;
    NodeItem* to;
    int in;
};

} // namespace

/** A wire being dragged from a socket, handed to the add-node popup so the new node connects to it. */
struct WireDrag
{
    std::string node;
    std::string socket;
    std::string type;
    bool input = false; // the loose end hangs off an input
    std::optional<std::pair<std::string, std::string>> detached; // (source, input) of a link picked up
};

/** The graph view. Builds items from gnGroupInfo and turns gestures into gn commands. */
class NodeCanvas final : public QGraphicsView
{
public:
    explicit NodeCanvas(QWidget* parent) : QGraphicsView(parent)
    {
        setScene(&mScene);
        mScene.setSceneRect(-20000, -20000, 40000, 40000);
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::RubberBandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFocusPolicy(Qt::StrongFocus);
        setBackgroundBrush(gray(34));
        setAcceptDrops(true);
        connect(&mScene, &QGraphicsScene::selectionChanged, this, [this] {
            if (mBuilding || !onSelect)
                return;
            auto sel = mScene.selectedItems();
            onSelect(sel.size() == 1 ? static_cast<NodeItem*>(sel[0])->name : std::string());
        });
    }

    std::function<void(const std::string&)> onSelect;
    std::function<void(QPointF)> onAddMenu;
    /** A fresh wire let go over empty canvas: offer the nodes it can connect to. */
    std::function<void(QPointF, const WireDrag&)> onWireDropped;
    std::function<void()> onFocused;
    /** Double-click on a Group node: the group it runs. */
    std::function<void(const std::string&)> onOpenGroup;
    /** Ctrl+Shift+click on a node: preview it in a Viewer. */
    std::function<void(const std::string&)> onViewNode;
    /** .lilanodes or .json files dropped on the canvas. */
    std::function<void(const QStringList&)> onFilesDropped;

    const std::string& group() const { return mGroup; }

    /** Output sockets of a node as (id, type), for wiring helpers. */
    std::vector<std::pair<std::string, std::string>> outputsOf(const std::string& node) const
    {
        std::vector<std::pair<std::string, std::string>> out;
        if (auto it = mNodes.find(node); it != mNodes.end())
            for (const SocketInfo& si : it->second->outputs)
                out.push_back({si.id, si.type});
        return out;
    }

    std::string typeOf(const std::string& node) const
    {
        auto it = mNodes.find(node);
        return it == mNodes.end() ? std::string() : it->second->type;
    }

    QPointF positionOf(const std::string& node) const
    {
        auto it = mNodes.find(node);
        return it == mNodes.end() ? QPointF() : it->second->pos();
    }

    void build(const std::string& group, const json& info, const std::set<std::string>& selected)
    {
        mBuilding = true;
        bool newGroup = group != mGroup;
        mGroup = group;
        mLinks.clear();
        mNodes.clear();
        mScene.clear();
        mTemp = nullptr;
        if (info.is_object())
        {
            std::map<std::string, bool> active;
            for (const json& d : info["definition"].value("nodes", json::array()))
                if (d.value("type", "") == "Viewer")
                    active[d.value("name", "")] = d.contains("values") && d["values"].value("active", false);
            for (const json& n : info.value("nodes", json::array()))
            {
                auto* item = new NodeItem(this, n);
                item->viewerActive = active[item->name];
                mScene.addItem(item);
                mNodes[item->name] = item;
                item->setSelected(selected.contains(item->name));
            }
            for (const json& l : info["definition"].value("links", json::array()))
            {
                auto f = mNodes.find(l[0].get<std::string>()), t = mNodes.find(l[2].get<std::string>());
                if (f == mNodes.end() || t == mNodes.end())
                    continue;
                int out = outputIndex(f->second, l[1].get<std::string>()), in = inputIndex(t->second, l[3].get<std::string>());
                if (out < 0 || in < 0)
                    continue;
                auto* link = new LinkItem(f->second, out, t->second, in);
                mScene.addItem(link);
                mLinks.push_back(link);
            }
        }
        mBuilding = false;
        if (newGroup)
            QTimer::singleShot(0, this, [this] { frame(false); });
    }

    bool wireActive() const { return mWireFrom != nullptr; }

    /** Ends a wire drag without linking and returns it. A picked-up link stays hidden until the next rebuild. */
    WireDrag takeWire()
    {
        WireDrag w = wireOf(mWireFrom, mWireSocket, mWireInput);
        w.detached = mDetached;
        mDetached.reset();
        mWireFrom = nullptr;
        removeTemp();
        return w;
    }

    /** The cursor in scene coordinates, or the view centre when the cursor is elsewhere. */
    QPointF cursorScenePos() const
    {
        QPoint p = viewport()->mapFromGlobal(QCursor::pos());
        return mapToScene(viewport()->rect().contains(p) ? p : viewport()->rect().center());
    }

    std::vector<std::string> selectedNames() const
    {
        std::vector<std::string> out;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i))
                out.push_back(n->name);
        return out;
    }

    void selectAll()
    {
        for (auto& [n, item] : mNodes)
            item->setSelected(true);
    }

    void nodeMoved(NodeItem* n)
    {
        for (LinkItem* l : mLinks)
            if (l->from == n || l->to == n)
                l->update();
    }

    void frame(bool selectedOnly)
    {
        QRectF r;
        for (auto& [name, item] : mNodes)
            if (!selectedOnly || item->isSelected())
                r = r.united(item->sceneBoundingRect());
        if (r.isEmpty())
        {
            if (selectedOnly)
                frame(false);
            return;
        }
        r.adjust(-60, -60, 60, 60);
        fitInView(r, Qt::KeepAspectRatio);
        // Don't zoom in past 1:1 for a small graph.
        if (transform().m11() > 1.0)
        {
            QPointF c = r.center();
            resetTransform();
            centerOn(c);
        }
    }

protected:
    void drawBackground(QPainter* p, const QRectF& rect) override
    {
        p->fillRect(rect, gray(34));
        double step = 25.0;
        p->setPen(QPen(gray(48), 0));
        for (double x = std::floor(rect.left() / step) * step; x < rect.right(); x += step)
            for (double y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step)
                p->drawPoint(QPointF(x, y));
    }

    void wheelEvent(QWheelEvent* e) override
    {
        double f = std::pow(1.0015, e->angleDelta().y());
        double s = transform().m11() * f;
        if (s < 0.1 || s > 4.0)
            return;
        scale(f, f);
    }

    void focusInEvent(QFocusEvent* e) override
    {
        QGraphicsView::focusInEvent(e);
        if (onFocused)
            onFocused();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        setFocus(Qt::MouseFocusReason);
        QPointF sp = mapToScene(e->pos());
        if (e->button() == Qt::MiddleButton)
        {
            mPanning = true;
            mLast = e->pos();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::ShiftModifier))
            if (NodeItem* n = nodeAt(sp); n && onViewNode)
            {
                onViewNode(n->name);
                return;
            }
        if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier))
            if (NodeItem* n = nodeAt(sp); n && n->isViewer() && n->eyeRect().contains(n->mapFromScene(sp)))
            {
                if (n->viewerActive)
                    runCmd("gnSetActiveViewer", args({mGroup}));
                else
                    runCmd("gnSetActiveViewer", args({mGroup, n->name}));
                return;
            }
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
        {
            mCutting = true;
            mCut = {sp};
            mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
            return;
        }
        if (e->button() == Qt::LeftButton)
            if (NodeItem* n = nodeAt(sp))
            {
                bool input = false;
                int s = n->socketAt(sp, input);
                if (s >= 0)
                {
                    startWire(n, s, input);
                    return;
                }
            }
        mPressPositions.clear();
        QGraphicsView::mousePressEvent(e);
        for (auto& [name, item] : mNodes)
            mPressPositions[name] = item->pos();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        QPointF sp = mapToScene(e->pos());
        if (mPanning)
        {
            QPoint d = e->pos() - mLast;
            mLast = e->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
            return;
        }
        if (mCutting)
        {
            mCut.push_back(sp);
            QPainterPath path(mCut.front());
            for (const QPointF& p : mCut)
                path.lineTo(p);
            mTemp->setPath(path);
            return;
        }
        if (mWireFrom)
        {
            QPointF a = mWireFrom->socketPos(mWireInput, mWireSocket);
            QPointF from = mWireInput ? sp : a, to = mWireInput ? a : sp;
            double dx = std::max(40.0, std::abs(to.x() - from.x()) * 0.5);
            QPainterPath path(from);
            path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
            mTemp->setPath(path);
            return;
        }
        QGraphicsView::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        QPointF sp = mapToScene(e->pos());
        if (mPanning)
        {
            mPanning = false;
            unsetCursor();
            return;
        }
        if (mCutting)
        {
            mCutting = false;
            finishCut();
            return;
        }
        if (mWireFrom)
        {
            finishWire(sp);
            return;
        }
        QGraphicsView::mouseReleaseEvent(e);
        commitMoves();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        NodeItem* n = nodeAt(mapToScene(e->pos()));
        if (!n && onAddMenu)
            onAddMenu(mapToScene(e->pos()));
        else if (n && !n->groupRef.empty() && onOpenGroup)
            onOpenGroup(n->groupRef);
        else
            QGraphicsView::mouseDoubleClickEvent(e);
    }

    bool viewportEvent(QEvent* e) override
    {
        if (e->type() == QEvent::ToolTip)
        {
            auto* he = static_cast<QHelpEvent*>(e);
            QPointF sp = mapToScene(he->pos());
            if (NodeItem* n = nodeAt(sp))
                if (QString tip = n->socketTip(sp); !tip.isEmpty())
                {
                    QToolTip::showText(he->globalPos(), tip, viewport());
                    return true;
                }
        }
        return QGraphicsView::viewportEvent(e);
    }

    static QStringList nodeFiles(const QMimeData* m)
    {
        QStringList out;
        if (m)
            for (const QUrl& u : m->urls())
                if (u.isLocalFile() && (u.toLocalFile().endsWith(".lilanodes", Qt::CaseInsensitive) || u.toLocalFile().endsWith(".json", Qt::CaseInsensitive)))
                    out << u.toLocalFile();
        return out;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (!nodeFiles(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QGraphicsView::dragEnterEvent(e);
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!nodeFiles(e->mimeData()).isEmpty())
            e->acceptProposedAction();
        else
            QGraphicsView::dragMoveEvent(e);
    }

    void dropEvent(QDropEvent* e) override
    {
        QStringList files = nodeFiles(e->mimeData());
        if (files.isEmpty() || !onFilesDropped)
        {
            QGraphicsView::dropEvent(e);
            return;
        }
        e->acceptProposedAction();
        onFilesDropped(files);
    }

private:
    static WireDrag wireOf(NodeItem* n, int socket, bool input)
    {
        const SocketInfo& s = input ? n->inputs[socket] : n->outputs[socket];
        return WireDrag{n->name, s.id, s.type, input, std::nullopt};
    }

    void removeTemp()
    {
        if (!mTemp)
            return;
        mScene.removeItem(mTemp);
        delete mTemp;
        mTemp = nullptr;
    }

    static int outputIndex(NodeItem* n, const std::string& id)
    {
        for (int i = 0; i < int(n->outputs.size()); ++i)
            if (n->outputs[i].id == id)
                return i;
        return -1;
    }
    static int inputIndex(NodeItem* n, const std::string& id)
    {
        for (int i = 0; i < int(n->inputs.size()); ++i)
            if (n->inputs[i].id == id)
                return i;
        return -1;
    }

    NodeItem* nodeAt(QPointF sp) const
    {
        // Sockets stick out of the node rect, so test with a margin.
        for (auto& [name, item] : mNodes)
            if (item->sceneBoundingRect().adjusted(-kSocketR - 4, 0, kSocketR + 4, 0).contains(sp))
                return item;
        return nullptr;
    }

    void startWire(NodeItem* n, int socket, bool input)
    {
        mDetached.reset();
        if (input && n->inputs[socket].linked)
        {
            // Grabbing a linked input picks the link up from its source, like Blender.
            for (LinkItem* l : mLinks)
                if (l->to == n && l->in == socket)
                {
                    mDetached = std::make_pair(l->from->name + "." + l->from->outputs[l->out].id, n->name + "." + n->inputs[socket].id);
                    n = l->from;
                    socket = l->out;
                    input = false;
                    l->hide();
                }
        }
        mWireFrom = n;
        mWireSocket = socket;
        mWireInput = input;
        mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 2.0));
    }

    void finishWire(QPointF sp)
    {
        NodeItem* from = mWireFrom;
        int fs = mWireSocket;
        bool fromInput = mWireInput;
        mWireFrom = nullptr;
        removeTemp();
        NodeItem* target = nodeAt(sp);
        bool tin = false;
        int ts = target ? target->socketAt(sp, tin) : -1;
        if (!target && !mDetached && onWireDropped)
        {
            onWireDropped(sp, wireOf(from, fs, fromInput));
            return;
        }
        UndoChunk chunk("Link");
        if (mDetached)
            runCmd("gnDisconnect", {{"args", {mGroup, mDetached->second}}, {"source", mDetached->first}});
        mDetached.reset();
        if (!target || ts < 0 || target == from || tin == fromInput)
        {
            if (!target)
                QTimer::singleShot(0, this, [this] { viewport()->update(); });
            return;
        }
        std::string out = fromInput ? target->name + "." + target->outputs[ts].id : from->name + "." + from->outputs[fs].id;
        std::string in = fromInput ? from->name + "." + from->inputs[fs].id : target->name + "." + target->inputs[ts].id;
        runCmd("gnConnect", args({mGroup, out, in}));
    }

    void finishCut()
    {
        if (mTemp)
        {
            mScene.removeItem(mTemp);
            delete mTemp;
            mTemp = nullptr;
        }
        QPainterPath cut(mCut.front());
        for (const QPointF& p : mCut)
            cut.lineTo(p);
        std::vector<std::pair<std::string, std::string>> hits;
        for (LinkItem* l : mLinks)
        {
            // Sample the curve against the stroke's segments.
            const QPainterPath& path = l->path();
            bool hit = false;
            for (int k = 0; k < 24 && !hit; ++k)
            {
                QLineF seg(path.pointAtPercent(k / 24.0), path.pointAtPercent((k + 1) / 24.0));
                for (size_t i = 1; i < mCut.size() && !hit; ++i)
                    hit = seg.intersects(QLineF(mCut[i - 1], mCut[i])) == QLineF::BoundedIntersection;
            }
            if (hit)
                hits.push_back({l->from->name + "." + l->from->outputs[l->out].id, l->to->name + "." + l->to->inputs[l->in].id});
        }
        mCut.clear();
        if (hits.empty())
            return;
        UndoChunk chunk("Cut Links");
        for (auto& [src, dst] : hits)
            runCmd("gnDisconnect", {{"args", {mGroup, dst}}, {"source", src}});
    }

    void commitMoves()
    {
        json positions = json::object();
        for (auto& [name, item] : mNodes)
        {
            auto it = mPressPositions.find(name);
            if (it != mPressPositions.end() && QLineF(it->second, item->pos()).length() > 0.5)
                positions[name] = {item->pos().x(), item->pos().y()};
        }
        mPressPositions.clear();
        if (!positions.empty())
            runCmd("gnMoveNodes", {{"args", {mGroup}}, {"positions", positions}});
    }

    QGraphicsScene mScene;
    std::string mGroup;
    std::map<std::string, NodeItem*> mNodes;
    std::vector<LinkItem*> mLinks;
    std::map<std::string, QPointF> mPressPositions;
    QGraphicsPathItem* mTemp = nullptr;
    NodeItem* mWireFrom = nullptr;
    int mWireSocket = -1;
    bool mWireInput = false;
    std::optional<std::pair<std::string, std::string>> mDetached; // (source, input) of a link picked up
    std::vector<QPointF> mCut;
    bool mCutting = false;
    bool mPanning = false;
    bool mBuilding = false;
    QPoint mLast;
};

namespace
{
NodeItem::NodeItem(NodeCanvas* canvas, const json& info) : mCanvas(canvas)
{
    name = info.value("name", "");
    type = info.value("type", "");
    category = info.value("category", "");
    label = qs(info.value("label", type));
    for (const json& s : info.value("inputs", json::array()))
        inputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), s.value("multi", false), s.value("linked", false),
                          s.value("value", json()), s.value("implicit", ""), s.value("doc", "")});
    for (const json& s : info.value("outputs", json::array()))
        outputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), false, false, json(), "", s.value("doc", "")});
    for (const json& p : info.value("props", json::array()))
        props.push_back({p.value("id", ""), p.value("value", "")});
    if (type == gn::kGroupNodeDeclType)
        for (auto& [id, v] : props)
            if (id == "group")
                groupRef = v;
    if (!groupRef.empty())
        label = qs(groupRef);
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    const json& pos = info["position"];
    setPos(pos[0].get<double>(), pos[1].get<double>());
    QString tip = label;
    if (!groupRef.empty())
        tip += "\nGroup node: double-click to open " + qs(groupRef) + ".";
    if (isViewer())
        tip += "\nClick the eye to preview in the viewport.";
    setToolTip(tip);
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged)
        mCanvas->nodeMoved(this);
    return QGraphicsItem::itemChange(change, value);
}

void NodeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    QRectF body(0, 0, kNodeWidth, height());
    p->setPen(isSelected() ? QPen(themeColor("nodeSelected"), 1.6) : QPen(gray(20), 1.0));
    p->setBrush(themeColor("nodeBody"));
    p->drawRoundedRect(body, 6, 6);
    QPainterPath head;
    head.addRoundedRect(QRectF(0, 0, kNodeWidth, kHeader), 6, 6);
    head.addRect(QRectF(0, kHeader - 6, kNodeWidth, 6));
    p->setPen(Qt::NoPen);
    p->setBrush(headerColor(type, category));
    p->drawPath(head.simplified());
    p->setPen(gray(232));
    QFont f = p->font();
    f.setBold(true);
    p->setFont(f);
    p->drawText(QRectF(8, 0, kNodeWidth - 16 - (isViewer() ? kHeader : 0.0), kHeader), Qt::AlignVCenter | Qt::AlignLeft, label);
    f.setBold(false);
    p->setFont(f);
    if (isViewer())
    {
        QRectF eye = eyeRect();
        if (viewerActive)
        {
            p->setPen(Qt::NoPen);
            p->setBrush(gray(150));
            p->drawRoundedRect(eye, 3, 3);
        }
        QIcon ic = icon(viewerActive ? "eye" : "eye-off");
        p->drawPixmap(eye.adjusted(2, 2, -2, -2).toRect(), ic.pixmap(QSize(32, 32), viewerActive ? QIcon::Active : QIcon::Normal));
    }

    double y = kHeader;
    for (size_t i = 0; i < outputs.size(); ++i, y += kRow)
    {
        p->setPen(gray(205));
        p->drawText(QRectF(8, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignRight, pretty(outputs[i].id));
        drawSocket(*p, QPointF(kNodeWidth, y + kRow * 0.5), outputs[i].type, outputs[i].field);
    }
    for (size_t i = 0; i < props.size(); ++i, y += kRow)
    {
        QRectF r(8, y + 2, kNodeWidth - 16, kRow - 4);
        p->setPen(QPen(gray(40), 1));
        p->setBrush(gray(44));
        p->drawRoundedRect(r, 3, 3);
        p->setPen(gray(200));
        QString text = props[i].second.empty() ? pretty(props[i].first) : pretty(props[i].second);
        p->drawText(r.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    for (size_t i = 0; i < inputs.size(); ++i, y += kRow)
    {
        const SocketInfo& s = inputs[i];
        QString text = pretty(s.id);
        if (!s.linked && s.type != "geometry")
        {
            QString v = !s.implicit.empty() && s.value.is_null() ? pretty(s.implicit) : valueText(s.value, s.type);
            if (!v.isEmpty())
                text += "  " + v;
        }
        p->setPen(s.linked || s.type == "geometry" ? gray(205) : gray(170));
        p->drawText(QRectF(12, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(text, Qt::ElideRight, int(kNodeWidth - 20)));
        drawSocket(*p, QPointF(0, y + kRow * 0.5), s.type, s.field);
        if (s.multi)
        {
            p->setPen(QPen(gray(18), 1));
            p->drawLine(QPointF(-2, y + kRow * 0.5), QPointF(2, y + kRow * 0.5));
        }
    }
}

/** Search-as-you-type node picker. Enter or a click adds the node. */
class AddNodePopup final : public QFrame
{
public:
    /** types as gnNodeTypes returns them; pick gets the type and, for a filtered list, the socket to link. */
    AddNodePopup(QWidget* parent, const json& types, std::function<void(const std::string&, const std::string&)> pick)
        : QFrame(parent, Qt::Popup), mPick(std::move(pick))
    {
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search nodes");
        mList = new QListWidget(this);
        lay->addWidget(mSearch);
        lay->addWidget(mList);
        resize(300, 380);
        for (const json& t : types)
            mAll.push_back({t.value("type", ""), qs(t.value("category", "") + " / " + t.value("label", "")), qs(t.value("doc", "")),
                            t.value("socket", "")});
        std::sort(mAll.begin(), mAll.end(), [](const Entry& a, const Entry& b) { return a.text < b.text; });
        filter();
        connect(mSearch, &QLineEdit::textChanged, this, [this] { filter(); });
        connect(mSearch, &QLineEdit::returnPressed, this, [this] { choose(mList->currentItem()); });
        connect(mList, &QListWidget::itemActivated, this, [this](QListWidgetItem* i) { choose(i); });
        connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* i) { choose(i); });
        mSearch->installEventFilter(this);
    }

    std::function<void(bool picked)> onClosed;

    /** Opens with its search field under the cursor, kept on screen. */
    void popup(QPoint global)
    {
        QPoint at = global - QPoint(20, 14);
        if (QScreen* s = QGuiApplication::screenAt(global))
        {
            QRect r = s->availableGeometry();
            at.setX(std::clamp(at.x(), r.left(), r.right() - width()));
            at.setY(std::clamp(at.y(), r.top(), r.bottom() - height()));
        }
        move(at);
        show();
        mSearch->setFocus();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == mSearch && e->type() == QEvent::KeyPress)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up)
            {
                int row = mList->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
                mList->setCurrentRow(std::clamp(row, 0, mList->count() - 1));
                return true;
            }
        }
        return QFrame::eventFilter(o, e);
    }

    void hideEvent(QHideEvent* e) override
    {
        QFrame::hideEvent(e);
        if (mPicked)
            return;
        if (onClosed)
            onClosed(false);
        deleteLater();
    }

private:
    struct Entry
    {
        std::string type;
        QString text;
        QString doc;
        std::string socket;
    };

    void filter()
    {
        mList->clear();
        QString q = mSearch->text().trimmed();
        for (const Entry& e : mAll)
            if (q.isEmpty() || e.text.contains(q, Qt::CaseInsensitive) || qs(e.type).contains(q, Qt::CaseInsensitive))
            {
                auto* item = new QListWidgetItem(e.socket.empty() ? e.text : e.text + "  (" + pretty(e.socket) + ")", mList);
                item->setData(Qt::UserRole, qs(e.type));
                item->setData(Qt::UserRole + 1, qs(e.socket));
                item->setToolTip(e.doc);
            }
        mList->setCurrentRow(0);
    }

    void choose(QListWidgetItem* i)
    {
        if (!i)
            return;
        std::string type = i->data(Qt::UserRole).toString().toStdString();
        std::string socket = i->data(Qt::UserRole + 1).toString().toStdString();
        mPicked = true;
        close();
        mPick(type, socket);
        deleteLater();
    }

    std::function<void(const std::string&, const std::string&)> mPick;
    bool mPicked = false;
    std::vector<Entry> mAll;
    QLineEdit* mSearch;
    QListWidget* mList;
};

QDoubleSpinBox* numberBox(QWidget* parent, double v, bool integer)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(-1e9, 1e9);
    s->setDecimals(integer ? 0 : 3);
    s->setSingleStep(integer ? 1.0 : 0.1);
    s->setValue(v);
    s->setKeyboardTracking(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return s;
}

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double toLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

/** A swatch button for a linear RGBA value; the swatch shows the colour itself, so it is data, not chrome. */
QWidget* colorEditor(QWidget* parent, const json& value, std::function<void(json)> set)
{
    auto* b = new QToolButton(parent);
    b->setMinimumWidth(60);
    auto rgba = std::make_shared<std::array<double, 4>>(std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
    if (value.is_array() && value.size() >= 3)
        for (size_t k = 0; k < std::min<size_t>(4, value.size()); ++k)
            (*rgba)[k] = value[k].is_number() ? value[k].get<double>() : 0.0;
    else if (value.is_number())
        (*rgba)[0] = (*rgba)[1] = (*rgba)[2] = value.get<double>();
    auto paint = [b, rgba] {
        QColor c = QColor::fromRgbF(float(toSrgb((*rgba)[0])), float(toSrgb((*rgba)[1])), float(toSrgb((*rgba)[2])));
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid rgb(20,20,20); min-height: 16px; }").arg(c.name()));
        b->setToolTip(QString("Linear %1, %2, %3, alpha %4").arg((*rgba)[0], 0, 'f', 3).arg((*rgba)[1], 0, 'f', 3).arg((*rgba)[2], 0, 'f', 3).arg((*rgba)[3], 0, 'f', 3));
    };
    paint();
    QObject::connect(b, &QToolButton::clicked, b, [b, rgba, paint, set] {
        QColor init = QColor::fromRgbF(float(toSrgb((*rgba)[0])), float(toSrgb((*rgba)[1])), float(toSrgb((*rgba)[2])), float((*rgba)[3]));
        QColor c = QColorDialog::getColor(init, b, "Color", QColorDialog::ShowAlphaChannel);
        if (!c.isValid())
            return;
        *rgba = {toLinear(c.redF()), toLinear(c.greenF()), toLinear(c.blueF()), c.alphaF()};
        paint();
        set(json::array({(*rgba)[0], (*rgba)[1], (*rgba)[2], (*rgba)[3]}));
    });
    return b;
}

/** Editor for one socket value; calls set with the new JSON value. A non-null def is restored by a middle click. */
QWidget* valueEditor(QWidget* parent, const std::string& type, const json& value, std::function<void(json)> set, const json& def = json())
{
    if (type == "color")
        return colorEditor(parent, value, std::move(set));
    if (type == "bool")
    {
        auto* c = new QCheckBox(parent);
        c->setChecked(value.is_boolean() ? value.get<bool>() : value.is_number() && value.get<double>() != 0.0);
        if (def.is_boolean() || def.is_number())
            setCheckDefault(c, def.is_boolean() ? def.get<bool>() : def.get<double>() != 0.0);
        QObject::connect(c, &QCheckBox::toggled, c, [set](bool on) { set(on); });
        return c;
    }
    if (type == "string")
    {
        auto* e = new QLineEdit(value.is_string() ? qs(value.get<std::string>()) : QString(), parent);
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, set] { set(e->text().toStdString()); });
        return e;
    }
    if (type == "vector" || type == "rotation")
    {
        auto* w = new QWidget(parent);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        std::array<QDoubleSpinBox*, 3> boxes;
        for (int k = 0; k < 3; ++k)
        {
            double v = value.is_array() && value.size() == 3 ? value[k].get<double>() : value.is_number() ? value.get<double>() : 0.0;
            boxes[k] = numberBox(w, v, false);
            if (def.is_array() && def.size() == 3 && def[k].is_number())
                setSpinDefault(boxes[k], def[k].get<double>());
            else if (def.is_number())
                setSpinDefault(boxes[k], def.get<double>());
            h->addWidget(boxes[k]);
        }
        for (int k = 0; k < 3; ++k)
            QObject::connect(boxes[k], &QDoubleSpinBox::valueChanged, w, [boxes, set] {
                set(json::array({boxes[0]->value(), boxes[1]->value(), boxes[2]->value()}));
            });
        return w;
    }
    bool integer = type == "int";
    auto* s = numberBox(parent, value.is_number() ? value.get<double>() : 0.0, integer);
    if (def.is_number())
        setSpinDefault(s, def.get<double>());
    QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [set, integer](double v) { set(integer ? json(int64_t(std::llround(v))) : json(v)); });
    return s;
}

/** The soft range, or the hard one, of an exposed input; 0 when it has neither end. */
double rangeSpan(const json& in)
{
    auto end = [&](const char* soft, const char* hard) -> std::optional<double> {
        if (in.contains(soft) && in[soft].is_number())
            return in[soft].get<double>();
        if (in.contains(hard) && in[hard].is_number())
            return in[hard].get<double>();
        return std::nullopt;
    };
    std::optional<double> lo = end("softMin", "min"), hi = end("softMax", "max");
    if (!hi)
        return 0.0;
    return *hi - lo.value_or(0.0);
}

/** Rounds up to 1, 2 or 5 times a power of ten. */
double niceStep(double raw)
{
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0})
        if (raw <= m * p)
            return m * p;
    return 10.0 * p;
}

/** Bounds every spin box under w by an exposed input's min, max and step; with no step, a scrub across ~1200 px covers the range. */
void applyLimits(QWidget* w, const json& in)
{
    QList<QDoubleSpinBox*> boxes = w->findChildren<QDoubleSpinBox*>();
    if (auto* self = qobject_cast<QDoubleSpinBox*>(w))
        boxes.push_back(self);
    for (QDoubleSpinBox* b : boxes)
    {
        QSignalBlocker block(b);
        double v = b->value();
        b->setRange(in.contains("min") ? in["min"].get<double>() : -1e9, in.contains("max") ? in["max"].get<double>() : 1e9);
        if (in.contains("step"))
            b->setSingleStep(in["step"].get<double>());
        else if (double span = rangeSpan(in); span > 0.0 && b->decimals() > 0)
            b->setSingleStep(niceStep(span / 200.0));
        b->setValue(v);
    }
}

QLabel* heading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet("color: rgb(225,225,225); padding-top: 6px;");
    return l;
}
} // namespace

NodeEditor::NodeEditor(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(6, 4, 6, 4);
    mGroups = new QComboBox(this);
    mGroups->setMinimumWidth(180);
    auto* newBtn = new QToolButton(this);
    newBtn->setText("New");
    newBtn->setToolTip("New geometry-nodes group");
    auto* applyBtn = new QToolButton(this);
    applyBtn->setText("Apply to Selection");
    applyBtn->setToolTip("Add this group as a modifier on the selected meshes");
    auto* addBtn = new QToolButton(this);
    addBtn->setText("Add Node");
    addBtn->setToolTip("Add a node (Tab or Shift+A over the canvas)");
    auto* saveBtn = new QToolButton(this);
    saveBtn->setText("Save to Library");
    auto* exportBtn = new QToolButton(this);
    exportBtn->setIcon(icon("file-export"));
    exportBtn->setToolTip("Export this group (and the groups nested in it) to a .lilanodes file to share");
    auto* importBtn = new QToolButton(this);
    importBtn->setIcon(icon("file-import"));
    importBtn->setToolTip("Import node groups from a .lilanodes or .json file; you can also drop the file on the canvas");
    bar->addWidget(new QLabel("Group", this));
    bar->addWidget(mGroups);
    bar->addWidget(newBtn);
    bar->addWidget(addBtn);
    bar->addWidget(applyBtn);
    bar->addWidget(saveBtn);
    bar->addWidget(exportBtn);
    bar->addWidget(importBtn);
    bar->addStretch(1);
    mSendBtn = new QToolButton(this);
    mSendBtn->setIcon(icon("send"));
    mSendBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mSendBtn->setPopupMode(QToolButton::MenuButtonPopup);
    auto* sendMenu = new QMenu(mSendBtn);
    animatedSend::fillMenu(sendMenu, this, [this] { return sendTargets(); });
    mSendBtn->setMenu(sendMenu);
    connect(sendMenu, &QMenu::aboutToShow, this, [this] { refreshSendButton(); });
    connect(mSendBtn, &QToolButton::clicked, this, [this] {
        animatedSend::sendAgain(sendTargets());
        refreshSendButton();
    });
    bar->addWidget(mSendBtn);
    // Sends stored after the command returns, so the label is read on the next turn of the event loop.
    mExecuteListener = ops::addExecuteListener([this](const ExecuteEvent& e) {
        if (e.done && (e.name == "unrealMap" || e.name.starts_with("export")))
            QTimer::singleShot(0, this, [this] { refreshSendButton(); });
    });
    lay->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal, this);
    mCanvas = new NodeCanvas(split);
    mSide = new QScrollArea(split);
    mSide->setWidgetResizable(true);
    mSide->setMinimumWidth(230);
    split->addWidget(mCanvas);
    split->addWidget(mSide);
    split->setStretchFactor(0, 1);
    split->setSizes({700, 260});
    lay->addWidget(split, 1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(0);
    connect(mRefresh, &QTimer::timeout, this, [this] { refresh(std::exchange(mPanelDirty, false)); });

    setProperty("hotkeyContext", "nodeEditor");
    mCanvas->onAddMenu = [this](QPointF scenePos) { openAddMenu(scenePos, nullptr); };
    mCanvas->onWireDropped = [this](QPointF scenePos, const WireDrag& w) { openAddMenu(scenePos, &w); };
    mCanvas->onFocused = [this] { lastFocused() = this; };
    mCanvas->onOpenGroup = [this](const std::string& g) {
        if (gn::findGroupNode(scene::Graph::current(), g))
            setGroup(qs(g));
        else
            showError("The group " + qs(g) + " isn't in this scene (only its embedded copy is).");
    };
    mCanvas->onViewNode = [this](const std::string& n) { viewNode(n); };
    mCanvas->onFilesDropped = [this](const QStringList& files) { importGroups(files); };
    connect(exportBtn, &QToolButton::clicked, this, [this] { exportGroup(); });
    connect(importBtn, &QToolButton::clicked, this, [this] {
        QStringList files = QFileDialog::getOpenFileNames(this, "Import Node Groups", QString(), "Node groups (*.lilanodes *.json);;All files (*)");
        if (!files.isEmpty())
            importGroups(files);
    });
    mCanvas->onSelect = [this](const std::string& n) {
        mSelectedNode = qs(n);
        rebuildSidePanel();
    };

    connect(mGroups, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        if (!mRefreshing && t != mGroup)
            setGroup(t);
    });
    connect(newBtn, &QToolButton::clicked, this, [] { actions::run(json("gn.newGroup")); });
    connect(addBtn, &QToolButton::clicked, this, [this] { openAddMenu(mCanvas->mapToScene(mCanvas->viewport()->rect().center()), nullptr); });
    connect(applyBtn, &QToolButton::clicked, this, [this] {
        if (mGroup.isEmpty())
            return;
        scene::Graph& g = scene::Graph::current();
        UndoChunk chunk("Apply Geometry Nodes");
        for (scene::NodeId id : std::vector<scene::NodeId>(g.selection()))
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                runCmd("gnApply", args({n->path(), mGroup.toStdString()}));
    });
    connect(saveBtn, &QToolButton::clicked, this, [this] {
        if (!mGroup.isEmpty())
        {
            json p = runCmd("gnSaveGroup", args({mGroup.toStdString()}));
            if (p.is_string())
                log::info("saved {}", p.get<std::string>());
        }
    });

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        scene::Graph& g = scene::Graph::current();
        if (c == scene::Change::Selection)
        {
            QTimer::singleShot(0, this, [this] {
                followSelection();
                refreshSendButton();
            });
            return;
        }
        if (c == scene::Change::Reset)
        {
            // A command's batched changes arrive as one Reset, a selection among them.
            QTimer::singleShot(0, this, [this] { followSelection(); });
            scheduleRefresh(gFieldEdits == 0);
            return;
        }
        if (c == scene::Change::Cleared || c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Renamed)
        {
            scheduleRefresh();
            return;
        }
        if (c == scene::Change::Value)
            if (scene::Node* n = g.node(id))
            {
                if (n->type().name == gn::kGroupNodeType)
                    scheduleRefresh(gFieldEdits == 0);
                // Evaluation writes the modifier's outputs too; only a change to a shown value rebuilds the panel.
                else if (gFieldEdits == 0 && qs(n->path()) == mModifier && !mValueCheck)
                {
                    mValueCheck = true;
                    QTimer::singleShot(0, this, [this] {
                        mValueCheck = false;
                        if (shownValues() != mShownValues)
                            scheduleRefresh();
                    });
                }
            }
    });
    editors().push_back(this);
    followSelection();
    refresh();
}

NodeEditor::~NodeEditor()
{
    // The canvas outlives this body; its scene emits selectionChanged as it clears on destruction.
    mCanvas->onSelect = nullptr;
    mCanvas->onFocused = nullptr;
    scene::Graph::current().removeListener(mListener);
    ops::removeExecuteListener(mExecuteListener);
    std::erase(editors(), this);
    if (lastFocused() == this)
        lastFocused() = nullptr;
}

void NodeEditor::setGroup(const QString& name)
{
    mGroup = name;
    mSelectedNode.clear();
    refresh();
}

void NodeEditor::frameAll() { mCanvas->frame(false); }

void NodeEditor::frameSelected() { mCanvas->frame(true); }

NodeEditor* NodeEditor::active()
{
    QWidget* focus = QApplication::focusWidget();
    for (NodeEditor* e : editors())
        if (focus && e->isAncestorOf(focus))
            return e;
    if (lastFocused())
        return lastFocused();
    for (NodeEditor* e : editors())
        if (e->isVisible())
            return e;
    return nullptr;
}

void NodeEditor::addNodeAtCursor()
{
    if (QApplication::activePopupWidget())
        return;
    if (mCanvas->wireActive())
    {
        WireDrag w = mCanvas->takeWire();
        openAddMenu(mCanvas->cursorScenePos(), &w);
        return;
    }
    openAddMenu(mCanvas->cursorScenePos(), nullptr);
}

void NodeEditor::openAddMenu(QPointF at, const WireDrag* wire)
{
    if (mGroup.isEmpty())
        return;
    std::string group = mGroup.toStdString();
    json query = {{"group", group}};
    std::optional<WireDrag> drag;
    if (wire)
    {
        drag = *wire;
        query["linkType"] = wire->type;
        query["linkInput"] = wire->input;
    }
    json types = runCmd("gnNodeTypes", query);
    if (!types.is_array())
        types = json::array();
    auto* pop = new AddNodePopup(this, types, [this, group, at, drag](const std::string& type, const std::string& socket) {
        // The new node sits beside the loose end, on the side its linked socket is on.
        QPointF pos(at.x() - kNodeWidth * 0.5, at.y() - 12.0);
        if (drag)
            pos = drag->input ? QPointF(at.x() - kNodeWidth - 10.0, at.y() - kHeader) : QPointF(at.x() + 10.0, at.y() - kHeader);
        UndoChunk chunk("Add Node");
        if (drag && drag->detached)
            runCmd("gnDisconnect", {{"args", {group, drag->detached->second}}, {"source", drag->detached->first}});
        json r = runCmd("gnAddNode", {{"args", {group, type}}, {"position", {pos.x(), pos.y()}}});
        if (!r.is_string())
            return;
        std::string name = r.get<std::string>();
        if (drag && !socket.empty())
        {
            std::string mine = name + "." + socket, theirs = drag->node + "." + drag->socket;
            runCmd("gnConnect", args({group, drag->input ? mine : theirs, drag->input ? theirs : mine}));
        }
        selectAfterRefresh({name});
    });
    // A cancelled pick shows the picked-up link again.
    if (drag)
        pop->onClosed = [this](bool) { scheduleRefresh(); };
    pop->popup(QCursor::pos());
}

namespace
{
/** {"args": [group, selected...]}, or null when nothing is selected. */
json selectionCall(const QString& group, const std::vector<std::string>& names)
{
    if (group.isEmpty() || names.empty())
        return nullptr;
    json a = json::array({group.toStdString()});
    for (const std::string& n : names)
        a.push_back(n);
    return {{"args", a}};
}
} // namespace

void NodeEditor::deleteSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (!call.is_null())
        runCmd("gnDeleteNode", call);
}

void NodeEditor::copySelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null())
        return;
    json text = runCmd("gnCopyNodes", call);
    if (text.is_string())
        QApplication::clipboard()->setText(qs(text.get<std::string>()));
}

void NodeEditor::cutSelected()
{
    UndoChunk chunk("Cut Nodes");
    copySelected();
    deleteSelected();
}

void NodeEditor::paste()
{
    if (mGroup.isEmpty())
        return;
    QString text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty())
    {
        showError("Nothing to paste: the clipboard has no text.");
        return;
    }
    QPointF at = mCanvas->cursorScenePos();
    try
    {
        json r = ops::execute("gnPasteNodes", {{"args", {mGroup.toStdString(), text.toStdString()}}, {"position", {at.x(), at.y()}}},
                               CallSource::UI);
        if (r.is_array())
            selectAfterRefresh(r.get<std::vector<std::string>>());
    }
    catch (const std::exception& e)
    {
        showError(qs(e.what()));
    }
}

void NodeEditor::duplicateSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null())
        return;
    call["offset"] = {40, 40};
    json r = runCmd("gnDuplicateNodes", call);
    if (r.is_array())
        selectAfterRefresh(r.get<std::vector<std::string>>());
}

void NodeEditor::selectAll() { mCanvas->selectAll(); }

void NodeEditor::groupSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null())
        return;
    call["name"] = "NodeGroup";
    try
    {
        json r = ops::execute("gnGroupNodes", call, CallSource::UI);
        if (r.is_object())
            selectAfterRefresh({r.value("node", "")});
    }
    catch (const std::exception& e)
    {
        showError(qs(e.what()));
    }
}

void NodeEditor::ungroupSelected()
{
    std::vector<std::string> names = mCanvas->selectedNames();
    UndoChunk chunk("Ungroup");
    std::vector<std::string> added;
    for (const std::string& n : names)
        if (mCanvas->typeOf(n) == gn::kGroupNodeDeclType)
        {
            try
            {
                json r = ops::execute("gnUngroup", args({mGroup.toStdString(), n}), CallSource::UI);
                if (r.is_array())
                    for (const json& x : r)
                        added.push_back(x.get<std::string>());
            }
            catch (const std::exception& e)
            {
                showError(qs(e.what()));
            }
        }
    if (!added.empty())
        selectAfterRefresh(added);
}

void NodeEditor::openSelectedGroup()
{
    for (const std::string& n : mCanvas->selectedNames())
        if (mCanvas->typeOf(n) == gn::kGroupNodeDeclType)
        {
            json info = runCmd("gnGroupInfo", args({mGroup.toStdString()}));
            for (const json& d : info.is_object() ? info["definition"].value("nodes", json::array()) : json::array())
                if (d.value("name", "") == n && d.contains("values"))
                {
                    std::string g = d["values"].value("group", "");
                    if (gn::findGroupNode(scene::Graph::current(), g))
                        setGroup(qs(g));
                    return;
                }
        }
}

void NodeEditor::exportGroup()
{
    if (mGroup.isEmpty())
        return;
    QString path = QFileDialog::getSaveFileName(this, "Export Node Group", mGroup + ".lilanodes", "Node groups (*.lilanodes)");
    if (path.isEmpty())
        return;
    json r = runCmd("gnExportGroup", args({mGroup.toStdString(), path.toStdString()}));
    if (r.is_string())
        log::info("exported {}", r.get<std::string>());
}

void NodeEditor::importGroups(const QStringList& files)
{
    UndoChunk chunk("Import Node Groups");
    QString last;
    for (const QString& f : files)
    {
        try
        {
            json names = ops::execute("gnImportGroup", args({f.toStdString()}), CallSource::UI);
            if (names.is_array() && !names.empty())
                last = qs(names.back().get<std::string>());
        }
        catch (const std::exception& e)
        {
            showError(qs(e.what()));
        }
    }
    if (!last.isEmpty())
        setGroup(last);
}

void NodeEditor::viewNode(const std::string& node)
{
    if (mGroup.isEmpty() || node.empty())
        return;
    std::string group = mGroup.toStdString();
    std::string type = mCanvas->typeOf(node);
    if (type == "Viewer" || type == "GroupOutput")
        return;
    std::string geometry, field, fieldType;
    for (auto& [id, t] : mCanvas->outputsOf(node))
    {
        if (t == "geometry" && geometry.empty())
            geometry = id;
        else if (t != "geometry" && t != "string" && field.empty())
        {
            field = id;
            fieldType = t;
        }
    }
    if (geometry.empty() && field.empty())
        return;
    UndoChunk chunk("View Node");
    std::string viewer;
    json info = runCmd("gnGroupInfo", args({group}));
    if (info.is_object())
        for (const json& n : info["definition"].value("nodes", json::array()))
            if (n.value("type", "") == "Viewer")
            {
                viewer = n.value("name", "");
                break;
            }
    if (viewer.empty())
    {
        QPointF at = mCanvas->positionOf(node) + QPointF(kNodeWidth + 60.0, 0.0);
        json r = runCmd("gnAddNode", {{"args", {group, "Viewer"}}, {"position", {at.x(), at.y()}}});
        if (!r.is_string())
            return;
        viewer = r.get<std::string>();
    }
    if (!field.empty())
    {
        std::string dt = fieldType == "rotation" ? "vector" : fieldType;
        runCmd("gnSetValue", args({group, viewer + ".dataType", dt}));
        runCmd("gnConnect", args({group, node + "." + field, viewer + ".value"}));
    }
    if (!geometry.empty())
        runCmd("gnConnect", args({group, node + "." + geometry, viewer + ".geometry"}));
    runCmd("gnSetActiveViewer", args({group, viewer}));
}

void NodeEditor::selectAfterRefresh(const std::vector<std::string>& names)
{
    mSelectNext = std::set<std::string>(names.begin(), names.end());
    mSelectedNode = names.size() == 1 ? qs(names.front()) : QString();
    scheduleRefresh();
}

void NodeEditor::showError(const QString& message)
{
    log::warn("{}", message.toStdString());
    QToolTip::showText(QCursor::pos(), message, mCanvas, QRect(), 8000);
}

void NodeEditor::scheduleRefresh(bool panel)
{
    mPanelDirty |= panel;
    if (!mRefresh->isActive())
        mRefresh->start();
}

void NodeEditor::followSelection()
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (gn::isGnModifier(*n))
        {
            if (scene::Node* grp = gn::groupOfModifier(g, *n))
                followModifier(*n, qs(grp->name()));
            return;
        }
        if (!n->type().isA("mesh"))
            continue;
        std::vector<scene::NodeId> stack = scene::modifiers::stack(g, id);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            if (scene::Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (scene::Node* grp = gn::groupOfModifier(g, *m))
                {
                    followModifier(*m, qs(grp->name()));
                    return;
                }
    }
}

void NodeEditor::followModifier(const scene::Node& modifier, const QString& group)
{
    QString path = qs(modifier.path());
    bool changed = path != mModifier;
    mModifier = path;
    if (group != mGroup)
        setGroup(group);
    else if (changed)
        scheduleRefresh();
}

nlohmann::json NodeEditor::shownValues() const
{
    json out = json::object();
    scene::Node* m = modifierInView();
    if (!m || mGroup.isEmpty())
        return out;
    json info = runCmd("gnGroupInfo", args({mGroup.toStdString()}));
    if (!info.is_object() || !info.contains("definition"))
        return out;
    scene::Graph& g = scene::Graph::current();
    for (const json& in : info["definition"].value("inputs", json::array()))
        if (std::string plug = m->path() + "." + in.value("name", ""); hasPlug(plug))
            out[plug] = scene::toJson(g.value(g.plug(plug)));
    return out;
}

scene::Node* NodeEditor::modifierInView() const
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* m = mModifier.isEmpty() ? nullptr : g.find(mModifier.toStdString());
    if (!m || !gn::isGnModifier(*m))
        return nullptr;
    scene::Node* grp = gn::groupOfModifier(g, *m);
    return grp && qs(grp->name()) == mGroup ? m : nullptr;
}

std::vector<std::string> NodeEditor::sendTargets() const
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* grp = mGroup.isEmpty() ? nullptr : gn::findGroupNode(g, mGroup.toStdString());
    if (!grp)
        return {};
    std::vector<scene::NodeId> meshes;
    for (scene::NodeId m : gn::instancesOf(g, grp->id()))
        if (scene::NodeId mesh = scene::modifiers::meshOf(g, m); g.node(mesh) && std::find(meshes.begin(), meshes.end(), mesh) == meshes.end())
            meshes.push_back(mesh);
    std::vector<std::string> out;
    for (scene::NodeId id : g.selection())
        if (std::find(meshes.begin(), meshes.end(), id) != meshes.end())
            out.push_back(g.node(id)->path());
    if (!out.empty())
        return out;
    if (scene::Node* m = modifierInView())
        if (scene::Node* mesh = g.node(scene::modifiers::meshOf(g, m->id())))
            return {mesh->path()};
    for (scene::NodeId id : meshes)
        out.push_back(g.node(id)->path());
    return out;
}

void NodeEditor::refreshSendButton()
{
    std::vector<std::string> targets = sendTargets();
    mSendBtn->setText(qs(animatedSend::lastLabel(targets)));
    mSendBtn->setEnabled(!targets.empty());
    mSendBtn->setToolTip(targets.empty() ? "Apply this group to a mesh to send its result"
                                         : "Send " + qs(targets.size() == 1 ? targets.front() : std::to_string(targets.size()) + " meshes") +
                                               " again the same way; the arrow picks Unreal or files and the kind");
}

void NodeEditor::refreshGroups()
{
    mRefreshing = true;
    std::vector<std::string> names = gn::groupNames(scene::Graph::current());
    mGroups->clear();
    for (const std::string& n : names)
        mGroups->addItem(qs(n));
    if (mGroup.isEmpty() && !names.empty())
        mGroup = qs(names.front());
    if (std::find(names.begin(), names.end(), mGroup.toStdString()) == names.end())
        mGroup = names.empty() ? QString() : qs(names.front());
    mGroups->setCurrentText(mGroup);
    mRefreshing = false;
}

void NodeEditor::refresh(bool panel)
{
    LILA_ZONE();

    refreshGroups();
    refreshSendButton();
    std::set<std::string> selected = std::move(mSelectNext);
    mSelectNext.clear();
    if (selected.empty() && !mSelectedNode.isEmpty())
        selected.insert(mSelectedNode.toStdString());
    json info = mGroup.isEmpty() ? json() : runCmd("gnGroupInfo", args({mGroup.toStdString()}));
    mCanvas->build(mGroup.toStdString(), info, selected);
    if (panel)
        rebuildSidePanel();
}

QWidget* NodeEditor::buildInterface(QWidget* panel, const json& info)
{
    std::string group = mGroup.toStdString();
    const json& def = info["definition"];
    json inputs = def.value("inputs", json::array());
    auto* box = new QWidget(panel);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    scene::Node* modifier = modifierInView();
    std::string modPath = modifier ? modifier->path() : std::string();
    mShownValues = shownValues();
    QLabel* title = heading(box, modifier ? "Parameters: " + qs(modPath) : "Parameter Defaults");
    title->setToolTip(modifier ? "The values this modifier uses. Middle click a field to restore the group's default."
                               : "Defaults for new modifiers using this group. Select a mesh with this group applied to edit its values.");
    v->addWidget(title);

    // Panels in first-use order, the unpanelled inputs first.
    std::vector<std::string> panels{""};
    for (const json& in : inputs)
        if (std::string pn = in.value("panel", ""); std::find(panels.begin(), panels.end(), pn) == panels.end())
            panels.push_back(pn);
    for (const std::string& pn : panels)
    {
        QFormLayout* form = nullptr;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const json& in = inputs[i];
            if (in.value("panel", "") != pn)
                continue;
            if (!form)
            {
                if (!pn.empty())
                {
                    auto* sub = new QLabel(qs(pn), box);
                    sub->setStyleSheet("color: rgb(190,190,190); padding-top: 4px; border-bottom: 1px solid rgb(70,70,70);");
                    v->addWidget(sub);
                }
                form = new QFormLayout();
                form->setContentsMargins(pn.empty() ? 0 : 8, 0, 0, 0);
                v->addLayout(form);
            }
            std::string name = in.value("name", ""), type = in.value("type", "float");
            QString shown = in.contains("label") ? qs(in["label"].get<std::string>()) : pretty(name);
            QString tip = qs(name) + ": " + qs(type) + (in.contains("subtype") ? " (" + qs(in["subtype"].get<std::string>()) + ")" : "");
            if (in.contains("description"))
                tip += "\n" + qs(in["description"].get<std::string>());
            auto* row = new QWidget(box);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QWidget* ed = nullptr;
            if (modifier && hasPlug(modPath + "." + name))
            {
                std::string plug = modPath + "." + name;
                scene::Graph& g = scene::Graph::current();
                ed = valueEditor(row, type, scene::toJson(g.value(g.plug(plug))), [plug](json val) {
                    ++gFieldEdits;
                    runCmd("setAttr", args({plug, val}));
                    --gFieldEdits;
                }, in.value("default", json()));
            }
            else
                ed = valueEditor(row, type, in.value("default", json()), [group, name](json val) {
                    setFromField(args({group, "GroupInput." + name, val}));
                });
            applyLimits(ed, in);
            ed->setToolTip(tip);
            h->addWidget(ed, 1);
            auto* more = new QToolButton(row);
            more->setIcon(icon("dots"));
            more->setToolTip("Rename, edit, move or remove this input");
            more->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(more);
            int index = int(i);
            int count = int(inputs.size());
            json current = in;
            bool labelled = in.contains("label");
            menu->addAction("Rename...", this, [this, group, name, shown, labelled] {
                bool ok = false;
                QString to = QInputDialog::getText(this, "Rename Input", "Name", QLineEdit::Normal, shown, &ok).trimmed();
                if (!ok || to.isEmpty() || to == shown)
                    return;
                std::string id = identifierFrom(to);
                if (labelled || to != pretty(id))
                    runCmd("gnSetInputInfo", {{"args", {group, name}}, {"label", to.toStdString()}});
                if (!id.empty() && id != name)
                    runCmd("gnRenameInput", args({group, name, id}));
            });
            menu->addAction("Edit Info...", this, [this, name, current] { editInputInfo(name, current); });
            QAction* up = menu->addAction("Move Up", this, [group, name, index] { runCmd("gnMoveInput", args({group, name, index - 1})); });
            up->setEnabled(index > 0);
            QAction* down = menu->addAction("Move Down", this, [group, name, index] { runCmd("gnMoveInput", args({group, name, index + 1})); });
            down->setEnabled(index + 1 < count);
            menu->addSeparator();
            menu->addAction("Remove", this, [group, name] { runCmd("gnRemoveInput", args({group, name})); });
            more->setMenu(menu);
            h->addWidget(more);
            auto* label = new QLabel(shown, box);
            label->setToolTip(tip);
            form->addRow(label, row);
        }
    }
    if (inputs.empty())
    {
        auto* none = new QLabel("Nothing exposed yet. Expose a node's input, or add one below.", box);
        none->setWordWrap(true);
        none->setStyleSheet("color: rgb(150,150,150);");
        v->addWidget(none);
    }

    auto* addRow = new QWidget(box);
    auto* ah = new QHBoxLayout(addRow);
    ah->setContentsMargins(0, 4, 0, 0);
    auto* addName = new QLineEdit(addRow);
    addName->setPlaceholderText("new input name");
    auto* addType = new QComboBox(addRow);
    for (const char* t : {"float", "int", "bool", "vector", "rotation", "color", "string"})
        addType->addItem(qs(t));
    auto* addBtn = new QToolButton(addRow);
    addBtn->setIcon(icon("plus"));
    addBtn->setToolTip("Add an exposed input; link it from the Group Input node");
    auto add = [group, addName, addType] {
        std::string n = addName->text().trimmed().toStdString();
        if (!n.empty())
            runCmd("gnAddInput", args({group, n, addType->currentText().toStdString()}));
    };
    QObject::connect(addBtn, &QToolButton::clicked, addBtn, add);
    QObject::connect(addName, &QLineEdit::returnPressed, addName, add);
    ah->addWidget(addName, 1);
    ah->addWidget(addType);
    ah->addWidget(addBtn);
    v->addWidget(addRow);

    json outputs = def.value("outputs", json::array());
    if (!outputs.empty())
    {
        v->addWidget(heading(box, "Outputs"));
        for (const json& o : outputs)
        {
            std::string name = o.value("name", "");
            auto* row = new QWidget(box);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* l = new QLabel(pretty(name) + "  (" + qs(o.value("type", "")) + ")", row);
            if (o.contains("description"))
                l->setToolTip(qs(o["description"].get<std::string>()));
            h->addWidget(l, 1);
            auto* rm = new QToolButton(row);
            rm->setIcon(icon("x"));
            rm->setToolTip("Remove this output");
            QObject::connect(rm, &QToolButton::clicked, rm, [group, name] { runCmd("gnRemoveOutput", args({group, name})); });
            h->addWidget(rm);
            v->addWidget(row);
        }
    }
    return box;
}

void NodeEditor::editInputInfo(const std::string& input, const json& current)
{
    QDialog dlg(this);
    dlg.setWindowTitle("Input " + qs(input));
    auto* form = new QFormLayout(&dlg);
    auto text = [&](const char* key) { return current.contains(key) ? qs(current[key].get<std::string>()) : QString(); };
    auto number = [&](const char* key) { return current.contains(key) ? QString::number(current[key].get<double>()) : QString(); };
    auto* label = new QLineEdit(text("label"), &dlg);
    label->setPlaceholderText(pretty(input));
    auto* desc = new QLineEdit(text("description"), &dlg);
    desc->setPlaceholderText("Tooltip shown on the modifier");
    auto* subtype = new QComboBox(&dlg);
    for (const char* st : {"", "factor", "percentage", "angle", "distance", "color"})
        subtype->addItem(*st ? pretty(st) : QString("None"), qs(st));
    subtype->setCurrentIndex(std::max(0, subtype->findData(text("subtype"))));
    std::array<std::pair<const char*, QLineEdit*>, 5> nums{};
    const char* numKeys[5] = {"min", "max", "softMin", "softMax", "step"};
    for (int k = 0; k < 5; ++k)
    {
        auto* e = new QLineEdit(number(numKeys[k]), &dlg);
        e->setPlaceholderText("none");
        nums[k] = {numKeys[k], e};
    }
    auto* panelEdit = new QLineEdit(text("panel"), &dlg);
    panelEdit->setPlaceholderText("none");
    form->addRow("Label", label);
    form->addRow("Description", desc);
    form->addRow("Subtype", subtype);
    form->addRow("Min", nums[0].second);
    form->addRow("Max", nums[1].second);
    form->addRow("Soft Min", nums[2].second);
    form->addRow("Soft Max", nums[3].second);
    form->addRow("Step", nums[4].second);
    form->addRow("Panel", panelEdit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    json call = {{"args", {mGroup.toStdString(), input}},
                 {"label", label->text().trimmed().toStdString()},
                 {"description", desc->text().trimmed().toStdString()},
                 {"subtype", subtype->currentData().toString().toStdString()},
                 {"panel", panelEdit->text().trimmed().toStdString()}};
    for (auto& [key, e] : nums)
    {
        bool ok = false;
        double d = e->text().trimmed().toDouble(&ok);
        call[key] = ok ? json(d) : json("");
    }
    runCmd("gnSetInputInfo", call);
}

void NodeEditor::rebuildSidePanel()
{
    auto* panel = new QWidget();
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(8, 4, 8, 8);
    lay->setSpacing(4);
    if (mGroup.isEmpty())
    {
        lay->addWidget(new QLabel("No group. Use New to make one.", panel));
        lay->addStretch(1);
        mSide->setWidget(panel);
        return;
    }
    std::string group = mGroup.toStdString();
    json info = runCmd("gnGroupInfo", args({group}));
    if (!info.is_object())
    {
        mSide->setWidget(panel);
        return;
    }

    lay->addWidget(buildInterface(panel, info));
    json mods = info.value("modifiers", json::array());
    QString used = mods.empty() ? "Not used by any mesh." : "Used by " + QString::number(mods.size()) + " modifier(s).";
    auto* usedLabel = new QLabel(used, panel);
    usedLabel->setStyleSheet("color: rgb(150,150,150);");
    lay->addWidget(usedLabel);
    if (info.contains("error"))
    {
        auto* err = new QLabel(qs(info["error"].get<std::string>()), panel);
        err->setWordWrap(true);
        err->setStyleSheet("color: rgb(235,235,235); font-style: italic;");
        lay->addWidget(err);
    }

    const json* node = nullptr;
    for (const json& n : info["nodes"])
        if (qs(n.value("name", "")) == mSelectedNode)
            node = &n;
    if (node)
    {
        std::string nodeName = node->value("name", "");
        lay->addWidget(heading(panel, qs(node->value("label", "")) + "  (" + qs(nodeName) + ")"));
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        for (const json& p : node->value("props", json::array()))
        {
            std::string id = p.value("id", "");
            std::string cur = p.value("value", "");
            json opts = p.value("options", json::array());
            if (node->value("type", "") == gn::kGroupNodeDeclType && id == "group")
            {
                auto* combo = new QComboBox(panel);
                for (const std::string& g : gn::groupNames(scene::Graph::current()))
                    if (g != group)
                        combo->addItem(qs(g));
                if (combo->findText(qs(cur)) < 0 && !cur.empty())
                    combo->addItem(qs(cur));
                combo->setCurrentIndex(cur.empty() ? -1 : combo->findText(qs(cur)));
                combo->setToolTip("The node group this node runs. Double-click the node to open it.");
                QObject::connect(combo, &QComboBox::activated, combo, [combo, group, nodeName](int) {
                    runCmd("gnSetValue", args({group, nodeName + ".group", combo->currentText().toStdString()}));
                });
                form->addRow("Group", combo);
                continue;
            }
            if (opts.empty())
            {
                auto* e = new QLineEdit(qs(cur), panel);
                QObject::connect(e, &QLineEdit::editingFinished, e, [e, group, nodeName, id] {
                    runCmd("gnSetValue", args({group, nodeName + "." + id, e->text().toStdString()}));
                });
                form->addRow(pretty(id), e);
                continue;
            }
            auto* combo = new QComboBox(panel);
            for (const json& o : opts)
                combo->addItem(pretty(o.get<std::string>()), qs(o.get<std::string>()));
            combo->setCurrentIndex(std::max(0, combo->findData(qs(cur))));
            QObject::connect(combo, &QComboBox::activated, combo, [combo, group, nodeName, id](int) {
                runCmd("gnSetValue", args({group, nodeName + "." + id, combo->currentData().toString().toStdString()}));
            });
            form->addRow(pretty(id), combo);
        }
        for (const json& s : node->value("inputs", json::array()))
        {
            std::string id = s.value("id", ""), type = s.value("type", "");
            if (type == "geometry")
                continue;
            if (s.value("linked", false))
            {
                auto* l = new QLabel("linked", panel);
                l->setStyleSheet("color: rgb(140,140,140);");
                form->addRow(pretty(id), l);
                continue;
            }
            auto* row = new QWidget(panel);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            json value = s.value("value", json());
            if (value.is_null() && s.contains("implicit"))
            {
                auto* l = new QLabel("implicit " + qs(s["implicit"].get<std::string>()), row);
                l->setStyleSheet("color: rgb(150,150,150);");
                h->addWidget(l, 1);
            }
            else
                h->addWidget(valueEditor(row, type, value, [group, nodeName, id](json v) { setFromField(args({group, nodeName + "." + id, v})); },
                                         s.value("default", json())),
                             1);
            auto* expose = new QToolButton(row);
            expose->setText("Expose");
            expose->setToolTip("Make this a group input: a keyable attr on the modifier");
            QObject::connect(expose, &QToolButton::clicked, expose, [group, nodeName, id] { runCmd("gnExposeInput", args({group, nodeName + "." + id})); });
            h->addWidget(expose);
            auto* label = new QLabel(pretty(id), panel);
            if (json def = s.value("default", json()); !def.is_null() && !value.is_null())
                onMiddleClickReset(label, [group, nodeName, id, def] { runCmd("gnSetValue", args({group, nodeName + "." + id, def})); },
                                   QString::fromStdString(valueReset::describe(def)), row);
            form->addRow(label, row);
        }
        lay->addLayout(form);
    }
    else
    {
        auto* hint = new QLabel("Select a node to edit its values.\n\nTab / Shift+A add a node under the cursor; while dragging a wire, or "
                                "letting one go on empty canvas, it lists only nodes that connect. Ctrl+drag cuts links, Delete / X "
                                "deletes, Ctrl+C / Ctrl+V copy and paste as text, Ctrl+D duplicates, F frames the selection, Home "
                                "frames all. Ctrl+G groups the selection, Ctrl+Alt+G ungroups, double-click a Group node to open its "
                                "group, Ctrl+Shift+click a node to preview it in a Viewer. Drop a .lilanodes file here to import it.",
                                panel);
        hint->setWordWrap(true);
        hint->setStyleSheet("color: rgb(150,150,150);");
        lay->addWidget(hint);
    }
    lay->addStretch(1);
    mSide->setWidget(panel);
}

namespace
{
void registerEditActions()
{
    auto add = [](const char* id, const char* label, void (NodeEditor::*fn)(), const char* doc) {
        actions::add({.id = id, .label = label, .category = "Node Editor",
                      .run = [fn] {
                          if (NodeEditor* e = NodeEditor::active())
                              (e->*fn)();
                      },
                      .doc = doc});
    };
    add("nodeEditor.addNode", "Add Node...", &NodeEditor::addNodeAtCursor,
        "Search for a node to add under the cursor. While dragging a wire it lists only nodes that connect to it.");
    add("nodeEditor.delete", "Delete Nodes", &NodeEditor::deleteSelected, "Delete the selected nodes and their links.");
    add("nodeEditor.copy", "Copy Nodes", &NodeEditor::copySelected,
        "Copy the selected nodes as text: paste them in another group or scene, or share them in a post.");
    add("nodeEditor.cut", "Cut Nodes", &NodeEditor::cutSelected, "Copy the selected nodes as text, then delete them.");
    add("nodeEditor.paste", "Paste Nodes", &NodeEditor::paste, "Paste nodes from clipboard text at the cursor.");
    add("nodeEditor.duplicate", "Duplicate Nodes", &NodeEditor::duplicateSelected, "Duplicate the selected nodes with the links between them.");
    add("nodeEditor.selectAll", "Select All Nodes", &NodeEditor::selectAll, "Select every node in the group.");
    add("nodeEditor.frameSelected", "Frame Selected Nodes", &NodeEditor::frameSelected, "Fit the view to the selected nodes.");
    add("nodeEditor.frameAll", "Frame All Nodes", &NodeEditor::frameAll, "Fit the view to the whole group.");
    add("nodeEditor.groupNodes", "Group Selected Nodes", &NodeEditor::groupSelected,
        "Collapse the selected nodes into a new node group, run by a Group node in their place.");
    add("nodeEditor.ungroup", "Ungroup", &NodeEditor::ungroupSelected, "Replace the selected Group nodes with the nodes of their groups.");
    add("nodeEditor.openGroup", "Open Group", &NodeEditor::openSelectedGroup, "Open the group the selected Group node runs.");
    add("nodeEditor.exportGroup", "Export Node Group...", &NodeEditor::exportGroup,
        "Write the group and the groups nested in it to a .lilanodes file to share.");
}
} // namespace

void registerNodeEditorActions()
{
    registerEditActions();
    registerGnViewerDrawer();
    actions::add({.id = "window.nodeEditor", .label = "Node Editor", .category = "Windows",
                  .run = [] {
                      for (NodeEditor* e : editors())
                          if (auto* d = qobject_cast<QDockWidget*>(e->parentWidget()); dockShowing(d))
                          {
                              toggleDock(d);
                              return;
                          }
                      if (showHook())
                          showHook()();
                  },
                  .doc = "Geometry-nodes editor.", .python = "lila.ui.run_action(\"window.nodeEditor\")"});
    actions::add({.id = "gn.newGroup", .label = "New Geometry Nodes Group", .category = "Geometry Nodes",
                  .run =
                      [] {
                          std::vector<std::string> names = gn::groupNames(scene::Graph::current());
                          std::string name = "GeometryNodes";
                          for (int i = 1; std::find(names.begin(), names.end(), name) != names.end() || !scene::Graph::current().findAll(name).empty(); ++i)
                              name = "GeometryNodes" + std::to_string(i);
                          json r = runCmd("gnCreateGroup", args({name}));
                          if (!r.is_string())
                              return;
                          if (showHook())
                              showHook()();
                          for (NodeEditor* e : editors())
                              e->setGroup(qs(r.get<std::string>()));
                      },
                  .doc = "Make a geometry-nodes group and open it in the node editor.",
                  .python = "lila.ops.gnCreateGroup(\"GeometryNodes\")"});
}

void setNodeEditorShowHook(std::function<void()> show) { showHook() = std::move(show); }
} // namespace lila::ui
