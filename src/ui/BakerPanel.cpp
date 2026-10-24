#include "ui/BakerPanel.h"
#include "ui/BakeCageView.h"

#include "bake/Arrange.h"
#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/Maps.h"
#include "bake/Naming.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/FlagFields.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/NodeLookup.h"
#include "ui/Outliner.h"
#include "ui/Panels.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <cmath>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;
namespace opt = ops::options;
constexpr const char* kPrefs = "baker";

enum ItemKind
{
    kGroupItem = 1,
    kLowsItem,
    kLowItem,
    kHighsItem,
    kHighItem,
    kCageItem,
    kLintItem,
};

constexpr int kKindRole = Qt::UserRole;
constexpr int kGroupRole = Qt::UserRole + 1;
constexpr int kNodeRole = Qt::UserRole + 2;
constexpr int kFileRole = Qt::UserRole + 3;
constexpr int kPresetRole = Qt::UserRole + 4;

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

std::string shortName(const std::string& path)
{
    const size_t bar = path.rfind('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

/** The project the panel last showed, for actions run without it (menus, shelves, hotkeys). */
std::string& currentProject()
{
    static std::string p;
    return p;
}

scene::Graph& graph() { return scene::Graph::current(); }

void openFolder(const QString& dir)
{
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

/** Shows the file selected in Explorer on Windows; elsewhere, and for a file not on disk, opens its folder. */
void revealFile(const QString& file)
{
#ifdef Q_OS_WIN
    if (QFileInfo::exists(file))
    {
        QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(file)});
        return;
    }
#endif
    openFolder(QFileInfo(file).absolutePath());
}

/** The folder a result wrote to: its manifest's, else its first output's. */
QString resultFolder(const json& result)
{
    std::string file = result.value("manifest", json()).is_string() ? result["manifest"].get<std::string>() : std::string();
    if (file.empty())
        for (const json& o : result.value("outputs", json::array()))
            if (file = o.value("file", std::string()); !file.empty())
                break;
    return file.empty() ? QString() : qs(std::filesystem::path(file).parent_path().generic_string());
}

/** project= for a command: the panel's project when it still exists, else none (the command picks the only one). */
json projectCall(json call = json::object())
{
    const std::string& p = currentProject();
    if (!p.empty())
        if (scene::Node* n = findNode(p); n && bake::isProject(*n))
            call["project"] = p;
    return call;
}

json runUi(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return nullptr;
    }
}

/** The backend bakes from the Baker run on: the preference, else the GPU when one is registered. */
std::string backendPref()
{
    const std::vector<std::string> names = bake::backendNames();
    const bool gpu = std::ranges::find(names, std::string("gpu")) != names.end();
    std::string b = PrefStore::get().value<std::string>(kPrefs, "/backend", gpu ? "gpu" : "cpu");
    return std::ranges::find(names, b) != names.end() ? b : std::string("cpu");
}

/** New projects start from the Baker's chosen target profile. */
json newProjectArgs()
{
    return {{"target", PrefStore::get().value<std::string>(kPrefs, "/target", "substancePainter")}};
}

std::string targetPref()
{
    if (scene::Node* n = findNode(currentProject()); n && bake::isProject(*n))
        if (std::string t = bake::projectSettings(*n).target; !t.empty())
            return t;
    return PrefStore::get().value<std::string>(kPrefs, "/target", "substancePainter");
}

QString backendBadge(const std::string& name) { return name == "cpu" ? "CPU" : "GPU"; }

/** A bake's backendMode as the badge shows it: RT (ray query), GPU (compute traversal) or CPU. */
QString modeBadge(const std::string& mode) { return mode == "rt" ? "RT" : mode == "compute" ? "GPU" : "CPU"; }

/** Groups of the project that hold a selected mesh (as low, high or cage), plus selected bakeGroup nodes. */
std::vector<std::string> groupsForSelection(const std::string& project)
{
    scene::Graph& g = graph();
    std::vector<scene::NodeId> projects;
    if (scene::Node* p = findNode(project); p && bake::isProject(*p))
        projects.push_back(p->id());
    else
        projects = bake::projects(g);
    std::vector<std::string> selected;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            selected.push_back(n->path());
    std::vector<std::string> out;
    for (scene::NodeId p : projects)
        for (scene::NodeId id : bake::groupsOf(g, p))
        {
            scene::Node* grp = g.node(id);
            const bake::GroupSettings s = bake::groupSettings(*grp);
            auto has = [&](const std::string& path) { return std::ranges::find(selected, path) != selected.end(); };
            bool hit = has(grp->path()) || has(s.cage) || std::ranges::any_of(s.lows, has);
            for (const bake::HighEntry& h : s.highs)
                hit = hit || (h.self ? std::ranges::any_of(s.lows, has) : has(h.node));
            if (hit)
                out.push_back(grp->path());
        }
    return out;
}

std::vector<std::string> selectedMeshes()
{
    std::vector<std::string> out;
    scene::Graph& g = graph();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->path());
    return out;
}

/** One group per selected mesh, each baking onto itself; one undo step. */
void groupPerMesh()
{
    const std::vector<std::string> meshes = selectedMeshes();
    if (meshes.empty())
    {
        log::warn("One Group per Mesh: select meshes first");
        return;
    }
    UndoChunk chunk("One Bake Group per Mesh");
    for (const std::string& m : meshes)
    {
        std::string name = bake::fileSafe(shortName(m)) + "_bake";
        if (findNode(name))
            name.clear();
        json call = projectCall({{"lows", {m}}, {"self", true}});
        if (!name.empty())
            call["args"] = {name};
        runUi("bakeGroup", call);
    }
}

void cancelBakes()
{
    for (const progress::Snapshot& s : progress::snapshot())
        if (s.title.starts_with("Baking") || s.title.starts_with("Preparing bake"))
            progress::cancel(s.id);
}

QString modifierSummary(const json& stack)
{
    QStringList parts;
    for (const json& m : stack.is_array() ? stack : json::array())
    {
        if (m.is_string())
        {
            parts << qs(m.get<std::string>());
            continue;
        }
        QString text = qs(m.value("kind", std::string("?")));
        const json params = m.value("params", json::object());
        if (params.contains("levels"))
            text += " " + qs(params["levels"].dump());
        parts << text;
    }
    return parts.join(" > ");
}

std::string entryKind(const json& m)
{
    return m.is_string() ? m.get<std::string>() : m.is_object() ? m.value("kind", std::string()) : std::string();
}

/** A bake stack entry's node type: kind "subdivision" is subdivisionModifier, as BakeModifierStack builds it. */
const scene::NodeType* modifierType(const std::string& kind)
{
    return kind.empty() ? nullptr : scene::types::find(kind + "Modifier");
}

/** The plugs a stack entry's params can set: visible inputs, minus the switches a bake stack has no use for. */
std::vector<const scene::AttrDef*> editableParams(const scene::NodeType& t)
{
    std::vector<const scene::AttrDef*> out;
    for (const scene::AttrDef& d : t.attrs)
    {
        if ((d.flags & scene::AttrHidden) || !(d.flags & scene::AttrInput))
            continue;
        if (d.type == scene::PlugType::Mesh || d.type == scene::PlugType::Data || d.type == scene::PlugType::Matrix)
            continue;
        if (d.name == "enabled" || d.name == "exportEnabled")
            continue;
        out.push_back(&d);
    }
    return out;
}

/** An entry as {kind, params} with params in their plug's type and defaults left out, so equal stacks compare equal. */
json canonicalEntry(const json& m)
{
    const std::string kind = entryKind(m);
    const scene::NodeType* t = modifierType(kind);
    json params = json::object();
    const json given = m.is_object() ? m.value("params", json::object()) : json::object();
    for (auto it = given.begin(); it != given.end(); ++it)
    {
        const int a = t ? t->attrIndex(it.key()) : -1;
        if (a < 0)
        {
            params[it.key()] = it.value();
            continue;
        }
        const scene::AttrDef& def = t->attrs[size_t(a)];
        try
        {
            json typed = scene::toJson(scene::fromJson(def, it.value()));
            if (typed != scene::toJson(def.defaultValue))
                params[it.key()] = std::move(typed);
        }
        catch (const std::exception&)
        {
            params[it.key()] = it.value();
        }
    }
    return {{"kind", kind}, {"params", params}};
}

json canonicalStack(const json& stack)
{
    json out = json::array();
    for (const json& m : stack.is_array() ? stack : json::array())
        out.push_back(canonicalEntry(m));
    return out;
}

scene::PlugValue paramValue(const scene::AttrDef& d, const json& params)
{
    if (params.is_object() && params.contains(d.name))
        try
        {
            return scene::fromJson(d, params[d.name]);
        }
        catch (const std::exception&)
        {
        }
    return d.defaultValue;
}

/** A plug value as its flag field holds it; enums go by name. */
json fieldValue(const scene::AttrDef& d, const scene::PlugValue& v)
{
    json j = scene::toJson(v);
    if (d.type == scene::PlugType::Enum && j.is_number_integer())
    {
        const int64_t i = j.get<int64_t>();
        return i >= 0 && i < int64_t(d.enumNames.size()) ? json(d.enumNames[size_t(i)]) : json("");
    }
    return j;
}

/** The highs' stacks a tree row edits: the High row's own, else the one every high of its group shares. */
json stackFor(const QTreeWidgetItem* it, bool* mixed = nullptr)
{
    if (mixed)
        *mixed = false;
    scene::Node* n = it ? findNode(ss(it->data(0, kGroupRole).toString())) : nullptr;
    if (!n || !bake::isGroup(*n))
        return json::array();
    const std::vector<bake::HighEntry> highs = bake::groupSettings(*n).highs;
    auto stackOf = [](const bake::HighEntry& h) { return h.modifiers.is_array() ? h.modifiers : json::array(); };
    if (it->data(0, kKindRole).toInt() == kHighItem)
    {
        const std::string node = ss(it->data(0, kNodeRole).toString());
        for (const bake::HighEntry& h : highs)
            if (node.empty() ? h.self : h.node == node)
                return stackOf(h);
        return json::array();
    }
    if (highs.empty())
        return json::array();
    const json first = canonicalStack(stackOf(highs.front()));
    for (const bake::HighEntry& h : highs)
        if (canonicalStack(stackOf(h)) != first)
        {
            if (mixed)
                *mixed = true;
            return json::array();
        }
    return stackOf(highs.front());
}

struct LintIssue
{
    std::string group;
    std::string mesh;
    std::string message;
    std::string severity;
};

/** bakeLint's result in whatever shape: a list, or {issues|warnings|findings: [...]}, of strings or objects. */
std::vector<LintIssue> lintIssues(const json& r)
{
    const json* list = &r;
    if (r.is_object())
        for (const char* k : {"issues", "warnings", "findings", "problems"})
            if (r.contains(k) && r[k].is_array())
            {
                list = &r[k];
                break;
            }
    std::vector<LintIssue> out;
    if (!list->is_array())
        return out;
    auto str = [](const json& o, std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (o.contains(k) && o[k].is_string())
                return o[k].get<std::string>();
        return std::string();
    };
    for (const json& i : *list)
    {
        if (i.is_string())
        {
            out.push_back({.message = i.get<std::string>()});
            continue;
        }
        if (!i.is_object())
            continue;
        LintIssue li{.group = str(i, {"group"}),
                     .mesh = str(i, {"mesh", "node", "low", "high"}),
                     .message = str(i, {"message", "text", "what", "detail"}),
                     .severity = str(i, {"severity", "level"})};
        if (li.message.empty())
            li.message = str(i, {"code", "check", "kind"});
        if (li.message.empty())
            li.message = i.dump();
        if (std::string code = str(i, {"code", "check"}); !code.empty() && code != li.message)
            li.message = code + ": " + li.message;
        out.push_back(li);
    }
    return out;
}

bool isRemoveKey(const QKeyEvent* k)
{
    return (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace) && (k->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
}

/** Drops of outliner nodes onto group rows; Delete and Backspace remove the selected rows, never scene objects. */
class GroupTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;
    std::function<void(const QStringList&, QTreeWidgetItem*)> dropped;
    std::function<void()> removeRows;

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ShortcutOverride && removeRows && isRemoveKey(static_cast<QKeyEvent*>(e)))
        {
            e->accept();
            return true;
        }
        return QTreeWidget::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (!removeRows || !isRemoveKey(e))
            return QTreeWidget::keyPressEvent(e);
        e->accept();
        removeRows();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dragEnterEvent(e);
        e->setDropAction(Qt::LinkAction);
        e->accept();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dragMoveEvent(e);
        e->setDropAction(Qt::LinkAction);
        e->accept();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kNodeNamesMime))
            return QTreeWidget::dropEvent(e);
        const QStringList names = QString::fromUtf8(e->mimeData()->data(kNodeNamesMime)).split('\n', Qt::SkipEmptyParts);
        QTreeWidgetItem* at = itemAt(e->position().toPoint());
        e->setDropAction(Qt::LinkAction);
        e->accept();
        if (dropped)
            dropped(names, at);
    }
};

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip, const QString& text = {})
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    if (!text.isEmpty())
    {
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    return b;
}

QDoubleSpinBox* distanceSpin(QWidget* parent, const QString& tip)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(0.0, 1e6);
    s->setDecimals(3);
    s->setSingleStep(0.1);
    s->setSuffix(" cm");
    s->setSpecialValueText("Auto");
    s->setKeyboardTracking(false);
    s->setToolTip(tip);
    return s;
}

