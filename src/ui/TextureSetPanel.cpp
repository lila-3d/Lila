#include "ui/TextureSetPanel.h"

// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "render/PaintGpu.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/NodeDef.h"
#include "tex/PaintStrokes.h"
#include "tex/SmartMaterial.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")

#include "core/Log.h"
#include "core/Paths.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/Node.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/GraphBackend.h"
#include "ui/Icons.h"
#include "ui/MaterialBrowser.h"
#include "ui/Menus.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/TexParams.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <map>
#include <optional>

namespace lila::ui
{
namespace
{

constexpr int kLayerThumb = 36;
constexpr int kRoleKind = Qt::UserRole;
constexpr int kRoleId = Qt::UserRole + 1;
constexpr int kRoleLayer = Qt::UserRole + 2;
constexpr int kRoleFolder = Qt::UserRole + 3;
constexpr const char* kAllChannels = "";

/** Lists the scene's materials, refreshed each time it opens, keeping `current` selected and listed. */
class MaterialSlotCombo : public QComboBox
{
public:
    MaterialSlotCombo(std::string current, QWidget* parent) : QComboBox(parent), mCurrent(std::move(current)) { fill(); }

    void showPopup() override
    {
        mCurrent = currentData().toString().toStdString();
        fill();
        QComboBox::showPopup();
    }

private:
    void fill()
    {
        std::vector<std::string> names;
        for (const scene::Node* m : scene::materials::all(scene::Graph::current()))
            names.push_back(m->path());
        if (std::ranges::find(names, mCurrent) == names.end())
            names.push_back(mCurrent);
        std::ranges::sort(names);
        clear();
        for (const std::string& n : names)
            addItem(QString::fromStdString(n), QString::fromStdString(n));
        setCurrentIndex(std::max(0, findData(QString::fromStdString(mCurrent))));
    }

    std::string mCurrent;
};

// Active paint layers ------------------------------------------------------------------------------------------------

struct ActiveState
{
    std::map<std::string, std::string> layers;
    std::map<int, std::function<void(const std::string&, const std::string&)>> listeners;
    int next = 1;
};

ActiveState& activeState()
{
    static ActiveState s;
    return s;
}

std::optional<tex::StackState> stackOf(const std::string& set)
{
    scene::Node* n = tex::findSet(scene::Graph::current(), set);
    if (!n)
        return std::nullopt;
    try
    {
        return tex::readStack(tex::readGraph(*n), tex::readSettings(*n));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::vector<TextureSetPanel*>& panelList()
{
    static std::vector<TextureSetPanel*> list;
    return list;
}

TextureSetPanel*& lastFocusedPanel()
{
    static TextureSetPanel* p = nullptr;
    return p;
}

// Names and icons ----------------------------------------------------------------------------------------------------

QString kindIcon(const std::string& kind, bool open = false)
{
    if (kind == "fill")
        return "bucket";
    if (kind == "paint")
        return "brush";
    if (kind == "procedural")
        return "topology-star-3";
    if (kind == "folder")
        return open ? "folder-open" : "folder";
    if (kind == "instance")
        return "link";
    if (kind == "filter")
        return "filter";
    return "square";
}

QString maskIcon(const std::string& kind)
{
    if (kind == "fill")
        return "square";
    if (kind == "generator")
        return "wand";
    if (kind == "filter")
        return "filter";
    if (kind == "levels")
        return "adjustments";
    if (kind == "paint")
        return "brush";
    if (kind == "anchor")
        return "anchor";
    return "mask";
}

QString capitalised(const std::string& s)
{
    QString q = qs(s);
    if (!q.isEmpty())
        q[0] = q[0].toUpper();
    return q;
}

QString channelLabel(const std::string& c)
{
    static const std::map<std::string, const char*> kShort = {{"baseColor", "BC"}, {"roughness", "R"}, {"metallic", "M"}, {"normal", "N"},
                                                             {"height", "H"},     {"ao", "AO"},       {"emissive", "E"}, {"opacity", "Op"}};
    if (auto it = kShort.find(c); it != kShort.end())
        return it->second;
    return capitalised(c.substr(0, 2));
}

QString nodeLabel(const std::string& type)
{
    const tex::NodeDef* d = tex::findNode(type);
    return d && !d->label.empty() ? qs(d->label) : qs(type);
}

QString maskName(const json& m)
{
    if (std::string n = m.value("name", ""); !n.empty())
        return qs(n);
    const std::string kind = m.value("kind", "fill");
    if (kind == "generator" || kind == "filter")
        return nodeLabel(m.value("type", ""));
    if (kind == "fill")
        return m.value("value", 1.0) >= 0.5 ? "Fill White" : "Fill Black";
    if (kind == "anchor")
        return "Anchor " + qs(m.value("anchor", ""));
    return capitalised(kind);
}

/** The node types usable as filters: Filter category, with an input and an output. */
std::vector<const tex::NodeDef*> filterNodes()
{
    std::vector<const tex::NodeDef*> out;
    for (const tex::NodeDef* d : tex::allNodes())
        if (d->category == "Filter" && !d->inputs.empty() && !d->outputs.empty())
            out.push_back(d);
    return out;
}

/** A node as texGraphInfo lists it, for texParams::nodePanel: the definition's parameters with values. */
json defNodeJson(const std::string& name, const std::string& type, const json& values)
{
    json j = {{"name", name}, {"type", type}};
    const tex::NodeDef* d = tex::findNode(type);
    if (!d)
    {
        j["error"] = "unknown node type " + type;
        return j;
    }
    j["label"] = d->label;
    j["category"] = d->category;
    json prms = json::array();
    for (const tex::ParamDecl& p : d->params)
    {
        json e = {{"id", p.id}, {"type", tex::paramTypeName(p.type)}, {"default", p.defaultValue}};
        e["value"] = values.is_object() && values.contains(p.id) ? values[p.id] : p.defaultValue;
        if (!p.options.empty())
            e["options"] = p.options;
        for (auto [key, v] : {std::pair{"min", &p.min}, {"max", &p.max}, {"softMin", &p.softMin}, {"softMax", &p.softMax}, {"step", &p.step}})
            if (*v)
                e[key] = **v;
        if (!p.label.empty())
            e["label"] = p.label;
        if (!p.doc.empty())
            e["doc"] = p.doc;
        prms.push_back(e);
    }
    j["params"] = prms;
    j["inputs"] = json::array();
    return j;
}

QLabel* dim(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet("color: #8a8a8a;");
    return l;
}

QLabel* boldHeading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QSpinBox* percentSpin(QWidget* parent, double opacity)
{
    auto* s = new QSpinBox(parent);
    s->setRange(0, 100);
    s->setSuffix("%");
    s->setKeyboardTracking(false);
    s->setValue(int(std::lround(opacity * 100.0)));
    s->setToolTip("Opacity");
    return s;
}

/** A label that reports double-clicks, for inline rename. */
class LayerNameLabel final : public QLabel
{
public:
    using QLabel::QLabel;
    std::function<void()> onDoubleClick;

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (onDoubleClick)
        {
            onDoubleClick();
            e->accept();
            return;
        }
        QLabel::mouseDoubleClickEvent(e);
    }
};

/** The stack's rows: drags reorder and drop into folders through the panel, library materials drop in as layers. */
class LayerTree final : public QTreeWidget
{
public:
    explicit LayerTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setColumnCount(1);
        setRootIsDecorated(true);
        setIndentation(14);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setProperty("hotkeyContext", "textureSets");
    }

    std::function<void(QTreeWidgetItem* dragged, QTreeWidgetItem* target, TextureSetPanel::Drop where)> onMove;
    std::function<void(const std::string& material, QTreeWidgetItem* target, TextureSetPanel::Drop where)> onMaterial;

    /** Where a drop at pos lands: above or below a row, or into a folder's middle. */
    TextureSetPanel::Drop dropAt(QTreeWidgetItem* target, QPoint pos) const
    {
        if (!target)
            return TextureSetPanel::Drop::Below;
        const QRect r = visualItemRect(target);
        const int y = pos.y() - r.top();
        if (target->data(0, kRoleFolder).toBool() && y > r.height() / 4 && y < r.height() * 3 / 4)
            return TextureSetPanel::Drop::Into;
        return y < r.height() / 2 ? TextureSetPanel::Drop::Above : TextureSetPanel::Drop::Below;
    }

protected:
    QStringList mimeTypes() const override
    {
        QStringList t = QTreeWidget::mimeTypes();
        t << kMaterialMime;
        return t;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMaterialMime))
        {
            e->acceptProposedAction();
            return;
        }
        QTreeWidget::dragEnterEvent(e);
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeWidget::dragMoveEvent(e);
        if (e->mimeData()->hasFormat(kMaterialMime))
            e->acceptProposedAction();
    }

    void dropEvent(QDropEvent* e) override
    {
        const QPoint pos = e->position().toPoint();
        QTreeWidgetItem* target = itemAt(pos);
        const TextureSetPanel::Drop where = dropAt(target, pos);
        if (e->mimeData()->hasFormat(kMaterialMime))
        {
            if (onMaterial)
                onMaterial(e->mimeData()->data(kMaterialMime).toStdString(), target, where);
        }
        else if (e->source() == this && currentItem() && onMove)
            onMove(currentItem(), target, where);
        // The panel rebuilds the rows from the stack; Qt must not move them itself.
        e->setDropAction(Qt::IgnoreAction);
        e->accept();
    }
};

std::vector<QTreeWidgetItem*> allItems(QTreeWidget* tree)
{
    std::vector<QTreeWidgetItem*> out;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        out.push_back(*it);
    return out;
}

struct Thumb
{
    QPointer<QLabel> label;
    QPointer<QToolButton> button;
    texPreview::Request request;
    QString fallback;
};
/** Texture Graph editors on another set switch to set, so the graph, the panel and the viewport show one set. */
void showInTextureGraphs(const std::string& set)
{
    if (set.empty())
        return;
    for (NodeEditor* e : NodeEditor::all())
        if (e->backend().id() == kTextureGraphBackend && e->group().toStdString() != set)
            e->setGroup(QString::fromStdString(set));
}

/** The set the first Texture Graph editor shows, or empty. */
std::string textureGraphSet()
{
    for (NodeEditor* e : NodeEditor::all())
        if (e->backend().id() == kTextureGraphBackend && !e->group().isEmpty())
            return e->group().toStdString();
    return {};
}

} // namespace

// Panel ----------------------------------------------------------------------------------------------------------------

struct TextureSetPanel::Impl
{
    TextureSetPanel* self = nullptr;
    std::string set;
    json view;
    tex::SetSettings settings;
    std::string viewKey, settingsKey, shelfKey;
    std::string selLayer, selMask;
    std::map<std::string, bool> expanded;
    bool building = false;
    bool keepProps = false;
    bool thumbsQueued = false;
    std::string propsKey;

