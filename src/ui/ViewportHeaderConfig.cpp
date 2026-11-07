#include "ui/ViewportHeaderConfig.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Icons.h"

#include <QApplication>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace lila::ui::viewportHeader
{
using json = nlohmann::json;

namespace
{
constexpr const char* kMime = "application/x-lila-header-entry";
constexpr int kEntryRole = Qt::UserRole + 1;
constexpr int kKindRole = Qt::UserRole + 2;

/** What a header tree row is: one of the two sides, a menu holding commands, or a plain entry. */
enum class RowKind
{
    Side,
    Menu,
    Entry,
};

bool isSeparator(const json& e)
{
    return e.is_string() && e.get<std::string>() == kSeparator;
}

bool isMenu(const json& e)
{
    return e.is_object() && e.contains("menu");
}

/** Built-ins and separators belong on the row only; a menu holds commands and separators. */
bool fitsInMenu(const json& e)
{
    return isSeparator(e) || (e.is_object() && e.contains("action"));
}
} // namespace

const std::vector<Builtin>& builtins()
{
    static const std::vector<Builtin> all = {
        {"componentMode", "Component Mode", "cube", "Object, Vertex, Edge and Face as one strip."},
        {"shading", "Shading", "sphere", "Wireframe, Shaded and Textured as one strip."},
        {"symmetry", "Symmetry", "brackets-contain", "Toggles symmetry; its arrow picks the plane."},
        {"softSelect", "Soft Select", "ripple", "Toggles soft select; its arrow sets radius and falloff."},
        {"selectionMasks", "Selection Masks", "filter", "What clicks can select: meshes, curves, joints, lights, locators, other."},
        {"snap", "Snap", "magnet", "Move, rotate and scale increments and what to snap to."},
        {"overlays", "Overlays", "eye", "Every display toggle for this view."},
        {"renderer", "Renderer", "aperture", "Which renderer draws this view, and its settings."},
        {"camera", "Camera", "camera", "The view's camera: persp, the ortho views, scene cameras, camera settings."},
    };
    return all;
}

const Builtin* builtin(const std::string& id)
{
    for (const Builtin& b : builtins())
        if (id == b.id)
            return &b;
    return nullptr;
}

json defaultConfig()
{
    return {
        {"left",
         {"componentMode", "shading", kSeparator, "symmetry", "softSelect", kSeparator, "selectionMasks", kSeparator,
          {{"menu", "Display"}, {"icon", "adjustments-horizontal"}, {"items", {"viewport.wireOnShaded", "viewport.defaultMaterial", "viewport.greyBaseColor"}}}}},
        {"right",
         {"snap", "overlays", "camera", kSeparator, {{"action", "viewport.isolate"}}, {{"action", "viewport.toggleImagePlanes"}}, kSeparator,
          {{"action", "view.presenter"}}, {{"action", "viewport.frameSelected"}},
          {{"action", "viewport.frameAll"}}, {{"action", "viewport.toggleQuad"}}, {{"action", "window.toggleMaximizePanel"}}}},
    };
}

json config()
{
    json c = PrefStore::get().value("general", kPref, json());
    if (!c.is_object() || !c.contains("left") || !c.contains("right") || !c["left"].is_array() || !c["right"].is_array())
        return defaultConfig();
    return c;
}

void setConfig(const json& c)
{
    PrefStore::get().set("general", kPref, c);
}

std::string entryLabel(const json& e)
{
    if (isSeparator(e))
        return "Separator";
    if (e.is_string())
    {
        const Builtin* b = builtin(e.get<std::string>());
        return b ? b->label : e.get<std::string>();
    }
    if (isMenu(e))
        return e.value("menu", std::string("Menu"));
    if (e.is_object() && e.contains("action"))
    {
        const std::string id = e.value("action", std::string());
        return actions::find(id) ? actions::displayLabel(id) : id;
    }
    return "?";
}

std::string entryIcon(const json& e)
{
    if (isSeparator(e))
        return "separator-vertical";
    if (e.is_string())
    {
        const Builtin* b = builtin(e.get<std::string>());
        return b ? b->icon : "square";
    }
    if (isMenu(e))
        return e.value("icon", std::string("menu-2"));
    if (e.is_object() && e.contains("action"))
    {
        std::string ic = actions::iconFor(e.value("action", std::string()));
        return ic.empty() ? "point" : ic;
    }
    return "square";
}

namespace
{
QTreeWidgetItem* makeRow(const json& e)
{
    auto* row = new QTreeWidgetItem;
    row->setText(0, QString::fromStdString(entryLabel(e)));
    row->setIcon(0, icon(QString::fromStdString(entryIcon(e))));
    json stored = e;
    if (isMenu(e))
    {
        stored.erase("items");
        row->setData(0, kKindRole, int(RowKind::Menu));
        QFont f = row->font(0);
        f.setWeight(QFont::DemiBold);
        row->setFont(0, f);
        row->setFlags(row->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
        for (const json& item : e.value("items", json::array()))
            row->addChild(makeRow(item.is_string() && !isSeparator(item) ? json{{"action", item}} : item));
    }
    else
    {
        row->setData(0, kKindRole, int(RowKind::Entry));
        row->setFlags((row->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    }
    if (isSeparator(e))
        row->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
    if (e.is_object() && e.contains("action"))
        row->setToolTip(0, QString::fromStdString(actions::tooltip(e.value("action", std::string()))));
    else if (e.is_string())
        if (const Builtin* b = builtin(e.get<std::string>()))
            row->setToolTip(0, b->doc);
    row->setData(0, kEntryRole, QString::fromStdString(stored.dump()));
    return row;
}

json rowEntry(const QTreeWidgetItem* row)
{
    json e = json::parse(row->data(0, kEntryRole).toString().toStdString(), nullptr, false);
    if (row->data(0, kKindRole).toInt() == int(RowKind::Menu))
    {
        json items = json::array();
        for (int i = 0; i < row->childCount(); ++i)
        {
            json c = rowEntry(row->child(i));
            items.push_back(c.is_object() && c.contains("action") ? c["action"] : c);
        }
        e["items"] = std::move(items);
    }
    return e;
}

/** The header's contents: the two sides, each a list of entries, menus holding commands. Accepts entries dragged in. */
class HeaderTree : public QTreeWidget
{
public:
    std::function<void()> changed;

    explicit HeaderTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDropIndicatorShown(true);
        setIndentation(16);
        setUniformRowHeights(true);
    }

    void load(const json& c)
    {
        clear();
        for (const char* side : {"left", "right"})
        {
            auto* root = new QTreeWidgetItem(this);
            root->setText(0, side == std::string("left") ? "Left" : "Right");
            root->setData(0, kKindRole, int(RowKind::Side));
            root->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled);
            QFont f = root->font(0);
            f.setWeight(QFont::DemiBold);
            root->setFont(0, f);
            root->setForeground(0, QApplication::palette().color(QPalette::Disabled, QPalette::Text));
            for (const json& e : c.value(side, json::array()))
                root->addChild(makeRow(e));
        }
        expandAll();
    }

    json save() const
    {
        json c = json::object();
        for (int s = 0; s < topLevelItemCount(); ++s)
        {
            json list = json::array();
            QTreeWidgetItem* root = topLevelItem(s);
            for (int i = 0; i < root->childCount(); ++i)
                list.push_back(rowEntry(root->child(i)));
            c[s == 0 ? "left" : "right"] = std::move(list);
        }
        return c;
    }

    /** Where a new entry goes: into the selected menu or side, else after the selected row, else the end of Left. */
    void insertEntry(const json& e)
    {
        QTreeWidgetItem* at = currentItem();
        QTreeWidgetItem* parent = topLevelItem(0);
        int index = parent ? parent->childCount() : 0;
        if (at)
        {
            const auto kind = RowKind(at->data(0, kKindRole).toInt());
            if (kind == RowKind::Side || (kind == RowKind::Menu && fitsInMenu(e)))
            {
                parent = at;
                index = at->childCount();
            }
            else if (at->parent())
            {
                parent = at->parent();
                index = parent->indexOfChild(at) + 1;
            }
        }
        if (!parent)
            return;
        if (RowKind(parent->data(0, kKindRole).toInt()) == RowKind::Menu && !fitsInMenu(e))
        {
            index = parent->parent()->indexOfChild(parent) + 1;
            parent = parent->parent();
        }
        QTreeWidgetItem* row = makeRow(e);
        parent->insertChild(index, row);
        parent->setExpanded(true);
        setCurrentItem(row);
        if (changed)
            changed();
    }

protected:
    QStringList mimeTypes() const override { return {kMime}; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty())
            return nullptr;
        auto* mime = new QMimeData;
        mime->setData(kMime, QByteArray::fromStdString(rowEntry(items.front()).dump()));
        return mime;
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMime))
            e->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent* e) override
    {
        QTreeWidget::dragMoveEvent(e);
        if (e->mimeData()->hasFormat(kMime))
            e->acceptProposedAction();
    }

    void dropEvent(QDropEvent* e) override
    {
        if (!e->mimeData()->hasFormat(kMime))
            return;
        const json entry = json::parse(e->mimeData()->data(kMime).toStdString(), nullptr, false);
        if (entry.is_discarded())
            return;
        QTreeWidgetItem* target = itemAt(e->position().toPoint());
        QTreeWidgetItem* parent = nullptr;
        int index = 0;
        if (!target)
        {
            parent = topLevelItem(topLevelItemCount() - 1);
            index = parent->childCount();
        }
        else
        {
            const auto kind = RowKind(target->data(0, kKindRole).toInt());
            const DropIndicatorPosition pos = dropIndicatorPosition();
            const bool onto = pos == QAbstractItemView::OnItem;
            if (kind == RowKind::Side || (kind == RowKind::Menu && onto && fitsInMenu(entry)))
            {
                parent = target;
                index = onto ? target->childCount() : 0;
                if (kind == RowKind::Side && pos == QAbstractItemView::AboveItem && indexOfTopLevelItem(target) > 0)
                {
                    parent = topLevelItem(indexOfTopLevelItem(target) - 1);
                    index = parent->childCount();
                }
            }
            else
            {
                parent = target->parent();
                index = parent->indexOfChild(target) + (pos == QAbstractItemView::BelowItem || onto ? 1 : 0);
            }
        }
        if (!parent)
            return;
        // A menu takes commands and separators only; anything else lands beside it.
        if (RowKind(parent->data(0, kKindRole).toInt()) == RowKind::Menu && !fitsInMenu(entry))
        {
            index = parent->parent()->indexOfChild(parent) + 1;
            parent = parent->parent();
        }
        if (e->source() == this)
            if (QTreeWidgetItem* moved = currentItem(); moved && moved->parent())
            {
                QTreeWidgetItem* from = moved->parent();
                if (moved == parent || parent->parent() == moved)
                    return;
                const int at = from->indexOfChild(moved);
                if (from == parent && at < index)
                    --index;
                from->takeChild(at);
            }
        QTreeWidgetItem* row = makeRow(entry);
        parent->insertChild(std::clamp(index, 0, parent->childCount()), row);
        parent->setExpanded(true);
        row->setExpanded(true);
        setCurrentItem(row);
        e->setDropAction(Qt::CopyAction);
        e->accept();
        if (changed)
            changed();
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
        {
            removeCurrent();
            return;
        }
        QTreeWidget::keyPressEvent(e);
    }

public:
    void removeCurrent()
    {
        QTreeWidgetItem* at = currentItem();
        if (!at || !at->parent())
            return;
        delete at;
        if (changed)
            changed();
    }
};