/** Known per-map options, so they get proper fields; any other key in a map's options gets a generic one. */
struct MapOption
{
    const char* map;
    const char* key;
    opt::FieldKind kind;
    json def;
    std::vector<std::string> choices;
    const char* doc;
};

const std::vector<MapOption>& mapOptions()
{
    static const std::vector<MapOption> o = {
        {"normal", "greenUp", opt::FieldKind::Check, false, {}, "OpenGL normals (green up); off writes DirectX, as Unreal reads them."},
        {"normal", "flipX", opt::FieldKind::Check, false, {}, "Flip the red channel."},
        {"height", "range", opt::FieldKind::Float, 0.0, {}, "Distance in cm that maps to 0 and 1 around the 0.5 low; 0 is auto."},
        {"id", "source", opt::FieldKind::Choice, "material", {"material", "object", "group", "vertexColor"}, "What gets a colour."},
        {"ao", "rays", opt::FieldKind::Int, 64, {}, "Rays per sample."},
        {"ao", "distance", opt::FieldKind::Float, 0.0, {}, "How far occluders count, cm; 0 is the group's high bounds."},
        {"ao", "scope", opt::FieldKind::Choice, "group", {"group", "self", "all"}, "Occluders: the group's highs, the hit high only, or every group's."},
    };
    return o;
}

opt::Field field(std::string flag, std::string label, opt::FieldKind kind, json def, std::string doc = {})
{
    opt::Field f;
    f.flag = std::move(flag);
    f.label = std::move(label);
    f.kind = kind;
    f.defaultValue = std::move(def);
    f.doc = std::move(doc);
    return f;
}

/** The low's first material slot, for the {material} token in the preview. */
std::string firstMaterial(const std::string& mesh)
{
    scene::Node* n = findNode(mesh);
    if (!n || !n->type().isA("mesh"))
        return {};
    try
    {
        if (scene::MeshPtr m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
            for (const auto& [slot, faces] : scene::materials::slotUsage(*m))
                if (faces > 0)
                    return slot;
    }
    catch (const std::exception&)
    {
    }
    return {};
}
} // namespace

QImage bakeThumbnail(const std::filesystem::path& file, int size)
{
    LILA_ZONE();

    std::string err;
    const image::Image img = image::read(file, &err);
    if (img.width <= 0 || img.height <= 0)
        return {};
    const double scale = std::min(1.0, double(size) / double(std::max(img.width, img.height)));
    const int tw = std::max(1, int(std::lround(img.width * scale)));
    const int th = std::max(1, int(std::lround(img.height * scale)));
    QImage out(tw, th, QImage::Format_RGB32);
    constexpr float kGround = 0.38f;
    for (int y = 0; y < th; ++y)
    {
        const int y0 = y * img.height / th, y1 = std::max(y0 + 1, (y + 1) * img.height / th);
        auto* line = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < tw; ++x)
        {
            const int x0 = x * img.width / tw, x1 = std::max(x0 + 1, (x + 1) * img.width / tw);
            // Box filter with a stride so huge maps don't cost a full pass per thumbnail.
            const int sy = std::max(1, (y1 - y0) / 4), sx = std::max(1, (x1 - x0) / 4);
            float acc[3] = {0.0f, 0.0f, 0.0f};
            int n = 0;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx)
                {
                    const float* p = &img.rgba[(size_t(yy) * size_t(img.width) + size_t(xx)) * 4];
                    const float a = std::clamp(p[3], 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c)
                        acc[c] += std::clamp(p[c], 0.0f, 1.0f) * a + kGround * (1.0f - a);
                    ++n;
                }
            auto byte = [&](int c) { return int(std::lround(acc[c] / float(n) * 255.0f)); };
            line[x] = qRgb(byte(0), byte(1), byte(2));
        }
    }
    return out;
}

QImage bakeContactSheet(const json& result, int tile, int columns)
{
    LILA_ZONE();

    const json outs = result.value("outputs", json::array());
    const int n = int(outs.size());
    if (n == 0)
        return {};
    if (columns <= 0)
        columns = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    const int rows = (n + columns - 1) / columns;
    constexpr int kLabel = 18, kGap = 4;
    QImage sheet(columns * (tile + kGap) + kGap, rows * (tile + kLabel + kGap) + kGap, QImage::Format_RGB32);
    sheet.fill(QColor(40, 40, 40));
    QPainter p(&sheet);
    QFont f = p.font();
    f.setPixelSize(11);
    p.setFont(f);
    for (int i = 0; i < n; ++i)
    {
        const json& o = outs[i];
        const int cx = kGap + (i % columns) * (tile + kGap), cy = kGap + (i / columns) * (tile + kLabel + kGap);
        const QImage t = bakeThumbnail(std::filesystem::path(o.value("file", std::string())), tile);
        if (!t.isNull())
            p.drawImage(cx + (tile - t.width()) / 2, cy + (tile - t.height()) / 2, t);
        else
        {
            p.setPen(QColor(150, 150, 150));
            p.drawText(QRect(cx, cy, tile, tile), Qt::AlignCenter, "unreadable");
        }
        p.setPen(QColor(210, 210, 210));
        const QString label = QString("%1  %2").arg(qs(o.value("map", std::string()))).arg(o.value("udim", 1001));
        p.drawText(QRect(cx, cy + tile, tile, kLabel), Qt::AlignCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, tile));
    }
    return sheet;
}

BakerPanel::BakerPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    v->addWidget(buildToolbar());

    auto* split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(buildGroups());
    mTabs = new QTabWidget(split);
    mTabs->setDocumentMode(true);
    mTabs->addTab(buildGroupDetails(), "Group");
    mTabs->addTab(buildMaps(), "Maps");
    mTabs->addTab(buildOutput(), "Output");
    mTabs->addTab(buildResults(), "Results");
    split->addWidget(mTabs);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    mPoll = new QTimer(this);
    mPoll->setInterval(150);
    connect(mPoll, &QTimer::timeout, this, &BakerPanel::pollProgress);
    mPoll->start();

    mLintTimer = new QTimer(this);
    mLintTimer->setSingleShot(true);
    mLintTimer->setInterval(500);
    connect(mLintTimer, &QTimer::timeout, this, [this] { runLint(false); });

    QPointer<BakerPanel> self(this);
    mGraphListener = graph().addListener([self](scene::Change c, scene::NodeId id) {
        if (!self)
            return;
        if (c == scene::Change::Selection)
        {
            self->updateButtons();
            return;
        }
        if (c == scene::Change::Value)
        {
            const scene::Node* n = graph().node(id);
            if (!n || !(bake::isProject(*n) || bake::isGroup(*n)))
                return;
        }
        self->scheduleRefresh();
        self->scheduleLint();
    });
    mBakeListener = bake::addBakeListener([self](const bake::BakeEvent& e) {
        if (self)
            self->onBakeEvent(e);
    });
    mArrangeListener = bake::arrange::addListener([self] {
        if (self)
            self->updateButtons();
    });
    mExecListener = ops::addExecuteListener([self](const ExecuteEvent& e) {
        if (!self || !e.done)
            return;
        if (e.name == "bake" || e.name == "bakeForTarget" || e.name == "newScene" || e.name == "openScene")
            self->scheduleRefresh();
    });
    refresh();
}

BakerPanel::~BakerPanel()
{
    graph().removeListener(mGraphListener);
    ops::removeExecuteListener(mExecListener);
    bake::removeBakeListener(mBakeListener);
    bake::arrange::removeListener(mArrangeListener);
}

void BakerPanel::onBakeEvent(const bake::BakeEvent& e)
{
    scene::Node* p = findNode(mProject);
    if (!p || p->id() != e.project)
        return;
    switch (e.kind)
    {
    case bake::BakeEvent::Kind::Started:
        mRunningBackend = e.data.value("backend", std::string());
        ++mThumbGeneration;
        mThumbs->clear();
        mThumbJobs = 0;
        mSummary->setText("Baking...");
        mSummaryBox->show();
        updateButtons();
        break;
    case bake::BakeEvent::Kind::Texture: addThumbnail(e.data, mThumbGeneration); break;
    case bake::BakeEvent::Kind::Finished:
        mRunningBackend.clear();
        if (e.data.contains("error"))
        {
            mSummary->setText(e.data.value("cancelled", false) ? QString("Bake cancelled.") : "Bake failed: " + qs(e.data.value("error", std::string())));
            mSummaryBox->show();
        }
        else
            showResult(e.data);
        updateButtons();
        break;
    }
}

void BakerPanel::addThumbnail(const json& o, uint64_t generation)
{
    const std::string file = o.value("file", std::string());
    for (int i = 0; i < mThumbs->count(); ++i)
        if (mThumbs->item(i)->data(kFileRole).toString() == qs(file))
            return;
    auto* item = new QListWidgetItem(icon("photo"), QString("%1\n%2").arg(qs(o.value("map", std::string()))).arg(qs(std::filesystem::path(file).filename().string())),
                                     mThumbs);
    item->setData(kFileRole, qs(file));
    item->setToolTip(QString("%1\n%2 x %3, %4 bit %5, UDIM %6")
                         .arg(qs(file))
                         .arg(o.value("width", 0))
                         .arg(o.value("height", 0))
                         .arg(o.value("bits", 8))
                         .arg(qs(o.value("format", std::string())))
                         .arg(o.value("udim", 1001)));
    ++mThumbJobs;
    QPointer<BakerPanel> self(this);
    const QString key = qs(file);
    QThreadPool::globalInstance()->start([self, file, key, generation] {
        QImage img = bakeThumbnail(std::filesystem::path(file), 128);
        QMetaObject::invokeMethod(qApp, [self, img = std::move(img), key, generation] {
            if (!self || self->mThumbGeneration != generation)
                return;
            --self->mThumbJobs;
            for (int i = 0; i < self->mThumbs->count(); ++i)
                if (QListWidgetItem* it = self->mThumbs->item(i); it->data(kFileRole).toString() == key && !img.isNull())
                    it->setIcon(QIcon(QPixmap::fromImage(img)));
        }, Qt::QueuedConnection);
    });
}

void BakerPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
    scheduleLint();
}