    QComboBox* sets = nullptr;
    QToolButton* follow = nullptr;
    QToolButton* settingsToggle = nullptr;
    QWidget* settingsBox = nullptr;
    QComboBox* channel = nullptr;
    QWidget* bannerBox = nullptr;
    QLabel* bannerText = nullptr;
    QPushButton* rebuild = nullptr;
    LayerTree* tree = nullptr;
    QScrollArea* propsScroll = nullptr;
    QWidget* props = nullptr;
    QListWidget* shelf = nullptr;
    QWidget* empty = nullptr;
    QTimer* refreshTimer = nullptr;
    int sceneListener = 0;
    int activeListener = 0;
    int graphListener = 0;
    std::vector<Thumb> thumbs;

    // Reading ------------------------------------------------------------------------------------------------------------

    scene::Node* setNode() const { return set.empty() ? nullptr : tex::findSet(scene::Graph::current(), set); }

    const json* findLayer(const json& layers, const std::string& id) const
    {
        for (const json& l : layers)
        {
            if (l.value("id", "") == id)
                return &l;
            if (auto kids = l.find("children"); kids != l.end())
                if (const json* c = findLayer(*kids, id))
                    return c;
        }
        return nullptr;
    }

    const json* layer(const std::string& id) const { return view.is_object() ? findLayer(view["layers"], id) : nullptr; }

    const json* mask(const std::string& layerId, const std::string& id) const
    {
        const json* l = layer(layerId);
        if (!l)
            return nullptr;
        auto masks = l->find("masks");
        if (masks == l->end())
            return nullptr;
        for (const json& m : *masks)
            if (m.value("id", "") == id)
                return &m;
        return nullptr;
    }

    std::vector<std::string> channelNames() const
    {
        std::vector<std::string> out;
        for (const tex::Channel& c : settings.channels)
            out.push_back(c.name);
        return out;
    }

    /** The channel the rows' blend and opacity edit; empty for all. */
    std::string shownChannel() const { return channel ? channel->currentData().toString().toStdString() : std::string(); }

    bool graphMode() const { return view.is_object() && view.value("mode", "") == "graph"; }

    // Commands ----------------------------------------------------------------------------------------------------------

    json args(std::initializer_list<json> list) const
    {
        json a = json::array({set});
        for (const json& x : list)
            a.push_back(x);
        return {{"args", a}};
    }

    json setLayer(const std::string& id, const json& patch) { return runUi("texSetLayer", args({id, patch})); }

    json setMask(const std::string& layerId, const std::string& maskId, const json& patch)
    {
        return runUi("texSetMask", args({layerId, maskId, patch}));
    }

    /** A layer channel's settings with the given keys changed, keeping the rest as resolved (so an explicit entry never flips it). */
    json channelEntry(const json& l, const std::string& c, const json& changes) const
    {
        json r = l.value("resolved", json::object()).value(c, json::object());
        json e = {{"enabled", r.value("enabled", true)}, {"opacity", r.value("opacity", 1.0)}};
        const json explicitMode = l.value("channels", json::object()).value(c, json::object()).value("mode", json());
        if (explicitMode.is_string())
            e["mode"] = explicitMode;
        e.update(changes);
        return e;
    }

    /** The channels a row edit applies to: the shown one, or every channel the layer writes (blend modes skip normal and height). */
    std::vector<std::string> targetChannels(const json& l, bool forMode) const
    {
        const std::string shown = shownChannel();
        if (!shown.empty())
            return {shown};
        std::vector<std::string> out;
        const json resolved = l.value("resolved", json::object());
        for (const tex::Channel& c : settings.channels)
            if (resolved.value(c.name, json::object()).value("enabled", false) && (!forMode || (c.name != "normal" && c.name != "height")))
                out.push_back(c.name);
        return out;
    }

