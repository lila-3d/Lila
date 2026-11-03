#include "ui/MaterialBrowser.h"

#include "core/Log.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "tex/Library.h"
#pragma pop_macro("slots")
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/MaterialThumbnails.h"
#include "ui/Menus.h"
#include "ui/Panels.h"
#include "ui/TextureGraphUi.h"
#include "ui/Theme.h"
#include "ui/ViewportHooks.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <deque>
#include <format>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
namespace library = tex::library;
using json = nlohmann::json;

constexpr int kMaterialIdRole = Qt::UserRole;
constexpr int kFilterRole = Qt::UserRole + 1;
constexpr int kThumb = 96;

QString qs(const std::string& s) { return QString::fromStdString(s); }

json runUi(const char* cmd, const json& call)
{
    try
    {
        return ops::execute(cmd, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

std::map<std::string, std::string>& presetChoices()
{
    static std::map<std::string, std::string> m;
    return m;
}

/** applyLibraryMaterial on targets (the selection when empty), with the preset chosen in the browser. */
json applyCall(const std::string& id, json args = json::array())
{
    json call = {{"args", json::array({id})}};
    for (const json& a : args)
        call["args"].push_back(a);
    if (std::string p = chosenPreset(id); !p.empty())
        call["preset"] = p;
    return call;
}

QIcon placeholder()
{
    static QIcon icon = [] {
        QPixmap pm(kThumb, kThumb);
        pm.fill(themeShade(40));
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themeShade(90));
        p.drawEllipse(QRectF(kThumb * 0.15, kThumb * 0.15, kThumb * 0.7, kThumb * 0.7));
        return QIcon(pm);
    }();
    return icon;
}

QString tooltipOf(const library::Entry& e)
{
    QString t = QString("<b>%1</b><br>%2").arg(qs(e.meta.name).toHtmlEscaped(), qs(e.id).toHtmlEscaped());
    if (!e.meta.category.empty())
        t += "<br>" + qs(e.meta.category).toHtmlEscaped();
    t += QString("<br>%1, %2 cm%3").arg(qs(e.meta.physical)).arg(e.meta.scaleCm).arg(e.meta.tileable ? ", tileable" : "");
    if (!e.meta.tags.empty())
    {
        QStringList tags;
        for (const std::string& s : e.meta.tags)
            tags << qs(s);
        t += "<br>" + tags.join(", ").toHtmlEscaped();
    }
    if (!e.meta.description.empty())
        t += "<br><i>" + qs(e.meta.description).toHtmlEscaped() + "</i>";
    t += "<br>" + qs(e.tier) + (e.owner.empty() ? QString() : " (" + qs(e.owner) + ")");
    return t;
}

/** The grid: drags carry the material id. */
class MaterialList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    QStringList mimeTypes() const override { return {kMaterialMime, "text/plain"}; }

    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        auto* m = new QMimeData();
        const QString id = items.front()->data(kMaterialIdRole).toString();
        m->setData(kMaterialMime, id.toUtf8());
        m->setText(id);
        return m;
    }
};

/** A drop of a library material over a viewport: onto a face selection, a face's slot, a whole mesh, or the selection. */
bool dropMaterial(const QMimeData& mime, const viewportHooks::DropTarget& target, bool commit)
{
    if (!mime.hasFormat(kMaterialMime))
        return false;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = target.node == scene::kNoNode ? nullptr : g.node(target.node);
    if (!commit)
        return !n || n->type().isA("mesh");
    const std::string id = mime.data(kMaterialMime).toStdString();
    if (!n)
    {
        runUi("applyLibraryMaterial", applyCall(id));
        return true;
    }
    if (!n->type().isA("mesh"))
        return false;
    json call = applyCall(id, {n->path()});
    const scene::components::Ids& faces = scene::components::selected(target.node, scene::ComponentMode::Face);
    scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (scene::components::mode() == scene::ComponentMode::Face && std::find(faces.begin(), faces.end(), target.face) != faces.end())
        call["faces"] = faces;
    else if (mesh && mesh->materialNames().size() > 1 && target.face < mesh->numFaces())
    {
        std::span<const int32_t> faceSlots = mesh->faceAttrs().get<int32_t>(geo::attr::kMaterial);
        if (!faceSlots.empty())
            call["slot"] = scene::materials::slotName(*mesh, faceSlots[target.face]);
    }
    runUi("applyLibraryMaterial", call);
    return true;
}