QWidget* BakerPanel::buildToolbar()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    auto* top = new QHBoxLayout;
    top->setSpacing(2);
    mProjects = new QComboBox(w);
    mProjects->setObjectName("bakerProjects");
    mProjects->setToolTip("The bake project shown: output settings, maps and groups");
    mProjects->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mProjects->setMinimumContentsLength(10);
    connect(mProjects, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating)
            setProject(ss(mProjects->currentData().toString()));
    });
    top->addWidget(mProjects, 1);
    mProjectMenu = toolButton(w, "dots", "New, rename or delete a bake project");
    mProjectMenu->setPopupMode(QToolButton::InstantPopup);
    auto* pm = new QMenu(mProjectMenu);
    pm->addAction(icon("plus"), "New Project", this, [this] {
        json r = run("createBakeProject", newProjectArgs(), false);
        if (r.is_string())
            setProject(r.get<std::string>());
    });
    pm->addAction(icon("pencil"), "Rename Project...", this, [this] {
        if (mProject.empty())
            return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename Bake Project", "Name", QLineEdit::Normal, qs(shortName(mProject)), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        json r = runUi("rename", {{"args", {mProject, ss(name)}}});
        if (r.is_string())
            setProject(r.get<std::string>());
    });
    pm->addAction(icon("trash"), "Delete Project", this, [this] {
        if (mProject.empty())
            return;
        std::vector<std::string> doomed;
        scene::Graph& g = graph();
        if (scene::Node* p = findNode(mProject))
            for (scene::NodeId id : bake::groupsOf(g, p->id()))
                doomed.push_back(g.node(id)->path());
        doomed.push_back(mProject);
        runUi("delete", {{"args", doomed}});
    });
    pm->addSeparator();
    pm->addAction(icon("device-floppy"), "Save Settings as Preset...", this, [this] {
        mPresetRow->show();
        mPresetName->setFocus();
        mPresetName->selectAll();
    });
    QAction* deletePreset = pm->addAction(icon("trash"), "Delete Preset", this, [this] {
        if (mTargets->currentData(kPresetRole).toBool() && runUi("deleteBakeTarget", {{"args", {mTarget}}}).is_string())
        {
            mTarget.clear();
            refreshTargets();
            updateButtons();
        }
    });
    pm->addSeparator();
    QAction* openOutput = pm->addAction(icon("folder"), "Open Output Folder", this, [this] { openFolder(outputFolder()); });
    connect(pm, &QMenu::aboutToShow, this, [this, deletePreset, openOutput] {
        openOutput->setEnabled(!outputFolder().isEmpty());
        const bool preset = mTargets->currentData(kPresetRole).toBool();
        deletePreset->setEnabled(preset);
        deletePreset->setText(preset ? "Delete Preset " + mTargets->currentText() : QString("Delete Preset"));
    });
    mProjectMenu->setMenu(pm);
    top->addWidget(mProjectMenu);

    mTargets = new QComboBox(w);
    mTargets->setObjectName("bakerTargets");
    mTargets->setToolTip("Target profile: the maps, conventions and file names a downstream tool expects");
    connect(mTargets, &QComboBox::currentIndexChanged, this, [this](int) {
        if (mUpdating)
            return;
        mTarget = ss(mTargets->currentData().toString());
        PrefStore::get().set(kPrefs, "/target", mTarget);
        if (mTargets->currentData(kPresetRole).toBool())
            if (const json r = run("applyBakeTarget", {{"args", {mTarget}}}, !mProject.empty()); r.is_object())
            {
                if (const std::string be = r.value("backend", std::string()); !be.empty())
                    PrefStore::get().set(kPrefs, "/backend", be);
                if (mProject.empty())
                    refreshProjects();
            }
        updateButtons();
    });
    top->addWidget(mTargets, 1);
    v->addLayout(top);

    mPresetRow = new QWidget(w);
    mPresetRow->setObjectName("bakerPresetRow");
    auto* pr = new QHBoxLayout(mPresetRow);
    pr->setContentsMargins(0, 0, 0, 0);
    pr->setSpacing(2);
    mPresetName = new QLineEdit(mPresetRow);
    mPresetName->setObjectName("bakerPresetName");
    mPresetName->setPlaceholderText("Preset name");
    mPresetName->setToolTip("Saves the output, maps, backend and group defaults (no meshes) as a target you can pick in any scene");
    connect(mPresetName, &QLineEdit::returnPressed, this, &BakerPanel::savePreset);
    pr->addWidget(mPresetName, 1);
    mPresetScope = new QComboBox(mPresetRow);
    mPresetScope->setObjectName("bakerPresetScope");
    mPresetScope->addItem("User", "user");
    mPresetScope->addItem("Workspace", "workspace");
    mPresetScope->setToolTip("User: every project on this machine. Workspace: this workspace's .lila/bake/targets, shared with it");
    pr->addWidget(mPresetScope);
    mPresetReplace = new QCheckBox("Replace", mPresetRow);
    mPresetReplace->setToolTip("Overwrite a preset of the same name");
    pr->addWidget(mPresetReplace);
    QToolButton* savePresetButton = toolButton(mPresetRow, "check", "Save the preset");
    savePresetButton->setObjectName("bakerPresetSave");
    connect(savePresetButton, &QToolButton::clicked, this, &BakerPanel::savePreset);
    pr->addWidget(savePresetButton);
    QToolButton* cancelPreset = toolButton(mPresetRow, "x", "Close without saving");
    connect(cancelPreset, &QToolButton::clicked, mPresetRow, &QWidget::hide);
    pr->addWidget(cancelPreset);
    mPresetRow->hide();
    v->addWidget(mPresetRow);

    mBakeTarget = new QToolButton(w);
    mBakeTarget->setObjectName("bakerBakeTarget");
    mBakeTarget->setIcon(icon("target"));
    mBakeTarget->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mBakeTarget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    mBakeTarget->setMinimumHeight(28);
    QFont bold = mBakeTarget->font();
    bold.setBold(true);
    mBakeTarget->setFont(bold);
    mBakeTarget->setPopupMode(QToolButton::MenuButtonPopup);
    mBakeTarget->setToolTip("One click: apply the target's maps and naming, auto-group by _low/_high names when there are no groups, "
                            "and bake everything it needs");
    connect(mBakeTarget, &QToolButton::clicked, this, [this] { bakeForTarget(false); });
    auto* tm = new QMenu(mBakeTarget);
    QAction* optional = tm->addAction("Include Optional Maps");
    optional->setCheckable(true);
    optional->setChecked(PrefStore::get().value<bool>(kPrefs, "/optional", false));
    connect(optional, &QAction::toggled, this, [](bool on) { PrefStore::get().set(kPrefs, "/optional", on); });
    tm->addAction("Apply Target Only", this, [this] { bakeForTarget(true); })->setToolTip("Set the maps and naming without baking");
    mBakeTarget->setMenu(tm);
    v->addWidget(mBakeTarget);

    auto* row = new QHBoxLayout;
    row->setSpacing(1);
    mBake = toolButton(w, "player-play", "Bake every group of the project", "Bake");
    mBake->setObjectName("bakerBake");
    connect(mBake, &QToolButton::clicked, this, [this] { bake({}); });
    row->addWidget(mBake);
    mBakeSelected = toolButton(w, "player-play", "Bake the groups selected here, or the groups holding the selected meshes", "Selected");
    connect(mBakeSelected, &QToolButton::clicked, this, [this] {
        std::vector<std::string> groups = selectedGroups();
        if (groups.empty())
            groups = groupsForSelection(mProject);
        if (groups.empty())
        {
            log::warn("Bake Selected: select a group here, or a mesh one holds");
            return;
        }
        bake(groups);
    });
    row->addWidget(mBakeSelected);
    mCancel = toolButton(w, "player-stop", "Cancel the running bake");
    connect(mCancel, &QToolButton::clicked, this, [] { cancelBakes(); });
    row->addWidget(mCancel);
    mProgress = new QProgressBar(w);
    mProgress->setObjectName("bakerProgress");
    mProgress->setRange(0, 1000);
    mProgress->setTextVisible(false);
    mProgress->setMaximumHeight(10);
    row->addWidget(mProgress, 1);
    mLintButton = toolButton(w, "alert-triangle", "Check the project for problems (bakeLint)");
    mLintButton->setObjectName("bakerLint");
    connect(mLintButton, &QToolButton::clicked, this, [this] { runLint(true); });
    row->addWidget(mLintButton);
    mBackend = new QToolButton(w);
    mBackend->setObjectName("bakerBackend");
    mBackend->setPopupMode(QToolButton::InstantPopup);
    mBackend->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mBackend->setToolTip("Where bakes run");
    QFont mono = mBackend->font();
    mono.setBold(true);
    if (mono.pointSizeF() > 0.0)
        mono.setPointSizeF(mono.pointSizeF() * 0.85);
    mBackend->setFont(mono);
    auto* bm = new QMenu(mBackend);
    connect(bm, &QMenu::aboutToShow, this, [this, bm] {
        bm->clear();
        const std::string cur = backendPref();
        for (const std::string& b : bake::backendNames())
        {
            QAction* a = bm->addAction(backendBadge(b) + "  (" + qs(b) + ")", this, [this, b] {
                PrefStore::get().set(kPrefs, "/backend", b);
                updateButtons();
            });
            a->setCheckable(true);
            a->setChecked(b == cur);
        }
    });
    mBackend->setMenu(bm);
    row->addWidget(mBackend);
    v->addLayout(row);
    return w;
}

QWidget* BakerPanel::buildGroups()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    QToolButton* add = toolButton(w, "plus", "New group: the selected meshes become its lows");
    connect(add, &QToolButton::clicked, this, [this] {
        json call = json::object();
        if (const std::vector<std::string> m = selectedMeshes(); !m.empty())
            call["lows"] = m;
        run("bakeGroup", call);
    });
    bar->addWidget(add);
    QToolButton* low = toolButton(w, "arrow-down", "Add the selected meshes to the current group as lows", "Low");
    connect(low, &QToolButton::clicked, this, [this] {
        if (const std::string g = currentGroup(); !g.empty() && !selectedMeshes().empty())
            run("bakeGroup", {{"args", {g}}, {"addLow", selectedMeshes()}});
    });
    bar->addWidget(low);
    QToolButton* high = toolButton(w, "arrow-up", "Add the selected meshes to the current group as highs", "High");
    connect(high, &QToolButton::clicked, this, [this] {
        if (const std::string g = currentGroup(); !g.empty() && !selectedMeshes().empty())
            run("bakeGroup", {{"args", {g}}, {"addHigh", selectedMeshes()}});
    });
    bar->addWidget(high);
    QToolButton* autoGroup = toolButton(w, "wand", "Auto-group by name: _low/_high, _lo/_hi, _LP/_HP (the selection, else every mesh)", "Auto");
    autoGroup->setObjectName("bakerAutoGroup");
    connect(autoGroup, &QToolButton::clicked, this, [this] { run("autoGroupBake", json::object()); });
    bar->addWidget(autoGroup);
    QToolButton* perMesh = toolButton(w, "layers-intersect", "One group per selected mesh, each baking onto itself", "Per Mesh");
    connect(perMesh, &QToolButton::clicked, this, [] { groupPerMesh(); });
    bar->addWidget(perMesh);
    mArrange = toolButton(w, "layout-columns", "Arrange: draw the groups side by side to inspect them; the meshes don't move and bakes run where they are", "Arrange");
    mArrange->setObjectName("bakerArrange");
    mArrange->setCheckable(true);
    connect(mArrange, &QToolButton::clicked, this, [this] {
        run("bakeArrange", {{"on", mArrange->isChecked()}});
        updateButtons();
    });
    bar->addWidget(mArrange);
    bar->addStretch(1);
    QToolButton* remove = toolButton(w, "trash", "Remove the selected rows: groups, or meshes from their group");
    remove->setObjectName("bakerRemoveRows");
    connect(remove, &QToolButton::clicked, this, &BakerPanel::removeSelectedRows);
    bar->addWidget(remove);
    v->addLayout(bar);

    auto* tree = new GroupTree(w);
    mGroups = tree;
    mGroups->setObjectName("bakerGroups");
    mGroups->setColumnCount(2);
    mGroups->setHeaderHidden(true);
    mGroups->setUniformRowHeights(true);
    mGroups->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mGroups->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mGroups->setAcceptDrops(true);
    mGroups->setDragDropMode(QAbstractItemView::DropOnly);
    mGroups->setContextMenuPolicy(Qt::CustomContextMenu);
    mGroups->setToolTip("Drag meshes here from the outliner: onto a group or its Low rows as lows, onto High rows as highs, "
                        "onto Cage as its cage, onto empty space for a new group");
    mGroups->header()->setStretchLastSection(true);
    mGroups->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Hotkeys::passThrough(mGroups);
    // Its own context binds Delete to bake.removeRows, ahead of the global edit.delete.
    mGroups->setProperty("hotkeyContext", "baker");
    tree->dropped = [this](const QStringList& names, QTreeWidgetItem* at) { dropNodes(names, at); };
    tree->removeRows = [this] { removeSelectedRows(); };
    connect(mGroups, &QTreeWidget::currentItemChanged, this, [this] {
        if (!mUpdating)
            refreshDetails();
        updateButtons();
    });
    connect(mGroups, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem* it, int) {
        const std::string n = ss(it->data(0, kNodeRole).toString());
        if (!n.empty())
            runUi("select", {{"args", {n}}});
    });
    connect(mGroups, &QTreeWidget::customContextMenuRequested, this, &BakerPanel::groupMenu);
    v->addWidget(mGroups, 1);
    return w;
}