    void setRowOpacity(const std::string& id, double opacity)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json ch = json::object();
        for (const std::string& c : targetChannels(*l, false))
            ch[c] = channelEntry(*l, c, {{"opacity", opacity}});
        if (ch.empty())
            return;
        setLayer(id, {{"channels", ch}});
    }

    void setRowMode(const std::string& id, const std::string& mode)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json ch = json::object();
        for (const std::string& c : targetChannels(*l, true))
            ch[c] = channelEntry(*l, c, {{"mode", mode}});
        if (ch.empty())
            return;
        setLayer(id, {{"channels", ch}});
    }

    void toggleChannel(const std::string& id, const std::string& c, bool on)
    {
        const json* l = layer(id);
        if (!l)
            return;
        json patch = {{"channels", {{c, channelEntry(*l, c, {{"enabled", on}})}}}};
        // A fill layer turned on for a channel it has no value for starts from the channel's default.
        if (on && l->value("kind", "") == "fill" && !l->value("material", json::object()).contains(c))
            if (const tex::Channel* ch = settings.channel(c))
                patch["material"] = {{c, tex::channelDefault(c, ch->type)}};
        setLayer(id, patch);
    }

    /** Placement flags for a new layer: above the selected layer (in its folder), else on top. */
    json placement() const
    {
        if (!selLayer.empty() && layer(selLayer))
            return {{"above", selLayer}};
        return json::object();
    }

    std::string addLayer(const std::string& kind, const std::string& filter)
    {
        if (!setNode())
            return {};
        json call = args({kind});
        call.update(placement());
        if (kind == "filter")
        {
            if (filter.empty())
                return {};
            call["filter"] = filter;
            call["name"] = nodeLabel(filter).toStdString();
        }
        if (kind == "instance")
        {
            const json* src = layer(selLayer);
            if (!src)
            {
                log::warn("Select the layer to instance first");
                return {};
            }
            call["source"] = selLayer;
            call["name"] = src->value("name", selLayer) + " instance";
        }
        std::string id;
        {
            UndoChunk chunk("Add " + capitalised(kind).toStdString() + " Layer");
            json r = runUi("texAddLayer", call);
            id = r.is_object() ? r.value("id", std::string()) : std::string();
            if (!id.empty() && kind == "paint")
                wirePaint(id);
        }
        if (!id.empty())
        {
            selLayer = id;
            selMask.clear();
            if (kind == "paint")
                textureSets::setActiveLayer(set, id);
        }
        self->refresh();
        return id;
    }

    /** Points a new paint layer's channels and mask at its paint buffer, as the paint tool does. */
    void wirePaint(const std::string& id)
    {
        scene::Node* n = setNode();
        if (!n)
            return;
        std::vector<std::string> channels = channelNames();
        channels.push_back(tex::paint::kMaskChannel);
        render::paint::wireLayer(*n, id, channels);
    }

    std::string addMask(const std::string& kind, const std::string& type)
    {
        if (selLayer.empty() || !layer(selLayer))
        {
            log::warn("Select a layer to add a mask to");
            return {};
        }
        json r;
        if (kind == "generator")
            r = runUi("texAddGenerator", args({selLayer, type}));
        else
        {
            json call = args({selLayer, kind == "black" || kind == "white" ? std::string("fill") : kind});
            if (kind == "black")
                call["value"] = 0.0;
            if (kind == "filter")
                call["type"] = type;
            if (kind == "anchor")
                call["anchor"] = type;
            r = runUi("texAddMask", call);
        }
        std::string id = r.is_object() ? r.value("id", std::string()) : std::string();
        if (!id.empty())
        {
            selMask = id;
            expanded[selLayer] = true;
        }
        self->refresh();
        return id;
    }

    // Building -----------------------------------------------------------------------------------------------------------

    void requestThumb(QLabel* label, QToolButton* button, const texPreview::Request& r, const QString& fallback)
    {
        thumbs.push_back({label, button, r, fallback});
    }

    void updateThumbs()
    {
        thumbsQueued = false;
        QPointer<TextureSetPanel> guard(self);
        for (Thumb& t : thumbs)
        {
            if (!t.label && !t.button)
                continue;
            texPreview::Result r = texPreview::get(t.request, [guard] {
                if (guard && !guard->d->thumbsQueued)
                {
                    guard->d->thumbsQueued = true;
                    QTimer::singleShot(0, guard, [guard] {
                        if (guard)
                            guard->d->updateThumbs();
                    });
                }
            });
            QPixmap pm;
            if (r.image)
            {
                const std::string name = t.request.socket.empty() ? t.request.channel : t.request.socket;
                QImage img = texPreview::toQImage(*r.image, r.type, texPreview::isColor(r.type, name));
                pm = QPixmap::fromImage(img.scaled(kLayerThumb - 2, kLayerThumb - 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
            else if (!r.error.empty())
                pm = icon(t.fallback).pixmap(20, 20);
            if (pm.isNull())
                continue;
            if (t.label)
                t.label->setPixmap(pm);
            if (t.button)
                t.button->setIcon(QIcon(pm));
        }
    }

    QLabel* thumbLabel(QWidget* parent, const QString& name)
    {
        auto* l = new QLabel(parent);
        l->setObjectName(name);
        l->setFixedSize(kLayerThumb, kLayerThumb);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet("border: 1px solid #3c3c3c; background: #1e1e1e;");
        return l;
    }

    void beginRename(QWidget* row, LayerNameLabel* label, const std::string& id, const std::string& current, const std::string& maskOf)
    {
        auto* edit = new QLineEdit(qs(current), row);
        edit->setObjectName("rename:" + qs(id));
        auto* box = qobject_cast<QBoxLayout*>(label->parentWidget()->layout());
        if (box)
        {
            int at = box->indexOf(label);
            box->insertWidget(at, edit, 1);
        }
        label->hide();
        edit->selectAll();
        edit->setFocus();
        QPointer<QLineEdit> guard(edit);
        QObject::connect(edit, &QLineEdit::editingFinished, edit, [this, guard, id, current, maskOf] {
            if (!guard || guard->property("done").toBool())
                return;
            guard->setProperty("done", true);
            const std::string next = guard->text().trimmed().toStdString();
            if (!next.empty() && next != current)
            {
                if (maskOf.empty())
                    setLayer(id, {{"name", next}});
                else
                    setMask(maskOf, id, {{"name", next}});
            }
            QTimer::singleShot(0, self, [this] { self->refresh(); });
        });
    }

    QWidget* layerRow(const json& l)
    {
        const std::string id = l.value("id", "");
        const std::string kind = l.value("kind", "fill");
        const bool enabled = l.value("enabled", true);
        const json resolved = l.value("resolved", json::object());
        auto* row = new QWidget();
        row->setObjectName("layer:" + qs(id));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(2, 2, 4, 2);
        h->setSpacing(4);

        auto* eye = toolButton(row, enabled ? "eye" : "eye-off", "Show or hide the layer");
        eye->setObjectName("eye:" + qs(id));
        QObject::connect(eye, &QToolButton::clicked, row, [this, id, enabled] { setLayer(id, {{"enabled", !enabled}}); });
        h->addWidget(eye);

        QLabel* thumb = thumbLabel(row, "thumb:" + qs(id));
        const std::string shown = shownChannel();
        if (kind == "folder")
            thumb->setPixmap(icon(kindIcon(kind)).pixmap(22, 22));
        else
        {
            std::string socket = shown.empty() ? std::string("baseColor") : shown;
            if (!settings.channel(socket) && !settings.channels.empty())
                socket = settings.channels.front().name;
            requestThumb(thumb, nullptr, {set, id, socket, {}, 64}, kindIcon(kind));
        }
        h->addWidget(thumb);

        const json masks = l.value("masks", json::array());
        if (!masks.empty())
        {
            auto* mt = new QToolButton(row);
            mt->setObjectName("maskThumb:" + qs(id));
            mt->setAutoRaise(true);
            mt->setIconSize(QSize(kLayerThumb - 4, kLayerThumb - 4));
            mt->setFixedSize(kLayerThumb, kLayerThumb);
            mt->setIcon(icon("mask"));
            mt->setToolTip("Mask stack: click to show or hide it");
            QObject::connect(mt, &QToolButton::clicked, row, [this, id] {
                expanded[id] = !expanded[id];
                for (QTreeWidgetItem* it : allItems(tree))
                    if (it->data(0, kRoleKind).toInt() == 0 && it->data(0, kRoleId).toString().toStdString() == id)
                        it->setExpanded(expanded[id]);
            });
            if (kind != "folder")
                requestThumb(nullptr, mt, {set, id, "mask", {}, 64}, "mask");
            h->addWidget(mt);
        }

        auto* mid = new QWidget(row);
        auto* v = new QVBoxLayout(mid);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        auto* top = new QWidget(mid);
        auto* th = new QHBoxLayout(top);
        th->setContentsMargins(0, 0, 0, 0);
        th->setSpacing(3);
        auto* kindLbl = new QLabel(top);
        kindLbl->setPixmap(icon(kindIcon(kind)).pixmap(14, 14));
        kindLbl->setToolTip(capitalised(kind) + " layer");
        th->addWidget(kindLbl);
        auto* name = new LayerNameLabel(qs(l.value("name", id)), top);
        name->setObjectName("name:" + qs(id));
        name->setToolTip("Double-click to rename");
        if (!enabled)
            name->setStyleSheet("color: #7a7a7a;");
        if (kind == "paint" && textureSets::activeLayer(set) == id)
        {
            QFont f = name->font();
            f.setBold(true);
            name->setFont(f);
            name->setToolTip("The active paint layer: painting goes here. Double-click to rename");
        }
        const std::string current = l.value("name", id);
        name->onDoubleClick = [this, row, name, id, current] { beginRename(row, name, id, current, {}); };
        th->addWidget(name, 1);
        if (!l.value("anchor", "").empty())
        {
            auto* a = new QLabel(top);
            a->setPixmap(icon("anchor").pixmap(12, 12));
            a->setToolTip("Anchor " + qs(l.value("anchor", "")));
            th->addWidget(a);
        }
        v->addWidget(top);

        auto* chRow = new QWidget(mid);
        auto* ch = new QHBoxLayout(chRow);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->setSpacing(1);
        for (const tex::Channel& c : settings.channels)
        {
            const bool on = resolved.value(c.name, json::object()).value("enabled", false);
            auto* b = new QToolButton(chRow);
            b->setObjectName("channel:" + qs(id) + ":" + qs(c.name));
            b->setText(channelLabel(c.name));
            b->setCheckable(true);
            b->setChecked(on);
            b->setAutoRaise(true);
            b->setToolTip(qs(c.name) + (on ? ": on, click to leave it to the layers below" : ": off, click to write it"));
            b->setStyleSheet(on ? "QToolButton { font-size: 8pt; padding: 0 2px; color: #e0e0e0; }"
                                : "QToolButton { font-size: 8pt; padding: 0 2px; color: #5e5e5e; }");
            QObject::connect(b, &QToolButton::toggled, row, [this, id, c = c.name](bool checked) { toggleChannel(id, c, checked); });
            ch->addWidget(b);
        }
        ch->addStretch(1);
        v->addWidget(chRow);
        h->addWidget(mid, 1);

        auto* right = new QWidget(row);
        auto* rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->setSpacing(1);
        std::string modeChannel = shown;
        if (modeChannel.empty())
            for (const tex::Channel& c : settings.channels)
                if (c.name != "normal" && c.name != "height" && resolved.value(c.name, json::object()).value("enabled", false))
                {
                    modeChannel = c.name;
                    break;
                }
        if (modeChannel.empty() && !settings.channels.empty())
            modeChannel = settings.channels.front().name;
        const json mr = resolved.value(modeChannel, json::object());
        auto* blend = new QComboBox(right);
        blend->setObjectName("blend:" + qs(id));
        blend->setToolTip(shown.empty() ? "Blend mode of every channel (normal and height keep theirs)" : "Blend mode of " + qs(shown));
        const std::vector<std::string> normalModes = {"rnm", "normal"};
        const auto& modes = modeChannel == "normal" && !shown.empty() ? normalModes : tex::layerBlendModes();
        for (const std::string& m : modes)
            blend->addItem(capitalised(m), qs(m));
        blend->setCurrentIndex(std::max(0, blend->findData(qs(mr.value("mode", "normal")))));
        QObject::connect(blend, &QComboBox::activated, row, [this, id, blend](int) { setRowMode(id, blend->currentData().toString().toStdString()); });
        rv->addWidget(blend);
        auto* opacity = percentSpin(right, mr.value("opacity", 1.0));
        opacity->setObjectName("opacity:" + qs(id));
        QObject::connect(opacity, &QSpinBox::valueChanged, row, [this, id](int v) { setRowOpacity(id, v / 100.0); });
        rv->addWidget(opacity);
        h->addWidget(right);
        return row;
    }

    QWidget* maskRow(const std::string& layerId, const json& m)
    {
        const std::string id = m.value("id", "");
        const bool enabled = m.value("enabled", true);
        auto* row = new QWidget();
        row->setObjectName("mask:" + qs(layerId) + ":" + qs(id));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(2, 1, 4, 1);
        h->setSpacing(4);
        auto* eye = toolButton(row, enabled ? "eye" : "eye-off", "Turn the mask on or off");
        eye->setObjectName("maskEye:" + qs(id));
        QObject::connect(eye, &QToolButton::clicked, row, [this, layerId, id, enabled] { setMask(layerId, id, {{"enabled", !enabled}}); });
        h->addWidget(eye);
        auto* k = new QLabel(row);
        k->setPixmap(icon(maskIcon(m.value("kind", ""))).pixmap(16, 16));
        k->setToolTip(capitalised(m.value("kind", "")) + " mask");
        h->addWidget(k);
        auto* name = new LayerNameLabel(maskName(m), row);
        name->setObjectName("maskName:" + qs(id));
        if (!enabled)
            name->setStyleSheet("color: #7a7a7a;");
        const std::string current = maskName(m).toStdString();
        name->onDoubleClick = [this, row, name, id, current, layerId] { beginRename(row, name, id, current, layerId); };
        h->addWidget(name, 1);
        auto* inv = toolButton(row, "circle-half-2", "Invert");
        inv->setObjectName("maskInvert:" + qs(id));
        inv->setCheckable(true);
        inv->setChecked(m.value("invert", false));
        QObject::connect(inv, &QToolButton::toggled, row, [this, layerId, id](bool on) { setMask(layerId, id, {{"invert", on}}); });
        h->addWidget(inv);
        auto* mode = new QComboBox(row);
        mode->setObjectName("maskMode:" + qs(id));
        mode->setToolTip("How it combines with the masks below");
        for (const std::string& md : tex::maskBlendModes())
            mode->addItem(capitalised(md), qs(md));
        mode->setCurrentIndex(std::max(0, mode->findData(qs(m.value("mode", "normal")))));
        QObject::connect(mode, &QComboBox::activated, row,
                         [this, layerId, id, mode](int) { setMask(layerId, id, {{"mode", mode->currentData().toString().toStdString()}}); });
        h->addWidget(mode);
        auto* op = percentSpin(row, m.value("opacity", 1.0));
        op->setObjectName("maskOpacity:" + qs(id));
        QObject::connect(op, &QSpinBox::valueChanged, row, [this, layerId, id](int v) { setMask(layerId, id, {{"opacity", v / 100.0}}); });
        h->addWidget(op);
        return row;
    }

    QTreeWidgetItem* addRow(QTreeWidgetItem* parent, QWidget* row)
    {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setSizeHint(0, row->sizeHint());
        tree->setItemWidget(item, 0, row);
        return item;
    }

    void addLayers(QTreeWidgetItem* parent, const json& layers)
    {
        for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        {
            const json& l = *it;
            const std::string id = l.value("id", "");
            const bool folder = l.value("kind", "") == "folder";
            QTreeWidgetItem* item = addRow(parent, layerRow(l));
            item->setData(0, kRoleKind, 0);
            item->setData(0, kRoleId, qs(id));
            item->setData(0, kRoleLayer, qs(id));
            item->setData(0, kRoleFolder, folder);
            const json masks = l.value("masks", json::array());
            for (auto m = masks.rbegin(); m != masks.rend(); ++m)
            {
                QTreeWidgetItem* mi = addRow(item, maskRow(id, *m));
                mi->setData(0, kRoleKind, 1);
                mi->setData(0, kRoleId, qs(m->value("id", "")));
                mi->setData(0, kRoleLayer, qs(id));
            }
            if (folder)
                addLayers(item, l.value("children", json::array()));
            auto e = expanded.find(id);
            item->setExpanded(e != expanded.end() ? e->second : folder);
        }
    }

    void rebuildTree()
    {
        building = true;
        const int scroll = tree->verticalScrollBar()->value();
        thumbs.clear();
        tree->clear();
        if (view.is_object())
            addLayers(nullptr, view["layers"]);
        QTreeWidgetItem* sel = nullptr;
        for (QTreeWidgetItem* it : allItems(tree))
        {
            const int kind = it->data(0, kRoleKind).toInt();
            const std::string id = it->data(0, kRoleId).toString().toStdString();
            const std::string owner = it->data(0, kRoleLayer).toString().toStdString();
            if ((selMask.empty() && kind == 0 && id == selLayer) || (!selMask.empty() && kind == 1 && id == selMask && owner == selLayer))
                sel = it;
        }
        if (sel)
            tree->setCurrentItem(sel);
        else
        {
            selLayer.clear();
            selMask.clear();
        }
        tree->verticalScrollBar()->setValue(scroll);
        building = false;
        updateThumbs();
    }

    void rebuildSettings()
    {
        delete settingsBox->layout();
        // Children of nested layouts too, which taking the top layout's items would miss.
        for (QWidget* w : settingsBox->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        {
            w->hide();
            w->deleteLater();
        }
        auto* v = new QVBoxLayout(settingsBox);
        v->setContentsMargins(4, 2, 4, 4);
        v->setSpacing(3);
        if (!setNode())
            return;

        v->addWidget(boldHeading(settingsBox, "Channels"));
        auto* chGrid = new QWidget(settingsBox);
        auto* g = new QGridLayout(chGrid);
        g->setContentsMargins(0, 0, 0, 0);
        g->setHorizontalSpacing(6);
        g->setVerticalSpacing(2);
        const tex::ChannelList extra = tex::extraChannels(settings);
        for (size_t i = 0; i < settings.channels.size(); ++i)
        {
            const tex::Channel& c = settings.channels[i];
            g->addWidget(new QLabel(qs(c.name), chGrid), int(i), 0);
            g->addWidget(dim(chGrid, qs(tex::portTypeName(c.type))), int(i), 1);
            auto* bits = new QComboBox(chGrid);
            bits->setObjectName("bits:" + qs(c.name));
            bits->addItem("8 bit", 8);
            bits->addItem("16 bit", 16);
            bits->addItem("32 bit float", 32);
            bits->setCurrentIndex(std::max(0, bits->findData(c.bits)));
            QObject::connect(bits, &QComboBox::activated, chGrid, [this, i, bits](int) {
                json s = settings.toJson();
                s["channels"][i]["bits"] = bits->currentData().toInt();
                runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
            });
            g->addWidget(bits, int(i), 2);
            if (std::any_of(extra.begin(), extra.end(), [&](const auto& e) { return e.first == c.name; }))
            {
                auto* rm = toolButton(chGrid, "trash", "Remove the channel");
                QObject::connect(rm, &QToolButton::clicked, chGrid, [this, i] {
                    json s = settings.toJson();
                    s["channels"].erase(s["channels"].begin() + std::ptrdiff_t(i));
                    runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
                });
                g->addWidget(rm, int(i), 3);
            }
        }
        v->addWidget(chGrid);
        auto* addRowW = new QWidget(settingsBox);
        auto* ah = new QHBoxLayout(addRowW);
        ah->setContentsMargins(0, 0, 0, 0);
        auto* newName = new QLineEdit(addRowW);
        newName->setObjectName("newChannel");
        newName->setPlaceholderText("New channel (wetness, strokeDir...)");
        auto* newType = new QComboBox(addRowW);
        for (const char* t : {"gray", "color", "vector"})
            newType->addItem(t, t);
        auto* add = toolButton(addRowW, "plus", "Add the channel");
        auto addChannel = [this, newName, newType] {
            const std::string n = newName->text().trimmed().toStdString();
            if (n.empty() || settings.channel(n))
                return;
            json s = settings.toJson();
            s["channels"].push_back({{"name", n}, {"type", newType->currentData().toString().toStdString()}, {"bits", 8}});
            runUi("texSetSettings", args({json{{"channels", s["channels"]}}}));
        };
        QObject::connect(add, &QToolButton::clicked, addRowW, addChannel);
        QObject::connect(newName, &QLineEdit::returnPressed, addRowW, addChannel);
        ah->addWidget(newName, 1);
        ah->addWidget(newType);
        ah->addWidget(add);
        v->addWidget(addRowW);

        v->addWidget(boldHeading(settingsBox, "Resolution"));
        auto sizeBox = [](QWidget* parent, int value, bool allowDefault) {
            auto* b = new QComboBox(parent);
            if (allowDefault)
                b->addItem("Set", 0);
            for (int s = 512; s <= 16384; s *= 2)
                b->addItem(QString::number(s), s);
            b->setCurrentIndex(std::max(0, b->findData(value)));
            return b;
        };
        auto* res = new QWidget(settingsBox);
        auto* rg = new QGridLayout(res);
        rg->setContentsMargins(0, 0, 0, 0);
        rg->setHorizontalSpacing(4);
        rg->setVerticalSpacing(2);
        rg->addWidget(new QLabel("Default", res), 0, 0);
        auto* w = sizeBox(res, settings.width, false);
        w->setObjectName("width");
        auto* hgt = sizeBox(res, settings.height, false);
        hgt->setObjectName("height");
        auto setSize = [this, w, hgt] {
            runUi("texSetSettings", args({json{{"width", w->currentData().toInt()}, {"height", hgt->currentData().toInt()}}}));
        };
        QObject::connect(w, &QComboBox::activated, res, setSize);
        QObject::connect(hgt, &QComboBox::activated, res, setSize);
        rg->addWidget(w, 0, 1);
        rg->addWidget(new QLabel("x", res), 0, 2);
        rg->addWidget(hgt, 0, 3);
        for (size_t i = 0; i < settings.tiles.size(); ++i)
        {
            const tex::Tile& t = settings.tiles[i];
            const int r = int(i) + 1;
            rg->addWidget(new QLabel(QString::number(t.udim), res), r, 0);
            auto* tw = sizeBox(res, t.width, true);
            tw->setObjectName("tileWidth:" + QString::number(t.udim));
            auto* th = sizeBox(res, t.height, true);
            tw->setToolTip("This tile's width; Set uses the default");
            auto setTile = [this, i, tw, th] {
                json s = settings.toJson();
                json& tile = s["tiles"][i];
                tile.erase("width");
                tile.erase("height");
                if (int v = tw->currentData().toInt(); v > 0)
                    tile["width"] = v;
                if (int v = th->currentData().toInt(); v > 0)
                    tile["height"] = v;
                runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
            };
            QObject::connect(tw, &QComboBox::activated, res, setTile);
            QObject::connect(th, &QComboBox::activated, res, setTile);
            rg->addWidget(tw, r, 1);
            rg->addWidget(new QLabel("x", res), r, 2);
            rg->addWidget(th, r, 3);
            if (settings.tiles.size() > 1)
            {
                auto* rm = toolButton(res, "trash", "Remove the tile");
                QObject::connect(rm, &QToolButton::clicked, res, [this, i] {
                    json s = settings.toJson();
                    s["tiles"].erase(s["tiles"].begin() + std::ptrdiff_t(i));
                    runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
                });
                rg->addWidget(rm, r, 4);
            }
        }
        v->addWidget(res);
        auto* tileRow = new QWidget(settingsBox);
        auto* tr = new QHBoxLayout(tileRow);
        tr->setContentsMargins(0, 0, 0, 0);
        auto* udim = new QSpinBox(tileRow);
        udim->setRange(1001, 1999);
        int next = 1001;
        for (const tex::Tile& t : settings.tiles)
            next = std::max(next, t.udim + 1);
        udim->setValue(std::min(next, 1999));
        auto* addTile = new QPushButton(icon("plus"), "Add UDIM Tile", tileRow);
        QObject::connect(addTile, &QPushButton::clicked, tileRow, [this, udim] {
            json s = settings.toJson();
            for (const json& t : s["tiles"])
                if (t.value("udim", 0) == udim->value())
                    return;
            s["tiles"].push_back({{"udim", udim->value()}});
            runUi("texSetSettings", args({json{{"tiles", s["tiles"]}}}));
        });
        tr->addWidget(udim);
        tr->addWidget(addTile, 1);
        v->addWidget(tileRow);

        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        auto* conv = new QComboBox(settingsBox);
        conv->addItem("OpenGL (Y+)", "opengl");
        conv->addItem("DirectX (Y-)", "directx");
        conv->setCurrentIndex(std::max(0, conv->findData(qs(settings.normalConvention))));
        QObject::connect(conv, &QComboBox::activated, settingsBox,
                         [this, conv](int) { runUi("texSetSettings", args({json{{"normalConvention", conv->currentData().toString().toStdString()}}})); });
        form->addRow("Normals", conv);
        auto cmSpin = [&](double value, double lo, double hi, const char* key, const char* tip) {
            auto* sp = new QDoubleSpinBox(settingsBox);
            sp->setRange(lo, hi);
            sp->setDecimals(2);
            sp->setSuffix(" cm");
            sp->setValue(value);
            sp->setToolTip(tip);
            QObject::connect(sp, &QDoubleSpinBox::editingFinished, settingsBox, [this, sp, key] {
                runUi("texSetSettings", args({json{{key, sp->value()}}}));
            });
            return sp;
        };
        form->addRow("Height depth", cmSpin(settings.heightDepth, 0.0, 1000.0, "heightDepth",
                                            "cm from height 0 to height 1: the relief the height channel stands for"));
        auto* uvRow = new QWidget(settingsBox);
        auto* uvl = new QHBoxLayout(uvRow);
        uvl->setContentsMargins(0, 0, 0, 0);
        uvl->addWidget(cmSpin(settings.uvWidth, 0.01, 100000.0, "uvWidth", "World size of the UV square along U"), 1);
        uvl->addWidget(new QLabel("x", uvRow));
        uvl->addWidget(cmSpin(settings.uvHeight, 0.01, 100000.0, "uvHeight", "World size of the UV square along V"), 1);
        form->addRow("UV size", uvRow);
        auto* fromHeight = new QCheckBox("Normals from height", settingsBox);
        fromHeight->setChecked(settings.normalFromHeight);
        fromHeight->setToolTip("Exports write the normal as detail over a 16-bit height, for materials that rebuild normals from it "
                               "(Unreal's MF_LilaHeightNormal), so steep sides light as walls");
        QObject::connect(fromHeight, &QCheckBox::toggled, settingsBox,
                         [this](bool on) { runUi("texSetSettings", args({json{{"normalFromHeight", on}}})); });
        form->addRow("Export", fromHeight);
        form->addRow("Material", materialPicker(settingsBox));
        auto* bake = new QLabel(settings.bakeProject.empty() ? QString("none") : qs(settings.bakeProject), settingsBox);
        bake->setToolTip("The bake project whose maps feed the set's mesh inputs (texSetSettings bakeProject=)");
        form->addRow("Bake project", bake);
        v->addLayout(form);
    }

    /** The material slot the set drives. An empty setting means the material named after the set. */
    QComboBox* materialPicker(QWidget* parent)
    {
        auto* box = new MaterialSlotCombo(settings.material.empty() ? set : settings.material, parent);
        box->setToolTip("Meshes using this material show the set's maps");
        QObject::connect(box, &QComboBox::activated, parent,
                         [this, box](int) { runUi("texSetSettings", args({json{{"material", box->currentData().toString().toStdString()}}})); });
        return box;
    }

    void rebuildChannelPicker()
    {
        const QString keep = channel->currentData().toString();
        channel->blockSignals(true);
        channel->clear();
        channel->addItem("All channels", QString(kAllChannels));
        for (const tex::Channel& c : settings.channels)
            channel->addItem(qs(c.name), qs(c.name));
        channel->setCurrentIndex(std::max(0, channel->findData(keep)));
        channel->blockSignals(false);
    }

    void rebuildBanner()
    {
        if (!graphMode())
        {
            bannerBox->hide();
            return;
        }
        const bool edited = view.value("edited", false);
        bannerText->setText(edited ? "This set was edited as nodes in ways its layers can't show. Rebuild Layers makes the graph from the "
                                     "stack again, dropping those edits."
                                   : "This set's graph was made as nodes. Adding a layer keeps it as the bottom layer of a new stack.");
        rebuild->setVisible(edited);
        bannerBox->show();
    }

    void rebuildShelf()
    {
        std::vector<tex::SmartMaterial> list;
        try
        {
            list = tex::listSmartMaterials();
        }
        catch (const std::exception& e)
        {
            log::warn("Smart materials: {}", e.what());
        }
        std::string key;
        for (const tex::SmartMaterial& m : list)
            key += m.name + "|" + m.kind + "|" + m.tier + "\n";
        if (key == shelfKey)
            return;
        shelfKey = key;
        shelf->clear();
        for (const tex::SmartMaterial& m : list)
        {
            auto* it = new QListWidgetItem(icon(m.kind == "mask" ? "mask" : "sparkles"), qs(m.name), shelf);
            it->setData(Qt::UserRole, qs(m.name));
            it->setData(Qt::UserRole + 1, qs(m.kind));
            it->setToolTip(qs(m.description.empty() ? m.name : m.description) + "\n" + (m.kind == "mask" ? "Smart mask" : "Smart material") + ", " +
                           qs(m.tier) + ". Double-click to apply.");
        }
    }

    // Properties ---------------------------------------------------------------------------------------------------------

    /** Edits from the properties area keep it in place (a dragged slider survives the refresh its edit causes). */
    void propsLayer(const json& patch)
    {
        keepProps = true;
        setLayer(selLayer, patch);
    }

    void propsMask(const json& patch)
    {
        keepProps = true;
        setMask(selLayer, selMask, patch);
    }

    void layerProps(QWidget* w, QVBoxLayout* v, const json& l)
    {
        const std::string kind = l.value("kind", "fill");
        const std::string id = l.value("id", "");
        auto* head = boldHeading(w, qs(l.value("name", id)) + "  (" + capitalised(kind) + " layer, " + qs(id) + ")");
        v->addWidget(head);

        if (kind == "fill" || kind == "paint")
        {
            json node = {{"name", id}, {"label", kind == "paint" ? "Brush Material" : "Material"}, {"params", json::array()}};
            json inputs = json::array();
            const json material = l.value("material", json::object());
            const json resolved = l.value("resolved", json::object());
            for (const tex::Channel& c : settings.channels)
            {
                if (!material.contains(c.name) && !resolved.value(c.name, json::object()).value("enabled", false))
                    continue;
                json def = tex::channelDefault(c.name, c.type);
                inputs.push_back({{"id", c.name}, {"type", tex::portTypeName(c.type)}, {"default", def}, {"value", material.value(c.name, def)}, {"linked", false}});
            }
            node["inputs"] = inputs;
            if (kind == "paint")
                v->addWidget(dim(w, "The values the brushes paint. Paint with Texture > Paint; strokes go to this layer while it is the active one."));
            v->addWidget(texParams::nodePanel(w, node, [this](const std::string& c, const json& value) { propsLayer({{"material", {{c, value}}}}); }));
        }
        else if (kind == "procedural")
        {
            v->addWidget(dim(w, "A texture graph whose outputs are named after channels (and mask). Open it as nodes to edit it."));
            auto* open = new QPushButton(icon("binary-tree"), "Open as Nodes", w);
            QObject::connect(open, &QPushButton::clicked, w, [this] { self->openSelectedAsNodes(); });
            v->addWidget(open);
        }
        else if (kind == "filter")
        {
            const std::string type = l.value("filter", "");
            v->addWidget(texParams::nodePanel(w, defNodeJson(id, type, l.value("filterValues", json::object())),
                                              [this](const std::string& p, const json& value) { propsLayer({{"filterValues", {{p, value}}}}); }));
        }
        else if (kind == "instance")
        {
            auto* form = new QFormLayout();
            auto* src = new QComboBox(w);
            src->setObjectName("instanceSource");
            std::function<void(const json&)> collect = [&](const json& layers) {
                for (const json& x : layers)
                {
                    const std::string k = x.value("kind", "");
                    if (k == "folder")
                        collect(x.value("children", json::array()));
                    else if (k != "instance")
                        src->addItem(qs(x.value("name", "")) + " (" + qs(x.value("id", "")) + ")", qs(x.value("id", "")));
                }
            };
            collect(view["layers"]);
            src->setCurrentIndex(src->findData(qs(l.value("source", ""))));
            QObject::connect(src, &QComboBox::activated, w, [this, src](int) { propsLayer({{"source", src->currentData().toString().toStdString()}}); });
            form->addRow("Source", src);
            v->addLayout(form);
        }

        v->addWidget(boldHeading(w, "Channels"));
        auto* grid = new QWidget(w);
        auto* g = new QGridLayout(grid);
        g->setContentsMargins(0, 0, 0, 0);
        g->setVerticalSpacing(2);
        int r = 0;
        const json resolved = l.value("resolved", json::object());
        for (const tex::Channel& c : settings.channels)
        {
            const json rc = resolved.value(c.name, json::object());
            auto* on = new QCheckBox(qs(c.name), grid);
            on->setObjectName("propChannel:" + qs(c.name));
            on->setChecked(rc.value("enabled", false));
            QObject::connect(on, &QCheckBox::toggled, grid, [this, id, c = c.name](bool b) {
                keepProps = false;
                toggleChannel(id, c, b);
            });
            g->addWidget(on, r, 0);
            auto* mode = new QComboBox(grid);
            mode->setObjectName("propMode:" + qs(c.name));
            const std::vector<std::string> normalModes = {"rnm", "normal"};
            for (const std::string& m : c.name == "normal" ? normalModes : tex::layerBlendModes())
                mode->addItem(capitalised(m), qs(m));
            mode->setCurrentIndex(std::max(0, mode->findData(qs(rc.value("mode", "normal")))));
            QObject::connect(mode, &QComboBox::activated, grid, [this, id, mode, c = c.name](int) {
                if (const json* cur = layer(id))
                    propsLayer({{"channels", {{c, channelEntry(*cur, c, {{"mode", mode->currentData().toString().toStdString()}})}}}});
            });
            g->addWidget(mode, r, 1);
            auto* op = percentSpin(grid, rc.value("opacity", 1.0));
            op->setObjectName("propOpacity:" + qs(c.name));
            QObject::connect(op, &QSpinBox::valueChanged, grid, [this, id, c = c.name](int value) {
                if (const json* cur = layer(id))
                    propsLayer({{"channels", {{c, channelEntry(*cur, c, {{"opacity", value / 100.0}})}}}});
            });
            g->addWidget(op, r, 2);
            ++r;
        }
        v->addWidget(grid);

        if (kind != "folder")
        {
            const json hb = l.value("heightBlend", json{{"enabled", false}, {"sharpness", 0.8}, {"influence", 1.0}});
            auto* hbOn = new QCheckBox("Height blend", w);
            hbOn->setToolTip("Mix the layer in by its height as well as its mask (needs its height channel on)");
            hbOn->setChecked(hb.value("enabled", false));
            v->addWidget(hbOn);
            auto* form = new QFormLayout();
            auto* sharp = new QDoubleSpinBox(w);
            sharp->setRange(0.0, 1.0);
            sharp->setSingleStep(0.05);
            sharp->setKeyboardTracking(false);
            sharp->setValue(hb.value("sharpness", 0.8));
            auto* infl = new QDoubleSpinBox(w);
            infl->setRange(0.0, 4.0);
            infl->setSingleStep(0.05);
            infl->setKeyboardTracking(false);
            infl->setValue(hb.value("influence", 1.0));
            auto setHb = [this, hbOn, sharp, infl] {
                propsLayer({{"heightBlend", {{"enabled", hbOn->isChecked()}, {"sharpness", sharp->value()}, {"influence", infl->value()}}}});
            };
            QObject::connect(hbOn, &QCheckBox::toggled, w, setHb);
            QObject::connect(sharp, &QDoubleSpinBox::valueChanged, w, setHb);
            QObject::connect(infl, &QDoubleSpinBox::valueChanged, w, setHb);
            form->addRow("Sharpness", sharp);
            form->addRow("Influence", infl);
            v->addLayout(form);
        }

        auto* aForm = new QFormLayout();
        auto* anchor = new QLineEdit(qs(l.value("anchor", "")), w);
        anchor->setObjectName("anchorName");
        anchor->setPlaceholderText("none");
        anchor->setToolTip("Names the composite right after this layer, for anchor masks and generator inputs of layers above");
        QObject::connect(anchor, &QLineEdit::editingFinished, w, [this, anchor, id, was = l.value("anchor", "")] {
            const std::string next = anchor->text().trimmed().toStdString();
            if (next == was)
                return;
            if (next.empty())
                setLayer(id, {{"anchor", nullptr}});
            else
                runUi("texAddAnchor", args({id, next}));
        });
        aForm->addRow("Anchor", anchor);
        v->addLayout(aForm);
    }

    void maskProps(QWidget* w, QVBoxLayout* v, const json& m)
    {
        const std::string kind = m.value("kind", "fill");
        const std::string id = m.value("id", "");
        v->addWidget(boldHeading(w, maskName(m) + "  (" + capitalised(kind) + " mask, " + qs(id) + ")"));
        if (kind == "fill")
        {
            auto* form = new QFormLayout();
            auto* value = new QDoubleSpinBox(w);
            value->setObjectName("maskValue");
            value->setRange(0.0, 1.0);
            value->setSingleStep(0.05);
            value->setKeyboardTracking(false);
            value->setValue(m.value("value", 1.0));
            QObject::connect(value, &QDoubleSpinBox::valueChanged, w, [this](double x) { propsMask({{"value", x}}); });
            form->addRow("Value", value);
            v->addLayout(form);
        }
        else if (kind == "generator" || kind == "filter" || kind == "levels")
        {
            const std::string type = kind == "levels" ? std::string("levels") : m.value("type", "");
            v->addWidget(texParams::nodePanel(w, defNodeJson(id, type, m.value("values", json::object())),
                                              [this](const std::string& p, const json& value) { propsMask({{"values", {{p, value}}}}); }));
            if (kind == "generator")
                if (const tex::NodeDef* def = tex::findNode(type); def && !def->inputs.empty())
                {
                    v->addWidget(boldHeading(w, "Inputs"));
                    auto* form = new QFormLayout();
                    const json bindings = m.value("bindings", json::object());
                    for (const tex::PortDecl& in : def->inputs)
                    {
                        auto* b = new QComboBox(w);
                        b->setObjectName("binding:" + qs(in.id));
                        b->addItem(tex::findMeshRole(in.id) ? "Mesh " + qs(in.id) : QString("Default"), QString());
                        b->addItem("None", "none");
                        for (const tex::MeshRole& role : tex::meshRoles())
                            b->addItem("Mesh " + qs(role.name), qs(role.name));
                        for (auto& [anchorName, owner] : view.value("anchors", json::object()).items())
                            for (const tex::Channel& c : settings.channels)
                                b->addItem("Anchor " + qs(anchorName) + "." + qs(c.name), qs("anchor:" + anchorName + "." + c.name));
                        b->setCurrentIndex(std::max(0, b->findData(qs(bindings.value(in.id, "")))));
                        QObject::connect(b, &QComboBox::activated, w, [this, b, port = in.id](int) {
                            const QString value = b->currentData().toString();
                            propsMask({{"bindings", {{port, value.isEmpty() ? json(nullptr) : json(value.toStdString())}}}});
                        });
                        form->addRow(qs(in.label.empty() ? in.id : in.label), b);
                    }
                    v->addLayout(form);
                }
        }
        else if (kind == "anchor")
        {
            auto* form = new QFormLayout();
            auto* a = new QComboBox(w);
            a->setObjectName("maskAnchor");
            for (auto& [anchorName, owner] : view.value("anchors", json::object()).items())
                a->addItem(qs(anchorName), qs(anchorName));
            a->setCurrentIndex(std::max(0, a->findData(qs(m.value("anchor", "")))));
            QObject::connect(a, &QComboBox::activated, w, [this, a](int) { propsMask({{"anchor", a->currentData().toString().toStdString()}}); });
            form->addRow("Anchor", a);
            auto* c = new QComboBox(w);
            for (const tex::Channel& ch : settings.channels)
                c->addItem(qs(ch.name), qs(ch.name));
            c->setCurrentIndex(std::max(0, c->findData(qs(m.value("channel", "height")))));
            QObject::connect(c, &QComboBox::activated, w, [this, c](int) { propsMask({{"channel", c->currentData().toString().toStdString()}}); });
            form->addRow("Channel", c);
            v->addLayout(form);
        }
        else if (kind == "paint")
        {
            v->addWidget(dim(w, "Image: " + (m.value("image", "").empty() ? QString("none") : qs(m.value("image", "")))));
        }
    }

    void rebuildProps()
    {
        std::string key = selLayer + "|" + selMask + "|";
        if (const json* l = layer(selLayer))
            key += selMask.empty() ? l->dump() : (mask(selLayer, selMask) ? mask(selLayer, selMask)->dump() : std::string());
        key += "|" + settingsKey;
        if (key == propsKey && props)
            return;
        propsKey = key;
        auto* w = new QWidget();
        w->setObjectName("textureSetProperties");
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(6, 4, 6, 6);
        v->setSpacing(4);
        const json* l = layer(selLayer);
        const json* m = selMask.empty() ? nullptr : mask(selLayer, selMask);
        if (m)
            maskProps(w, v, *m);
        else if (l)
            layerProps(w, v, *l);
        else
            v->addWidget(dim(w, setNode() ? "Select a layer or mask to edit it." : "No texture set."));
        v->addStretch(1);
        propsScroll->setWidget(w);
        props = w;
    }

    // Refresh ------------------------------------------------------------------------------------------------------------

    void refreshSets()
    {
        std::vector<std::string> names = tex::setNames(scene::Graph::current());
        sets->blockSignals(true);
        sets->clear();
        for (const std::string& n : names)
            sets->addItem(icon("texture"), qs(n), qs(n));
        if (set.empty() || std::find(names.begin(), names.end(), set) == names.end())
            set = names.empty() ? std::string() : names.front();
        sets->setCurrentIndex(sets->findData(qs(set)));
        sets->blockSignals(false);
    }

    void refresh()
    {
        refreshSets();
        scene::Node* n = setNode();
        json nextView;
        tex::SetSettings nextSettings;
        if (n)
        {
            try
            {
                tex::Graph g = tex::readGraph(*n);
                nextSettings = tex::readSettings(*n);
                nextView = tex::stackView(tex::readStack(g, nextSettings), nextSettings, g);
            }
            catch (const std::exception& e)
            {
                log::warn("Texture Set panel: {}", e.what());
            }
        }
        empty->setVisible(!n);
        const std::string sKey = n ? nextSettings.toJson().dump() : std::string();
        const std::string vKey = set + "|" + nextView.dump() + "|" + sKey + "|" + shownChannel() + "|" + textureSets::activeLayer(set);
        settings = nextSettings;
        view = nextView;
        if (sKey != settingsKey || !settingsBox->layout())
        {
            settingsKey = sKey;
            rebuildSettings();
            rebuildChannelPicker();
        }
        if (vKey != viewKey)
        {
            viewKey = vKey;
            rebuildTree();
        }
        else
            updateThumbs();
        rebuildBanner();
        rebuildShelf();
        if (!std::exchange(keepProps, false))
            rebuildProps();
    }

    void scheduleRefresh()
    {
        if (!refreshTimer->isActive())
            refreshTimer->start();
    }

    void onCurrentItem(QTreeWidgetItem* it)
    {
        if (building)
            return;
        if (!it)
        {
            selLayer.clear();
            selMask.clear();
        }
        else if (it->data(0, kRoleKind).toInt() == 1)
        {
            selLayer = it->data(0, kRoleLayer).toString().toStdString();
            selMask = it->data(0, kRoleId).toString().toStdString();
        }
        else
        {
            selLayer = it->data(0, kRoleId).toString().toStdString();
            selMask.clear();
        }
        if (const json* l = layer(selLayer); l && selMask.empty() && l->value("kind", "") == "paint")
            textureSets::setActiveLayer(set, selLayer);
        keepProps = false;
        rebuildProps();
    }

    // Menus --------------------------------------------------------------------------------------------------------------

    void fillLayerMenu(QMenu* menu)
    {
        menu->addAction(icon("bucket"), "Add Fill Layer", [this] { addLayer("fill", {}); });
        menu->addAction(icon("brush"), "Add Paint Layer", [this] { addLayer("paint", {}); });
        menu->addAction(icon("topology-star-3"), "Add Procedural Layer", [this] { addLayer("procedural", {}); });
        menu->addAction(icon("folder-plus"), "Add Folder", [this] { addLayer("folder", {}); });
        QMenu* filters = menu->addMenu(icon("filter"), "Add Filter Layer");
        for (const tex::NodeDef* def : filterNodes())
            filters->addAction(qs(def->label.empty() ? def->type : def->label), [this, t = def->type] { addLayer("filter", t); });
        const json* sel = layer(selLayer);
        const std::string selKind = sel ? sel->value("kind", "") : std::string();
        QAction* inst = menu->addAction(icon("link"), "Add Instance of Selected", [this] { addLayer("instance", {}); });
        inst->setEnabled(sel && selKind != "folder" && selKind != "instance");
    }

    void fillMaskMenu(QMenu* menu)
    {
        const bool any = layer(selLayer) != nullptr;
        QMenu* m = menu->addMenu(icon("mask"), "Add Mask");
        m->setEnabled(any);
        m->addAction(icon("square"), "Fill White", [this] { addMask("white", {}); });
        m->addAction(icon("square"), "Fill Black", [this] { addMask("black", {}); });
        m->addAction(icon("brush"), "Paint", [this] { addMask("paint", {}); });
        m->addAction(icon("adjustments"), "Levels", [this] { addMask("levels", {}); });
        QMenu* f = m->addMenu(icon("filter"), "Filter");
        for (const tex::NodeDef* def : filterNodes())
            f->addAction(qs(def->label.empty() ? def->type : def->label), [this, t = def->type] { addMask("filter", t); });
        QMenu* a = m->addMenu(icon("anchor"), "Anchor");
        const json anchors = view.is_object() ? view.value("anchors", json::object()) : json::object();
        a->setEnabled(!anchors.empty());
        for (auto& [name, owner] : anchors.items())
            a->addAction(qs(name), [this, n = name] { addMask("anchor", n); });
        QMenu* g = menu->addMenu(icon("wand"), "Add Generator");
        g->setEnabled(any);
        for (const std::string& t : tex::generatorTypes())
            g->addAction(nodeLabel(t), [this, t] { addMask("generator", t); });
        QAction* anchor = menu->addAction(icon("anchor"), "Add Anchor...", [this] {
            bool ok = false;
            QString name = QInputDialog::getText(self, "Add Anchor", "Anchor name", QLineEdit::Normal, "anchor", &ok);
            if (ok && !name.trimmed().isEmpty())
                runUi("texAddAnchor", args({selLayer, name.trimmed().toStdString()}));
        });
        anchor->setEnabled(any);
    }

    void contextMenu(const QPoint& pos)
    {
        if (QTreeWidgetItem* it = tree->itemAt(pos))
            tree->setCurrentItem(it);
        if (!setNode())
            return;
        QMenu menu(tree);
        fillLayerMenu(&menu);
        menu.addSeparator();
        fillMaskMenu(&menu);
        menu.addSeparator();
        const bool any = layer(selLayer) != nullptr;
        QAction* dup = menu.addAction(icon("copy"), "Duplicate", [this] { self->duplicateSelected(); });
        dup->setEnabled(any && selMask.empty());
        menu.addAction(icon("trash"), selMask.empty() ? "Delete Layer" : "Delete Mask", [this] { self->deleteSelected(); })->setEnabled(any);
        menu.addSeparator();
        QAction* nodes = menu.addAction(icon("binary-tree"), "Open as Nodes", [this] { self->openSelectedAsNodes(); });
        nodes->setShortcut(QKeySequence("Ctrl+G"));
        nodes->setEnabled(any);
        menu.addAction(icon("sparkles"), "Save as Smart Material...", [this] { saveSmart(false); })->setEnabled(any);
        const json* l = layer(selLayer);
        menu.addAction(icon("mask"), "Save Masks as Smart Mask...", [this] { saveSmart(true); })
            ->setEnabled(l && !l->value("masks", json::array()).empty());
        menu.exec(tree->viewport()->mapToGlobal(pos));
    }

    void saveSmart(bool masks)
    {
        const json* l = layer(selLayer);
        if (!l)
            return;
        bool ok = false;
        QString name = QInputDialog::getText(self, masks ? "Save Smart Mask" : "Save Smart Material", "Name", QLineEdit::Normal,
                                             qs(l->value("name", selLayer)), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        json call = args({selLayer});
        call["name"] = name.trimmed().toStdString();
        if (masks)
            call["mask"] = true;
        if (json r = runUi("texSaveSmartMaterial", call); r.is_string())
            log::info("saved {}", r.get<std::string>());
        shelfKey.clear();
        rebuildShelf();
    }
};

TextureSetPanel::TextureSetPanel(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>())
{
    d->self = this;
    panelList().push_back(this);
    setObjectName("textureSetPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* top = new QHBoxLayout();
    d->sets = new QComboBox(this);
    d->sets->setObjectName("setPicker");
    d->sets->setToolTip("The texture set shown");
    d->follow = toolButton(this, "pointer", "Follow the selection: show the set of the selected mesh's material");
    d->follow->setCheckable(true);
    d->follow->setChecked(true);
    auto* newSet = toolButton(this, "plus", "New texture set");
    QObject::connect(newSet, &QToolButton::clicked, this, [] { actions::run(json("texture.newSet")); });
    top->addWidget(d->sets, 1);
    top->addWidget(d->follow);
    top->addWidget(newSet);
    v->addLayout(top);

    d->settingsToggle = new QToolButton(this);
    d->settingsToggle->setText("Channels and Resolution");
    d->settingsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    d->settingsToggle->setIcon(icon("chevron-right"));
    d->settingsToggle->setCheckable(true);
    d->settingsToggle->setAutoRaise(true);
    d->settingsBox = new QWidget(this);
    d->settingsBox->setObjectName("setSettings");
    d->settingsBox->hide();
    connect(d->settingsToggle, &QToolButton::toggled, this, [this](bool on) {
        d->settingsBox->setVisible(on);
        d->settingsToggle->setIcon(icon(on ? "chevron-down" : "chevron-right"));
    });
    v->addWidget(d->settingsToggle);
    v->addWidget(d->settingsBox);

    d->bannerBox = new QWidget(this);
    d->bannerBox->setObjectName("graphModeBanner");
    d->bannerBox->setStyleSheet("#graphModeBanner { background: #3a3a3a; border: 1px solid #555555; }");
    auto* bh = new QHBoxLayout(d->bannerBox);
    bh->setContentsMargins(6, 4, 6, 4);
    auto* warn = new QLabel(d->bannerBox);
    warn->setPixmap(icon("alert-triangle").pixmap(16, 16));
    bh->addWidget(warn);
    d->bannerText = new QLabel(d->bannerBox);
    d->bannerText->setWordWrap(true);
    bh->addWidget(d->bannerText, 1);
    d->rebuild = new QPushButton(icon("refresh"), "Rebuild Layers", d->bannerBox);
    d->rebuild->setObjectName("rebuildLayers");
    connect(d->rebuild, &QPushButton::clicked, this, [this] { runUi("texRebuildLayers", d->args({})); });
    bh->addWidget(d->rebuild);
    d->bannerBox->hide();
    v->addWidget(d->bannerBox);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(1);
    auto* addFill = toolButton(this, "bucket", "Add a fill layer above the selection");
    connect(addFill, &QToolButton::clicked, this, [this] { addLayer("fill"); });
    auto* addPaint = toolButton(this, "brush", "Add a paint layer above the selection");
    connect(addPaint, &QToolButton::clicked, this, [this] { addLayer("paint"); });
    auto* addFolder = toolButton(this, "folder-plus", "Add a folder above the selection");
    connect(addFolder, &QToolButton::clicked, this, [this] { addLayer("folder"); });
    auto* more = toolButton(this, "plus", "Add a layer of any kind");
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
        moreMenu->clear();
        d->fillLayerMenu(moreMenu);
    });
    more->setMenu(moreMenu);
    auto* maskBtn = toolButton(this, "mask", "Add a mask or generator to the selected layer");
    maskBtn->setPopupMode(QToolButton::InstantPopup);
    auto* maskMenu = new QMenu(maskBtn);
    connect(maskMenu, &QMenu::aboutToShow, this, [this, maskMenu] {
        maskMenu->clear();
        d->fillMaskMenu(maskMenu);
    });
    maskBtn->setMenu(maskMenu);
    auto* dup = toolButton(this, "copy", "Duplicate the selected layer");
    connect(dup, &QToolButton::clicked, this, [this] { duplicateSelected(); });
    auto* del = toolButton(this, "trash", "Delete the selected layer or mask");
    connect(del, &QToolButton::clicked, this, [this] { deleteSelected(); });
    d->channel = new QComboBox(this);
    d->channel->setObjectName("rowChannel");
    d->channel->setToolTip("The channel the rows' thumbnails, blend modes and opacities show and edit");
    connect(d->channel, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
    for (QToolButton* b : {addFill, addPaint, addFolder, more, maskBtn, dup, del})
        bar->addWidget(b);
    bar->addStretch(1);
    bar->addWidget(d->channel);
    v->addLayout(bar);

    d->empty = new QWidget(this);
    auto* eh = new QVBoxLayout(d->empty);
    eh->addWidget(dim(d->empty, "No texture set. Make one for the selected mesh's material to start a layer stack."));
    auto* make = new QPushButton(icon("plus"), "New Texture Set", d->empty);
    connect(make, &QPushButton::clicked, this, [] { actions::run(json("texture.newSet")); });
    eh->addWidget(make);
    v->addWidget(d->empty);

    auto* split = new QSplitter(Qt::Vertical, this);
    d->tree = new LayerTree(split);
    d->tree->setObjectName("layerTree");
    deletion::claim(d->tree, [this] { deleteSelected(); });
    d->propsScroll = new QScrollArea(split);
    d->propsScroll->setWidgetResizable(true);
    d->propsScroll->setFrameShape(QFrame::NoFrame);
    auto* shelfBox = new QWidget(split);
    auto* sv = new QVBoxLayout(shelfBox);
    sv->setContentsMargins(0, 2, 0, 0);
    sv->setSpacing(2);
    auto* shelfHead = new QHBoxLayout();
    shelfHead->addWidget(boldHeading(shelfBox, "Smart Materials"));
    shelfHead->addStretch(1);
    auto* reload = toolButton(shelfBox, "refresh", "Reread the smart material libraries");
    connect(reload, &QToolButton::clicked, this, [this] {
        d->shelfKey.clear();
        d->rebuildShelf();
    });
    shelfHead->addWidget(reload);
    sv->addLayout(shelfHead);
    d->shelf = new QListWidget(shelfBox);
    d->shelf->setObjectName("smartShelf");
    d->shelf->setViewMode(QListView::IconMode);
    d->shelf->setFlow(QListView::LeftToRight);
    d->shelf->setWrapping(true);
    d->shelf->setResizeMode(QListView::Adjust);
    d->shelf->setIconSize(QSize(24, 24));
    d->shelf->setGridSize(QSize(92, 52));
    d->shelf->setWordWrap(true);
    d->shelf->setMovement(QListView::Static);
    connect(d->shelf, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) { applySmartMaterial(it->data(Qt::UserRole).toString().toStdString()); });
    sv->addWidget(d->shelf, 1);
    deletion::claim(d->shelf, nullptr);
    auto* apply = new QPushButton(icon("sparkles"), "Apply", shelfBox);
    apply->setToolTip("Apply the chosen smart material above the selected layer, or the smart mask to it");
    connect(apply, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem* it = d->shelf->currentItem())
            applySmartMaterial(it->data(Qt::UserRole).toString().toStdString());
    });
    sv->addWidget(apply);
    split->addWidget(d->tree);
    split->addWidget(d->propsScroll);
    split->addWidget(shelfBox);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 1);
    v->addWidget(split, 1);

    d->tree->onMove = [this](QTreeWidgetItem* dragged, QTreeWidgetItem* target, Drop where) {
        const bool draggedMask = dragged->data(0, kRoleKind).toInt() == 1;
        const std::string id = dragged->data(0, kRoleId).toString().toStdString();
        if (draggedMask)
        {
            if (!target || target->data(0, kRoleKind).toInt() != 1 ||
                target->data(0, kRoleLayer).toString() != dragged->data(0, kRoleLayer).toString())
                return;
            d->selLayer = dragged->data(0, kRoleLayer).toString().toStdString();
            moveLayer(id, target->data(0, kRoleId).toString().toStdString(), where);
            return;
        }
        // Dropped on a mask row: the mask's layer is the target.
        std::string targetId = target ? target->data(0, kRoleLayer).toString().toStdString() : std::string();
        moveLayer(id, targetId, where);
    };
    d->tree->onMaterial = [this](const std::string& material, QTreeWidgetItem* target, Drop where) {
        dropMaterial(material, target ? target->data(0, kRoleLayer).toString().toStdString() : std::string(), where);
    };
    connect(d->tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) { d->onCurrentItem(it); });
    connect(d->tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) { d->contextMenu(p); });
    connect(d->tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* it) {
        if (!d->building)
            d->expanded[it->data(0, kRoleId).toString().toStdString()] = true;
    });
    connect(d->tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* it) {
        if (!d->building)
            d->expanded[it->data(0, kRoleId).toString().toStdString()] = false;
    });
    connect(d->sets, &QComboBox::activated, this, [this](int) { setTextureSet(d->sets->currentData().toString().toStdString()); });

    d->refreshTimer = new QTimer(this);
    d->refreshTimer->setSingleShot(true);
    d->refreshTimer->setInterval(0);
    connect(d->refreshTimer, &QTimer::timeout, this, [this] { d->refresh(); });

    QPointer<TextureSetPanel> guard(this);
    d->sceneListener = scene::Graph::current().addListener([this, guard](scene::Change c, scene::NodeId id) {
        if (!guard)
            return;
        if (c == scene::Change::Selection || c == scene::Change::Reset)
        {
            QTimer::singleShot(0, this, [this] {
                if (!d->follow->isChecked())
                    return;
                if (GraphBackendPtr b = graphBackends::find(kTextureGraphBackend))
                {
                    GraphBackend::Owner o = b->followSelection(scene::Graph::current());
                    if (!o.group.empty() && o.group != d->set)
                    {
                        d->set = o.group;
                        showInTextureGraphs(o.group);
                        d->selLayer.clear();
                        d->selMask.clear();
                        d->scheduleRefresh();
                    }
                }
            });
            if (c == scene::Change::Selection)
                return;
        }
        if (c == scene::Change::Value)
        {
            scene::Node* n = scene::Graph::current().node(id);
            if (!n || n->type().name != tex::kTextureSetType)
                return;
        }
        d->scheduleRefresh();
    });
    d->activeListener = textureSets::addActiveLayerListener([this](const std::string& set, const std::string& layer) {
        if (set != d->set)
            return;
        if (d->selLayer != layer || !d->selMask.empty())
        {
            d->selLayer = layer;
            d->selMask.clear();
        }
        d->scheduleRefresh();
    });

    // A Texture Graph editor switching sets takes this panel along.
    d->graphListener = NodeEditor::addGroupListener([this, guard](NodeEditor& e) {
        if (!guard || e.backend().id() != kTextureGraphBackend)
            return;
        const std::string group = e.group().toStdString();
        if (group.empty() || group == d->set)
            return;
        d->set = group;
        d->selLayer.clear();
        d->selMask.clear();
        d->scheduleRefresh();
    });

    QTimer::singleShot(0, this, [this] {
        if (GraphBackendPtr b = graphBackends::find(kTextureGraphBackend))
            if (GraphBackend::Owner o = b->followSelection(scene::Graph::current()); !o.group.empty())
                d->set = o.group;
        if (d->set.empty())
            d->set = textureGraphSet();
        d->refresh();
        showInTextureGraphs(d->set);
    });
    d->refresh();
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now && isAncestorOf(now))
            lastFocusedPanel() = this;
    });
}

