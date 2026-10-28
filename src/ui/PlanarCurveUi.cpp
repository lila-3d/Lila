#include "ui/CurveUi.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/DocsUi.h"
#include "ui/FontPicker.h"
#include "ui/Icons.h"
#include "ui/MultilineEdit.h"
#include "ui/SpinScrub.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <map>
#include <memory>
#include <set>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;
using scene::Graph;
using scene::NodeId;

void cmd(std::string id, std::string label, std::string category, const char* command, json call, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = std::move(category), .doc = std::move(doc), .python = py,
                  .command = command, .call = std::move(call)});
}

/** The curveText nodes a node is built from, nearest first. */
std::vector<NodeId> textSources(NodeId from)
{
    Graph& g = Graph::current();
    std::map<NodeId, std::vector<NodeId>> sources;
    for (const auto& [dst, src] : g.connections())
        sources[dst.node].push_back(src.node);
    std::vector<NodeId> found, frontier{from};
    std::set<NodeId> seen{from};
    for (int depth = 0; depth < 8 && !frontier.empty() && seen.size() < 256; ++depth)
    {
        std::vector<NodeId> next;
        for (NodeId id : frontier)
            for (NodeId s : sources[id])
                if (seen.insert(s).second)
                {
                    scene::Node* n = g.node(s);
                    if (n && n->type().isA("curveText"))
                        found.push_back(s);
                    else
                        next.push_back(s);
                }
        frontier = std::move(next);
    }
    return found;
}

QDoubleSpinBox* spin(double lo, double hi, double step, int decimals, const QString& suffix, double def)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    setSpinDefault(s, def);
    return s;
}

QToolButton* toggle(const char* iconName, const QString& tip)
{
    auto* b = new QToolButton;
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    return b;
}