QWidget* BakerPanel::buildGroupDetails()
{
    mDetails = new QWidget(this);
    auto* v = new QVBoxLayout(mDetails);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto edit = [this](const char* flag, double value) {
        const std::string g = currentGroup();
        if (mUpdating || g.empty())
            return;
        scene::Node* n = findNode(g);
        if (!n)
            return;
        const bake::GroupSettings s = bake::groupSettings(*n);
        const double cur = std::string_view(flag) == "frontal" ? s.frontal : std::string_view(flag) == "rear" ? s.rear : s.cageOffset;
        if (std::abs(cur - value) > 1e-9)
            run("bakeGroup", {{"args", {g}}, {flag, value}}, false);
    };
    mFrontal = distanceSpin(mDetails, "How far outside the low rays start; Auto measures it against the highs (shown after a bake or Check)");
    connect(mFrontal, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("frontal", x); });
    form->addRow("Frontal", mFrontal);
    mRear = distanceSpin(mDetails, "How far inside the low rays reach; Auto measures it against the highs (shown after a bake or Check)");
    connect(mRear, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("rear", x); });
    form->addRow("Rear", mRear);
    mCageOffset = new QDoubleSpinBox(mDetails);
    mCageOffset->setRange(0.0, 100.0);
    mCageOffset->setDecimals(3);
    mCageOffset->setSingleStep(0.05);
    mCageOffset->setPrefix("x ");
    mCageOffset->setKeyboardTracking(false);
    mCageOffset->setToolTip("Scales the auto cage's push; a painted bakeOffset point attribute scales it per point");
    connect(mCageOffset, &QDoubleSpinBox::valueChanged, this, [edit](double x) { edit("cageOffset", x); });
    form->addRow("Cage Offset", mCageOffset);

    mCageRow = new QWidget(mDetails);
    auto* cageRow = new QHBoxLayout(mCageRow);
    cageRow->setContentsMargins(0, 0, 0, 0);
    cageRow->setSpacing(2);
    mShowCage = toolButton(mCageRow, "box-model", "Show the group's cage in the viewport: a translucent shell where rays start", "Show");
    mShowCage->setObjectName("bakerShowCage");
    mShowCage->setCheckable(true);
    connect(mShowCage, &QToolButton::clicked, this, [this](bool on) {
        if (scene::Node* n = findNode(currentGroup()); n && bake::isGroup(*n))
            bakeCage::setShown(n->id(), on);
    });
    QToolButton* paint = toolButton(mCageRow, "brush", "Paint the cage's offsets on the low, in the viewport or the UV editor", "Paint");
    connect(paint, &QToolButton::clicked, this, [this] {
        scene::Node* n = findNode(currentGroup());
        if (!n || !bake::isGroup(*n) || bake::groupSettings(*n).lows.empty())
            return;
        runUi("select", {{"args", {bake::groupSettings(*n).lows.front()}}});
        actions::run(json("bake.paintCage"));
    });
    QToolButton* estimate = toolButton(mCageRow, "wand", "Estimate the cage: paint the offsets a near-zero-miss bake needs and set the distances", "Estimate");
    estimate->setObjectName("bakerEstimateCage");
    connect(estimate, &QToolButton::clicked, this, [this] {
        const std::string g = currentGroup();
        if (g.empty())
            return;
        const json r = run("bakeEstimateCage", {{"args", {g}}}, false);
        if (r.is_object() && r.contains("after"))
            mCageStatus->setText(QString("Rays missing: %1% before, %2% after")
                                     .arg(r["before"].value("missRate", 0.0) * 100.0, 0, 'f', 2)
                                     .arg(r["after"].value("missRate", 0.0) * 100.0, 0, 'f', 2));
    });
    mLivePreview = toolButton(mCageRow, "live-view", "Live preview: rebake a low resolution preview on the lows as the cage changes; nothing is written", "Live");
    mLivePreview->setObjectName("bakerLivePreview");
    mLivePreview->setCheckable(true);
    connect(mLivePreview, &QToolButton::clicked, this, [](bool on) { bakeCage::setLivePreview(on); });
    bakeCage::onChanged(this, [this] { scheduleRefresh(); });
    cageRow->addWidget(mShowCage);
    cageRow->addWidget(paint);
    cageRow->addWidget(estimate);
    cageRow->addWidget(mLivePreview);
    cageRow->addStretch(1);
    form->addRow("Cage", mCageRow);
    mCageStatus = new QLabel(mDetails);
    mCageStatus->setWordWrap(true);
    form->addRow(QString(), mCageStatus);
    mAoScope = new QComboBox(mDetails);
    mAoScope->addItem("Group", "group");
    mAoScope->addItem("Self", "self");
    mAoScope->addItem("All Groups", "all");
    mAoScope->setToolTip("Which highs occlude AO rays: this group's (so overlapping groups never shadow each other), only the hit high, or every group's");
    connect(mAoScope, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"aoScope", ss(mAoScope->currentData().toString())}}, false);
    });
    form->addRow("AO Occluders", mAoScope);
    mBackfaces = new QCheckBox("Ignore backfaces", mDetails);
    mBackfaces->setToolTip("Rays only hit high faces facing them");
    connect(mBackfaces, &QCheckBox::toggled, this, [this](bool on) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"ignoreBackfaces", on}}, false);
    });
    form->addRow(QString(), mBackfaces);
    mSelf = new QCheckBox("Self as high", mDetails);
    mSelf->setToolTip("Bake the lows onto themselves through a bake-only modifier stack (e.g. Smooth x2): no high mesh needed");
    connect(mSelf, &QCheckBox::toggled, this, [this](bool on) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"self", on}}, false);
    });
    form->addRow(QString(), mSelf);
    v->addLayout(form);

    mStackBox = new QWidget(mDetails);
    auto* sv = new QVBoxLayout(mStackBox);
    sv->setContentsMargins(0, 4, 0, 0);
    sv->setSpacing(2);
    mStackTitle = new QLabel(mStackBox);
    mStackTitle->setWordWrap(true);
    sv->addWidget(mStackTitle);
    auto* pr = new QHBoxLayout;
    pr->addWidget(new QLabel("Preset", mStackBox));
    mPreset = new QComboBox(mStackBox);
    mPreset->setObjectName("bakerPreset");
    mPreset->addItem("Custom", QString());
    for (const char* p : {"None", "Smooth x1", "Smooth x2", "Smooth x3", "Smooth x4", "Auto Bevel + Smooth"})
        mPreset->addItem(p, p);
    mPreset->setToolTip("Bake-only modifiers evaluated into the bake's high snapshot; the scene never sees the dense mesh");
    connect(mPreset, &QComboBox::activated, this, [this](int i) {
        const QString preset = mPreset->itemData(i).toString();
        const std::string g = currentGroup();
        if (preset.isEmpty() || g.empty())
            return;
        json call = {{"args", {g}}, {"preset", ss(preset)}};
        if (QTreeWidgetItem* it = mGroups->currentItem(); it && it->data(0, kKindRole).toInt() == kHighItem)
        {
            const std::string n = ss(it->data(0, kNodeRole).toString());
            if (n.empty())
                call["self"] = true;
            else
                call["addHigh"] = {n};
        }
        run("bakeGroup", call, false);
    });
    pr->addWidget(mPreset, 1);
    sv->addLayout(pr);
    mStack = new QListWidget(mStackBox);
    mStack->setObjectName("bakerStack");
    mStack->setMaximumHeight(90);
    connect(mStack, &QListWidget::currentRowChanged, this, [this](int) { refreshParams(); });
    sv->addWidget(mStack);
    mStackButtons = new QWidget(mStackBox);
    auto* sb = new QHBoxLayout(mStackButtons);
    sb->setContentsMargins(0, 0, 0, 0);
    sb->setSpacing(1);
    QToolButton* addMod = toolButton(mStackButtons, "plus", "Add a modifier to the bake-only stack");
    addMod->setPopupMode(QToolButton::InstantPopup);
    auto* km = new QMenu(addMod);
    connect(km, &QMenu::aboutToShow, this, [this, km] {
        km->clear();
        for (const std::string& k : scene::modifiers::kinds())
        {
            QAction* a = km->addAction(qs(k), this, [this, k] {
                json s = currentStack();
                s.push_back({{"kind", k}, {"params", k == "subdivision" ? json{{"levels", 2}} : json::object()}});
                setStack(s);
                refreshDetails();
                mStack->setCurrentRow(mStack->count() - 1);
            });
            a->setToolTip(qs(scene::modifiers::kindDoc(k)));
        }
    });
    addMod->setMenu(km);
    sb->addWidget(addMod);
    auto move = [this](int delta) {
        json s = currentStack();
        const int r = mStack->currentRow(), to = r + delta;
        if (r < 0 || to < 0 || to >= int(s.size()))
            return;
        std::swap(s[r], s[to]);
        setStack(s);
        mStack->setCurrentRow(to);
    };
    QToolButton* up = toolButton(mStackButtons, "arrow-up", "Move the modifier earlier");
    connect(up, &QToolButton::clicked, this, [move] { move(-1); });
    sb->addWidget(up);
    QToolButton* down = toolButton(mStackButtons, "arrow-down", "Move the modifier later");
    connect(down, &QToolButton::clicked, this, [move] { move(1); });
    sb->addWidget(down);
    QToolButton* del = toolButton(mStackButtons, "trash", "Remove the modifier");
    connect(del, &QToolButton::clicked, this, [this] {
        json s = currentStack();
        const int r = mStack->currentRow();
        if (r < 0 || r >= int(s.size()))
            return;
        s.erase(s.begin() + r);
        setStack(s);
    });
    sb->addWidget(del);
    sb->addStretch(1);
    sv->addWidget(mStackButtons);
    mParams = new QWidget(mStackBox);
    mParams->setObjectName("bakerStackParams");
    mParamForm = new QFormLayout(mParams);
    mParamForm->setContentsMargins(0, 2, 0, 0);
    mParamForm->setSpacing(3);
    mParamForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sv->addWidget(mParams);
    v->addWidget(mStackBox);
    v->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(mDetails);
    return scroll;
}

QWidget* BakerPanel::buildMaps()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(1);
    mAddMap = toolButton(w, "plus", "Add a map to the project", "Add Map");
    mAddMap->setPopupMode(QToolButton::InstantPopup);
    auto* am = new QMenu(mAddMap);
    connect(am, &QMenu::aboutToShow, this, [this, am] {
        am->clear();
        std::vector<std::string> have;
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
            for (const bake::MapSettings& m : bake::projectSettings(*p).maps)
                have.push_back(m.map);
        for (const bake::MapDef* d : bake::allMaps())
        {
            QAction* a = am->addAction(qs(d->label.empty() ? d->id : d->label), this, [this, id = d->id] { run("setBakeMap", {{"args", {id}}}); });
            a->setToolTip(qs(d->doc));
            a->setEnabled(std::ranges::find(have, d->id) == have.end());
        }
    });
    mAddMap->setMenu(am);
    bar->addWidget(mAddMap);
    bar->addStretch(1);
    QToolButton* remove = toolButton(w, "trash", "Remove the selected map from the project");
    connect(remove, &QToolButton::clicked, this, [this] {
        if (QTreeWidgetItem* it = mMaps->currentItem())
            run("setBakeMap", {{"args", {ss(it->data(0, kNodeRole).toString())}}, {"remove", true}});
    });
    bar->addWidget(remove);
    v->addLayout(bar);

    mMaps = new QTreeWidget(w);
    mMaps->setObjectName("bakerMaps");
    mMaps->setColumnCount(4);
    mMaps->setHeaderLabels({"Map", "Suffix", "Bits", "Format"});
    mMaps->setRootIsDecorated(false);
    mMaps->setUniformRowHeights(true);
    mMaps->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    Hotkeys::passThrough(mMaps);
    connect(mMaps, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int column) {
        if (mUpdating || column != 0)
            return;
        run("setBakeMap", {{"args", {ss(it->data(0, kNodeRole).toString())}}, {"enabled", it->checkState(0) == Qt::Checked}});
    });
    connect(mMaps, &QTreeWidget::currentItemChanged, this, [this] {
        if (!mUpdating)
            refreshMapSettings();
    });
    v->addWidget(mMaps, 1);

    mMapSettings = new QWidget(w);
    mMapForm = new QFormLayout(mMapSettings);
    mMapForm->setContentsMargins(0, 2, 0, 0);
    mMapForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addWidget(mMapSettings);
    return w;
}

QWidget* BakerPanel::buildOutput()
{
    auto* w = new QWidget(this);
    auto* form = new QFormLayout(w);
    form->setContentsMargins(4, 4, 4, 4);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto set = [this](const char* flag, json value) {
        if (!mUpdating)
            run("setBakeOutput", {{flag, std::move(value)}});
    };

    auto* folderRow = new QHBoxLayout;
    mFolder = new QLineEdit(w);
    mFolder->setObjectName("bakerFolder");
    mFolder->setToolTip("Where textures are written; relative paths are under the workspace, else the scene file's folder");
    connect(mFolder, &QLineEdit::textEdited, this, [this] { mFolderResolved->setText(qs(bake::resolveFolder(ss(mFolder->text())).generic_string())); });
    connect(mFolder, &QLineEdit::editingFinished, this, [this, set] {
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p) && bake::projectSettings(*p).output.folder != ss(mFolder->text()))
            set("folder", ss(mFolder->text()));
    });
    folderRow->addWidget(mFolder, 1);
    QToolButton* browse = toolButton(w, "folder-open", "Pick the output folder");
    connect(browse, &QToolButton::clicked, this, [this, set] {
        const QString start = qs(bake::resolveFolder(ss(mFolder->text())).generic_string());
        const QString dir = QFileDialog::getExistingDirectory(this, "Bake Output Folder", start);
        if (!dir.isEmpty())
            set("folder", ss(dir));
    });
    folderRow->addWidget(browse);
    QToolButton* open = toolButton(w, "folder", "Open the output folder");
    connect(open, &QToolButton::clicked, this, [this] {
        openFolder(qs(bake::resolveFolder(ss(mFolder->text())).generic_string()));
    });
    folderRow->addWidget(open);
    form->addRow("Folder", folderRow);
    mFolderResolved = new QLabel(w);
    mFolderResolved->setObjectName("bakerFolderResolved");
    mFolderResolved->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mFolderResolved->setWordWrap(true);
    mFolderResolved->setToolTip("The folder the textures are written to");
    form->addRow(QString(), mFolderResolved);

    auto* nameRow = new QHBoxLayout;
    mName = new QLineEdit(w);
    mName->setObjectName("bakerName");
    mName->setToolTip("File name template; groups whose names resolve to the same file share one texture");
    connect(mName, &QLineEdit::textEdited, this, [this] { updatePreview(); });
    connect(mName, &QLineEdit::editingFinished, this, [this, set] {
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p) && bake::projectSettings(*p).output.name != ss(mName->text()))
            set("name", ss(mName->text()));
    });
    nameRow->addWidget(mName, 1);
    QToolButton* tokens = toolButton(w, "braces", "Insert a token");
    tokens->setPopupMode(QToolButton::InstantPopup);
    auto* tokMenu = new QMenu(tokens);
    for (const std::string& t : bake::nameTokens())
        tokMenu->addAction("{" + qs(t) + "}", this, [this, t] {
            mName->insert("{" + qs(t) + "}");
            updatePreview();
            mName->setFocus();
        });
    tokens->setMenu(tokMenu);
    nameRow->addWidget(tokens);
    form->addRow("Name", nameRow);
    mPreview = new QLabel(w);
    mPreview->setObjectName("bakerPreview");
    mPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mPreview->setWordWrap(true);
    mPreview->setToolTip("The file names this template gives for the first group");
    form->addRow(QString(), mPreview);

    mFormat = new QComboBox(w);
    mFormat->addItem("PNG", "png");
    mFormat->addItem("OpenEXR (half)", "exr");
    mFormat->addItem("TGA", "tga");
    mFormat->addItem("TIFF", "tif");
    mFormat->addItem("PSD (layered)", "psd");
    connect(mFormat, &QComboBox::currentIndexChanged, this, [this, set](int) {
        set("format", ss(mFormat->currentData().toString()));
        updatePreview();
    });
    form->addRow("Format", mFormat);
    mBits = new QComboBox(w);
    mBits->addItem("8 bit", 8);
    mBits->addItem("16 bit", 16);
    mBits->addItem("32 bit float", 32);
    mBits->setToolTip("PNG and PSD take 8 or 16, TIFF 8, 16 or 32 float, EXR 16 (half) or 32 float; TGA is always 8");
    connect(mBits, &QComboBox::currentIndexChanged, this, [this, set](int) { set("bits", mBits->currentData().toInt()); });
    form->addRow("Bits", mBits);
    mResolution = new QComboBox(w);
    mResolution->setObjectName("bakerResolution");
    for (int r : {256, 512, 1024, 2048, 4096, 8192})
        mResolution->addItem(QString("%1 x %1").arg(r), r);
    connect(mResolution, &QComboBox::activated, this, [this, set](int) {
        if (const int r = mResolution->currentData().toInt(); r > 0)
            set("resolution", r);
    });
    form->addRow("Resolution", mResolution);
    mSupersampling = new QComboBox(w);
    for (int s : {1, 4, 16, 64})
        mSupersampling->addItem(s == 1 ? QString("1 (off)") : QString("%1 (%2 x %2)").arg(s).arg(int(std::sqrt(s))), s);
    mSupersampling->setToolTip("Samples per texel");
    connect(mSupersampling, &QComboBox::currentIndexChanged, this, [this, set](int) { set("supersampling", mSupersampling->currentData().toInt()); });
    form->addRow("Supersampling", mSupersampling);
    mPadding = new QSpinBox(w);
    mPadding->setRange(-1, 1024);
    mPadding->setSuffix(" px");
    mPadding->setSpecialValueText("Fill");
    mPadding->setKeyboardTracking(false);
    mPadding->setToolTip("Texels of dilation past island borders; Fill floods the whole texture");
    connect(mPadding, &QSpinBox::valueChanged, this, [set](int x) { set("padding", x); });
    form->addRow("Padding", mPadding);

    auto pipeline = [this](const char* flag) {
        return [this, flag](bool on) {
            if (!mUpdating)
                run("setBakeProject", {{flag, on}});
        };
    };
    mAutoAssign = new QCheckBox("Assign to the lows' materials after baking", w);
    mAutoAssign->setObjectName("bakerAutoAssign");
    mAutoAssign->setToolTip("Every bake wires its textures into the materials of the lows it was baked for (bakeAssign)");
    connect(mAutoAssign, &QCheckBox::toggled, this, pipeline("autoAssign"));
    form->addRow(QString(), mAutoAssign);
    mRebakeOnSave = new QCheckBox("Rebake on save", w);
    mRebakeOnSave->setObjectName("bakerRebakeOnSave");
    mRebakeOnSave->setToolTip("Saving the scene rebakes this project when its meshes or settings changed since the last bake");
    connect(mRebakeOnSave, &QCheckBox::toggled, this, pipeline("rebakeOnSave"));
    form->addRow(QString(), mRebakeOnSave);
    return w;
}