TextureSetPanel::~TextureSetPanel()
{
    scene::Graph::current().removeListener(d->sceneListener);
    textureSets::removeActiveLayerListener(d->activeListener);
    NodeEditor::removeGroupListener(d->graphListener);
    std::erase(panelList(), this);
    if (lastFocusedPanel() == this)
        lastFocusedPanel() = nullptr;
}

void TextureSetPanel::setTextureSet(const std::string& set)
{
    if (set == d->set)
        return;
    d->set = set;
    d->selLayer.clear();
    d->selMask.clear();
    d->refresh();
    showInTextureGraphs(set);
}

std::string TextureSetPanel::textureSet() const { return d->set; }

void TextureSetPanel::refresh()
{
    d->refreshTimer->stop();
    d->refresh();
}

std::string TextureSetPanel::selectedLayer() const { return d->selLayer; }
std::string TextureSetPanel::selectedMask() const { return d->selMask; }

void TextureSetPanel::selectLayer(const std::string& id)
{
    for (QTreeWidgetItem* it : allItems(d->tree))
        if (it->data(0, kRoleKind).toInt() == 0 && it->data(0, kRoleId).toString().toStdString() == id)
        {
            d->tree->setCurrentItem(it);
            return;
        }
}

void TextureSetPanel::selectMask(const std::string& layer, const std::string& mask)
{
    d->expanded[layer] = true;
    for (QTreeWidgetItem* it : allItems(d->tree))
        if (it->data(0, kRoleKind).toInt() == 1 && it->data(0, kRoleId).toString().toStdString() == mask &&
            it->data(0, kRoleLayer).toString().toStdString() == layer)
        {
            if (it->parent())
                it->parent()->setExpanded(true);
            d->tree->setCurrentItem(it);
            return;
        }
}

