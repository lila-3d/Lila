#pragma once

// The node editor's canvas: graph items and the view that edits them. Internal to lila_ui.

#include "ui/NodeEditor.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "gn/Group.h"
#include "gn/Routing.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/AnimatedSend.h"
#include "ui/DockSidebar.h"
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
#include <QPointer>
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
#include <QtMath>

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
class NodeCanvas;

namespace canvas
{
inline QColor gray(int v, int alpha = 255) { return themeShade(v, alpha); }

constexpr double kNodeWidth = 170.0;
constexpr double kHeader = 24.0;
constexpr double kRow = 20.0;
constexpr double kSocketR = 5.0;

/** general prefs the canvas reads. */
inline constexpr const char* kPrefAutoOffset = "/nodeEditor/autoOffset";
inline constexpr const char* kPrefMinimap = "/nodeEditor/minimap";
/** "curved", "straight" or "rightAngle". */
inline constexpr const char* kPrefWireStyle = "/nodeEditor/wireStyle";
inline constexpr const char* kPrefDimUnrelated = "/nodeEditor/dimUnrelated";
/** In a grey theme, draw every wire one grey rather than in its socket type's shade. */
inline constexpr const char* kPrefGreyWires = "/nodeEditor/greyWiresInMonotone";

enum class WireStyle : uint8_t
{
    Curved,
    Straight,
    RightAngle,
};

struct CanvasPrefs
{
    bool autoOffset = true;
    bool minimap = false;
    WireStyle wires = WireStyle::Curved;
    bool dimUnrelated = false;
    bool greyWires = false;
};

inline CanvasPrefs readCanvasPrefs()
{
    PrefStore& s = PrefStore::get();
    CanvasPrefs p;
    p.autoOffset = s.value<bool>("general", kPrefAutoOffset, true);
    p.minimap = s.value<bool>("general", kPrefMinimap, false);
    std::string style = s.value<std::string>("general", kPrefWireStyle, "curved");
    p.wires = style == "straight" ? WireStyle::Straight : style == "rightAngle" ? WireStyle::RightAngle : WireStyle::Curved;
    p.dimUnrelated = s.value<bool>("general", kPrefDimUnrelated, false);
    p.greyWires = s.value<bool>("general", kPrefGreyWires, false);
    return p;
}

/** How every canvas draws its wires; set from the prefs. */
inline WireStyle gWireStyle = WireStyle::Curved;

inline QString qs(const std::string& s) { return QString::fromStdString(s); }

/** "sizeX" -> "Size X". */
inline QString pretty(const std::string& id)
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
inline std::string identifierFrom(const QString& text)
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

/** Runs a command from the UI; failures go to the log rather than a dialog. Queries run quietly, edits echo like any UI action. */
inline json runCmd(const std::string& name, const json& call, bool query = false)
{
    try
    {
        return ops::execute(name, call, query ? CallSource::Script : CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

inline json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

inline json runVerb(const GraphBackend& b, std::string_view verb, const json& call)
{
    json adapted = call;
    b.adaptCall(verb, adapted);
    return runCmd(b.command(verb), adapted, b.isQuery(verb));
}

inline bool hasPlug(const std::string& path)
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
inline int gFieldEdits = 0;

inline void setFromField(const GraphBackend& b, const json& call)
{
    ++gFieldEdits;
    runVerb(b, "SetValue", call);
    --gFieldEdits;
}

/** Fields are diamonds (like Blender); otherwise the backend's shape and mark. */
inline void drawSocket(QPainter& p, QPointF c, const GraphBackend::SocketStyle& style, bool field)
{
    double r = style.radius;
    p.setPen(QPen(gray(18), 1.2));
    p.setBrush(style.color);
    if (field)
    {
        QPolygonF d;
        d << c + QPointF(0, -r - 1) << c + QPointF(r + 1, 0) << c + QPointF(0, r + 1) << c + QPointF(-r - 1, 0);
        p.drawPolygon(d);
    }
    else if (style.shape == GraphBackend::SocketShape::Square)
        p.drawRect(QRectF(c.x() - r + 0.5, c.y() - r + 0.5, 2 * r - 1, 2 * r - 1));
    else if (style.shape == GraphBackend::SocketShape::Pill)
        p.drawRoundedRect(QRectF(c.x() - r - 2, c.y() - r + 1.5, 2 * r + 4, 2 * r - 3), 3, 3);
    else
        p.drawEllipse(c, r, r);
    if (style.mark == GraphBackend::SocketMark::Dot)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(30));
        p.drawEllipse(c, 1.8, 1.8);
    }
    else if (style.mark == GraphBackend::SocketMark::Hole)
    {
        p.setPen(Qt::NoPen);
        p.setBrush(gray(45));
        p.drawEllipse(c, r * 0.5, r * 0.5);
    }
    else if (style.mark == GraphBackend::SocketMark::Ring)
    {
        p.setPen(QPen(gray(45), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r * 0.55, r * 0.55);
    }
}

inline QString valueText(const json& v, const std::string& type)
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
    bool viewer = false;
    bool viewerActive = false;
    std::string groupRef; // the group a Group node runs
    int thumbSize = 0;
    QImage thumb;

    bool isViewer() const { return viewer; }
    /** A reroute: a dot centred on its position, its input and output both at the centre. */
    bool reroute = false;
    /** Send or Receive: the name it carries. */
    bool portal = false;
    std::string portalName;
    /** Shown as a badge on the header, such as a Receive naming no Send. */
    QString problem;
    QRectF badgeRect() const { return QRectF(kNodeWidth - kHeader + 2, 3, kHeader - 6, kHeader - 6); }
    /** Header label, with a portal's name. */
    QString title() const
    {
        if (!portal)
            return label;
        return label + QString(" %1 ").arg(QChar(0x00b7)) + (portalName.empty() ? QString("(unnamed)") : qs(portalName));
    }
    QString shownTitle() const { return muted ? title() + " (muted)" : title(); }
    static constexpr double kRerouteR = 6.0;
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

    QRectF boundingRect() const override
    {
        if (reroute)
            return QRectF(-kRerouteR - 4, -kRerouteR - 4, 2 * kRerouteR + 8, 2 * kRerouteR + 8);
        return QRectF(-2, -2, kNodeWidth + 4, height() + 4);
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;

    /** Drawn as its header only; with only linked sockets; bypassed (drawn dimmed). */
    bool collapsed = false;
    bool hideUnused = false;
    bool muted = false;
    /** The socket rows drawn: indices into outputs and inputs. */
    std::vector<int> shownOut, shownIn;

    /** Picks the rows to draw from the flags and what is linked; call after either changes. */
    void layoutRows()
    {
        prepareGeometryChange();
        shownOut.clear();
        shownIn.clear();
        if (collapsed)
            return;
        for (int i = 0; i < int(outputs.size()); ++i)
            if (!hideUnused || outputs[size_t(i)].linked)
                shownOut.push_back(i);
        for (int i = 0; i < int(inputs.size()); ++i)
            if (!hideUnused || inputs[size_t(i)].linked)
                shownIn.push_back(i);
    }

    /** Where the socket rows start: under the header and the thumbnail. */
    double top() const { return kHeader + (thumbSize > 0 ? thumbSize + 8.0 : 0.0); }
    double height() const
    {
        if (collapsed)
            return kHeader;
        return top() + kRow * double(shownOut.size() + props.size() + shownIn.size()) + 6.0;
    }
    /** Set while the node hides in a collapsed frame: where its wires end instead, on the frame's title. */
    std::optional<QPointF> inProxy, outProxy;

    /** Socket centre in scene coordinates; a socket not drawn sits on the header's edge. */
    QPointF socketPos(bool input, int index) const
    {
        if (input ? inProxy.has_value() : outProxy.has_value())
            return input ? *inProxy : *outProxy;
        if (reroute)
            return mapToScene(QPointF(0.0, 0.0));
        const std::vector<int>& shown = input ? shownIn : shownOut;
        auto it = std::find(shown.begin(), shown.end(), index);
        double x = input ? 0.0 : kNodeWidth;
        if (it == shown.end())
            return mapToScene(QPointF(x, kHeader * 0.5));
        double k = double(it - shown.begin());
        double row = input ? double(shownOut.size() + props.size()) + k : k;
        return mapToScene(QPointF(x, top() + kRow * row + kRow * 0.5));
    }
    /** Socket under a scene point, -1 for none. */
    int socketAt(QPointF scenePos, bool& input) const
    {
        // A reroute's middle drags it; its rim starts a new wire from its output.
        if (reroute)
        {
            double d = QLineF(mapToScene(QPointF(0.0, 0.0)), scenePos).length();
            input = false;
            return d > kRerouteR - 1.0 && d <= kRerouteR + 5.0 && !outputs.empty() ? 0 : -1;
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            bool in = pass == 1;
            for (int i : in ? shownIn : shownOut)
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

/** A link by its ends: fromNode, fromSocket, toNode, toSocket. */
using LinkKey = std::array<std::string, 4>;

constexpr double kTieSpacing = 7.0; // between wires in a tie's ribbon
constexpr double kTieRun = 44.0;    // length of the parallel run through a tie
constexpr double kTieBand = 10.0;   // the band's width along the ribbon

/** The ribbon direction of a tie at angle degrees (0 left to right, y down). */
inline QPointF tieDirection(double angle)
{
    double r = qDegreesToRadians(angle);
    return {std::cos(r), std::sin(r)};
}

inline double dot(QPointF a, QPointF b) { return a.x() * b.x() + a.y() * b.y(); }

/** A wire from p leaving along tp to q arriving along tq, in the canvas's wire style. */
inline void bend(QPainterPath& path, QPointF p, QPointF tp, QPointF q, QPointF tq)
{
    if (gWireStyle == WireStyle::Straight)
    {
        path.lineTo(q);
        return;
    }
    if (gWireStyle == WireStyle::RightAngle)
    {
        // Out along the tangent, across, then in: a short stub each end keeps the turns clear of the sockets.
        QPointF out = p + tp * 16.0, in = q - tq * 16.0;
        path.lineTo(out);
        if (in.x() >= out.x())
        {
            double mid = (out.x() + in.x()) * 0.5;
            path.lineTo(mid, out.y());
            path.lineTo(mid, in.y());
        }
        else
        {
            double mid = (out.y() + in.y()) * 0.5;
            path.lineTo(out.x(), mid);
            path.lineTo(in.x(), mid);
        }
        path.lineTo(in);
        path.lineTo(q);
        return;
    }
    double k = std::max(24.0, QLineF(p, q).length() * 0.4);
    path.cubicTo(p + tp * k, q - tq * k, q);
}

class LinkItem final : public QGraphicsPathItem
{
public:
    LinkItem(NodeItem* from, int out, NodeItem* to, int in, QColor color = gray(165), double width = 2.0)
        : from(from), out(out), to(to), in(in), base(color, width)
    {
        setZValue(-1);
        if (from->outputs[out].field)
            base.setStyle(Qt::DashLine);
        setPen(base);
        update();
    }

    /** Folded: an end hides in a collapsed frame with the other. Filtered: the selection-only view leaves it out. */
    bool folded = false;
    bool filtered = false;
    void applyVisibility() { setVisible(!folded && !filtered); }

    LinkKey key() const { return {from->name, from->outputs[out].id, to->name, to->inputs[in].id}; }

    /** Drawn as selected: it runs through a selected reroute. */
    void setHighlight(bool on)
    {
        if (on == highlight)
            return;
        highlight = on;
        QPen p = base;
        if (on)
        {
            p.setColor(themeColor("nodeSelected"));
            p.setWidthF(base.widthF() + 1.0);
        }
        setPen(p);
        setZValue(on ? -0.8 : -1.0);
    }
    bool highlight = false;

    void update()
    {
        QPointF a = from->socketPos(false, out), b = to->socketPos(true, in);
        QPainterPath path(a);
        if (passes.empty() && gWireStyle == WireStyle::Curved)
        {
            double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
            path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
            setPath(path);
            return;
        }
        QPointF at = a, along(1.0, 0.0);
        for (auto& [entry, exit] : passes)
        {
            QPointF d = exit - entry;
            d /= std::max(1e-9, std::hypot(d.x(), d.y()));
            bend(path, at, along, entry, d);
            path.lineTo(exit);
            at = exit;
            along = d;
        }
        bend(path, at, along, b, QPointF(1.0, 0.0));
        setPath(path);
    }

    NodeItem* from;
    int out;
    NodeItem* to;
    int in;
    /** Where the wire runs through each of its ties, in order from its source: (entry, exit). */
    std::vector<std::pair<QPointF, QPointF>> passes;
    /** Its pen when not highlighted. */
    QPen base;
};

/** A frame's box behind the nodes it holds: a title band with its label and collapse arrow, and a resize grip. */
class FrameItem final : public QGraphicsItem
{
public:
    explicit FrameItem(int index) : index(index) { setZValue(-2.0); }

    static constexpr double kTitle = 22.0;
    int index;
    QString label;
    QSizeF size{300.0, 200.0};
    QColor color;
    bool collapsed = false;

    void setBox(QSizeF s, bool fold)
    {
        prepareGeometryChange();
        size = s;
        collapsed = fold;
    }
    QRectF box() const { return QRectF(0.0, 0.0, size.width(), collapsed ? kTitle : size.height()); }
    QRectF titleRect() const { return QRectF(0.0, 0.0, size.width(), kTitle); }
    QRectF arrowRect() const { return QRectF(3.0, 3.0, kTitle - 6.0, kTitle - 6.0); }
    QRectF gripRect() const { return collapsed ? QRectF() : QRectF(size.width() - 14.0, size.height() - 14.0, 14.0, 14.0); }
    QRectF boundingRect() const override { return box().adjusted(-2, -2, 2, 2); }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        QColor body = color, title = color;
        body.setAlpha(collapsed ? 200 : 55);
        title.setAlpha(170);
        p->setPen(QPen(color.darker(160), 1.0));
        p->setBrush(body);
        p->drawRoundedRect(box(), 5, 5);
        QPainterPath head;
        head.addRoundedRect(titleRect(), 5, 5);
        if (!collapsed)
            head.addRect(QRectF(0.0, kTitle - 5.0, size.width(), 5.0));
        p->setPen(Qt::NoPen);
        p->setBrush(title);
        p->drawPath(head.simplified());
        QColor ink = title.lightness() > 140 ? gray(25) : gray(235);
        p->setPen(ink);
        p->setBrush(ink);
        QRectF a = arrowRect().adjusted(4, 4, -4, -4);
        QPolygonF tri;
        if (collapsed)
            tri << a.topLeft() << QPointF(a.right(), a.center().y()) << a.bottomLeft();
        else
            tri << a.topLeft() << a.topRight() << QPointF(a.center().x(), a.bottom());
        p->drawPolygon(tri);
        QFont f = p->font();
        f.setBold(true);
        p->setFont(f);
        p->drawText(titleRect().adjusted(kTitle, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(label, Qt::ElideRight, int(size.width() - kTitle - 6)));
        if (!collapsed)
        {
            QRectF g = gripRect();
            p->setPen(QPen(ink, 1.0));
            for (double k = 4.0; k < 13.0; k += 4.0)
                p->drawLine(QPointF(g.right() - k, g.bottom() - 2), QPointF(g.right() - 2, g.bottom() - k));
        }
    }
};

/** A zip tie as the canvas holds it while editing. */
struct TieView
{
    std::vector<LinkKey> links;
    QPointF pos;
    double angle = 0.0;
    std::string color;
};

inline std::vector<TieView> tiesFrom(const json& def)
{
    std::vector<TieView> out;
    for (const gn::GTie& t : gn::tiesFromJson(def.value("ties", json::array())))
    {
        TieView v{{}, QPointF(t.position.x, t.position.y), t.angle, t.color};
        for (const gn::GLink& l : t.links)
            v.links.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
        out.push_back(std::move(v));
    }
    return out;
}

inline json tiesJson(const std::vector<TieView>& ties)
{
    std::vector<gn::GTie> out;
    for (const TieView& v : ties)
    {
        gn::GTie t;
        for (const LinkKey& k : v.links)
            t.links.push_back({k[0], k[1], k[2], k[3]});
        t.position = {v.pos.x(), v.pos.y()};
        t.angle = v.angle;
        t.color = v.color;
        out.push_back(std::move(t));
    }
    return gn::tiesToJson(out);
}

/** Whether the theme colours its sockets; a grey theme (Monotone) gets grey tie choices instead of hues. */
inline bool chromaticTheme() { return themeColor("socketGeometry").hsvSaturationF() > 0.15f; }

struct TieSwatch
{
    const char* id;
    const char* label;
    const char* hex; // empty for a grey level
    int level = 0;
};

constexpr TieSwatch kTieHues[] = {
    {"red", "Red", "#e5534b"},    {"orange", "Orange", "#f0a33a"}, {"yellow", "Yellow", "#e6d14a"}, {"green", "Green", "#6cc24a"},
    {"teal", "Teal", "#2fb8a6"},  {"blue", "Blue", "#4a8fe5"},     {"purple", "Purple", "#a663c7"}, {"pink", "Pink", "#e56bb0"},
};
constexpr TieSwatch kTieGreys[] = {{"light", "Light", "", 215}, {"dark", "Dark", "", 85}};

/** A palette colour by name, for ties and frames; empty is the theme's grey at level. Hues stay hues only in a chromatic theme. */
inline QColor paletteColor(const std::string& id, int level)
{
    if (id.empty())
        return gray(level);
    for (const TieSwatch& s : kTieGreys)
        if (id == s.id)
            return gray(s.level);
    for (const TieSwatch& s : kTieHues)
        if (id == s.id)
        {
            QColor c(s.hex);
            return chromaticTheme() ? c : gray(qGray(c.rgb()));
        }
    return gray(level);
}

inline QColor tieColor(const std::string& id) { return paletteColor(id, 150); }

/** Default, the greys and, in a chromatic theme, the hues, each with a swatch. */
inline void fillPaletteMenu(QMenu* menu, std::function<void(std::string)> pick)
{
    auto swatch = [](const QColor& c) {
        QPixmap px(14, 14);
        px.fill(c);
        return QIcon(px);
    };
    menu->addAction(swatch(tieColor({})), "Default", menu, [pick] { pick({}); });
    for (const TieSwatch& s : kTieGreys)
        menu->addAction(swatch(tieColor(s.id)), s.label, menu, [pick, id = std::string(s.id)] { pick(id); });
    if (!chromaticTheme())
        return;
    menu->addSeparator();
    for (const TieSwatch& s : kTieHues)
        menu->addAction(swatch(tieColor(s.id)), s.label, menu, [pick, id = std::string(s.id)] { pick(id); });
}

/** The band a tie draws across its ribbon, centred on its position and turned to its angle. */
class TieItem final : public QGraphicsItem
{
public:
    explicit TieItem(int index) : index(index) { setZValue(-0.5); }

    int index;
    int count = 2;
    QColor color;
    bool hot = false;

    void setCount(int n)
    {
        prepareGeometryChange();
        count = n;
    }

    QRectF band() const
    {
        double h = double(std::max(count, 2) - 1) * kTieSpacing + 12.0;
        return QRectF(-kTieBand * 0.5, -h * 0.5, kTieBand, h);
    }
    QRectF boundingRect() const override { return band().adjusted(-4, -8, 4, 4); }
    QPainterPath shape() const override
    {
        QPainterPath p;
        p.addRect(band().adjusted(-3, -6, 3, 3));
        return p;
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        QRectF r = band();
        p->setPen(QPen(hot ? gray(235) : gray(18), hot ? 1.6 : 1.0));
        p->setBrush(color);
        p->drawRoundedRect(r, 3, 3);
        // The tie's lock block at one end.
        p->drawRoundedRect(QRectF(-kTieBand * 0.5 - 1.0, r.top() - 5.0, kTieBand + 2.0, 6.0), 1.5, 1.5);
        p->setPen(QPen(color.lightness() > 128 ? color.darker(150) : color.lighter(160), 0.8));
        for (double y = r.top() + 4.0; y < r.bottom() - 2.0; y += 3.0)
            p->drawLine(QPointF(r.left() + 2.0, y), QPointF(r.right() - 2.0, y));
    }
};

} // namespace canvas

using namespace canvas;


/** A wire being dragged from a socket, handed to the add-node popup so the new node connects to it. */
struct WireDrag
{
    std::string node;
    std::string socket;
    std::string type;
    bool input = false; // the loose end hangs off an input
    std::optional<std::pair<std::string, std::string>> detached; // (source, input) of a link picked up
};

/** The graph view. Builds items from the backend's GroupInfo and turns gestures into its commands. */
class NodeCanvas final : public QGraphicsView
{
public:
    /** Set by the editor before anything is built; never null after. */
    const GraphBackend* backend = nullptr;

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
        gWireStyle = mPrefs.wires;
        connect(&mScene, &QGraphicsScene::selectionChanged, this, [this] {
            // The editor clears onSelect as it goes; the scene then deletes items the lists still hold.
            if (!onSelect)
                return;
            updateEmphasis();
            if (mBuilding)
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
    /** Group files (the backend's suffixes) dropped on the canvas. */
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
        mTieItems.clear();
        mTies.clear();
        mInsertLink = nullptr;
        mFrameItems.clear();
        mFrames.clear();
        mFrameDrag = -1;
        mLazyFrom = nullptr;
        mHoverNode = nullptr;
        mTieDrag = -1;
        mTieStroke = false;
        mHotTie = -1;
        mScene.clear();
        mTemp = nullptr;
        if (info.is_object())
        {
            std::map<std::string, bool> active;
            std::string viewerType = backend->viewerType();
            for (const json& d : info["definition"].value("nodes", json::array()))
                if (!viewerType.empty() && d.value("type", "") == viewerType)
                    active[d.value("name", "")] = d.contains("values") && d["values"].value("active", false);
            std::map<std::string, std::string> portalNames;
            std::map<std::string, gn::GNode> flags;
            for (const json& d : info["definition"].value("nodes", json::array()))
            {
                if (gn::isRoutingType(d.value("type", "")) && d.contains("values"))
                    portalNames[d.value("name", "")] = d["values"].value("name", "");
                gn::getNodeFlags(flags[d.value("name", "")], d);
            }
            for (const json& n : info.value("nodes", json::array()))
            {
                auto* item = new NodeItem(this, n);
                item->portalName = portalNames[item->name];
                const gn::GNode& f = flags[item->name];
                item->collapsed = f.collapsed;
                item->hideUnused = f.hideUnused;
                item->muted = f.muted;
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
                f->second->outputs[size_t(out)].linked = true;
                const std::string& type = f->second->outputs[size_t(out)].type;
                auto* link = new LinkItem(f->second, out, t->second, in, wireColor(type), backend->wireWidth(type));
                mScene.addItem(link);
                mLinks.push_back(link);
            }
            for (auto& [name, item] : mNodes)
                item->layoutRows();
            for (LinkItem* l : mLinks)
                l->update();
            mTies = tiesFrom(info["definition"]);
            for (int i = 0; i < int(mTies.size()); ++i)
            {
                auto* item = new TieItem(i);
                mScene.addItem(item);
                mTieItems.push_back(item);
            }
            mFrames = gn::framesFromJson(info["definition"].value("frames", json::array()));
            for (int i = 0; i < int(mFrames.size()); ++i)
            {
                auto* item = new FrameItem(i);
                mScene.addItem(item);
                mFrameItems.push_back(item);
            }
            layoutFrames();
            layoutTies();
            updateEmphasis();
        }
        mInfo = info;
        refreshThumbs();
        mBuilding = false;
        if (newGroup)
            QTimer::singleShot(0, this, [this] { frame(false); });
    }

    /** Asks the backend for every node's thumbnail again; it calls back as newer ones come in. */
    void refreshThumbs()
    {
        QPointer<NodeCanvas> self(this);
        for (auto& [name, item] : mNodes)
        {
            if (item->thumbSize <= 0)
                continue;
            item->thumb = backend->thumbnail(mGroup, mInfo, name, [self, group = mGroup] {
                if (self && self->mGroup == group && !self->mThumbsQueued)
                {
                    self->mThumbsQueued = true;
                    QTimer::singleShot(30, self, [self] {
                        self->mThumbsQueued = false;
                        self->refreshThumbs();
                    });
                }
            });
            item->update();
        }
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
        if (!mTies.empty())
        {
            layoutTies();
            return;
        }
        for (LinkItem* l : mLinks)
            if (l->from == n || l->to == n)
                l->update();
    }

    /** Held zip-tie key: a drag then gathers the wires it crosses into a tie (or into the tie it starts on). */
    void setZipKey(bool down)
    {
        if (down == mZipKey)
            return;
        mZipKey = down;
        if (down)
            viewport()->setCursor(Qt::CrossCursor);
        else if (!mTieStroke)
            viewport()->unsetCursor();
    }

    /**
     * From the selected portal node, selects and centres its Send (source) or every Receive of its name (readers). False
     * when there is none.
     */
    bool jumpToPortal(bool source)
    {
        NodeItem* at = nullptr;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i); n && n->portal)
                at = n;
        if (!at || at->portalName.empty())
            return false;
        std::vector<NodeItem*> hits;
        for (auto& [name, item] : mNodes)
            if (item->portal && item->portalName == at->portalName && (item->type == gn::kSendType) == source)
                hits.push_back(item);
        if (hits.empty())
            return false;
        mScene.clearSelection();
        QRectF r;
        for (NodeItem* n : hits)
        {
            n->setSelected(true);
            r = r.united(n->sceneBoundingRect());
        }
        centerOn(r.center());
        return true;
    }

    /** The selection's upstream and downstream drawn lit, the rest dimmed: as a toggle, or while a key is held. */
    void setFlowHighlight(bool on, bool hold)
    {
        (hold ? mFlowHold : mFlowToggle) = on;
        updateEmphasis();
    }
    bool flowHighlight() const { return mFlowToggle; }

    /** Shows only the wires on the selected nodes, hiding the rest, until toggled off. */
    void setSelectedWiresOnly(bool on)
    {
        mSelectedWiresOnly = on;
        updateEmphasis();
    }
    bool selectedWiresOnly() const { return mSelectedWiresOnly; }

    enum class Align : uint8_t
    {
        Left,
        Center,
        Right,
        Top,
        Middle,
        Bottom,
    };
    std::vector<NodeItem*> selectedNodes() const;
    /** Lines the selected nodes' edges or centres up (NodeCanvasLayout.cpp). */
    void align(Align a);
    /** Spaces three or more selected nodes evenly between the outermost two. */
    void distribute(bool horizontal);
    /** Moves selected nodes up or down so the wire into each runs level, upstream first; a lone node lines up with its source. */
    void straighten();
    /**
     * Lays the selection (or, with fewer than two selected, the whole graph) out in columns from left to right by data
     * flow, ordered to keep wires short and uncrossed; frames refit around their members.
     */
    void autoLayout();

    /** RMB on the canvas, outside ties and frames: the editor's menu of node actions. */
    std::function<void(QPoint)> onContextMenu;

    /** A frame around the selected nodes, or an empty one at the cursor when nothing is selected. */
    void frameSelection()
    {
        if (!backend->supports("SetFrames") || mGroup.empty())
            return;
        gn::GFrame f;
        f.label = "Frame";
        QRectF r;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i))
            {
                r = r.united(n->sceneBoundingRect());
                f.nodes.push_back(n->name);
            }
        if (r.isNull())
            r = QRectF(cursorScenePos() - QPointF(150, 100), QSizeF(300, 200));
        else
            r.adjust(-30, -30 - FrameItem::kTitle, 30, 30);
        f.position = {r.x(), r.y()};
        f.size = {r.width(), r.height()};
        std::vector<gn::GFrame> next = mFrames;
        // Nodes going into the new frame leave the ones they were in.
        for (gn::GFrame& other : next)
            std::erase_if(other.nodes, [&](const std::string& m) { return std::find(f.nodes.begin(), f.nodes.end(), m) != f.nodes.end(); });
        next.push_back(std::move(f));
        commitFrames(next);
    }

    const std::vector<gn::GFrame>& frames() const { return mFrames; }

    /**
     * Flips a node flag ("muted", "collapsed", "hideUnused") on the selection: on for all when any lacks it, else off.
     */
    void toggleFlag(const char* flag)
    {
        std::vector<NodeItem*> nodes = selectedNodes();
        std::erase_if(nodes, [&](NodeItem* n) {
            return n->reroute || n->portal || (std::string_view(flag) == "muted" && (n->type == "GroupInput" || n->type == "GroupOutput" ||
                                                                                      n->type == "materialOutput"));
        });
        if (nodes.empty() || !backend->supports("SetNodeFlags"))
            return;
        auto get = [flag](NodeItem* n) { return std::string_view(flag) == "muted" ? n->muted : std::string_view(flag) == "collapsed" ? n->collapsed : n->hideUnused; };
        bool on = std::any_of(nodes.begin(), nodes.end(), [&](NodeItem* n) { return !get(n); });
        json a = json::array({mGroup});
        for (NodeItem* n : nodes)
            a.push_back(n->name);
        runVerb(*backend, "SetNodeFlags", {{"args", a}, {flag, on}});
    }

    /** The nodes as (name, what shows on the header), for the find popup. */
    std::vector<std::pair<std::string, QString>> nodeList() const
    {
        std::vector<std::pair<std::string, QString>> out;
        for (auto& [name, n] : mNodes)
            out.push_back({name, n->title()});
        return out;
    }

    /** Selects a node and centres the view on it. */
    void focusNode(const std::string& name)
    {
        auto it = mNodes.find(name);
        if (it == mNodes.end())
            return;
        mScene.clearSelection();
        it->second->setSelected(true);
        centerOn(it->second->sceneBoundingRect().center());
    }

    /** Saves the view (centre and zoom) under a slot in the graph, or brings a saved one back. False when the slot is empty. */
    void storeBookmark(int slot)
    {
        if (mGroup.empty() || !backend->supports("SetBookmarks"))
            return;
        json marks = mInfo.is_object() ? mInfo["definition"].value("bookmarks", json::object()) : json::object();
        QPointF c = mapToScene(viewport()->rect().center());
        marks[std::to_string(slot)] = {c.x(), c.y(), transform().m11()};
        runVerb(*backend, "SetBookmarks", {{"args", {mGroup, marks}}});
    }
    bool recallBookmark(int slot)
    {
        json marks = mInfo.is_object() ? mInfo["definition"].value("bookmarks", json::object()) : json::object();
        auto it = marks.find(std::to_string(slot));
        if (it == marks.end())
            return false;
        double zoom = std::clamp((*it)[2].get<double>(), 0.1, 4.0);
        resetTransform();
        scale(zoom, zoom);
        centerOn((*it)[0].get<double>(), (*it)[1].get<double>());
        return true;
    }

    void setMinimap(bool on)
    {
        PrefStore::get().set("general", kPrefMinimap, on);
        mPrefs.minimap = on;
        viewport()->update();
    }
    bool minimap() const { return mPrefs.minimap; }

    /**
     * Merges the selected nodes' outputs through the backend's merge node for their type: one multi-input node, or a
     * chain of two-input ones. Returns an error to show, empty when it worked.
     */
    QString mergeSelected(const std::string& op)
    {
        std::vector<NodeItem*> nodes = selectedNodes();
        std::erase_if(nodes, [](NodeItem* n) { return n->outputs.empty(); });
        if (nodes.size() < 2)
            return "Select two or more nodes with outputs to merge.";
        std::sort(nodes.begin(), nodes.end(), [](NodeItem* a, NodeItem* b) { return a->pos().y() < b->pos().y(); });
        std::string type = nodes.front()->outputs.front().type;
        std::vector<std::string> sources;
        for (NodeItem* n : nodes)
        {
            int pick = -1;
            for (int pass = 0; pass < 2 && pick < 0; ++pass)
                for (int o = 0; o < int(n->outputs.size()) && pick < 0; ++o)
                    if (pass == 0 ? n->outputs[size_t(o)].type == type : backend->canLink(n->outputs[size_t(o)].type, type))
                        pick = o;
            if (pick < 0)
                return qs(n->name + " has no output that merges with " + type);
            sources.push_back(n->name + "." + n->outputs[size_t(pick)].id);
        }
        std::optional<GraphBackend::MergeNode> plan = backend->mergeNode(type, op);
        if (!plan)
            return qs("Nothing here merges " + type + " values by " + op);
        double right = -1e18, y = 0.0;
        for (NodeItem* n : nodes)
        {
            right = std::max(right, n->sceneBoundingRect().right());
            y += n->pos().y();
        }
        y /= double(nodes.size());
        QPointF at(right + 80.0, y);
        UndoChunk chunk("Merge Nodes");
        auto addNode = [&](QPointF p) -> std::string {
            json r = runVerb(*backend, "AddNode", {{"args", {mGroup, plan->type}}, {"position", {p.x(), p.y()}}});
            if (!r.is_string())
                return {};
            std::string name = r.get<std::string>();
            for (auto& [k, v] : plan->values.items())
                runVerb(*backend, "SetValue", args({mGroup, name + "." + k, v}));
            return name;
        };
        if (plan->multi)
        {
            std::string m = addNode(at);
            for (const std::string& s : sources)
                runVerb(*backend, "Connect", args({mGroup, s, m + "." + plan->inA}));
            return {};
        }
        std::string prev = sources[0];
        for (size_t i = 1; i < sources.size(); ++i)
        {
            std::string m = addNode(at + QPointF(220.0 * double(i - 1), 0.0));
            if (m.empty())
                return "Couldn't add " + qs(plan->type);
            runVerb(*backend, "Connect", args({mGroup, prev, m + "." + plan->inA}));
            runVerb(*backend, "Connect", args({mGroup, sources[i], m + "." + plan->inB}));
            prev = m + "." + plan->out;
        }
        return {};
    }

    /** Re-reads the canvas prefs and redraws with them. */
    void reloadPrefs()
    {
        mPrefs = readCanvasPrefs();
        gWireStyle = mPrefs.wires;
        for (LinkItem* l : mLinks)
        {
            l->base.setColor(wireColor(l->from->outputs[size_t(l->out)].type));
            l->setPen(l->base);
            l->highlight = false;
            l->update();
        }
        layoutTies();
        updateEmphasis();
        viewport()->update();
    }

    /** The ties as the canvas shows them, for tests. */
    const std::vector<TieView>& ties() const { return mTies; }

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
    void drawForeground(QPainter* p, const QRectF&) override
    {
        drawGhostLines(p);
        if (mPrefs.minimap)
        {
            p->save();
            p->resetTransform();
            drawMinimap(p);
            p->restore();
        }
    }

    void drawGhostLines(QPainter* p)
    {
        if (mHoverPortal.empty())
            return;
        // Ghost lines from the hovered portal's Send to every Receive of its name.
        std::vector<NodeItem*> sends, receives;
        for (auto& [name, item] : mNodes)
            if (item->portal && item->portalName == mHoverPortal)
                (item->type == gn::kSendType ? sends : receives).push_back(item);
        QPen pen(themeColor("nodeSelected"), 1.5 / std::max(0.05, transform().m11()), Qt::DashLine);
        p->setPen(pen);
        p->setBrush(Qt::NoBrush);
        for (NodeItem* s : sends)
            for (NodeItem* r : receives)
            {
                QPointF a = s->mapToScene(QPointF(kNodeWidth, kHeader * 0.5)), b = r->mapToScene(QPointF(0.0, kHeader * 0.5));
                double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
                QPainterPath path(a);
                path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
                p->drawPath(path);
            }
    }

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
        if (e->button() == Qt::LeftButton && mPrefs.minimap && minimapRect().contains(e->pos()))
        {
            mMinimapDrag = true;
            centerOn(fromMinimap(e->pos()));
            return;
        }
        if (e->button() == Qt::LeftButton && mZipKey && backend->supports("SetTies"))
        {
            mTieStroke = true;
            mTieStart = sp;
            mTieTarget = tieAt(sp);
            mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
            return;
        }
        if (e->button() == Qt::RightButton && (e->modifiers() & Qt::AltModifier))
            if (NodeItem* n = nodeAt(sp))
            {
                mLazyFrom = n;
                mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
                return;
            }
        if (e->button() == Qt::RightButton && !nodeAt(sp))
        {
            if (int t = tieAt(sp); t >= 0)
            {
                tieMenu(t, e->globalPosition().toPoint());
                return;
            }
            if (int f = frameAt(sp, true); f >= 0)
            {
                frameMenu(f, e->globalPosition().toPoint());
                return;
            }
        }
        if (e->button() == Qt::RightButton && onContextMenu)
        {
            if (NodeItem* n = nodeAt(sp); n && !n->isSelected())
            {
                mScene.clearSelection();
                n->setSelected(true);
            }
            onContextMenu(e->globalPosition().toPoint());
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
                    runVerb(*backend, "SetActiveViewer", args({mGroup}));
                else
                    runVerb(*backend, "SetActiveViewer", args({mGroup, n->name}));
                return;
            }
        // Ctrl on a linked input picks its link up rather than starting a cut.
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
            if (NodeItem* n = nodeAt(sp))
            {
                bool input = false;
                int s = n->socketAt(sp, input);
                if (s >= 0 && input && n->inputs[size_t(s)].linked)
                {
                    startWire(n, s, true);
                    return;
                }
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
                    startWire(n, s, input, e->modifiers() & Qt::ShiftModifier);
                    return;
                }
            }
        if (e->button() == Qt::LeftButton && !nodeAt(sp) && tieAt(sp) < 0)
            if (int f = frameAt(sp, false); f >= 0)
            {
                FrameItem* item = mFrameItems[size_t(f)];
                QPointF local = item->mapFromScene(sp);
                if (item->arrowRect().contains(local))
                {
                    std::vector<gn::GFrame> next = mFrames;
                    next[size_t(f)].collapsed = !next[size_t(f)].collapsed;
                    commitFrames(next);
                    return;
                }
                bool grip = item->gripRect().contains(local);
                if (grip || item->titleRect().contains(local))
                {
                    mFrameDrag = f;
                    mFrameResize = grip;
                    mFrameGrab = sp;
                    mFrameBefore = mFrames[size_t(f)];
                    mFrameNodeStart.clear();
                    for (const std::string& m : mFrames[size_t(f)].nodes)
                        if (auto it = mNodes.find(m); it != mNodes.end())
                            mFrameNodeStart[m] = it->second->pos();
                    return;
                }
            }
        if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier) && !nodeAt(sp) && tieAt(sp) < 0 &&
            backend->supports("AddNode"))
        {
            mRerouteStroke = true;
            mTieStart = sp;
            mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
            return;
        }
        if (e->button() == Qt::LeftButton && !nodeAt(sp))
            if (int t = tieAt(sp); t >= 0)
            {
                mTieDrag = t;
                mTieRotate = e->modifiers() & Qt::ShiftModifier;
                mTieGrab = sp - mTies[t].pos;
                mTieBefore = mTies[t];
                return;
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
        if (mMinimapDrag)
        {
            centerOn(fromMinimap(e->pos()));
            return;
        }
        if (mTieStroke || mRerouteStroke)
        {
            QPainterPath path(mTieStart);
            path.lineTo(sp);
            mTemp->setPath(path);
            return;
        }
        if (mTieDrag >= 0)
        {
            dragTie(sp, e->modifiers());
            return;
        }
        if (mFrameDrag >= 0)
        {
            dragFrame(sp);
            return;
        }
        if (mLazyFrom)
        {
            QPointF a = mLazyFrom->sceneBoundingRect().center();
            QPainterPath path(a);
            path.lineTo(sp);
            mTemp->setPath(path);
            return;
        }
        if (e->buttons() == Qt::NoButton)
        {
            hoverTie(tieAt(sp));
            NodeItem* over = nodeAt(sp);
            if (over != mHoverNode)
            {
                mHoverNode = over;
                if (mPrefs.dimUnrelated)
                    updateEmphasis();
            }
            std::string portal = over && over->portal ? over->portalName : std::string();
            if (portal != mHoverPortal)
            {
                mHoverPortal = portal;
                viewport()->update();
            }
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
        if (e->buttons() & Qt::LeftButton)
            findInsertLink();
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
        if (mTieStroke)
        {
            finishTieStroke(sp);
            return;
        }
        if (mRerouteStroke)
        {
            mRerouteStroke = false;
            removeTemp();
            std::vector<std::pair<LinkItem*, QPointF>> at;
            QLineF stroke(mTieStart, sp);
            for (LinkItem* l : mLinks)
                if (auto p = crossing(l->path(), stroke))
                    at.push_back({l, *p});
            insertReroutes(at);
            return;
        }
        if (std::exchange(mMinimapDrag, false))
            return;
        if (NodeItem* from = std::exchange(mLazyFrom, nullptr))
        {
            removeTemp();
            if (NodeItem* to = nodeAt(sp); to && to != from)
                lazyConnect(from, to);
            return;
        }
        if (mFrameDrag >= 0)
        {
            int f = std::exchange(mFrameDrag, -1);
            if (mFrames[size_t(f)] == mFrameBefore)
                return;
            UndoChunk chunk(mFrameResize ? "Resize Frame" : "Move Frame");
            json positions = json::object();
            for (auto& [m, start] : mFrameNodeStart)
                if (auto it = mNodes.find(m); it != mNodes.end() && it->second->pos() != start)
                    positions[m] = {it->second->pos().x(), it->second->pos().y()};
            if (!positions.empty())
                runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
            commitFrames(mFrames);
            return;
        }
        if (mTieDrag >= 0)
        {
            int t = std::exchange(mTieDrag, -1);
            if (mTies[t].pos != mTieBefore.pos || mTies[t].angle != mTieBefore.angle)
                commitTies(mTies);
            return;
        }
        if (mWireFrom)
        {
            finishWire(sp);
            return;
        }
        QGraphicsView::mouseReleaseEvent(e);
        if (LinkItem* l = std::exchange(mInsertLink, nullptr))
        {
            l->setHighlight(false);
            UndoChunk chunk("Insert Node");
            commitMoves();
            insertOnLink(l);
            return;
        }
        commitMoves();
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        QPointF sp = mapToScene(e->pos());
        NodeItem* n = nodeAt(sp);
        if (!n)
            if (int f = frameAt(sp, true); f >= 0)
            {
                renameFrame(f);
                return;
            }
        if (!n)
            if (LinkItem* l = linkAt(sp))
            {
                if (NodeItem* far = otherEnd(l, sp))
                {
                    mScene.clearSelection();
                    far->setSelected(true);
                    centerOn(far->sceneBoundingRect().center());
                }
                else if (backend->supports("AddNode"))
                    insertReroutes({{l, nearestOn(l->path(), sp)}});
                return;
            }
        if (!n && onAddMenu)
            onAddMenu(mapToScene(e->pos()));
        else if (n && !n->groupRef.empty() && onOpenGroup)
            onOpenGroup(n->groupRef);
        else if (n && n->type == gn::kReceiveType)
            jumpToPortal(true);
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
            {
                if (QString tip = n->socketTip(sp); !tip.isEmpty())
                {
                    QToolTip::showText(he->globalPos(), tip, viewport());
                    return true;
                }
                if (n->portal)
                {
                    QToolTip::showText(he->globalPos(), portalTip(n), viewport());
                    return true;
                }
            }
            else if (LinkItem* l = linkAt(sp))
            {
                QToolTip::showText(he->globalPos(), wireTip(l), viewport());
                return true;
            }
        }
        return QGraphicsView::viewportEvent(e);
    }

    QStringList nodeFiles(const QMimeData* m) const
    {
        QStringList out;
        if (!m || !backend->supports("ImportGroup"))
            return out;
        std::vector<std::string> suffixes = backend->fileSuffixes();
        for (const QUrl& u : m->urls())
            if (u.isLocalFile())
                for (const std::string& suffix : suffixes)
                    if (u.toLocalFile().endsWith("." + qs(suffix), Qt::CaseInsensitive))
                    {
                        out << u.toLocalFile();
                        break;
                    }
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

    /** copy: a linked input hands out a second wire from its source and keeps its own link. */
    void startWire(NodeItem* n, int socket, bool input, bool copy = false)
    {
        mDetached.reset();
        if (copy && input && n->inputs[socket].linked)
            for (LinkItem* l : mLinks)
                if (l->to == n && l->in == socket)
                {
                    n = l->from;
                    socket = l->out;
                    input = false;
                    break;
                }
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
        if (target && target->reroute && ts < 0)
        {
            tin = !fromInput;
            ts = 0;
        }
        if (!target && !mDetached && onWireDropped)
        {
            onWireDropped(sp, wireOf(from, fs, fromInput));
            return;
        }
        UndoChunk chunk("Link");
        if (mDetached)
            runVerb(*backend, "Disconnect", {{"args", {mGroup, mDetached->second}}, {"source", mDetached->first}});
        mDetached.reset();
        if (!target || ts < 0 || target == from || tin == fromInput)
        {
            if (!target)
                QTimer::singleShot(0, this, [this] { viewport()->update(); });
            return;
        }
        std::string out = fromInput ? target->name + "." + target->outputs[ts].id : from->name + "." + from->outputs[fs].id;
        std::string in = fromInput ? from->name + "." + from->inputs[fs].id : target->name + "." + target->inputs[ts].id;
        runVerb(*backend, "Connect", args({mGroup, out, in}));
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
            runVerb(*backend, "Disconnect", {{"args", {mGroup, dst}}, {"source", src}});
    }

    /** Where a wire first crosses a straight stroke, if it does. */
    static std::optional<QPointF> crossing(const QPainterPath& path, const QLineF& stroke)
    {
        for (int k = 0; k < 48; ++k)
        {
            QPointF hit;
            if (QLineF(path.pointAtPercent(k / 48.0), path.pointAtPercent((k + 1) / 48.0)).intersects(stroke, &hit) == QLineF::BoundedIntersection)
                return hit;
        }
        return std::nullopt;
    }

    static QPointF nearestOn(const QPainterPath& path, QPointF p)
    {
        QPointF best = path.pointAtPercent(0.0);
        for (int k = 1; k <= 64; ++k)
            if (QPointF q = path.pointAtPercent(k / 64.0); QLineF(q, p).length() < QLineF(best, p).length())
                best = q;
        return best;
    }

    /** The visible wire within a few pixels of a scene point. */
    LinkItem* linkAt(QPointF sp) const
    {
        double reach = 6.0 / std::max(0.05, transform().m11());
        LinkItem* best = nullptr;
        double bestD = reach;
        for (LinkItem* l : mLinks)
        {
            if (!l->isVisible() || !l->path().controlPointRect().adjusted(-reach, -reach, reach, reach).contains(sp))
                continue;
            double d = QLineF(nearestOn(l->path(), sp), sp).length();
            if (d < bestD)
            {
                bestD = d;
                best = l;
            }
        }
        return best;
    }

    /** Splits each wire at its point with a new reroute, as one undo step. */
    void insertReroutes(const std::vector<std::pair<LinkItem*, QPointF>>& at)
    {
        if (at.empty())
            return;
        struct Cut
        {
            std::string from, to;
            QPointF p;
        };
        std::vector<Cut> cuts;
        for (auto& [l, p] : at)
            cuts.push_back({l->from->name + "." + l->from->outputs[l->out].id, l->to->name + "." + l->to->inputs[l->in].id, p});
        UndoChunk chunk("Add Reroute");
        for (const Cut& c : cuts)
        {
            json r = runVerb(*backend, "AddNode", {{"args", {mGroup, gn::kRerouteType}}, {"position", {c.p.x(), c.p.y()}}});
            if (!r.is_string())
                return;
            std::string name = r.get<std::string>();
            runVerb(*backend, "Disconnect", {{"args", {mGroup, c.to}}, {"source", c.from}});
            runVerb(*backend, "Connect", args({mGroup, c.from, name + "." + gn::kRerouteIn}));
            runVerb(*backend, "Connect", args({mGroup, name + "." + gn::kRerouteOut, c.to}));
        }
    }

    /**
     * The wires running through each selected reroute: up its chain of reroutes to the socket feeding it and down every
     * branch to the inputs it reaches.
     */
    std::set<LinkItem*> rerouteChains() const
    {
        std::set<LinkItem*> lit;
        std::vector<NodeItem*> stack;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i); n && n->reroute)
                stack.push_back(n);
        std::set<NodeItem*> seen(stack.begin(), stack.end());
        while (!stack.empty())
        {
            NodeItem* r = stack.back();
            stack.pop_back();
            for (LinkItem* l : mLinks)
            {
                NodeItem* next = l->to == r ? l->from : l->from == r ? l->to : nullptr;
                if (!next)
                    continue;
                lit.insert(l);
                if (next->reroute && seen.insert(next).second)
                    stack.push_back(next);
            }
        }
        return lit;
    }

    /** The selected nodes and everything upstream and downstream of them. */
    std::set<NodeItem*> selectionFlow() const
    {
        std::set<NodeItem*> flow;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i))
                flow.insert(n);
        for (bool up : {true, false})
        {
            std::vector<NodeItem*> stack(flow.begin(), flow.end());
            std::set<NodeItem*> seen(flow.begin(), flow.end());
            while (!stack.empty())
            {
                NodeItem* at = stack.back();
                stack.pop_back();
                for (LinkItem* l : mLinks)
                    if (NodeItem* next = up ? (l->to == at ? l->from : nullptr) : (l->from == at ? l->to : nullptr); next && seen.insert(next).second)
                        stack.push_back(next);
            }
            flow.insert(seen.begin(), seen.end());
        }
        return flow;
    }

    /**
     * Draws wires and nodes by what matters now: wires through a selected reroute light up; with the flow highlight on,
     * the selection's upstream and downstream light up and everything else dims.
     */
    void updateEmphasis()
    {
        std::set<LinkItem*> lit = rerouteChains();
        bool flowOn = (mFlowToggle || mFlowHold) && !mScene.selectedItems().isEmpty();
        std::set<NodeItem*> flow = flowOn ? selectionFlow() : std::set<NodeItem*>{};
        if (flowOn)
            for (LinkItem* l : mLinks)
                if (flow.contains(l->from) && flow.contains(l->to))
                    lit.insert(l);
        // Dim wires off the hovered and selected nodes, by pref.
        std::set<NodeItem*> focus;
        if (mPrefs.dimUnrelated)
        {
            for (QGraphicsItem* i : mScene.selectedItems())
                if (auto* n = dynamic_cast<NodeItem*>(i))
                    focus.insert(n);
            if (mHoverNode)
                focus.insert(mHoverNode);
        }
        std::set<NodeItem*> picked;
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i))
                picked.insert(n);
        bool refilter = false;
        for (LinkItem* l : mLinks)
        {
            bool filtered = mSelectedWiresOnly && !picked.contains(l->from) && !picked.contains(l->to);
            if (filtered != l->filtered)
            {
                l->filtered = filtered;
                l->applyVisibility();
                refilter = true;
            }
        }
        if (refilter && !mTies.empty())
            layoutTies();
        for (LinkItem* l : mLinks)
        {
            l->setHighlight(lit.contains(l));
            bool unrelated = !focus.empty() && !focus.contains(l->from) && !focus.contains(l->to);
            l->setOpacity((flowOn && !lit.contains(l)) || (unrelated && !lit.contains(l)) ? 0.25 : 1.0);
        }
        for (auto& [name, n] : mNodes)
            n->setOpacity(flowOn && !flow.contains(n) ? 0.35 : 1.0);
    }

    /** Within reach of a wire's end, the node at its other end. */
    NodeItem* otherEnd(LinkItem* l, QPointF sp) const
    {
        double reach = 40.0 / std::max(0.05, transform().m11());
        QPointF a = l->from->socketPos(false, l->out), b = l->to->socketPos(true, l->in);
        double da = QLineF(a, sp).length(), db = QLineF(b, sp).length();
        if (std::min(da, db) > reach)
            return nullptr;
        return da < db ? l->to : l->from;
    }

    int tieAt(QPointF sp) const
    {
        for (TieItem* t : mTieItems)
            if (t->shape().contains(t->mapFromScene(sp)))
                return t->index;
        return -1;
    }

    void hoverTie(int t)
    {
        if (t == mHotTie)
            return;
        for (TieItem* item : mTieItems)
        {
            item->hot = item->index == t;
            item->update();
        }
        mHotTie = t;
        if (!mZipKey)
        {
            if (t >= 0)
                viewport()->setCursor(Qt::OpenHandCursor);
            else
                viewport()->unsetCursor();
        }
    }

    /**
     * Routes every tied wire through its ties. In each tie the wires sit side by side across the band, ordered by where
     * their ends lie across it so the ribbon untangles, and run kTieRun along the tie's direction.
     */
    void layoutTies()
    {
        std::map<LinkKey, LinkItem*> byKey;
        for (LinkItem* l : mLinks)
        {
            l->passes.clear();
            byKey[l->key()] = l;
        }
        struct Pass
        {
            double t; // where along the wire the tie sits
            QPointF entry, exit;
        };
        std::map<LinkItem*, std::vector<Pass>> passes;
        for (int i = 0; i < int(mTies.size()); ++i)
        {
            const TieView& tie = mTies[i];
            QPointF d = tieDirection(tie.angle), across(-d.y(), d.x());
            std::vector<std::pair<double, LinkItem*>> order;
            for (const LinkKey& k : tie.links)
                if (auto it = byKey.find(k); it != byKey.end() && it->second->isVisible())
                {
                    LinkItem* l = it->second;
                    QPointF mid = (l->from->socketPos(false, l->out) + l->to->socketPos(true, l->in)) * 0.5;
                    order.push_back({dot(mid - tie.pos, across), l});
                }
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t s = 0; s < order.size(); ++s)
            {
                LinkItem* l = order[s].second;
                QPointF a = l->from->socketPos(false, l->out), b = l->to->socketPos(true, l->in);
                QPointF run = dot(b - a, d) < 0.0 ? -d : d;
                QPointF c = tie.pos + across * ((double(s) - double(order.size() - 1) * 0.5) * kTieSpacing);
                QPointF ab = b - a;
                double t = dot(tie.pos - a, ab) / std::max(1e-9, dot(ab, ab));
                passes[l].push_back({t, c - run * (kTieRun * 0.5), c + run * (kTieRun * 0.5)});
            }
            if (i < int(mTieItems.size()))
            {
                TieItem* item = mTieItems[i];
                item->setCount(int(order.size()));
                item->color = tieColor(tie.color);
                item->setPos(tie.pos);
                item->setRotation(tie.angle);
                item->setVisible(order.size() >= 2);
                item->update();
            }
        }
        for (auto& [l, list] : passes)
        {
            std::sort(list.begin(), list.end(), [](const Pass& a, const Pass& b) { return a.t < b.t; });
            for (const Pass& p : list)
                l->passes.push_back({p.entry, p.exit});
        }
        for (LinkItem* l : mLinks)
            l->update();
    }

    void dragTie(QPointF sp, Qt::KeyboardModifiers mods)
    {
        TieView& tie = mTies[mTieDrag];
        if (mTieRotate)
        {
            QPointF v = sp - tie.pos;
            if (std::hypot(v.x(), v.y()) < 4.0)
                return;
            double a = qRadiansToDegrees(std::atan2(v.y(), v.x()));
            // Ctrl turns freely; otherwise it steps by 15 degrees.
            tie.angle = (mods & Qt::ControlModifier) ? a : std::round(a / 15.0) * 15.0;
        }
        else
            tie.pos = sp - mTieGrab;
        layoutTies();
    }

    /** A wire's ends and type, and what drives it when the backend can say cheaply. */
    QString wireTip(LinkItem* l) const
    {
        const SocketInfo& s = l->from->outputs[size_t(l->out)];
        QString tip = qs(l->from->name + "." + s.id) + QString("  %1  ").arg(QChar(0x2192)) + qs(l->to->name + "." + l->to->inputs[size_t(l->in)].id);
        tip += "\n" + qs(s.type) + (s.field ? " field" : "");
        if (QString summary = backend->wireSummary(mInfo, l->from->name, s.id); !summary.isEmpty())
            tip += "\n" + summary;
        return tip;
    }

    QString portalTip(NodeItem* n) const
    {
        QString name = n->portalName.empty() ? QString("(no name)") : qs(n->portalName);
        int readers = 0;
        NodeItem* send = nullptr;
        for (auto& [k, item] : mNodes)
            if (item->portal && item->portalName == n->portalName)
            {
                if (item->type == gn::kReceiveType)
                    ++readers;
                else
                    send = item;
            }
        QString tip;
        if (n->type == gn::kSendType)
            tip = "Send " + name + QString(", read by %1 Receive%2").arg(readers).arg(readers == 1 ? "" : "s");
        else
            tip = "Receive " + name + (send ? " from " + qs(send->name) : QString());
        if (n->type == gn::kReceiveType && send)
            for (LinkItem* l : mLinks)
                if (l->to == send)
                    tip += ", fed by " + qs(l->from->name + "." + l->from->outputs[l->out].id);
        if (!n->problem.isEmpty())
            tip += "\n" + n->problem;
        return tip + "\nDouble-click a Receive to jump to its Send.";
    }

    /** Links whose wire crosses the segment a-b, in mLinks order. */
    std::vector<LinkKey> linksCrossing(QPointF a, QPointF b) const
    {
        std::vector<LinkKey> out;
        QLineF stroke(a, b);
        for (LinkItem* l : mLinks)
        {
            if (!l->isVisible())
                continue;
            const QPainterPath& path = l->path();
            for (int k = 0; k < 48; ++k)
                if (QLineF(path.pointAtPercent(k / 48.0), path.pointAtPercent((k + 1) / 48.0)).intersects(stroke) == QLineF::BoundedIntersection)
                {
                    out.push_back(l->key());
                    break;
                }
        }
        return out;
    }

    void finishTieStroke(QPointF end)
    {
        mTieStroke = false;
        removeTemp();
        if (!mZipKey)
            viewport()->unsetCursor();
        std::vector<LinkKey> crossed = linksCrossing(mTieStart, end);
        std::vector<TieView> next = mTies;
        if (mTieTarget >= 0 && mTieTarget < int(next.size()))
        {
            TieView& tie = next[mTieTarget];
            size_t before = tie.links.size();
            for (const LinkKey& k : crossed)
                if (std::find(tie.links.begin(), tie.links.end(), k) == tie.links.end())
                    tie.links.push_back(k);
            if (tie.links.size() != before)
                commitTies(next);
            return;
        }
        if (crossed.size() < 2)
        {
            QToolTip::showText(QCursor::pos(), "A zip tie needs two or more wires: drag across them while holding the key.", this);
            return;
        }
        QPointF v = end - mTieStart;
        QPointF dir(-v.y(), v.x());
        QPointF flow(0.0, 0.0);
        for (LinkItem* l : mLinks)
            if (std::find(crossed.begin(), crossed.end(), l->key()) != crossed.end())
            {
                QPointF f = l->to->socketPos(true, l->in) - l->from->socketPos(false, l->out);
                flow += f / std::max(1e-9, std::hypot(f.x(), f.y()));
            }
        if (dot(dir, flow) < 0.0)
            dir = -dir;
        double angle = qRadiansToDegrees(std::atan2(dir.y(), dir.x()));
        angle = std::round(angle / 15.0) * 15.0;
        next.push_back({crossed, (mTieStart + end) * 0.5, angle == -0.0 ? 0.0 : angle, {}});
        commitTies(next);
    }

    static QString linkLabel(const LinkKey& k) { return qs(k[0] + "." + k[1]) + QString("  %1  ").arg(QChar(0x2192)) + qs(k[2] + "." + k[3]); }

    void tieMenu(int t, QPoint global)
    {
        QMenu menu(this);
        fillPaletteMenu(menu.addMenu("Colour"), [this, t](std::string id) {
            std::vector<TieView> next = mTies;
            next[t].color = std::move(id);
            commitTies(next);
        });
        menu.addAction(QString("Rotate 90%1").arg(QChar(0xb0)), this, [this, t] {
            std::vector<TieView> next = mTies;
            next[t].angle = std::fmod(next[t].angle + 90.0, 360.0);
            commitTies(next);
        });
        menu.addAction("Level", this, [this, t] {
            std::vector<TieView> next = mTies;
            next[t].angle = 0.0;
            commitTies(next);
        });
        QMenu* remove = menu.addMenu("Remove Wire");
        for (size_t i = 0; i < mTies[t].links.size(); ++i)
            remove->addAction(linkLabel(mTies[t].links[i]), this, [this, t, i] {
                std::vector<TieView> next = mTies;
                next[t].links.erase(next[t].links.begin() + std::ptrdiff_t(i));
                if (next[t].links.size() < 2)
                    next.erase(next.begin() + t);
                commitTies(next);
            });
        menu.addSeparator();
        menu.addAction("Delete Tie", this, [this, t] {
            std::vector<TieView> next = mTies;
            next.erase(next.begin() + t);
            commitTies(next);
        });
        menu.exec(global);
    }

    void commitTies(const std::vector<TieView>& ties) { runVerb(*backend, "SetTies", {{"args", {mGroup, tiesJson(ties)}}}); }

    /** The one node being dragged, when it has no links yet and so can drop into a wire. */
    NodeItem* loneDraggedNode() const
    {
        QList<QGraphicsItem*> sel = mScene.selectedItems();
        if (sel.size() != 1 || mPressPositions.empty())
            return nullptr;
        auto* n = dynamic_cast<NodeItem*>(sel[0]);
        if (!n || n->pos() == mPressPositions.at(n->name))
            return nullptr;
        for (LinkItem* l : mLinks)
            if (l->from == n || l->to == n)
                return nullptr;
        return n;
    }

    /** Lights the wire a dragged unlinked node would drop into: the one running most through it. */
    void findInsertLink()
    {
        LinkItem* best = nullptr;
        if (NodeItem* n = loneDraggedNode())
        {
            QRectF r = n->sceneBoundingRect();
            int most = 0;
            for (LinkItem* l : mLinks)
            {
                // A level wire has a flat control rect, which intersects nothing until padded.
                if (!l->isVisible() || !l->path().controlPointRect().adjusted(-1, -1, 1, 1).intersects(r))
                    continue;
                int inside = 0;
                for (int k = 0; k <= 32; ++k)
                    inside += r.contains(l->path().pointAtPercent(k / 32.0)) ? 1 : 0;
                auto [in, out] = insertSockets(n, l);
                if (inside > most && in >= 0 && out >= 0)
                {
                    most = inside;
                    best = l;
                }
            }
        }
        if (best == mInsertLink)
            return;
        if (mInsertLink)
            mInsertLink->setHighlight(false);
        mInsertLink = best;
        if (best)
            best->setHighlight(true);
    }

    /** The input and output of n that a wire would run through: the same types first, then any that link. */
    std::pair<int, int> insertSockets(NodeItem* n, LinkItem* l) const
    {
        const std::string& fromType = l->from->outputs[l->out].type;
        const std::string& toType = l->to->inputs[l->in].type;
        auto pick = [](const std::vector<SocketInfo>& list, auto&& same, auto&& fits) {
            for (int i = 0; i < int(list.size()); ++i)
                if (same(list[i].type))
                    return i;
            for (int i = 0; i < int(list.size()); ++i)
                if (fits(list[i].type))
                    return i;
            return -1;
        };
        if (n->reroute)
            return {0, 0};
        int in = pick(n->inputs, [&](const std::string& t) { return t == fromType; }, [&](const std::string& t) { return backend->canLink(fromType, t); });
        int out = pick(n->outputs, [&](const std::string& t) { return t == toType; }, [&](const std::string& t) { return backend->canLink(t, toType); });
        return {in, out};
    }

    /** Wires n into the link, then (with the auto-offset pref) slides what is downstream right to make room. */
    void insertOnLink(LinkItem* l)
    {
        NodeItem* n = nullptr;
        for (QGraphicsItem* i : mScene.selectedItems())
            n = dynamic_cast<NodeItem*>(i);
        if (!n)
            return;
        auto [in, out] = insertSockets(n, l);
        if (in < 0 || out < 0)
            return;
        std::string src = l->from->name + "." + l->from->outputs[l->out].id, dst = l->to->name + "." + l->to->inputs[l->in].id;
        std::string mineIn = n->name + "." + (n->reroute ? std::string(gn::kRerouteIn) : n->inputs[in].id);
        std::string mineOut = n->name + "." + (n->reroute ? std::string(gn::kRerouteOut) : n->outputs[out].id);
        NodeItem* to = l->to;
        runVerb(*backend, "Disconnect", {{"args", {mGroup, dst}}, {"source", src}});
        runVerb(*backend, "Connect", args({mGroup, src, mineIn}));
        runVerb(*backend, "Connect", args({mGroup, mineOut, dst}));
        if (!mPrefs.autoOffset)
            return;
        double shift = n->sceneBoundingRect().right() + 40.0 - to->sceneBoundingRect().left();
        if (shift <= 0.0 || to == n)
            return;
        std::set<NodeItem*> downstream{to};
        std::vector<NodeItem*> stack{to};
        while (!stack.empty())
        {
            NodeItem* at = stack.back();
            stack.pop_back();
            for (LinkItem* k : mLinks)
                if (k->from == at && k->to != n && downstream.insert(k->to).second)
                    stack.push_back(k->to);
        }
        json positions = json::object();
        for (NodeItem* d : downstream)
            positions[d->name] = {d->pos().x() + shift, d->pos().y()};
        runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
    }

    /** A wire's colour: its socket type's, or one grey in a grey theme when the pref asks. */
    QColor wireColor(const std::string& type) const
    {
        if (mPrefs.greyWires && !chromaticTheme())
            return gray(165);
        return backend->socketStyle(type).color;
    }

    /** The minimap's box in the viewport's bottom-right corner. */
    QRect minimapRect() const
    {
        QRect v = viewport()->rect();
        int w = std::min(220, v.width() / 3), h = std::min(150, v.height() / 3);
        return QRect(v.right() - w - 10, v.bottom() - h - 10, w, h);
    }

    /** Everything on the canvas, padded. */
    QRectF graphBounds() const
    {
        QRectF r;
        for (auto& [name, n] : mNodes)
            if (n->isVisible())
                r = r.united(n->sceneBoundingRect());
        for (FrameItem* f : mFrameItems)
            r = r.united(f->sceneBoundingRect());
        return r.isNull() ? QRectF(-200, -150, 400, 300) : r.adjusted(-60, -60, 60, 60);
    }

    /** Scene to minimap, keeping the aspect: the graph and the view both fit. */
    QTransform minimapTransform() const
    {
        QRectF g = graphBounds().united(mapToScene(viewport()->rect()).boundingRect());
        QRectF m = QRectF(minimapRect()).adjusted(4, 4, -4, -4);
        double s = std::min(m.width() / g.width(), m.height() / g.height());
        QPointF off = m.center() - g.center() * s;
        return QTransform(s, 0, 0, s, off.x(), off.y());
    }

    QPointF fromMinimap(QPoint p) const { return minimapTransform().inverted().map(QPointF(p)); }

    void drawMinimap(QPainter* p)
    {
        QRect box = minimapRect();
        QTransform t = minimapTransform();
        p->setPen(QPen(gray(70), 1.0));
        p->setBrush(gray(26, 225));
        p->drawRoundedRect(box, 4, 4);
        p->setClipRect(box.adjusted(1, 1, -1, -1));
        QFont f = p->font();
        f.setPixelSize(9);
        p->setFont(f);
        for (size_t i = 0; i < mFrameItems.size() && i < mFrames.size(); ++i)
        {
            QRectF r = t.mapRect(mFrameItems[i]->sceneBoundingRect());
            QColor c = mFrameItems[i]->color;
            c.setAlpha(110);
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRect(r);
            p->setPen(gray(225));
            p->drawText(r.adjusted(2, 1, -2, 0), Qt::AlignTop | Qt::AlignLeft, mFrameItems[i]->label);
        }
        p->setPen(QPen(gray(120), 1.0));
        for (LinkItem* l : mLinks)
            if (l->isVisible())
                p->drawLine(t.map(l->from->socketPos(false, l->out)), t.map(l->to->socketPos(true, l->in)));
        p->setPen(Qt::NoPen);
        for (auto& [name, n] : mNodes)
            if (n->isVisible())
            {
                p->setBrush(n->isSelected() ? themeColor("nodeSelected") : gray(150));
                p->drawRect(t.mapRect(n->sceneBoundingRect()));
            }
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(gray(235), 1.0));
        p->drawRect(t.mapRect(mapToScene(viewport()->rect()).boundingRect()));
    }

    /** The topmost frame under a scene point: its title only, or anywhere in its box. */
    int frameAt(QPointF sp, bool titleOnly) const
    {
        for (int i = int(mFrameItems.size()) - 1; i >= 0; --i)
        {
            FrameItem* f = mFrameItems[size_t(i)];
            QPointF local = f->mapFromScene(sp);
            if ((titleOnly ? f->titleRect() : f->box().adjusted(0, 0, 2, 2)).contains(local))
                return i;
        }
        return -1;
    }

    /** Places the frame items and hides the nodes of collapsed frames, their wires ending on the frame's title. */
    void layoutFrames()
    {
        for (auto& [name, n] : mNodes)
        {
            n->inProxy.reset();
            n->outProxy.reset();
            n->setVisible(true);
        }
        std::map<NodeItem*, size_t> hiddenIn;
        for (size_t i = 0; i < mFrames.size() && i < mFrameItems.size(); ++i)
        {
            const gn::GFrame& f = mFrames[i];
            FrameItem* item = mFrameItems[i];
            item->label = f.label.empty() ? QString("Frame") : qs(f.label);
            item->color = paletteColor(f.color, 95);
            item->setBox(QSizeF(f.size.x, f.size.y), f.collapsed);
            item->setPos(f.position.x, f.position.y);
            item->update();
            if (!f.collapsed)
                continue;
            QPointF left(f.position.x, f.position.y + FrameItem::kTitle * 0.5), right(f.position.x + f.size.x, left.y());
            for (const std::string& m : f.nodes)
                if (auto it = mNodes.find(m); it != mNodes.end())
                {
                    it->second->inProxy = left;
                    it->second->outProxy = right;
                    it->second->setVisible(false);
                    hiddenIn[it->second] = i;
                }
        }
        for (LinkItem* l : mLinks)
        {
            // Wires inside one collapsed frame have nowhere to show.
            auto a = hiddenIn.find(l->from), b = hiddenIn.find(l->to);
            l->folded = a != hiddenIn.end() && b != hiddenIn.end() && a->second == b->second;
            l->applyVisibility();
            l->update();
        }
    }

    void dragFrame(QPointF sp)
    {
        gn::GFrame& f = mFrames[size_t(mFrameDrag)];
        QPointF d = sp - mFrameGrab;
        if (mFrameResize)
            f.size = {std::max(80.0, mFrameBefore.size.x + d.x()), std::max(FrameItem::kTitle + 20.0, mFrameBefore.size.y + d.y())};
        else
        {
            f.position = mFrameBefore.position + glm::dvec2(d.x(), d.y());
            for (auto& [m, start] : mFrameNodeStart)
                if (auto it = mNodes.find(m); it != mNodes.end())
                    it->second->setPos(start + d);
        }
        layoutFrames();
        layoutTies();
    }

    void renameFrame(int f)
    {
        bool ok = false;
        QString text = QInputDialog::getText(this, "Rename Frame", "Label", QLineEdit::Normal, qs(mFrames[size_t(f)].label), &ok);
        if (!ok)
            return;
        std::vector<gn::GFrame> next = mFrames;
        next[size_t(f)].label = text.trimmed().toStdString();
        commitFrames(next);
    }

    void frameMenu(int f, QPoint global)
    {
        QMenu menu(this);
        menu.addAction("Rename...", this, [this, f] { renameFrame(f); });
        QMenu* colors = menu.addMenu("Colour");
        fillPaletteMenu(colors, [this, f](std::string id) {
            std::vector<gn::GFrame> next = mFrames;
            next[size_t(f)].color = std::move(id);
            commitFrames(next);
        });
        menu.addAction(mFrames[size_t(f)].collapsed ? "Expand" : "Collapse", this, [this, f] {
            std::vector<gn::GFrame> next = mFrames;
            next[size_t(f)].collapsed = !next[size_t(f)].collapsed;
            commitFrames(next);
        });
        menu.addAction("Select Contents", this, [this, f] {
            mScene.clearSelection();
            for (const std::string& m : mFrames[size_t(f)].nodes)
                if (auto it = mNodes.find(m); it != mNodes.end())
                    it->second->setSelected(true);
        });
        menu.addSeparator();
        menu.addAction("Delete Frame", this, [this, f] {
            std::vector<gn::GFrame> next = mFrames;
            next.erase(next.begin() + f);
            commitFrames(next);
        });
        menu.exec(global);
    }

    void commitFrames(const std::vector<gn::GFrame>& frames)
    {
        runVerb(*backend, "SetFrames", {{"args", {mGroup, gn::framesToJson(frames)}}});
    }

    /** Moved nodes join the frame they were dropped in, and leave one they were dragged out of. */
    bool updateMembership(const json& moved, std::vector<gn::GFrame>& frames) const
    {
        bool changed = false;
        for (auto& [name, pos] : moved.items())
        {
            auto it = mNodes.find(name);
            if (it == mNodes.end())
                continue;
            QPointF c = it->second->sceneBoundingRect().center();
            int into = -1;
            for (int i = int(frames.size()) - 1; i >= 0 && into < 0; --i)
            {
                const gn::GFrame& f = frames[size_t(i)];
                if (!f.collapsed && QRectF(f.position.x, f.position.y, f.size.x, f.size.y).contains(c))
                    into = i;
            }
            for (int i = 0; i < int(frames.size()); ++i)
            {
                std::vector<std::string>& m = frames[size_t(i)].nodes;
                bool in = std::find(m.begin(), m.end(), name) != m.end();
                if (in && i != into)
                {
                    std::erase(m, name);
                    changed = true;
                }
                else if (!in && i == into)
                {
                    m.push_back(name);
                    changed = true;
                }
            }
        }
        return changed;
    }

    /** One undoable MoveNodes for every node whose spot differs. */
    void moveTo(const std::map<NodeItem*, QPointF>& to, const char* label);

    /**
     * Links from to to through the best pair of sockets: same type into a free input first, then same type into a linked
     * one, then any linkable pair, earlier sockets winning ties.
     */
    void lazyConnect(NodeItem* from, NodeItem* to)
    {
        int bestOut = -1, bestIn = -1;
        double best = 0.0;
        for (int o = 0; o < int(from->outputs.size()); ++o)
            for (int i = 0; i < int(to->inputs.size()); ++i)
            {
                const std::string& a = from->outputs[size_t(o)].type;
                const SocketInfo& in = to->inputs[size_t(i)];
                double score = a == in.type ? 3.0 : backend->canLink(a, in.type) ? 1.0 : 0.0;
                if (score > 0.0 && !in.linked)
                    score += 1.5;
                if (score > best)
                {
                    best = score;
                    bestOut = o;
                    bestIn = i;
                }
            }
        if (bestOut < 0)
        {
            QToolTip::showText(QCursor::pos(), qs(from->name + " has no output that fits an input of " + to->name), this);
            return;
        }
        std::string out = from->name + "." + (from->reroute ? std::string(gn::kRerouteOut) : from->outputs[size_t(bestOut)].id);
        std::string in = to->name + "." + (to->reroute ? std::string(gn::kRerouteIn) : to->inputs[size_t(bestIn)].id);
        runVerb(*backend, "Connect", args({mGroup, out, in}));
    }