void logLint(const std::string& target, const json& r)
{
    if (r.is_null())
        return;
    if (r.value("passed", false) && r.value("warnings", 0) == 0)
    {
        log::info("{}: material values look physically plausible", target);
        return;
    }
    for (const json& c : r.value("checks", json::array()))
    {
        const std::string st = c.value("status", "");
        if (st == "fail")
            log::error("{}: {}: {}", target, c.value("id", ""), c.value("message", ""));
        else if (st == "warn")
            log::warn("{}: {}: {}", target, c.value("id", ""), c.value("message", ""));
    }
    for (const json& f : r.value("fixed", json::array()))
        log::info("{}: {}", target, f.get<std::string>());
}
} // namespace

std::string chosenPreset(const std::string& id)
{
    auto it = presetChoices().find(id);
    return it == presetChoices().end() ? std::string() : it->second;
}

struct MaterialBrowser::Impl
{
    MaterialBrowser* self = nullptr;
    QLineEdit* search = nullptr;
    QToolButton* favourites = nullptr;
    QTreeWidget* tree = nullptr;
    MaterialList* list = nullptr;
    QComboBox* presets = nullptr;
    QLabel* status = nullptr;
    QTimer thumbTimer;
    bool inFlight = false;
    std::deque<std::string> pending;
    std::set<std::string> failed;
    std::map<std::string, QListWidgetItem*> items;

    QString currentFilter() const
    {
        QTreeWidgetItem* it = tree->currentItem();
        return it ? it->data(0, kFilterRole).toString() : QString("all");
    }