std::string TextureSetPanel::addLayer(const std::string& kind, const std::string& filter) { return d->addLayer(kind, filter); }

std::string TextureSetPanel::addMask(const std::string& kind, const std::string& type) { return d->addMask(kind, type); }

void TextureSetPanel::moveLayer(const std::string& id, const std::string& target, Drop where)
{
    if (!d->setNode() || id == target)
        return;
    // A mask among its layer's masks: the rows are top first, the stack bottom first.
    if (const json* m = d->mask(d->selLayer, id))
    {
        json masks = d->layer(d->selLayer)->value("masks", json::array());
        json moved = *m;
        masks.erase(std::remove_if(masks.begin(), masks.end(), [&](const json& x) { return x.value("id", "") == id; }), masks.end());
        auto at = std::find_if(masks.begin(), masks.end(), [&](const json& x) { return x.value("id", "") == target; });
        if (at == masks.end())
            return;
        masks.insert(where == Drop::Below ? at : at + 1, moved);
        d->setLayer(d->selLayer, {{"masks", masks}});
        return;
    }
    json call = d->args({id});
    if (target.empty())
        call["index"] = 0;
    else if (where == Drop::Into)
        call["parent"] = target;
    else
        call[where == Drop::Above ? "above" : "below"] = target;
    runUi("texMoveLayer", call);
    d->selLayer = id;
    d->selMask.clear();
    refresh();
}