QWidget* BakerPanel::buildResults()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);
    auto* split = new QSplitter(Qt::Vertical, w);
    split->setChildrenCollapsible(false);
    mSummaryBox = new QWidget(split);
    auto* row = new QHBoxLayout(mSummaryBox);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    auto* scroll = new QScrollArea(mSummaryBox);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    mSummary = new QLabel("No bake yet this session.", scroll);
    mSummary->setObjectName("bakerSummary");
    mSummary->setWordWrap(true);
    mSummary->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    mSummary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    connect(mSummary, &QLabel::linkActivated, this, [](const QString& link) { QDesktopServices::openUrl(QUrl(link)); });
    scroll->setWidget(mSummary);
    row->addWidget(scroll, 1);
    auto* openOutput = toolButton(mSummaryBox, "folder", "Open the output folder");
    openOutput->setObjectName("bakerOpenFolder");
    connect(openOutput, &QToolButton::clicked, this, [this] { openFolder(outputFolder()); });
    row->addWidget(openOutput, 0, Qt::AlignTop);
    auto* clear = toolButton(mSummaryBox, "x", "Clear the bake messages");
    clear->setObjectName("bakerClearSummary");
    connect(clear, &QToolButton::clicked, this, [this] {
        mSummary->clear();
        mSummaryBox->hide();
    });
    row->addWidget(clear, 0, Qt::AlignTop);
    mThumbs = new QListWidget(split);
    mThumbs->setObjectName("bakerThumbs");
    mThumbs->setViewMode(QListView::IconMode);
    mThumbs->setIconSize(QSize(128, 128));
    mThumbs->setGridSize(QSize(144, 164));
    mThumbs->setResizeMode(QListView::Adjust);
    mThumbs->setMovement(QListView::Static);
    mThumbs->setWordWrap(true);
    mThumbs->setToolTip("Double-click to open a map; right-click to show it in its folder or copy its path");
    mThumbs->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mThumbs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mThumbs, &QListWidget::itemDoubleClicked, this,
            [](QListWidgetItem* it) { QDesktopServices::openUrl(QUrl::fromLocalFile(it->data(kFileRole).toString())); });
    connect(mThumbs, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QListWidgetItem* hit = mThumbs->itemAt(at);
        if (!hit)
            return;
        if (!hit->isSelected())
            mThumbs->setCurrentItem(hit);
        QMenu* m = thumbnailMenu(this);
        m->setAttribute(Qt::WA_DeleteOnClose);
        m->popup(mThumbs->viewport()->mapToGlobal(at));
    });
    split->addWidget(mSummaryBox);
    split->addWidget(mThumbs);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({60, 400});
    v->addWidget(split, 1);
    return w;
}

void BakerPanel::setProject(const std::string& path)
{
    if (path != mProject)
        mResultFolder.clear();
    mProject = path;
    currentProject() = path;
    refresh();
}

void BakerPanel::scheduleRefresh()
{
    if (mPending)
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void BakerPanel::refresh()
{
    LILA_ZONE();

    mUpdating = true;
    refreshProjects();
    refreshTargets();
    refreshGroups();
    refreshDetails();
    refreshMaps();
    refreshOutput();
    mUpdating = false;
    updateButtons();
    if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
    {
        const json last = bake::lastResult(p->id());
        if (!last.is_null() && last.dump() != mShownResult)
            showResult(last);
    }
}

void BakerPanel::refreshProjects()
{
    scene::Graph& g = graph();
    const std::vector<scene::NodeId> ids = bake::projects(g);
    std::vector<std::string> paths;
    for (scene::NodeId id : ids)
        paths.push_back(g.node(id)->path());
    if (std::ranges::find(paths, mProject) == paths.end())
    {
        const std::string& remembered = currentProject();
        mProject = std::ranges::find(paths, remembered) != paths.end() ? remembered : paths.empty() ? std::string() : paths.front();
    }
    currentProject() = mProject;
    QSignalBlocker b(mProjects);
    mProjects->clear();
    for (const std::string& p : paths)
        mProjects->addItem(icon("texture"), qs(shortName(p)), qs(p));
    if (paths.empty())
        mProjects->addItem("(no bake project)", QString());
    mProjects->setCurrentIndex(std::max(0, mProjects->findData(qs(mProject))));
}

void BakerPanel::refreshTargets()
{
    if (mTarget.empty())
        mTarget = targetPref();
    if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
        if (std::string t = bake::projectSettings(*p).target; !t.empty())
            mTarget = t;
    QSignalBlocker b(mTargets);
    mTargets->clear();
    for (const bake::TargetProfile& t : bake::targets())
    {
        const bool preset = !bake::isBuiltinTarget(t);
        mTargets->addItem(qs(t.label.empty() ? t.name : t.label), qs(t.name));
        mTargets->setItemData(mTargets->count() - 1, preset, kPresetRole);
        const QString where = preset ? QString("Preset, applied when picked (%1)").arg(qs(t.file.generic_string())) : QString();
        mTargets->setItemData(mTargets->count() - 1, t.doc.empty() ? where : where.isEmpty() ? qs(t.doc) : qs(t.doc) + "\n" + where, Qt::ToolTipRole);
    }
    int at = mTargets->findData(qs(mTarget));
    if (at < 0 && mTargets->count() > 0)
    {
        at = 0;
        mTarget = ss(mTargets->itemData(0).toString());
    }
    mTargets->setCurrentIndex(at);
}

void BakerPanel::refreshGroups()
{
    scene::Graph& g = graph();
    const std::string current = currentGroup();
    const int currentKind = mGroups->currentItem() ? mGroups->currentItem()->data(0, kKindRole).toInt() : 0;
    const QString currentNode = mGroups->currentItem() ? mGroups->currentItem()->data(0, kNodeRole).toString() : QString();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);

    json stats = json::object();
    scene::Node* project = findNode(mProject);
    if (project && bake::isProject(*project))
        if (const json last = bake::lastResult(project->id()); last.is_object())
            for (const json& s : last["stats"].value("groups", json::array()))
                stats[s.value("group", std::string())] = s;

    mGroups->setUpdatesEnabled(false);
    mGroups->clear();
    QTreeWidgetItem* restore = nullptr;
    if (!project || !bake::isProject(*project))
    {
        auto* hint = new QTreeWidgetItem(mGroups, {"Make a project, or Bake for a target to start"});
        hint->setForeground(0, dim);
        mGroups->setUpdatesEnabled(true);
        return;
    }
    auto child = [&](QTreeWidgetItem* parent, int kind, const std::string& group, const QString& text, const QString& info, const char* iconName,
                     const std::string& node = {}) {
        auto* it = new QTreeWidgetItem(parent, {text, info});
        it->setData(0, kKindRole, kind);
        it->setData(0, kGroupRole, qs(group));
        it->setData(0, kNodeRole, qs(node));
        if (iconName)
            it->setIcon(0, icon(iconName));
        it->setForeground(1, dim);
        if (group == current && kind == currentKind && it->data(0, kNodeRole).toString() == currentNode)
            restore = it;
        return it;
    };
    for (scene::NodeId id : bake::groupsOf(g, project->id()))
    {
        scene::Node* grp = g.node(id);
        const std::string path = grp->path();
        const bake::GroupSettings s = bake::groupSettings(*grp);
        QString info = QString("%1 low, %2 high").arg(s.lows.size()).arg(s.highs.size());
        if (const json st = stats.value(grp->name(), json()); st.is_object() && st.value("samples", 0) > 0)
            info += QString(", %1% missed").arg(100.0 * st.value("misses", 0.0) / st.value("samples", 1.0), 0, 'f', 2);
        auto* item = new QTreeWidgetItem(mGroups, {qs(grp->name()), info});
        item->setData(0, kKindRole, kGroupItem);
        item->setData(0, kGroupRole, qs(path));
        item->setIcon(0, icon("box"));
        item->setForeground(1, dim);
        if (path == current && currentKind == kGroupItem)
            restore = item;

        QTreeWidgetItem* lows = child(item, kLowsItem, path, "Low", s.lows.empty() ? "drop meshes here" : QString(), "arrow-down");
        for (const std::string& l : s.lows)
        {
            QTreeWidgetItem* r = child(lows, kLowItem, path, qs(shortName(l)), QString(), "cube", l);
            if (!findNode(l))
                r->setText(1, "missing");
        }
        QTreeWidgetItem* highs = child(item, kHighsItem, path, "High", s.highs.empty() ? "drop meshes here, or tick Self as high" : QString(), "arrow-up");
        for (const bake::HighEntry& h : s.highs)
        {
            QTreeWidgetItem* r = child(highs, kHighItem, path, h.self ? QString("(self)") : qs(shortName(h.node)), modifierSummary(h.modifiers),
                                       h.self ? "refresh" : "cube", h.self ? std::string() : h.node);
            if (!h.self && !findNode(h.node))
                r->setText(1, "missing");
        }
        child(item, kCageItem, path, "Cage", s.cage.empty() ? QString("auto, offset x%1").arg(s.cageOffset) : qs(shortName(s.cage)), "box", s.cage);

        for (const LintIssue& li : lintIssues(mLint))
        {
            const bool mine = li.group == path || li.group == grp->name() || (!li.mesh.empty() && (std::ranges::find(s.lows, li.mesh) != s.lows.end() ||
                                                                                                   std::ranges::any_of(s.highs, [&](const bake::HighEntry& h) { return h.node == li.mesh; })));
            if (!mine)
                continue;
            QTreeWidgetItem* r = child(item, kLintItem, path, qs(li.message), qs(li.severity), "alert-triangle", li.mesh);
            r->setToolTip(0, qs(li.message));
        }
        item->setExpanded(true);
        lows->setExpanded(true);
        highs->setExpanded(true);
    }
    // Issues no group claims sit at the top level.
    for (const LintIssue& li : lintIssues(mLint))
    {
        bool claimed = false;
        for (int i = 0; i < mGroups->topLevelItemCount() && !claimed; ++i)
            for (int c = 0; c < mGroups->topLevelItem(i)->childCount() && !claimed; ++c)
                if (QTreeWidgetItem* r = mGroups->topLevelItem(i)->child(c); r->data(0, kKindRole).toInt() == kLintItem && r->text(0) == qs(li.message))
                    claimed = true;
        if (!claimed)
        {
            auto* r = new QTreeWidgetItem(mGroups, {qs(li.message), qs(li.severity)});
            r->setData(0, kKindRole, kLintItem);
            r->setData(0, kNodeRole, qs(li.mesh));
            r->setIcon(0, icon("alert-triangle"));
            r->setToolTip(0, qs(li.message));
        }
    }
    if (restore)
        mGroups->setCurrentItem(restore);
    else if (mGroups->topLevelItemCount() > 0 && mGroups->topLevelItem(0)->data(0, kKindRole).toInt() == kGroupItem)
        mGroups->setCurrentItem(mGroups->topLevelItem(0));
    mGroups->setUpdatesEnabled(true);
}