    void rebuildTree()
    {
        const QString keep = currentFilter();
        QSignalBlocker block(tree);
        tree->clear();
        auto add = [&](QTreeWidgetItem* parent, const QString& label, const QString& filter) {
            auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            it->setText(0, label);
            it->setData(0, kFilterRole, filter);
            return it;
        };
        add(nullptr, "All Materials", "all");
        add(nullptr, "Favourites", "fav");
        std::map<std::string, QTreeWidgetItem*> cats;
        QTreeWidgetItem* catRoot = add(nullptr, "Categories", "all");
        for (const std::string& c : library::categories())
        {
            size_t slash = c.rfind('/');
            QTreeWidgetItem* parent = slash == std::string::npos ? catRoot : cats[c.substr(0, slash)];
            cats[c] = add(parent ? parent : catRoot, qs(slash == std::string::npos ? c : c.substr(slash + 1)), qs("cat:" + c));
        }
        catRoot->setExpanded(true);
        QTreeWidgetItem* tagRoot = add(nullptr, "Tags", "all");
        for (const std::string& t : library::tags())
            add(tagRoot, qs(t), qs("tag:" + t));
        QTreeWidgetItem* tierRoot = add(nullptr, "Source", "all");
        for (const char* t : library::kTiers)
            add(tierRoot, qs(t), qs(std::string("tier:") + t));
        tree->setCurrentItem(tree->topLevelItem(0));
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, kFilterRole).toString() == keep && keep != "all")
                tree->setCurrentItem(*it);
    }

    void fill()
    {
        const std::string keep = self->selectedId();
        library::Query q;
        q.text = search->text().toStdString();
        q.favouritesOnly = favourites->isChecked();
        const QString f = currentFilter();
        if (f == "fav")
            q.favouritesOnly = true;
        else if (f.startsWith("cat:"))
            q.category = f.mid(4).toStdString();
        else if (f.startsWith("tag:"))
            q.tag = f.mid(4).toStdString();
        else if (f.startsWith("tier:"))
            q.tier = f.mid(5).toStdString();
        std::vector<library::Entry> found = library::search(q);
        list->clear();
        items.clear();
        pending.clear();
        for (const library::Entry& e : found)
        {
            auto* it = new QListWidgetItem(qs(e.meta.name), list);
            it->setData(kMaterialIdRole, qs(e.id));
            it->setToolTip(tooltipOf(e));
            if (library::isFavourite(e.id))
                it->setText(QString::fromUtf8("★ ") + qs(e.meta.name));
            std::filesystem::path file = materialThumbs::cachedFile(e.id);
            if (!file.empty())
                it->setIcon(QIcon(QString::fromStdWString(file.wstring())));
            else
            {
                it->setIcon(placeholder());
                if (!failed.count(e.id))
                    pending.push_back(e.id);
            }
            items[e.id] = it;
        }
        status->setText(QString("%1 material%2").arg(found.size()).arg(found.size() == 1 ? "" : "s"));
        if (!keep.empty())
            self->select(keep);
        if (!pending.empty() && render::Renderer::get().running())
            thumbTimer.start();
    }

    /** One thumbnail per tick, so the panel stays responsive while a library renders for the first time. */
    /**
     * Starts the next thumbnail while the panel shows, one at a time. Rendering runs on the thumbnail worker and the icon
     * arrives through the main queue, so the panel never waits on the GPU.
     */
    void renderNext()
    {
        if (pending.empty() || !render::Renderer::get().running())
        {
            thumbTimer.stop();
            return;
        }
        if (inFlight || !self->isVisible())
            return;
        std::string id = pending.front();
        pending.pop_front();
        inFlight = true;
        QPointer<MaterialBrowser> alive(self);
        materialThumbs::renderAsync(id, {}, [this, alive, id](const materialThumbs::Result& r, const std::string& err) {
            if (!alive)
                return;
            inFlight = false;
            if (!err.empty())
            {
                failed.insert(id);
                log::warn("material thumbnail {}: {}", id, err);
            }
            else if (auto it = items.find(id); it != items.end())
                it->second->setIcon(QIcon(QString::fromStdWString(r.file.wstring())));
        });
    }

    void fillPresets()
    {
        QSignalBlocker block(presets);
        presets->clear();
        presets->addItem("Defaults", QString());
        const std::string id = self->selectedId();
        presets->setEnabled(!id.empty());
        if (id.empty())
            return;
        json p = library::presets(id);
        for (auto& [name, values] : p.items())
            presets->addItem(qs(name), qs(name));
        int at = presets->findData(qs(chosenPreset(id)));
        presets->setCurrentIndex(at < 0 ? 0 : at);
    }

    void contextMenu(const QPoint& pos)
    {
        QListWidgetItem* it = list->itemAt(pos);
        if (!it)
            return;
        const std::string id = it->data(kMaterialIdRole).toString().toStdString();
        QMenu menu(list);
        menu.addAction(icon("check"), "Apply to Selection", [this] { self->applySelected(); });
        const std::string set = currentTextureSet();
        if (ops::find("texAddLayer") && !set.empty())
            menu.addAction(icon("stack-2"), qs("Add as Layer to " + set), [id, set] {
                json call = applyCall(id);
                call["args"] = json::array({id});
                call["set"] = set;
                call["layer"] = true;
                runUi("applyLibraryMaterial", call);
            });
        menu.addSeparator();
        const bool fav = library::isFavourite(id);
        menu.addAction(icon("star"), fav ? "Remove from Favourites" : "Add to Favourites", [this, id, fav] {
            runUi("materialFavourite", {{"args", {id}}, {"on", !fav}});
            fill();
        });
        menu.addAction(icon("checklist"), "Check Values", [id] { logLint(id, runUi("materialLint", {{"args", {id}}})); });
        menu.addAction(icon("refresh"), "Render Thumbnail Again", [this, id] {
            materialThumbs::Options o;
            o.force = true;
            QPointer<MaterialBrowser> alive(self);
            materialThumbs::renderAsync(id, o, [this, alive, id](const materialThumbs::Result& r, const std::string& err) {
                if (!alive)
                    return;
                if (!err.empty())
                    log::error("material thumbnail {}: {}", id, err);
                else if (auto i = items.find(id); i != items.end())
                    i->second->setIcon(QIcon(QString::fromStdWString(r.file.wstring())));
            });
        });
        if (!set.empty())
            menu.addAction(icon("device-floppy"), qs("Save Preset from " + set + "..."), [this, id, set] {
                bool ok = false;
                QString name = QInputDialog::getText(self, "Save Preset", "Preset name:", QLineEdit::Normal, "Preset", &ok);
                if (!ok || name.trimmed().isEmpty())
                    return;
                runUi("materialPresets", {{"args", {id}}, {"save", name.trimmed().toStdString()}, {"set", set}});
                fillPresets();
            });
        menu.addSeparator();
        menu.addAction(icon("copy"), "Copy Id", [id] { QApplication::clipboard()->setText(qs(id)); });
        menu.exec(list->viewport()->mapToGlobal(pos));
    }
};