/** Everything that can go on the header: the built-in items, a separator, and every registered command by category. */
class SourceTree : public QTreeWidget
{
public:
    explicit SourceTree(QWidget* parent) : QTreeWidget(parent)
    {
        setHeaderHidden(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setUniformRowHeights(true);
        setIndentation(16);
    }

    void fill(const QString& filter)
    {
        clear();
        auto group = [this](const QString& title) {
            auto* g = new QTreeWidgetItem(this);
            g->setText(0, title);
            g->setFlags(Qt::ItemIsEnabled);
            QFont f = g->font(0);
            f.setWeight(QFont::DemiBold);
            g->setFont(0, f);
            return g;
        };
        auto matches = [&](const QString& text) { return filter.isEmpty() || text.contains(filter, Qt::CaseInsensitive); };
        QTreeWidgetItem* items = group("Header Items");
        for (const Builtin& b : builtins())
            if (matches(b.label))
                items->addChild(makeRow(json(b.id)));
        if (matches("Separator"))
            items->addChild(makeRow(json(kSeparator)));
        if (matches("Menu"))
            items->addChild(makeRow(json{{"menu", "Menu"}, {"icon", "menu-2"}, {"items", json::array()}}));
        std::map<std::string, std::vector<const Action*>> byCategory;
        for (const Action* a : actions::all())
            if (matches(QString::fromStdString(a->label)) || matches(QString::fromStdString(a->category)))
                byCategory[a->category.empty() ? "Other" : a->category].push_back(a);
        for (auto& [cat, list] : byCategory)
        {
            std::sort(list.begin(), list.end(), [](const Action* x, const Action* y) { return x->label < y->label; });
            QTreeWidgetItem* g = group(QString::fromStdString(cat));
            for (const Action* a : list)
                g->addChild(makeRow(json{{"action", a->id}}));
        }
        for (int i = topLevelItemCount() - 1; i >= 0; --i)
            if (topLevelItem(i)->childCount() == 0)
                delete takeTopLevelItem(i);
        if (!filter.isEmpty())
            expandAll();
        else
            topLevelItem(0)->setExpanded(true);
    }

protected:
    QStringList mimeTypes() const override { return {kMime}; }

    QMimeData* mimeData(const QList<QTreeWidgetItem*>& rows) const override
    {
        if (rows.isEmpty() || !rows.front()->parent())
            return nullptr;
        auto* mime = new QMimeData;
        mime->setData(kMime, QByteArray::fromStdString(rowEntry(rows.front()).dump()));
        return mime;
    }
};

class HeaderEditor : public QWidget
{
public:
    explicit HeaderEditor(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Customize Viewport Header");
        setAttribute(Qt::WA_DeleteOnClose);
        resize(640, 560);
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 12);
        root->setSpacing(8);
        auto* hint = new QLabel("Drag commands from the right onto the header. Changes show in every viewport as you make them.", this);
        hint->setWordWrap(true);
        controls::setRole(hint, "hint");
        root->addWidget(hint);