public:
    /**
     * Trades the links and values of the selected node's first two inputs of one type, such as a Math node's two
     * operands. False when it has no such pair.
     */
    bool swapInputs()
    {
        std::vector<NodeItem*> nodes = selectedNodes();
        if (nodes.size() != 1)
            return false;
        NodeItem* n = nodes[0];
        int a = -1, b = -1;
        for (int i = 0; i < int(n->inputs.size()) && b < 0; ++i)
            for (int j = i + 1; j < int(n->inputs.size()); ++j)
                if (n->inputs[size_t(i)].type == n->inputs[size_t(j)].type && !backend->valueless(n->inputs[size_t(i)].type) == !backend->valueless(n->inputs[size_t(j)].type))
                {
                    a = i;
                    b = j;
                    break;
                }
        if (a < 0)
            return false;
        const SocketInfo& sa = n->inputs[size_t(a)];
        const SocketInfo& sb = n->inputs[size_t(b)];
        std::vector<std::string> intoA, intoB;
        for (LinkItem* l : mLinks)
            if (l->to == n && (l->in == a || l->in == b))
                (l->in == a ? intoA : intoB).push_back(l->from->name + "." + l->from->outputs[size_t(l->out)].id);
        std::string pa = n->name + "." + sa.id, pb = n->name + "." + sb.id;
        json va = sa.value, vb = sb.value;
        UndoChunk chunk("Swap Inputs");
        if (!intoA.empty())
            runVerb(*backend, "Disconnect", args({mGroup, pa}));
        if (!intoB.empty())
            runVerb(*backend, "Disconnect", args({mGroup, pb}));
        if (!backend->valueless(sa.type) && !va.is_null() && !vb.is_null() && va != vb)
        {
            runVerb(*backend, "SetValue", args({mGroup, pa, vb}));
            runVerb(*backend, "SetValue", args({mGroup, pb, va}));
        }
        for (const std::string& s : intoA)
            runVerb(*backend, "Connect", args({mGroup, s, pb}));
        for (const std::string& s : intoB)
            runVerb(*backend, "Connect", args({mGroup, s, pa}));
        return true;
    }