void BakerPanel::removeSelectedRows()
{
    const QList<QTreeWidgetItem*> rows = mGroups->selectedItems();
    std::vector<std::string> doomed;
    for (QTreeWidgetItem* it : rows)
        if (it->data(0, kKindRole).toInt() == kGroupItem)
            doomed.push_back(ss(it->data(0, kGroupRole).toString()));
    UndoChunk chunk("Remove from Bake Groups");
    for (QTreeWidgetItem* it : rows)
    {
        const std::string g = ss(it->data(0, kGroupRole).toString());
        const std::string n = ss(it->data(0, kNodeRole).toString());
        if (std::ranges::find(doomed, g) != doomed.end())
            continue;
        switch (it->data(0, kKindRole).toInt())
        {
        case kLowItem: run("bakeGroup", {{"args", {g}}, {"removeLow", {n}}}, false); break;
        case kHighItem:
            if (n.empty())
                run("bakeGroup", {{"args", {g}}, {"self", false}}, false);
            else
                run("bakeGroup", {{"args", {g}}, {"removeHigh", {n}}}, false);
            break;
        case kCageItem:
            if (!n.empty())
                run("bakeGroup", {{"args", {g}}, {"cage", ""}}, false);
            break;
        default: break;
        }
    }
    if (!doomed.empty())
        runUi("delete", {{"args", doomed}});
}

std::string BakerPanel::currentGroup() const
{
    if (QTreeWidgetItem* it = mGroups->currentItem())
        return ss(it->data(0, kGroupRole).toString());
    return {};
}

std::vector<std::string> BakerPanel::selectedGroups() const
{
    std::vector<std::string> out;
    for (QTreeWidgetItem* it : mGroups->selectedItems())
        if (const std::string g = ss(it->data(0, kGroupRole).toString()); !g.empty() && std::ranges::find(out, g) == out.end())
            out.push_back(g);
    return out;
}

void BakerPanel::refreshDetails()
{
    const bool was = std::exchange(mUpdating, true);
    const std::string g = currentGroup();
    scene::Node* n = g.empty() ? nullptr : findNode(g);
    const bool has = n && bake::isGroup(*n);
    if (mCageStatus && mCageGroup != g)
        mCageStatus->clear();
    mCageGroup = g;
    mCageRow->setEnabled(has);
    for (QWidget* w : {static_cast<QWidget*>(mFrontal), static_cast<QWidget*>(mRear), static_cast<QWidget*>(mCageOffset),
                       static_cast<QWidget*>(mAoScope), static_cast<QWidget*>(mBackfaces), static_cast<QWidget*>(mSelf)})
        w->setEnabled(has);
    mStackBox->setVisible(has);
    if (has)
    {
        const bake::GroupSettings s = bake::groupSettings(*n);
        // The latest measured Auto distances: a full Check's, else the last bake's.
        json measured;
        auto findMeasured = [&](const json& list) {
            if (measured.is_null() && list.is_array())
                for (const json& d : list)
                    if (d.is_object() && d.value("group", std::string()) == n->name() && d.value("measured", false))
                        measured = d;
        };
        findMeasured(mLint.is_object() ? mLint.value("distances", json()) : json());
        if (scene::Node* p = findNode(mProject); p && bake::isProject(*p))
            if (const json last = bake::lastResult(p->id()); last.is_object() && last.contains("stats"))
                findMeasured(last["stats"].value("groups", json()));
        auto autoText = [&](const char* key) {
            return measured.is_null() ? QString("Auto") : QString("Auto (%1 cm)").arg(measured.value(key, 0.0), 0, 'f', 1);
        };
        mFrontal->setSpecialValueText(autoText("frontal"));
        mRear->setSpecialValueText(autoText("rear"));
        mFrontal->setValue(s.frontal);
        mRear->setValue(s.rear);
        mCageOffset->setValue(s.cageOffset);
        mShowCage->setChecked(bakeCage::shown(n->id()));
        mLivePreview->setChecked(bakeCage::livePreview());
        mAoScope->setCurrentIndex(std::max(0, mAoScope->findData(qs(s.aoScope))));
        mBackfaces->setChecked(s.ignoreBackfaces);
        mSelf->setChecked(std::ranges::any_of(s.highs, [](const bake::HighEntry& h) { return h.self; }));

        QTreeWidgetItem* it = mGroups->currentItem();
        const bool high = it && it->data(0, kKindRole).toInt() == kHighItem;
        bool mixed = false;
        const json stack = stackFor(it, &mixed);
        const bool editable = high || (!mixed && !s.highs.empty());
        const int row = mStack->currentRow();
        {
            QSignalBlocker b(mStack);
            mStack->clear();
            for (const json& m : stack)
            {
                auto* r = new QListWidgetItem(m.is_string() ? qs(m.get<std::string>()) : modifierSummary(json::array({m})), mStack);
                if (m.is_object() && m.contains("params"))
                    r->setToolTip(qs(m["params"].dump()));
            }
            mStack->setCurrentRow(std::min(std::max(row, 0), mStack->count() - 1));
        }
        mStack->setVisible(editable);
        mStackButtons->setVisible(editable);
        mParams->setVisible(editable);
        if (high)
            mStackTitle->setText("Bake-only modifiers on " + it->text(0) + ", evaluated into the bake's snapshot only:");
        else if (s.highs.empty())
            mStackTitle->setText("Bake-only modifiers: add a high, or tick Self as high, to give it a stack.");
        else if (mixed)
            mStackTitle->setText("The highs carry different stacks. Pick a preset to give every high the same one, or select a High row to edit "
                                 "its own.");
        else
            mStackTitle->setText("Bake-only modifiers shared by every high in the group; edits apply to all of them:");
        mPreset->setEnabled(!s.highs.empty());
        mPreset->setItemText(0, mixed ? "Mixed" : "Custom");
        int preset = 0;
        if (!mixed)
        {
            const json canonical = canonicalStack(stack);
            for (int i = 1; i < mPreset->count() && preset == 0; ++i)
                if (const json p = bake::modifierPreset(ss(mPreset->itemData(i).toString())); p.is_array() && canonicalStack(p) == canonical)
                    preset = i;
        }
        mPreset->setCurrentIndex(preset);
        refreshParams();
    }
    mUpdating = was;
}

json BakerPanel::currentStack() const
{
    return stackFor(mGroups->currentItem());
}

void BakerPanel::setStack(const json& stack)
{
    QTreeWidgetItem* it = mGroups->currentItem();
    const std::string g = it ? ss(it->data(0, kGroupRole).toString()) : std::string();
    if (g.empty())
        return;
    json call = {{"args", {g}}, {"modifiers", stack}};
    // Without a high named, bakeGroup sets every high's stack.
    if (it->data(0, kKindRole).toInt() == kHighItem)
    {
        const std::string node = ss(it->data(0, kNodeRole).toString());
        if (node.empty())
            call["self"] = true;
        else
            call["addHigh"] = {node};
    }
    run("bakeGroup", call, false);
}

void BakerPanel::refreshParams()
{
    const json stack = currentStack();
    const int row = mStack->currentRow();
    const json entry = row >= 0 && row < int(stack.size()) ? stack[size_t(row)] : json();
    const std::string kind = entryKind(entry);
    const scene::NodeType* type = modifierType(kind);
    const json params = entry.is_object() ? entry.value("params", json::object()) : json::object();
    QTreeWidgetItem* it = mGroups->currentItem();
    std::string key;
    if (it && !kind.empty())
        key = ss(it->data(0, kGroupRole).toString()) + "|" + ss(it->data(0, kNodeRole).toString()) + "|" + std::to_string(it->data(0, kKindRole).toInt()) +
              "|" + std::to_string(row) + "|" + kind;
    // Rebuilding under a field being edited would drop its focus and spin repeat.
    if (!key.empty() && key == mParamKey)
    {
        for (const auto& set : mParamSetters)
            set(params);
        return;
    }
    mParamKey = key;
    mParamSetters.clear();
    while (mParamForm->rowCount() > 0)
        mParamForm->removeRow(0);
    if (kind.empty())
        return;
    if (!type)
    {
        mParamForm->addRow(new QLabel(qs("'" + kind + "' is not a registered modifier; bakes skip it"), mParams));
        return;
    }
    for (const scene::AttrDef* d : editableParams(*type))
    {
        opt::Field f = field(d->name, d->niceName.empty() ? opt::prettyLabel(d->name) : d->niceName, opt::FieldKind::Text, json(), d->doc);
        switch (d->type)
        {
        case scene::PlugType::Bool: f.kind = opt::FieldKind::Check; break;
        case scene::PlugType::Int: f.kind = opt::FieldKind::Int; break;
        case scene::PlugType::Float: f.kind = opt::FieldKind::Float; break;
        case scene::PlugType::Vec3: f.kind = opt::FieldKind::Vec3; break;
        case scene::PlugType::Enum: f.kind = opt::FieldKind::Choice; f.choices = d->enumNames; break;
        default: break;
        }
        f.defaultValue = fieldValue(*d, d->defaultValue);
        if (d->minValue)
            f.min = *d->minValue;
        if (d->maxValue)
            f.max = *d->maxValue;
        if (d->step)
            f.step = *d->step;
        FlagFieldWidgets w = makeFlagField(
            f, fieldValue(*d, paramValue(*d, params)),
            [this, row, name = d->name](const json& v) {
                if (mUpdating)
                    return;
                json s = currentStack();
                if (row < 0 || row >= int(s.size()))
                    return;
                json& m = s[size_t(row)];
                if (m.is_string())
                    m = {{"kind", m.get<std::string>()}};
                if (!m.contains("params") || !m["params"].is_object())
                    m["params"] = json::object();
                if (m["params"].contains(name) && m["params"][name] == v)
                    return;
                m["params"][name] = v;
                setStack(s);
            },
            mParams);
        w.field->setObjectName(qs("bakerParam_" + d->name));
        if (w.label)
            mParamForm->addRow(w.label, w.field);
        else
            mParamForm->addRow(QString(), w.field);
        mParamSetters.push_back([d, set = w.set](const json& p) {
            if (set)
                set(fieldValue(*d, paramValue(*d, p)));
        });
    }
}

void BakerPanel::refreshMaps()
{
    const std::string current = mMaps->currentItem() ? ss(mMaps->currentItem()->data(0, kNodeRole).toString()) : std::string();
    QSignalBlocker b(mMaps);
    mMaps->clear();
    scene::Node* p = findNode(mProject);
    if (p && bake::isProject(*p))
    {
        const bake::ProjectSettings s = bake::projectSettings(*p);
        const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
        QTreeWidgetItem* restore = nullptr;
        for (const bake::MapSettings& m : s.maps)
        {
            const bake::MapDef* def = bake::findMap(m.map);
            auto* it = new QTreeWidgetItem(mMaps);
            it->setText(0, qs(def && !def->label.empty() ? def->label : m.map));
            it->setData(0, kNodeRole, qs(m.map));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, m.enabled ? Qt::Checked : Qt::Unchecked);
            it->setText(1, qs(m.suffix.empty() && def ? def->suffix : m.suffix));
            it->setText(2, m.bits ? QString::number(m.bits) : QString("(%1)").arg(s.output.bits));
            it->setText(3, m.format.empty() ? QString("(%1)").arg(qs(s.output.format)) : qs(m.format));
            if (!def)
            {
                for (int c = 0; c < 4; ++c)
                    it->setForeground(c, dim);
                it->setToolTip(0, "Not implemented in this build; bakes skip it");
            }
            else
                it->setToolTip(0, qs(def->doc));
            if (m.map == current)
                restore = it;
        }
        mMaps->setCurrentItem(restore ? restore : mMaps->topLevelItem(0));
    }
    refreshMapSettings();
}

void BakerPanel::refreshMapSettings()
{
    QTreeWidgetItem* it = mMaps->currentItem();
    scene::Node* p = findNode(mProject);
    const std::string id = it ? ss(it->data(0, kNodeRole).toString()) : std::string();
    const bake::ProjectSettings s = p && bake::isProject(*p) ? bake::projectSettings(*p) : bake::ProjectSettings{};
    auto m = std::ranges::find_if(s.maps, [&](const bake::MapSettings& x) { return x.map == id; });
    // Rebuilding under a field being edited would drop its focus and spin repeat.
    const std::string formKey = m == s.maps.end() ? std::string() : mProject + m->toJson().dump() + s.output.format + std::to_string(s.output.bits);
    if (formKey == mMapFormKey && mMapForm->rowCount() > 0)
        return;
    mMapFormKey = formKey;
    while (mMapForm->rowCount() > 0)
        mMapForm->removeRow(0);
    if (m == s.maps.end())
        return;
    const bake::MapDef* def = bake::findMap(id);

    auto add = [this, id](const opt::Field& f, const json& value, std::function<json(const json&)> toFlag) {
        FlagFieldWidgets w = makeFlagField(
            f, value,
            [this, id, flag = f.flag, toFlag](const json& v) {
                if (!mUpdating)
                    run("setBakeMap", {{"args", {id}}, {flag, toFlag ? toFlag(v) : v}});
            },
            mMapSettings);
        if (w.label)
            mMapForm->addRow(w.label, w.field);
        else
            mMapForm->addRow(QString(), w.field);
    };
    add(field("suffix", "Suffix", opt::FieldKind::Text, def ? json(def->suffix) : json(""), "The {map} token; empty uses the map's default"), m->suffix,
        {});
    opt::Field bits = field("bits", "Bits", opt::FieldKind::Choice, "0", "PNG bit depth for this map");
    bits.choices = {"0", "8", "16", "32"};
    bits.choiceLabels = {QString("Output's (%1)").arg(s.output.bits).toStdString(), "8", "16", "32"};
    add(bits, std::to_string(m->bits), [](const json& v) { return json(std::stoi(v.get<std::string>())); });
    opt::Field format = field("format", "Format", opt::FieldKind::Choice, "", "File format for this map");
    format.choices = {"", "png", "exr", "tga", "tif"};
    format.choiceLabels = {"Output's (" + s.output.format + ")", "PNG", "OpenEXR", "TGA", "TIFF"};
    add(format, m->format, {});

    std::vector<std::string> shown;
    auto option = [&](const opt::Field& f, const json& value) {
        shown.push_back(f.flag);
        FlagFieldWidgets w = makeFlagField(
            f, value,
            [this, id, key = f.flag](const json& v) {
                if (!mUpdating)
                    run("setBakeMap", {{"args", {id}}, {"options", {{key, v}}}});
            },
            mMapSettings);
        mMapForm->addRow(w.label ? w.label : new QLabel(mMapSettings), w.field);
    };
    for (const MapOption& o : mapOptions())
        if (id == o.map)
        {
            opt::Field f = field(o.key, opt::prettyLabel(o.key), o.kind, o.def, o.doc);
            f.choices = o.choices;
            option(f, m->options.value(o.key, o.def));
        }
    for (auto& [key, value] : m->options.items())
    {
        if (std::ranges::find(shown, key) != shown.end())
            continue;
        if (value.is_boolean())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Check, false), value);
        else if (value.is_number_integer())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Int, 0), value);
        else if (value.is_number())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Float, 0.0), value);
        else if (value.is_string())
            option(field(key, opt::prettyLabel(key), opt::FieldKind::Text, ""), value);
    }
    // Anything else (palettes, nested settings) through raw JSON.
    auto* raw = new QLineEdit(qs(m->options.dump()), mMapSettings);
    raw->setToolTip("Every option of this map as JSON, merged in on Enter (null removes a key)" + (def ? "\n\n" + qs(def->doc) : QString()));
    connect(raw, &QLineEdit::editingFinished, this, [this, raw, id, before = m->options] {
        json v = json::parse(ss(raw->text()), nullptr, false);
        if (v.is_discarded() || !v.is_object())
        {
            log::warn("Baker: map options must be a JSON object");
            return;
        }
        if (v != before)
            run("setBakeMap", {{"args", {id}}, {"options", v}});
    });
    mMapForm->addRow("Options", raw);
}