        auto* columns = new QHBoxLayout;
        columns->setSpacing(12);
        auto* left = new QVBoxLayout;
        auto* headerTitle = new QLabel("Header", this);
        QFont bold = headerTitle->font();
        bold.setWeight(QFont::DemiBold);
        headerTitle->setFont(bold);
        left->addWidget(headerTitle);
        mHeader = new HeaderTree(this);
        mHeader->changed = [this] { apply(); };
        mHeader->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(mHeader, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) { rowMenu(mHeader->itemAt(at), mHeader->viewport()->mapToGlobal(at)); });
        connect(mHeader, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
            if (row && RowKind(row->data(0, kKindRole).toInt()) == RowKind::Menu)
                rename(row);
        });
        left->addWidget(mHeader, 1);
        auto* headerButtons = new QHBoxLayout;
        headerButtons->setSpacing(2);
        auto* newMenu = new QToolButton(this);
        newMenu->setText("New Menu");
        controls::setRole(newMenu, "push");
        connect(newMenu, &QToolButton::clicked, this, [this] {
            const QString title = QInputDialog::getText(this, "New Menu", "Name", QLineEdit::Normal, "Display");
            if (!title.trimmed().isEmpty())
                mHeader->insertEntry(json{{"menu", title.trimmed().toStdString()}, {"icon", "menu-2"}, {"items", json::array()}});
        });
        auto* remove = new QToolButton(this);
        remove->setText("Remove");
        controls::setRole(remove, "push");
        connect(remove, &QToolButton::clicked, this, [this] { mHeader->removeCurrent(); });
        headerButtons->addWidget(newMenu);
        headerButtons->addWidget(remove);
        left->addLayout(headerButtons);
        columns->addLayout(left, 1);

        auto* right = new QVBoxLayout;
        auto* addTitle = new QLabel("Commands", this);
        addTitle->setFont(bold);
        right->addWidget(addTitle);
        auto* search = new QLineEdit(this);
        search->setPlaceholderText("Search commands");
        search->addAction(icon("search"), QLineEdit::LeadingPosition);
        search->setClearButtonEnabled(true);
        right->addWidget(search);
        mSource = new SourceTree(this);
        mSource->fill({});
        connect(search, &QLineEdit::textChanged, this, [this](const QString& t) { mSource->fill(t.trimmed()); });
        connect(mSource, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row) {
            if (row && row->parent())
                mHeader->insertEntry(rowEntry(row));
        });
        right->addWidget(mSource, 1);
        auto* add = new QToolButton(this);
        add->setText("Add to Header");
        controls::setRole(add, "push");
        connect(add, &QToolButton::clicked, this, [this] {
            if (QTreeWidgetItem* row = mSource->currentItem(); row && row->parent())
                mHeader->insertEntry(rowEntry(row));
        });
        right->addWidget(add);
        columns->addLayout(right, 1);
        root->addLayout(columns, 1);

        auto* footer = new QHBoxLayout;
        auto* reset = new QToolButton(this);
        reset->setText("Reset to Default");
        controls::setRole(reset, "push");
        connect(reset, &QToolButton::clicked, this, [this] {
            mHeader->load(defaultConfig());
            PrefStore::get().remove("general", kPref);
        });
        footer->addWidget(reset);
        footer->addStretch(1);
        auto* done = new QPushButton("Done", this);
        done->setDefault(true);
        connect(done, &QPushButton::clicked, this, &QWidget::close);
        footer->addWidget(done);
        root->addLayout(footer);

        mHeader->load(config());
    }