MaterialBrowser::MaterialBrowser(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>())
{
    d->self = this;
    setObjectName("MaterialBrowser");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* top = new QHBoxLayout();
    d->search = new QLineEdit(this);
    d->search->setPlaceholderText("Search materials");
    d->search->setClearButtonEnabled(true);
    d->search->addAction(icon("search"), QLineEdit::LeadingPosition);
    d->favourites = new QToolButton(this);
    d->favourites->setIcon(icon("star"));
    d->favourites->setCheckable(true);
    d->favourites->setToolTip("Only favourites");
    auto* reload = new QToolButton(this);
    reload->setIcon(icon("refresh"));
    reload->setToolTip("Rescan the material folders");
    top->addWidget(d->search, 1);
    top->addWidget(d->favourites);
    top->addWidget(reload);
    v->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal, this);
    d->tree = new QTreeWidget(split);
    d->tree->setHeaderHidden(true);
    d->tree->setMinimumWidth(110);
    d->list = new MaterialList(split);
    d->list->setViewMode(QListView::IconMode);
    d->list->setIconSize(QSize(kThumb, kThumb));
    d->list->setGridSize(QSize(kThumb + 16, kThumb + 34));
    d->list->setResizeMode(QListView::Adjust);
    d->list->setMovement(QListView::Static);
    d->list->setWordWrap(true);
    d->list->setUniformItemSizes(true);
    d->list->setSelectionMode(QAbstractItemView::SingleSelection);
    d->list->setDragEnabled(true);
    d->list->setDragDropMode(QAbstractItemView::DragOnly);
    d->list->setContextMenuPolicy(Qt::CustomContextMenu);
    split->addWidget(d->tree);
    split->addWidget(d->list);
    split->setStretchFactor(1, 1);
    split->setSizes({140, 400});
    v->addWidget(split, 1);

    auto* bottom = new QHBoxLayout();
    bottom->addWidget(new QLabel("Preset", this));
    d->presets = new QComboBox(this);
    d->presets->setToolTip("Parameter preset applied with the material");
    bottom->addWidget(d->presets, 1);
    auto* apply = new QPushButton("Apply", this);
    apply->setToolTip("Put the material on the selected meshes or faces (or drag it onto a mesh)");
    bottom->addWidget(apply);
    v->addLayout(bottom);
    d->status = new QLabel(this);
    d->status->setStyleSheet(themedStyleSheet("color: #8a8a8a;"));
    v->addWidget(d->status);

    d->thumbTimer.setInterval(30);
    QObject::connect(&d->thumbTimer, &QTimer::timeout, this, [this] { d->renderNext(); });
    QObject::connect(d->search, &QLineEdit::textChanged, this, [this] { d->fill(); });
    QObject::connect(d->favourites, &QToolButton::toggled, this, [this] { d->fill(); });
    QObject::connect(reload, &QToolButton::clicked, this, [this] { refresh(); });
    QObject::connect(d->tree, &QTreeWidget::currentItemChanged, this, [this] { d->fill(); });
    QObject::connect(d->list, &QListWidget::currentItemChanged, this, [this] { d->fillPresets(); });
    QObject::connect(d->list, &QListWidget::itemDoubleClicked, this, [this] { applySelected(); });
    QObject::connect(d->list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& p) { d->contextMenu(p); });
    QObject::connect(d->presets, &QComboBox::currentIndexChanged, this, [this] {
        const std::string id = selectedId();
        if (!id.empty())
            presetChoices()[id] = d->presets->currentData().toString().toStdString();
    });
    QObject::connect(apply, &QPushButton::clicked, this, [this] { applySelected(); });
    refresh();
}