void BakerPanel::refreshOutput()
{
    scene::Node* p = findNode(mProject);
    const bool has = p && bake::isProject(*p);
    for (QWidget* w : {static_cast<QWidget*>(mFolder), static_cast<QWidget*>(mName), static_cast<QWidget*>(mFormat), static_cast<QWidget*>(mBits),
                       static_cast<QWidget*>(mResolution), static_cast<QWidget*>(mSupersampling), static_cast<QWidget*>(mPadding),
                       static_cast<QWidget*>(mAutoAssign), static_cast<QWidget*>(mRebakeOnSave)})
        w->setEnabled(has);
    if (!has)
    {
        mPreview->clear();
        mFolderResolved->clear();
        return;
    }
    const bool was = std::exchange(mUpdating, true);
    const bake::ProjectSettings ps = bake::projectSettings(*p);
    const bake::OutputSettings& o = ps.output;
    if (!mFolder->hasFocus())
        mFolder->setText(qs(o.folder));
    mFolderResolved->setText(qs(bake::resolveFolder(ss(mFolder->text())).generic_string()));
    mAutoAssign->setChecked(ps.autoAssign);
    mRebakeOnSave->setChecked(ps.rebakeOnSave);
    if (!mName->hasFocus())
        mName->setText(qs(o.name));
    mFormat->setCurrentIndex(std::max(0, mFormat->findData(qs(o.format))));
    mBits->setCurrentIndex(std::max(0, mBits->findData(o.bits)));
    mBits->setEnabled(o.format == "png");
    int res = mResolution->findData(o.width == o.height ? o.width : -1);
    if (res < 0)
    {
        const int custom = mResolution->findData(-1);
        const QString text = QString("%1 x %2").arg(o.width).arg(o.height);
        if (custom >= 0)
            mResolution->setItemText(custom, text);
        else
            mResolution->addItem(text, -1);
        res = mResolution->findData(-1);
    }
    mResolution->setCurrentIndex(res);
    mSupersampling->setCurrentIndex(std::max(0, mSupersampling->findData(o.supersampling)));
    mPadding->setValue(o.padding);
    mUpdating = was;
    updatePreview();
}

void BakerPanel::updatePreview()
{
    scene::Graph& g = graph();
    scene::Node* p = findNode(mProject);
    if (!p || !bake::isProject(*p))
        return;
    const bake::ProjectSettings s = bake::projectSettings(*p);
    bake::NameTokens t;
    t.project = p->name();
    t.ext = s.output.format;
    t.width = s.output.width;
    t.height = s.output.height;
    t.group = "group";
    t.mesh = "mesh";
    t.material = "material";
    for (scene::NodeId id : bake::groupsOf(g, p->id()))
    {
        const bake::GroupSettings gs = bake::groupSettings(*g.node(id));
        t.group = g.node(id)->name();
        if (!gs.lows.empty())
        {
            t.mesh = shortName(gs.lows.front());
            if (std::string m = firstMaterial(gs.lows.front()); !m.empty())
                t.material = m;
        }
        break;
    }
    const std::string tmpl = ss(mName->text());
    QStringList names;
    int enabled = 0;
    for (const bake::MapSettings& m : s.maps)
    {
        if (!m.enabled)
            continue;
        ++enabled;
        if (names.size() >= 3)
            continue;
        const bake::MapDef* def = bake::findMap(m.map);
        t.map = !m.suffix.empty() ? m.suffix : def ? def->suffix : m.map;
        t.ext = m.format.empty() ? s.output.format : m.format;
        names << qs(bake::expandName(tmpl, t));
    }
    QString text = names.join("\n");
    if (enabled > names.size())
        text += QString("\n... %1 more").arg(enabled - names.size());
    if (const std::vector<std::string> unknown = bake::unknownTokens(tmpl); !unknown.empty())
    {
        QStringList u;
        for (const std::string& x : unknown)
            u << "{" + qs(x) + "}";
        text += "\nUnknown tokens kept as written: " + u.join(" ");
    }
    if (!bake::hasToken(tmpl, "group") && !bake::hasToken(tmpl, "mesh") && !bake::hasToken(tmpl, "material") && bake::groupsOf(g, p->id()).size() > 1)
        text += "\nEvery group writes into the same files (no {group}, {mesh} or {material})";
    mPreview->setText(text);
}

void BakerPanel::updateButtons()
{
    scene::Node* p = findNode(mProject);
    const bool has = p && bake::isProject(*p);
    const bool groups = has && !bake::groupsOf(graph(), p->id()).empty();
    mBake->setEnabled(groups);
    mBakeSelected->setEnabled(groups);
    mProjectMenu->menu()->actions().value(1)->setEnabled(has);
    mProjectMenu->menu()->actions().value(2)->setEnabled(has);
    mAddMap->setEnabled(has);
    if (mArrange)
    {
        mArrange->setEnabled(has);
        QSignalBlocker block(mArrange);
        mArrange->setChecked(has && bake::arrange::active(graph(), p->id()));
    }
    QString label = mTargets->currentText();
    mBakeTarget->setText("Bake for " + (label.isEmpty() ? QString("Target") : label));
    mBakeTarget->setEnabled(mTargets->count() > 0);
    const std::string backend = backendPref();
    const json last = p && bake::isProject(*p) ? bake::lastResult(p->id()) : json();
    if (!mRunningBackend.empty())
    {
        mBackend->setText(backendBadge(mRunningBackend));
        mBackend->setToolTip("Baking on the " + qs(mRunningBackend) + " backend; click to change where the next bake runs");
    }
    else if (last.is_object() && last.contains("backendMode"))
    {
        const std::string mode = last.value("backendMode", std::string("cpu"));
        mBackend->setText(modeBadge(mode));
        mBackend->setToolTip(QString("The last bake ran on the %1 backend (%2); the next runs on %3. Click to change.")
                                 .arg(qs(last.value("backend", std::string("cpu"))))
                                 .arg(mode == "rt" ? "ray query" : mode == "compute" ? "compute traversal" : "CPU")
                                 .arg(qs(backend)));
    }
    else
    {
        mBackend->setText(backendBadge(backend));
        mBackend->setToolTip("Bakes run on the " + qs(backend) + " backend; click to change");
    }
    mLintButton->setEnabled(has && ops::find("bakeLint"));
    mLintButton->setToolTip(ops::find("bakeLint") ? "Check the project for problems (bakeLint)" : "Lint is not available in this build");
}

void BakerPanel::scheduleLint()
{
    if (ops::find("bakeLint") && !mProject.empty() && isVisible())
        mLintTimer->start();
}

void BakerPanel::runLint(bool full)
{
    if (!ops::find("bakeLint") || mProject.empty() || !findNode(mProject))
        return;
    json r;
    try
    {
        // The automatic pass skips the highs: loading and tracing them is a bake's worth of work.
        r = ops::execute("bakeLint", {{"project", mProject}, {"highs", full}}, CallSource::Script);
    }
    catch (const std::exception& e)
    {
        log::warn("bakeLint: {}", e.what());
        return;
    }
    if (r == mLint)
        return;
    mLint = r;
    const bool was = std::exchange(mUpdating, true);
    refreshGroups();
    refreshDetails();
    mUpdating = was;
}

void BakerPanel::pollProgress()
{
    if (!isVisible())
        return;
    double fraction = -1.0;
    bool cancellable = false;
    QString status;
    for (const progress::Snapshot& s : progress::snapshot())
        if (s.title.starts_with("Baking") || s.title.starts_with("Preparing bake"))
        {
            fraction = s.determinate ? s.fraction : 0.0;
            cancellable = s.cancellable && !s.cancelled;
            status = qs(s.title + (s.status.empty() ? "" : ": " + s.status));
        }
    mCancel->setEnabled(cancellable);
    if (fraction < 0.0)
    {
        mProgress->setValue(0);
        mProgress->setToolTip("No bake running");
        return;
    }
    mProgress->setValue(int(fraction * 1000.0));
    mProgress->setToolTip(status);
}

json BakerPanel::run(const char* command, json call, bool withProject)
{
    if (withProject && !mProject.empty() && !call.contains("project"))
        call["project"] = mProject;
    return runUi(command, call);
}

void BakerPanel::bake(const std::vector<std::string>& groups)
{
    json call = {{"backend", backendPref()}, {"async", true}};
    if (!groups.empty())
        call["groups"] = groups;
    if (run("bake", call).is_object())
        mTabs->setCurrentIndex(mTabs->count() - 1);
}

void BakerPanel::savePreset()
{
    const QString name = mPresetName->text().trimmed();
    if (name.isEmpty())
    {
        log::warn("Baker: name the preset");
        return;
    }
    const json r = run("saveBakeTarget", {{"args", {ss(name)}},
                                          {"scope", ss(mPresetScope->currentData().toString())},
                                          {"overwrite", mPresetReplace->isChecked()},
                                          {"backend", backendPref()}});
    if (!r.is_object())
        return;
    mPresetRow->hide();
    mPresetName->clear();
    mTarget = r.value("name", ss(name));
    PrefStore::get().set(kPrefs, "/target", mTarget);
    const bool was = std::exchange(mUpdating, true);
    refreshTargets();
    {
        QSignalBlocker b(mTargets);
        mTargets->setCurrentIndex(std::max(0, mTargets->findData(qs(mTarget))));
    }
    mUpdating = was;
    updateButtons();
}

void BakerPanel::bakeForTarget(bool dryRun)
{
    if (mTarget.empty())
        return;
    json call = {{"target", mTarget}, {"optional", PrefStore::get().value<bool>(kPrefs, "/optional", false)}, {"backend", backendPref()}};
    if (dryRun)
        call["dryRun"] = true;
    else
        call["async"] = true;
    json r = run("bakeForTarget", call, !mProject.empty());
    if (!r.is_object())
        return;
    if (mProject.empty())
        refreshProjects();
    if (!dryRun)
        mTabs->setCurrentIndex(mTabs->count() - 1);
}