private:
    void apply() { setConfig(mHeader->save()); }

    void rename(QTreeWidgetItem* row)
    {
        json e = json::parse(row->data(0, kEntryRole).toString().toStdString(), nullptr, false);
        bool ok = false;
        const QString title = QInputDialog::getText(this, "Rename Menu", "Name", QLineEdit::Normal, row->text(0), &ok);
        if (!ok || title.trimmed().isEmpty())
            return;
        e["menu"] = title.trimmed().toStdString();
        row->setData(0, kEntryRole, QString::fromStdString(e.dump()));
        row->setText(0, title.trimmed());
        apply();
    }

    void rowMenu(QTreeWidgetItem* row, const QPoint& global)
    {
        if (!row || !row->parent())
            return;
        QMenu menu;
        if (RowKind(row->data(0, kKindRole).toInt()) == RowKind::Menu)
            menu.addAction("Rename...", this, [this, row] { rename(row); });
        menu.addAction(icon("x"), "Remove", this, [this] { mHeader->removeCurrent(); });
        mHeader->setCurrentItem(row);
        menu.exec(global);
    }

    HeaderTree* mHeader;
    SourceTree* mSource;
};

QPointer<HeaderEditor> gEditor;
} // namespace

void openEditor(QWidget* anchor)
{
    if (!gEditor)
    {
        QWidget* owner = anchor ? anchor->window() : QApplication::activeWindow();
        gEditor = new HeaderEditor(owner);
        if (owner)
            gEditor->move(owner->geometry().center() - QPoint(gEditor->width() / 2, gEditor->height() / 2));
    }
    gEditor->show();
    gEditor->raise();
    gEditor->activateWindow();
}

void registerActions()
{
    actions::add({.id = "viewport.customizeHeader",
                  .label = "Customize Viewport Header...",
                  .category = "View",
                  .run = [] { openEditor(); },
                  .enabled = {},
                  .doc = "Choose what the row at the top of each viewport shows: add any command, make your own menus of them, reorder "
                         "or remove items. Also on the header's right-click menu.",
                  .icon = "layout-navbar"});
    actions::add({.id = "viewport.resetHeader",
                  .label = "Reset Viewport Header",
                  .category = "View",
                  .run = [] { PrefStore::get().remove("general", kPref); },
                  .enabled = {},
                  .doc = "Puts the viewport header back to Lila's default items.",
                  .icon = "restore"});
}
} // namespace lila::ui::viewportHeader