MaterialBrowser::~MaterialBrowser() = default;

void MaterialBrowser::refresh()
{
    library::reload();
    d->failed.clear();
    d->rebuildTree();
    d->fill();
    d->fillPresets();
}

QStringList MaterialBrowser::shownIds() const
{
    QStringList out;
    for (int i = 0; i < d->list->count(); ++i)
        out << d->list->item(i)->data(kMaterialIdRole).toString();
    return out;
}

std::string MaterialBrowser::selectedId() const
{
    QListWidgetItem* it = d->list->currentItem();
    return it ? it->data(kMaterialIdRole).toString().toStdString() : std::string();
}

void MaterialBrowser::select(const std::string& id)
{
    if (auto it = d->items.find(id); it != d->items.end())
        d->list->setCurrentItem(it->second);
}

std::string MaterialBrowser::preset() const { return d->presets->currentData().toString().toStdString(); }

void MaterialBrowser::applySelected()
{
    const std::string id = selectedId();
    if (id.empty())
        return;
    runUi("applyLibraryMaterial", applyCall(id));
}

QLineEdit* MaterialBrowser::searchField() const { return d->search; }
QTreeWidget* MaterialBrowser::filterTree() const { return d->tree; }
QListWidget* MaterialBrowser::grid() const { return d->list; }
QComboBox* MaterialBrowser::presetBox() const { return d->presets; }

void registerMaterialBrowserUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kMaterialBrowserDock,
                 .title = "Material Browser",
                 .create = [](QWidget* p) -> QWidget* { return new MaterialBrowser(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "palette",
                 .open = false,
                 .toggleAction = "window.materialBrowser"});
    viewportHooks::setDropHandler("materialBrowser", dropMaterial);

    actions::add({.id = "material.lint",
                  .label = "Check Material Values",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          if (set.empty())
                          {
                              log::warn("Check Material Values: no texture set");
                              return;
                          }
                          logLint(set, runUi("materialLint", {{"args", {set}}}));
                      },
                  .doc = "Check the current texture set's values against physical ranges: albedo, metal reflectance, binary metallic, "
                         "roughness, normals, height and tile seams. Problems go to the output.",
                  .python = "materialLint(\"RockTex\")"});
    actions::add({.id = "material.lintFix",
                  .label = "Fix Material Values",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          if (!set.empty())
                              logLint(set, runUi("materialLint", {{"args", {set}}, {"fix", true}}));
                      },
                  .doc = "Clamp the current texture set's out-of-range values (albedo, metallic, roughness, height) with Levels nodes.",
                  .python = "materialLint(\"RockTex\", fix=True)"});
    actions::add({.id = "material.saveToLibrary",
                  .label = "Save Set to Material Library",
                  .category = "Texture",
                  .run =
                      [] {
                          const std::string set = currentTextureSet();
                          if (set.empty())
                              return;
                          json r = runUi("saveLibraryMaterial", {{"args", {set}}});
                          if (r.is_object())
                              log::info("Saved {} to {}", set, r.value("file", ""));
                      },
                  .doc = "Save the current texture set to your material library, where the Material Browser and applyLibraryMaterial find it.",
                  .python = "saveLibraryMaterial(\"RockTex\", category=\"Stone\", scaleCm=200)"});
    menus::addActions("Texture", {"", "window.materialBrowser", "material.saveToLibrary", "material.lint", "material.lintFix"});
}
} // namespace lila::ui