void BakerPanel::showResult(const json& result)
{
    mShownResult = result.dump();
    mResultFolder = resultFolder(result);
    const json stats = result.value("stats", json::object());
    const double samples = stats.value("samples", 0.0);
    const bool dryRun = result.value("dryRun", false);
    const QString to = mResultFolder.isEmpty() ? QString()
                                               : QString(" to <a href=\"%1\">%2</a>")
                                                     .arg(QUrl::fromLocalFile(mResultFolder).toString().toHtmlEscaped(), mResultFolder.toHtmlEscaped());
    QStringList lines;
    if (dryRun)
        lines << QString("Planned %1 files%2 (dry run).").arg(result.value("outputs", json::array()).size()).arg(to);
    else
        lines << QString("%1 files in %2 s%3. %4 samples, %5% missed, %6 backfaces.")
                     .arg(result.value("outputs", json::array()).size())
                     .arg(result.value("seconds", 0.0), 0, 'f', 2)
                     .arg(to)
                     .arg(samples, 0, 'f', 0)
                     .arg(samples > 0.0 ? 100.0 * stats.value("misses", 0.0) / samples : 0.0, 0, 'f', 2)
                     .arg(stats.value("backfaces", 0));
    if (scene::Node* p = findNode(mProject); !dryRun && p && bake::isProject(*p) && bake::projectSettings(*p).autoAssign)
        if (const json a = bake::autoAssigned(p->id()); a.is_object())
            lines << (a.contains("error") ? "Assigning to the materials failed: " + qs(a.value("error", std::string())).toHtmlEscaped()
                                          : QString("Assigned %1 maps to the lows' materials.").arg(a.value("assigned", json::array()).size()));
    for (const json& g : stats.value("groups", json::array()))
        if (const double s = g.value("samples", 0.0); s > 0.0 && g.value("misses", 0.0) / s > 0.01)
            lines << QString("%1: %2% of rays missed; raise its frontal/rear distance or cage offset.")
                         .arg(qs(g.value("group", std::string())).toHtmlEscaped())
                         .arg(100.0 * g.value("misses", 0.0) / s, 0, 'f', 1);
    for (const json& w : result.value("warnings", json::array()))
        if (w.is_string())
            lines << qs(w.get<std::string>()).toHtmlEscaped();
    mSummary->setText("<html>" + lines.join("<br>") + "</html>");
    mSummaryBox->show();
    if (result.contains("lint") && result["lint"].is_object() && result["lint"] != mLint)
    {
        mLint = result["lint"];
        const bool was = std::exchange(mUpdating, true);
        refreshGroups();
        mUpdating = was;
    }

    // Thumbnails that filled in while the bake ran stay; files of an earlier result go.
    std::set<std::string> files;
    for (const json& o : result.value("outputs", json::array()))
        files.insert(o.value("file", std::string()));
    for (int i = mThumbs->count() - 1; i >= 0; --i)
        if (!files.contains(ss(mThumbs->item(i)->data(kFileRole).toString())))
            delete mThumbs->takeItem(i);
    for (const json& o : result.value("outputs", json::array()))
    {
        if (result.value("dryRun", false))
        {
            auto* item = new QListWidgetItem(icon("photo"), QString("%1\n%2")
                                                                .arg(qs(o.value("map", std::string())))
                                                                .arg(qs(std::filesystem::path(o.value("file", std::string())).filename().string())),
                                             mThumbs);
            item->setData(kFileRole, qs(o.value("file", std::string())));
            continue;
        }
        addThumbnail(o, mThumbGeneration);
    }
    if (const json m = result.value("manifest", json()); m.is_string())
        mSummary->setToolTip("Manifest: " + qs(m.get<std::string>()));
}

QString BakerPanel::outputFolder() const
{
    if (!mResultFolder.isEmpty())
        return mResultFolder;
    scene::Node* p = findNode(mProject);
    return p && bake::isProject(*p) ? qs(bake::resolveFolder(bake::projectSettings(*p).output.folder).generic_string()) : QString();
}

QMenu* BakerPanel::thumbnailMenu(QWidget* parent)
{
    QStringList files;
    for (QListWidgetItem* it : mThumbs->selectedItems())
        files << it->data(kFileRole).toString();
    if (files.isEmpty())
        if (QListWidgetItem* it = mThumbs->currentItem())
            files << it->data(kFileRole).toString();
    auto* m = new QMenu(parent);
    m->setObjectName("bakerThumbMenu");
    const bool any = !files.isEmpty();
    m->addAction(icon("external-link"), files.size() > 1 ? "Open All" : "Open", m, [files] {
          for (const QString& f : files)
              QDesktopServices::openUrl(QUrl::fromLocalFile(f));
      })->setEnabled(any);
    m->addAction(icon("folder-search"), "Open Containing Folder", m, [files] { revealFile(files.front()); })->setEnabled(any);
    m->addSeparator();
    m->addAction(icon("copy"), files.size() > 1 ? "Copy Paths" : "Copy Path", m, [files] {
          QStringList native;
          for (const QString& f : files)
              native << QDir::toNativeSeparators(f);
          QApplication::clipboard()->setText(native.join('\n'));
      })->setEnabled(any);
    m->addAction(icon("file-text"), files.size() > 1 ? "Copy File Names" : "Copy File Name", m, [files] {
          QStringList names;
          for (const QString& f : files)
              names << QFileInfo(f).fileName();
          QApplication::clipboard()->setText(names.join('\n'));
      })->setEnabled(any);
    m->addSeparator();
    m->addAction(icon("folder"), "Open Output Folder", m, [this] { openFolder(outputFolder()); })->setEnabled(!outputFolder().isEmpty());
    return m;
}

void BakerPanel::dropNodes(const QStringList& names, QTreeWidgetItem* target)
{
    // A dropped mesh joins alone; a dropped group brings every mesh under it.
    std::vector<std::string> meshes;
    auto add = [&](const scene::Node& n) {
        if (std::ranges::find(meshes, n.path()) == meshes.end())
            meshes.push_back(n.path());
    };
    scene::Graph& g = graph();
    for (const QString& n : names)
    {
        scene::Node* node = findNode(ss(n));
        if (!node)
            continue;
        if (node->type().isA("mesh"))
            add(*node);
        else
            for (scene::NodeId id : g.descendants(node->id()))
                if (scene::Node* d = g.node(id); d && d->type().isA("mesh"))
                    add(*d);
    }
    if (meshes.empty())
    {
        log::warn("Baker: no meshes to add to the bake group");
        return;
    }
    if (!target || target->data(0, kGroupRole).toString().isEmpty())
    {
        if (mProject.empty())
            if (json r = run("createBakeProject", newProjectArgs(), false); r.is_string())
                setProject(r.get<std::string>());
        run("bakeGroup", {{"lows", meshes}});
        return;
    }
    const std::string grp = ss(target->data(0, kGroupRole).toString());
    switch (target->data(0, kKindRole).toInt())
    {
    case kHighsItem:
    case kHighItem: run("bakeGroup", {{"args", {grp}}, {"addHigh", meshes}}, false); break;
    case kCageItem: run("bakeGroup", {{"args", {grp}}, {"cage", meshes.front()}}, false); break;
    default: run("bakeGroup", {{"args", {grp}}, {"addLow", meshes}}, false); break;
    }
}

void BakerPanel::groupMenu(const QPoint& at)
{
    QTreeWidgetItem* it = mGroups->itemAt(at);
    QMenu menu(this);
    if (!it)
    {
        menu.addAction(icon("plus"), "New Group from Selection", this, [this] { run("bakeGroup", {{"lows", selectedMeshes()}}); });
        menu.addAction(icon("wand"), "Auto Group by Name", this, [this] { run("autoGroupBake", json::object()); });
        menu.exec(mGroups->viewport()->mapToGlobal(at));
        return;
    }
    const std::string g = ss(it->data(0, kGroupRole).toString());
    const std::string node = ss(it->data(0, kNodeRole).toString());
    const int kind = it->data(0, kKindRole).toInt();
    if (!g.empty())
    {
        menu.addAction(icon("player-play"), "Bake This Group", this, [this, g] { bake({g}); });
        menu.addSeparator();
        menu.addAction("Add Selected as Low", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"addLow", selectedMeshes()}}, false); });
        menu.addAction("Add Selected as High", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"addHigh", selectedMeshes()}}, false); });
        menu.addAction("Use Selected as Cage", this, [this, g] {
            if (const std::vector<std::string> m = selectedMeshes(); !m.empty())
                run("bakeGroup", {{"args", {g}}, {"cage", m.front()}}, false);
        });
    }
    if (!node.empty())
        menu.addAction(icon("pointer"), "Select", this, [node] { runUi("select", {{"args", {node}}}); });
    if (kind == kGroupItem)
    {
        menu.addAction(icon("pointer"), "Select Lows and Highs", this, [g] {
            scene::Node* n = findNode(g);
            if (!n)
                return;
            const bake::GroupSettings s = bake::groupSettings(*n);
            std::vector<std::string> all = s.lows;
            for (const bake::HighEntry& h : s.highs)
                if (!h.self)
                    all.push_back(h.node);
            runUi("select", {{"args", all}});
        });
        menu.addAction(icon("pencil"), "Rename...", this, [this, g] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "Rename Bake Group", "Name", QLineEdit::Normal, qs(shortName(g)), &ok).trimmed();
            if (ok && !name.isEmpty())
                runUi("rename", {{"args", {g, ss(name)}}});
        });
        menu.addAction(icon("trash"), "Delete Group", this, [g] { runUi("delete", {{"args", {g}}}); });
    }
    if (kind == kCageItem && !node.empty())
        menu.addAction("Use Auto Cage", this, [this, g] { run("bakeGroup", {{"args", {g}}, {"cage", ""}}, false); });
    if (kind == kLintItem)
    {
        const QString text = it->text(0);
        menu.addAction(icon("copy"), "Copy Message", this, [text] { QApplication::clipboard()->setText(text); });
    }
    menu.exec(mGroups->viewport()->mapToGlobal(at));
}

void registerBakerPanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kBakerDock,
                 .title = "Baker",
                 .create = [](QWidget* p) -> QWidget* { return new BakerPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "texture",
                 .open = false,
                 .toggleAction = "window.baker"});

    actions::add({.id = "bake.forTarget",
                  .label = "Bake for Target",
                  .category = "Bake",
                  .run =
                      [] {
                          json call = projectCall({{"target", targetPref()},
                                                   {"optional", PrefStore::get().value<bool>(kPrefs, "/optional", false)},
                                                   {"backend", backendPref()},
                                                   {"async", true}});
                          runUi("bakeForTarget", call);
                      },
                  .doc = "One click: apply the Baker's target profile (Smart Material Inputs, Unreal, ...) to the project, auto-group "
                         "_low/_high meshes when it has no groups, and bake every map the target needs with the names it expects.",
                  .python = "lila.bake.bake_for_target(\"substancePainter\")"});
    actions::add({.id = "bake.bake",
                  .label = "Bake",
                  .category = "Bake",
                  .run = [] { runUi("bake", projectCall({{"backend", backendPref()}, {"async", true}})); },
                  .doc = "Bake every group of the bake project and write its maps.",
                  .python = "lila.bake.bake()"});
    actions::add({.id = "bake.bakeSelected",
                  .label = "Bake Selected",
                  .category = "Bake",
                  .run =
                      [] {
                          const std::vector<std::string> groups = groupsForSelection(currentProject());
                          if (groups.empty())
                          {
                              log::warn("Bake Selected: no bake group holds the selection");
                              return;
                          }
                          runUi("bake", projectCall({{"groups", groups}, {"backend", backendPref()}, {"async", true}}));
                      },
                  .enabled = [] { return actions::hasSelection(); },
                  .doc = "Bake only the groups holding the selected meshes (as lows, highs or cage).",
                  .python = "lila.bake.bake(groups=[\"crate_bake\"])"});
    actions::add({.id = "bake.cancel",
                  .label = "Cancel Bake",
                  .category = "Bake",
                  .run = [] { cancelBakes(); },
                  .doc = "Stop the running bake; nothing is written for the maps not finished.",
                  .python = "lila.ui.run_action(\"bake.cancel\")"});
    actions::add({.id = "bake.removeRows",
                  .label = "Remove from Bake Group",
                  .category = "Bake",
                  .run =
                      [] {
                          for (QWidget* w = QApplication::focusWidget(); w; w = w->parentWidget())
                              if (auto* panel = qobject_cast<BakerPanel*>(w))
                              {
                                  panel->removeSelectedRows();
                                  return;
                              }
                      },
                  .doc = "In the Baker's groups tree: take the selected Low and High meshes out of their group and delete selected "
                         "groups. The meshes themselves stay in the scene.",
                  .python = "bakeGroup(\"crate_bake\", removeHigh=[\"crate_high\"])"});
    actions::add({.id = "bake.newProject",
                  .label = "New Bake Project",
                  .category = "Bake",
                  .run =
                      [] {
                          if (json r = runUi("createBakeProject", newProjectArgs()); r.is_string())
                              currentProject() = r.get<std::string>();
                          panels::show(kBakerDock);
                      },
                  .doc = "Create a bake project (output folder, naming, resolution, maps) and show it in the Baker.",
                  .python = "createBakeProject()"});
    actions::add({.id = "bake.autoGroup",
                  .label = "Auto Group by Name",
                  .category = "Bake",
                  .run = [] { runUi("autoGroupBake", projectCall()); },
                  .doc = "Make bake groups by name: _low/_high, _lo/_hi and _LP/_HP pairs from the selection, else every mesh.",
                  .python = "autoGroupBake()"});
    actions::add({.id = "bake.groupPerMesh",
                  .label = "One Group per Selected Mesh",
                  .category = "Bake",
                  .run = [] { groupPerMesh(); },
                  .enabled = [] { return actions::hasSelection(); },
                  .doc = "Give each selected mesh its own bake group baking onto itself, so overlapping meshes never affect each other.",
                  .python = "bakeGroup(\"rock_bake\", lows=[\"rock\"], self=True)"});
    actions::add({.id = "bake.lint",
                  .label = "Check Bake Setup",
                  .category = "Bake",
                  .run =
                      [] {
                          if (!ops::find("bakeLint"))
                              return;
                          json r = runUi("bakeLint", projectCall());
                          const std::vector<LintIssue> issues = lintIssues(r);
                          if (issues.empty())
                              log::info("Bake setup: no problems found");
                          for (const LintIssue& i : issues)
                              log::warn("Bake setup: {}", i.message);
                          panels::show(kBakerDock);
                      },
                  .enabled = [] { return ops::find("bakeLint") != nullptr; },
                  .doc = "Look for what spoils bakes: hard edges off UV seams, overlapping or missing UVs, texel density outliers, "
                         "lows with no high in range, cage mismatches.",
                  .python = "bakeLint()"});

    menus::addMenu({.id = "Bake", .title = "Bake", .before = "Rig"});
    menus::addActions("Bake", {"window.baker", "", "bake.forTarget", "bake.bake", "bake.bakeSelected", "bake.cancel", "", "bake.newProject",
                               "bake.autoGroup", "bake.groupPerMesh", "", "bake.lint"});
}
} // namespace lila::ui