private:

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
        if (positions.empty())
            return;
        UndoChunk chunk("Move Nodes");
        runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
        std::vector<gn::GFrame> next = mFrames;
        if (backend->supports("SetFrames") && updateMembership(positions, next))
            commitFrames(next);
    }

    QGraphicsScene mScene;
    std::string mGroup;
    json mInfo;
    bool mThumbsQueued = false;
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
    std::vector<TieView> mTies;
    std::vector<TieItem*> mTieItems;
    bool mZipKey = false;
    bool mTieStroke = false;
    QPointF mTieStart;
    int mTieTarget = -1; // the tie a stroke started on, which it adds to
    int mTieDrag = -1;
    bool mTieRotate = false;
    QPointF mTieGrab;
    TieView mTieBefore;
    int mHotTie = -1;
    std::string mHoverPortal; // portal name whose ghost lines are drawn
    std::vector<gn::GFrame> mFrames;
    std::vector<FrameItem*> mFrameItems;
    int mFrameDrag = -1;
    bool mFrameResize = false;
    QPointF mFrameGrab;
    gn::GFrame mFrameBefore;
    std::map<std::string, QPointF> mFrameNodeStart;
    bool mFlowToggle = false;
    bool mSelectedWiresOnly = false;
    bool mFlowHold = false;
    bool mRerouteStroke = false;
    bool mMinimapDrag = false;
    NodeItem* mHoverNode = nullptr;
    NodeItem* mLazyFrom = nullptr; // Alt+RMB drag: the node to connect from
    LinkItem* mInsertLink = nullptr; // the wire a dragged node would drop into
    CanvasPrefs mPrefs = readCanvasPrefs();
    bool mPanning = false;
    bool mBuilding = false;
    QPoint mLast;
};
} // namespace lila::ui