void TextureSetPanel::dropMaterial(const std::string& material, const std::string& target, Drop where)
{
    if (!d->setNode() || material.empty())
        return;
    UndoChunk chunk("Add Material Layer");
    json call = {{"args", json::array({material})}, {"set", d->set}, {"layer", true}};
    if (std::string p = chosenPreset(material); !p.empty())
        call["preset"] = p;
    json r = runUi("applyLibraryMaterial", call);
    std::string id = r.is_object() ? r.value("layer", json::object()).value("id", std::string()) : std::string();
    if (id.empty())
        return;
    if (!target.empty())
    {
        json move = d->args({id});
        if (where == Drop::Into)
            move["parent"] = target;
        else
            move[where == Drop::Above ? "above" : "below"] = target;
        runUi("texMoveLayer", move);
    }
    d->selLayer = id;
    d->selMask.clear();
    refresh();
}

void TextureSetPanel::duplicateSelected()
{
    scene::Node* n = d->setNode();
    const json* l = d->layer(d->selLayer);
    if (!n || !l || !d->selMask.empty())
        return;
    // A layer saved and applied as a smart material comes back with fresh layer and mask ids and unique anchor names.
    tex::SmartMaterial sm;
    sm.name = l->value("name", d->selLayer) + " copy";
    json copy = *l;
    copy.erase("resolved");
    std::function<void(json&)> strip = [&](json& x) {
        x.erase("resolved");
        if (x.contains("children"))
            for (json& c : x["children"])
                strip(c);
    };
    strip(copy);
    copy["name"] = sm.name;
    sm.layer = copy;
    const std::filesystem::path dir = paths::fromUtf8(QDir::temp().filePath("lila_layer_copies").toStdString());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (d->set + "_" + d->selLayer + ".lilasmart");
    try
    {
        tex::saveSmartMaterial(sm, file);
    }
    catch (const std::exception& e)
    {
        log::warn("Duplicate layer: {}", e.what());
        return;
    }
    json call = d->args({paths::toUtf8(file)});
    call["above"] = d->selLayer;
    call["addMaps"] = false;
    call["name"] = sm.name;
    json r = runUi("texApplySmartMaterial", call);
    std::filesystem::remove(file, ec);
    if (r.is_object() && r.contains("layer"))
    {
        d->selLayer = r["layer"].get<std::string>();
        d->selMask.clear();
    }
    refresh();
}