/** Edits one curveText node: its text in a box of several lines, the font by family and style, and the formatting. */
QWidget* textPanel(NodeId textNode)
{
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 4);
    v->setSpacing(4);

    auto node = [textNode]() -> scene::Node* { return Graph::current().node(textNode); };
    auto get = [node](const char* attr) -> scene::PlugValue {
        scene::Node* n = node();
        return n ? Graph::current().value({n->id(), n->type().attrIndex(attr), -1}) : scene::PlugValue{};
    };
    auto set = [node](const char* attr, json value) {
        scene::Node* n = node();
        if (!n)
            return;
        try
        {
            ops::execute("setAttr", {{"args", {n->path() + "." + attr, std::move(value)}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    };
    auto num = [get](const char* attr) {
        scene::PlugValue pv = get(attr);
        if (auto* d = std::get_if<double>(&pv))
            return *d;
        if (auto* i = std::get_if<int64_t>(&pv))
            return double(*i);
        return 0.0;
    };
    auto flag = [get](const char* attr) {
        scene::PlugValue pv = get(attr);
        auto* b = std::get_if<bool>(&pv);
        return b && *b;
    };
    auto str = [get](const char* attr) {
        scene::PlugValue pv = get(attr);
        auto* s = std::get_if<std::string>(&pv);
        return s ? *s : std::string();
    };
    std::vector<std::function<void()>> updaters;

    auto* text = new MultilineEdit;
    text->setToolTip("The text. Enter starts a new line; it updates when you pause typing, Ctrl+Enter applies at once.");
    QObject::connect(text, &MultilineEdit::committed, w, [set](const QString& t) { set("text", t.toStdString()); });
    updaters.push_back([text, str] { text->showText(str("text")); });
    v->addWidget(text);

    auto* font = new FontPicker;
    QObject::connect(font, &FontPicker::chosen, w, [set](const QString& f) { set("font", f.toStdString()); });
    updaters.push_back([font, str] { font->setValue(str("font")); });
    v->addWidget(font);

    // Formatting bar: bold, italic, underline, strike, then alignment.
    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    auto* bold = toggle("bold", "Bold: the family's bold style, or a heavier outline when it has none (Weight).");
    auto* italic = toggle("italic", "Italic: the family's italic style, or a slant when it has none (Slant).");
    auto* under = toggle("underline", "Underline");
    auto* strike = toggle("strikethrough", "Strikethrough");
    for (QToolButton* b : {bold, italic, under, strike})
        bar->addWidget(b);
    bar->addSpacing(8);
    auto* aligns = new QButtonGroup(w);
    const char* alignIcons[] = {"align-left", "align-center", "align-right", "align-justified"};
    const char* alignTips[] = {"Align left", "Center", "Align right", "Justify: wrapped lines fill the wrap width (or the widest line)"};
    for (int k = 0; k < 4; ++k)
    {
        auto* b = toggle(alignIcons[k], alignTips[k]);
        aligns->addButton(b, k);
        bar->addWidget(b);
    }
    bar->addStretch(1);
    bar->addWidget(docs::helpButton("curves?id=text", w));
    v->addLayout(bar);

    auto sizeOf = [num] { return std::max(1e-3, num("size")); };
    QObject::connect(bold, &QToolButton::clicked, w, [font, set, num, sizeOf](bool on) {
        std::string s = font->styleWith(on, styleIsItalic(font->style()));
        if (!s.empty())
        {
            font->setStyle(s);
            if (num("weight") != 0.0)
                set("weight", 0.0);
        }
        else
            set("weight", on ? sizeOf() * 0.035 : 0.0);
    });
    QObject::connect(italic, &QToolButton::clicked, w, [font, set, num](bool on) {
        std::string s = font->styleWith(styleIsBold(font->style()), on);
        if (!s.empty())
        {
            font->setStyle(s);
            if (num("slant") != 0.0)
                set("slant", 0.0);
        }
        else
            set("slant", on ? 12.0 : 0.0);
    });
    QObject::connect(under, &QToolButton::clicked, w, [set](bool on) { set("underline", on); });
    QObject::connect(strike, &QToolButton::clicked, w, [set](bool on) { set("strikethrough", on); });
    QObject::connect(aligns, &QButtonGroup::idClicked, w, [set](int id) { set("alignment", int64_t(id)); });
    updaters.push_back([=] {
        QSignalBlocker b1(bold), b2(italic), b3(under), b4(strike);
        bold->setChecked(styleIsBold(font->style()) || num("weight") > 0.0);
        italic->setChecked(styleIsItalic(font->style()) || num("slant") != 0.0);
        under->setChecked(flag("underline"));
        strike->setChecked(flag("strikethrough"));
        if (QAbstractButton* b = aligns->button(int(num("alignment"))))
            b->setChecked(true);
    });

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto addSpin = [&](const char* attr, const QString& label, const QString& tip, QDoubleSpinBox* s) {
        s->setToolTip(tip);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, w, [set, attr](double d) { set(attr, d); });
        updaters.push_back([s, num, attr] {
            if (s->hasFocus())
                return;
            QSignalBlocker block(s);
            s->setValue(num(attr));
        });
        form->addRow(label, s);
    };
    auto addCombo = [&](const char* attr, const QString& label, const QString& tip, QStringList items) {
        auto* c = new QComboBox;
        c->addItems(items);
        c->setToolTip(tip);
        setComboDefault(c, 0);
        QObject::connect(c, &QComboBox::activated, w, [set, attr](int i) { set(attr, int64_t(i)); });
        updaters.push_back([c, num, attr] {
            QSignalBlocker block(c);
            c->setCurrentIndex(int(num(attr)));
        });
        form->addRow(label, c);
    };
    auto addCheck = [&](const char* attr, const QString& label, const QString& tip, bool def) {
        auto* c = new QCheckBox(label);
        c->setToolTip(tip);
        setCheckDefault(c, def);
        QObject::connect(c, &QCheckBox::clicked, w, [set, attr](bool on) { set(attr, on); });
        updaters.push_back([c, flag, attr] {
            QSignalBlocker block(c);
            c->setChecked(flag(attr));
        });
        form->addRow(QString(), c);
    };
    addSpin("size", "Size", "Em size.", spin(0.001, 1e5, 0.5, 3, " cm", 10.0));
    addCombo("anchor", "Anchor", "Which part of the block sits on the origin.", {"Baseline", "Top", "Middle", "Bottom"});
    addSpin("wrapWidth", "Wrap Width", "Lines break between words to fit this width; 0 breaks only at new lines.", spin(0.0, 1e5, 1.0, 3, " cm", 0.0));
    addSpin("letterSpacing", "Letter Spacing", "Extra space after each character.", spin(-1e4, 1e4, 0.1, 3, " cm", 0.0));
    addSpin("wordSpacing", "Word Spacing", "Extra space after each space.", spin(-1e4, 1e4, 0.1, 3, " cm", 0.0));
    addSpin("lineSpacing", "Line Spacing", "Baseline to baseline, in the font's line heights.", spin(0.0, 100.0, 0.05, 3, QString(), 1.0));
    addCombo("textCase", "Case", "Changes the letters' case without editing the text.", {"As Typed", "UPPER", "lower", "Title"});
    addSpin("width", "Width", "Horizontal scale of the glyphs: below 1 condenses, above 1 widens.", spin(0.01, 100.0, 0.05, 3, QString(), 1.0));
    addSpin("slant", "Slant", "Oblique slant; positive leans right.", spin(-60.0, 60.0, 1.0, 2, QString::fromUtf8("°"), 0.0));
    addSpin("weight", "Weight", "Grows each letter's outline (negative thins it).", spin(-1e4, 1e4, 0.01, 3, " cm", 0.0));
    addCheck("kerning", "Kerning", "Use the font's kerning.", true);
    addCheck("mergeOverlaps", "Merge Overlaps", "Join a letter's overlapping contours so each area fills once.", true);
    v->addLayout(form);

    auto refresh = [updaters, guard = QPointer<QWidget>(w)] {
        if (!guard)
            return;
        for (const auto& u : updaters)
            u();
    };
    refresh();
    // Deferred: listeners fire mid-edit, when the graph must not be evaluated.
    auto queued = std::make_shared<bool>(false);
    int listener = Graph::current().addListener([w, refresh, queued](scene::Change, NodeId) {
        if (*queued)
            return;
        *queued = true;
        QTimer::singleShot(0, w, [refresh, queued] {
            *queued = false;
            refresh();
        });
    });
    QObject::connect(w, &QObject::destroyed, [listener] { Graph::current().removeListener(listener); });
    return w;
}

/** The Text section on a curve or mesh made from text: one panel per curveText it is built from. */
QWidget* textSection(NodeId id)
{
    std::vector<NodeId> texts = textSources(id);
    if (texts.empty())
        return nullptr;
    if (texts.size() == 1)
        return textPanel(texts.front());
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    for (NodeId t : texts)
    {
        auto* title = new QLabel(QString::fromStdString(Graph::current().node(t)->name()));
        title->setContentsMargins(8, 2, 4, 0);
        v->addWidget(title);
        v->addWidget(textPanel(t));
    }
    return w;
}
} // namespace

void registerPlanarCurveUi()
{
    cmd("curve.text", "Text", "Curves", "textCurves", json::object(),
        "Makes text as closed Bezier curves from a font, kept live: select the curve (or what you built from it) and edit the text, font and formatting in the Attribute Editor's Text section.");
    cmd("curve.textMesh", "Text Mesh", "Curves", "textMesh", json::object(),
        "Makes text as a bevelled solid mesh in one step. The text, font, depth and bevels stay live: select the mesh and edit them in the Attribute Editor.");
    attributeEditor::addSection("curve", "Text", textSection);
    attributeEditor::addSection("mesh", "Text", textSection);
    cmd("curve.bevelPlus", "Bevel Plus", "Curves", "bevelPlus", json::object(),
        "Turns the selected closed planar curves into a bevelled solid; curves inside others become holes. Live history.");
    cmd("curve.booleanUnion", "Curve Union", "Curves", "curveBoolean", {{"operation", "union"}},
        "Joins the selected closed planar curves into one outline, live.");
    cmd("curve.booleanSubtract", "Curve Subtract", "Curves", "curveBoolean", {{"operation", "subtract"}},
        "Cuts the other selected closed curves out of the first selected, live.");
    cmd("curve.booleanIntersect", "Curve Intersect", "Curves", "curveBoolean", {{"operation", "intersect"}},
        "Keeps what the selected closed curves all cover, live.");
    cmd("curve.booleanXor", "Curve Difference (Xor)", "Curves", "curveBoolean", {{"operation", "xor"}},
        "Keeps what an odd number of the selected closed curves cover, live.");
    cmd("curve.offsetRegion", "Offset Region", "Curves", "offsetRegion", json::object(),
        "Grows or shrinks the region the selected closed curves bound, merging where loops meet, live.");
    cmd("curve.insetSkeleton", "Inset Along Skeleton", "Curves", "insetCurve", json::object(),
        "Insets the selected closed curves along the straight skeleton: mitred corners, splitting where the shape pinches. Live.");
    cmd("curve.centerLine", "Center Line", "Curves", "curveCenterLine", json::object(),
        "Makes the centre lines (straight skeleton) of the region the selected closed curves bound, live.");
    cmd("curve.roof", "Roof from Footprint", "Curves", "roof", json::object(),
        "Builds a roof over the selected closed footprint curves: hips, valleys and ridges at one pitch, gables where asked. Live.");
    cmd("mesh.insetRegion", "Inset Region", "Mesh", "polyInsetRegion", json::object(),
        "Insets each flat region of the selected faces as one along the straight skeleton, splitting where it pinches.");
}
} // namespace lila::ui