void TextureSetPanel::deleteSelected()
{
    if (!d->setNode() || !d->layer(d->selLayer))
        return;
    if (!d->selMask.empty())
        runUi("texRemoveMask", d->args({d->selLayer, d->selMask}));
    else
        runUi("texRemoveLayer", d->args({d->selLayer}));
    d->selMask.clear();
    d->selLayer.clear();
    refresh();
}

void TextureSetPanel::openSelectedAsNodes()
{
    if (!d->setNode())
        return;
    panels::show(kTextureGraphDock);
    for (QWidget* w : QApplication::allWidgets())
        if (auto* e = dynamic_cast<NodeEditor*>(w); e && e->backend().id() == kTextureGraphBackend)
        {
            if (d->layer(d->selLayer))
                e->showNode(qs(d->set), d->selLayer);
            else
                e->setGroup(qs(d->set));
        }
}

void TextureSetPanel::applySmartMaterial(const std::string& name)
{
    if (!d->setNode() || name.empty())
        return;
    bool mask = false;
    for (int i = 0; i < d->shelf->count(); ++i)
        if (d->shelf->item(i)->data(Qt::UserRole).toString().toStdString() == name)
            mask = d->shelf->item(i)->data(Qt::UserRole + 1).toString() == "mask";
    json call = d->args({name});
    if (mask)
    {
        if (!d->layer(d->selLayer))
        {
            log::warn("Select the layer the smart mask goes on");
            return;
        }
        call["layer"] = d->selLayer;
    }
    else
        call.update(d->placement());
    json r = runUi("texApplySmartMaterial", call);
    if (r.is_object() && r.contains("layer"))
    {
        d->selLayer = r["layer"].get<std::string>();
        d->selMask.clear();
    }
    else if (mask)
        d->expanded[d->selLayer] = true;
    refresh();
}

QTreeWidget* TextureSetPanel::tree() const { return d->tree; }
QComboBox* TextureSetPanel::setPicker() const { return d->sets; }
QListWidget* TextureSetPanel::smartShelf() const { return d->shelf; }
QWidget* TextureSetPanel::properties() const { return d->props; }
QWidget* TextureSetPanel::banner() const { return d->bannerBox; }

TextureSetPanel* TextureSetPanel::active()
{
    QWidget* focus = QApplication::focusWidget();
    for (TextureSetPanel* p : panelList())
        if (focus && p->isAncestorOf(focus))
            return p;
    if (lastFocusedPanel())
        return lastFocusedPanel();
    for (TextureSetPanel* p : panelList())
        if (p->isVisible())
            return p;
    return panelList().empty() ? nullptr : panelList().front();
}

// Active layer API -------------------------------------------------------------------------------------------------------

namespace textureSets
{
std::string activeLayer(const std::string& set)
{
    auto it = activeState().layers.find(set);
    if (it == activeState().layers.end() || it->second.empty())
        return {};
    std::optional<tex::StackState> st = stackOf(set);
    if (!st || st->graphMode)
        return {};
    const tex::Layer* l = st->stack.find(it->second);
    return l && l->kind == tex::LayerKind::Paint ? it->second : std::string();
}

void setActiveLayer(const std::string& set, const std::string& layer)
{
    std::string& cur = activeState().layers[set];
    if (cur == layer)
        return;
    cur = layer;
    auto listeners = activeState().listeners;
    for (auto& [id, fn] : listeners)
        fn(set, layer);
}

int addActiveLayerListener(std::function<void(const std::string&, const std::string&)> fn)
{
    const int id = activeState().next++;
    activeState().listeners[id] = std::move(fn);
    return id;
}

void removeActiveLayerListener(int id) { activeState().listeners.erase(id); }
} // namespace textureSets

// Registration -----------------------------------------------------------------------------------------------------------

void registerTextureSetPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kTextureSetsDock,
                 .title = "Texture Set",
                 .create = [](QWidget* p) -> QWidget* { return new TextureSetPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "stack-2",
                 .open = false,
                 .toggleAction = "window.textureSets"});

    auto onPanel = [](auto fn) {
        return [fn] {
            if (TextureSetPanel* p = TextureSetPanel::active())
                fn(*p);
            else
                log::warn("Open the Texture Set panel (Texture > Texture Set) first");
        };
    };
    actions::add({.id = "textureSets.addFillLayer",
                  .label = "Add Fill Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("fill"); }),
                  .doc = "Add a fill layer (a material and a mask) above the layer selected in the Texture Set panel.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"fill\")"});
    actions::add({.id = "textureSets.addPaintLayer",
                  .label = "Add Paint Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("paint"); }),
                  .doc = "Add a paint layer above the selection in the Texture Set panel and make it the one painting goes into.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"paint\")"});
    actions::add({.id = "textureSets.addFolder",
                  .label = "Add Layer Folder",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addLayer("folder"); }),
                  .doc = "Add a folder above the selection in the Texture Set panel; drop layers onto it to put them inside.",
                  .python = "lila.ops.texAddLayer(\"RockTex\", \"folder\")"});
    actions::add({.id = "textureSets.addMask",
                  .label = "Add Black Mask",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.addMask("black"); }),
                  .doc = "Add a black fill mask to the selected layer, hiding it until generators or paint reveal it.",
                  .python = "lila.ops.texAddMask(\"RockTex\", \"L1\", \"fill\", value=0)"});
    actions::add({.id = "textureSets.duplicate",
                  .label = "Duplicate Layer",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.duplicateSelected(); }),
                  .doc = "Copy the selected layer (with its masks, or a folder with its contents) right above it.",
                  .python = "lila.ops.texSaveSmartMaterial(\"RockTex\", \"L1\", \"copy.lilasmart\")\n"
                            "lila.ops.texApplySmartMaterial(\"RockTex\", \"copy.lilasmart\", above=\"L1\")"});
    actions::add({.id = "textureSets.delete",
                  .label = "Delete Layer or Mask",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.deleteSelected(); }),
                  .doc = "Remove the layer or mask selected in the Texture Set panel.",
                  .python = "lila.ops.texRemoveLayer(\"RockTex\", \"L1\")"});
    actions::add({.id = "textureSets.openAsNodes",
                  .label = "Open Layer as Nodes",
                  .category = "Texture",
                  .run = onPanel([](TextureSetPanel& p) { p.openSelectedAsNodes(); }),
                  .doc = "Show the selected layer's node in the Texture Graph editor: the stack is the set's graph, each layer a group node.",
                  .python = "lila.ops.texGraphInfo(\"RockTex\")"});
    actions::add({.id = "textureSets.rebuildLayers",
                  .label = "Rebuild Layers",
                  .category = "Texture",
                  .run =
                      [] {
                          std::string set = TextureSetPanel::active() ? TextureSetPanel::active()->textureSet() : currentTextureSet();
                          if (!set.empty())
                              runUi("texRebuildLayers", {{"args", json::array({set})}});
                      },
                  .doc = "Rebuild the texture set's graph from its layer stack, dropping node edits the stack can't show.",
                  .python = "lila.ops.texRebuildLayers(\"RockTex\")"});

    menus::addActions("Texture", {"", "window.textureSets", "textureSets.addFillLayer", "textureSets.addPaintLayer", "textureSets.addFolder",
                                  "textureSets.rebuildLayers"});
}
} // namespace lila::ui
