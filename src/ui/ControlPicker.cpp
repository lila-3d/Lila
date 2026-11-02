#include "ui/ControlPicker.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "scene/Picker.h"
#include "scene/Poses.h"
#include "ui/Actions.h"
#include "ui/ColorPopup.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/FileDialog.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <map>
#include <set>

namespace lila::ui
{
namespace
{
namespace pk = scene::picker;
using scene::NodeId;

QString qs(const std::string& s) { return QString::fromStdString(s); }
QColor gray(int v, int a = 255) { return themeShade(v, a); }

bool runUi(const char* command, const json& call, json* result = nullptr)
{
    try
    {
        json r = ops::execute(command, call, CallSource::UI);
        if (result)
            *result = std::move(r);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

QColor itemColor(const std::string& c, const QColor& fallback = QColor(128, 128, 128))
{
    QColor q(qs(c));
    return q.isValid() ? q : fallback;
}

/** Black or white, whichever reads on the fill. */
QColor contrast(const QColor& fill)
{
    return fill.lightnessF() > 0.6 ? QColor(20, 20, 20) : QColor(240, 240, 240);
}

/** A path, workspace-relative path, scene-relative path or data: URI. */
QPixmap loadImage(const std::string& ref)
{
    static std::map<std::string, QPixmap> cache;
    if (ref.empty())
        return {};
    if (auto it = cache.find(ref); it != cache.end())
        return it->second;
    QPixmap pm;
    if (ref.starts_with("data:"))
    {
        size_t comma = ref.find(',');
        if (comma != std::string::npos)
        {
            std::string bytes = scene::poses::base64Decode(std::string_view(ref).substr(comma + 1));
            pm.loadFromData(reinterpret_cast<const uchar*>(bytes.data()), uint(bytes.size()));
        }
    }
    else
    {
        namespace fs = std::filesystem;
        fs::path p = paths::fromUtf8(ref);
        std::vector<fs::path> tries = {p};
        if (p.is_relative())
        {
            if (!Workspace::get().root().empty())
                tries.push_back(Workspace::get().root() / p);
            if (!io::currentPath().empty())
                tries.push_back(io::currentPath().parent_path() / p);
        }
        for (const fs::path& t : tries)
            if (pm.load(QString::fromStdWString(t.wstring())))
                break;
    }
    cache[ref] = pm;
    return pm;
}

std::vector<std::string> sortedIds(const std::set<std::string>& s) { return {s.begin(), s.end()}; }

class ControlPickerPanel;

/** Draws one page and turns clicks and drags into picker commands. */
class PickerCanvas : public QWidget
{
public:
    PickerCanvas(ControlPickerPanel* panel, QWidget* parent) : QWidget(parent), mPanel(panel)
    {
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
        setMinimumSize(120, 120);
        setContextMenuPolicy(Qt::DefaultContextMenu);
        setObjectName("pickerCanvas");
    }

    const pk::Page* page() const;
    pk::Page* editablePage();

    void frame()
    {
        const pk::Page* p = page();
        if (!p || width() < 10 || height() < 10)
            return;
        double z = std::min((width() - 24) / p->width, (height() - 24) / p->height);
        mZoom = std::clamp(z, 0.05, 20.0);
        mPan = QPointF((width() - p->width * mZoom) * 0.5, (height() - p->height * mZoom) * 0.5);
        mFramed = true;
        update();
    }

    void resetFraming() { mFramed = false; }

    std::set<std::string> editSelection;
    double grid = 8.0;
    bool snapping = true;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override
    {
        if (!mFramed)
            frame();
    }
    void showEvent(QShowEvent*) override
    {
        if (!mFramed)
            frame();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        QPointF at = e->position();
        QPointF before = toPage(at);
        double steps = e->angleDelta().y() / 120.0;
        mZoom = std::clamp(mZoom * std::pow(1.15, steps), 0.05, 20.0);
        mPan += at - toView(before);
        mFramed = true;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    enum class Drag
    {
        None,
        Pan,
        Marquee,
        Move,
        Resize,
    };

    QPointF toPage(QPointF v) const { return (v - mPan) / mZoom; }
    QPointF toView(QPointF p) const { return p * mZoom + mPan; }
    QRectF viewRect(const pk::Item& i) const { return QRectF(toView({i.x, i.y}), QSizeF(i.w * mZoom, i.h * mZoom)); }
    QPainterPath shapePath(const pk::Item& i) const;
    /** The resize handle of the one edit-selected item, in view space. */
    std::optional<QRectF> handle() const;
    void selectFromClick(const std::vector<std::string>& ids, Qt::KeyboardModifiers mods);
    void commitGeometry();

    ControlPickerPanel* mPanel;
    double mZoom = 1.0;
    QPointF mPan{12, 12};
    bool mFramed = false;
    Drag mDrag = Drag::None;
    QPointF mPress, mLast;
    Qt::KeyboardModifiers mPressMods;
    /** Item geometry when a move or resize began, to compute deltas from and to commit only real changes. */
    std::map<std::string, QRectF> mStart;
};

class ControlPickerPanel : public QWidget
{
public:
    explicit ControlPickerPanel(QWidget* parent) : QWidget(parent)
    {
        sActive = this;
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        auto* top = new QHBoxLayout;
        mPicker = new QComboBox(this);
        mPicker->setToolTip("Picker document: scene pickers and pickers rig templates provide");
        mPicker->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        mPicker->setMinimumContentsLength(10);
        top->addWidget(mPicker, 1);
        mNamespace = new QComboBox(this);
        mNamespace->setToolTip("Character: the namespace the picker's targets resolve in. It follows the selection");
        mNamespace->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        top->addWidget(mNamespace, 1);
        mEditButton = toolButton("pencil", "Edit mode: move, resize, add and recolour items");
        mEditButton->setCheckable(true);
        top->addWidget(mEditButton);
        auto* menuButton = toolButton("menu-2", "Picker, page and file commands");
        menuButton->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(menuButton);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] { fillMenu(menu); });
        menuButton->setMenu(menu);
        top->addWidget(menuButton);
        layout->addLayout(top);

        mEditBar = new QWidget(this);
        auto* edit = new QHBoxLayout(mEditBar);
        edit->setContentsMargins(0, 0, 0, 0);
        edit->setSpacing(1);
        auto add = [&](const char* iconName, const char* tip, std::function<void()> fn) {
            QToolButton* b = toolButton(iconName, tip);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            edit->addWidget(b);
            return b;
        };
        add("square", "Add a rectangle", [this] { addShape("rect"); });
        add("circle", "Add a circle", [this] { addShape("circle"); });
        add("typography", "Add a text label", [this] { addShape("text"); });
        add("hand-click", "Add an item per selected node", [this] { addFromSelection(); });
        add("trash", "Delete the selected items", [this] { removeItems(); });
        add("flip-vertical", "Mirror the selected items to the other side", [this] { mirrorItems(); });
        add("palette", "Colour of the selected items", [this] { pickColor(); });
        edit->addSpacing(6);
        add("layout-align-left", "Align left edges", [this] { alignItems("left"); });
        add("layout-align-center", "Centre horizontally", [this] { alignItems("hcenter"); });
        add("layout-align-right", "Align right edges", [this] { alignItems("right"); });
        add("layout-align-top", "Align top edges", [this] { alignItems("top"); });
        add("layout-align-middle", "Centre vertically", [this] { alignItems("vcenter"); });
        add("layout-align-bottom", "Align bottom edges", [this] { alignItems("bottom"); });
        add("layout-distribute-horizontal", "Even horizontal gaps", [this] { distributeItems("horizontal"); });
        add("layout-distribute-vertical", "Even vertical gaps", [this] { distributeItems("vertical"); });
        edit->addSpacing(6);
        QToolButton* snapButton = toolButton("grid-dots", "Snap moves and resizes to the grid");
        snapButton->setCheckable(true);
        snapButton->setChecked(true);
        edit->addWidget(snapButton);
        auto* gridSize = new QDoubleSpinBox(mEditBar);
        gridSize->setRange(1, 128);
        gridSize->setDecimals(0);
        gridSize->setValue(8);
        gridSize->setToolTip("Grid size in page units");
        edit->addWidget(gridSize);
        edit->addStretch(1);
        layout->addWidget(mEditBar);
        mEditBar->hide();

        mTabs = new QTabBar(this);
        mTabs->setDrawBase(false);
        mTabs->setExpanding(false);
        layout->addWidget(mTabs);

        mCanvas = new PickerCanvas(this, this);
        layout->addWidget(mCanvas, 1);
        // Outside edit mode the picked items are rig controls, which Delete must not remove from the scene.
        deletion::claim(this, [this] {
            if (editing())
                removeItems();
        });

        connect(snapButton, &QToolButton::toggled, this, [this](bool on) { mCanvas->snapping = on; });
        connect(gridSize, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            mCanvas->grid = v;
            mCanvas->update();
        });
        connect(mEditButton, &QToolButton::toggled, this, [this](bool on) { setEditing(on); });
        connect(mPicker, &QComboBox::activated, this, [this] {
            mName = mPicker->currentData().toString().toStdString();
            mNs.reset();
            mCanvas->editSelection.clear();
            mCanvas->resetFraming();
            reload();
            mCanvas->frame();
        });
        connect(mNamespace, &QComboBox::activated, this, [this] {
            mNs = mNamespace->currentData().toString().toStdString();
            refreshResolved();
        });
        connect(mTabs, &QTabBar::currentChanged, this, [this](int i) {
            if (mSyncingTabs || i < 0)
                return;
            mPage = mTabs->tabText(i).toStdString();
            mCanvas->editSelection.clear();
            mCanvas->resetFraming();
            refreshResolved();
            mCanvas->frame();
        });

        mPickerListener = pk::addListener([this] { scheduleReload(); });
        mGraphListener = scene::Graph::current().addListener([this](scene::Change c, NodeId) {
            if (c == scene::Change::Selection)
            {
                followSelection();
                mCanvas->update();
            }
            else if (c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Renamed ||
                     c == scene::Change::Reset || c == scene::Change::Cleared)
                scheduleReload();
        });
        reload();
    }

    ~ControlPickerPanel() override
    {
        pk::removeListener(mPickerListener);
        scene::Graph::current().removeListener(mGraphListener);
    }

    static QPointer<ControlPickerPanel> sActive;

    // State the canvas reads.
    std::optional<pk::Document> doc;
    std::string pageName() const { return mPage; }
    std::string nameSpace() const { return mNs.value_or(doc ? doc->nameSpace : std::string()); }
    bool editing() const { return mEditButton->isChecked(); }
    const std::vector<NodeId>& resolved(const std::string& itemId) const
    {
        static const std::vector<NodeId> kNone;
        auto it = mResolved.find(itemId);
        return it == mResolved.end() ? kNone : it->second;
    }
    bool isTemplate() const { return doc && !pk::findScene(doc->name); }

    json call(json extra = json::object()) const
    {
        extra["args"] = json::array({doc ? doc->name : std::string()});
        return extra;
    }

    json pageCall(json extra = json::object()) const
    {
        json c = call(std::move(extra));
        c["page"] = mPage;
        return c;
    }

    /** Template pickers are read only; editing one first makes a scene copy of the same name, which shadows it. */
    bool ensureEditable()
    {
        if (!doc)
            return false;
        if (!isTemplate())
            return true;
        return runUi("createPicker", {{"args", {doc->name}}, {"fromTemplate", doc->name}});
    }

    void edit(json extra)
    {
        if (ensureEditable())
            runUi("editPicker", pageCall(std::move(extra)));
    }

    void select(const std::vector<std::string>& ids, const char* mode)
    {
        if (!doc)
            return;
        json c = call({{"items", ids}, {"mode", mode}, {"page", mPage}});
        c["namespace"] = nameSpace();
        runUi("pickerSelect", c);
    }

    void activate(const pk::Item& item)
    {
        json c = call();
        c["args"].push_back(item.id);
        c["namespace"] = nameSpace();
        runUi("pickerRun", c);
    }

    void setEditing(bool on)
    {
        if (mEditButton->isChecked() != on)
            mEditButton->setChecked(on);
        mEditBar->setVisible(on);
        mCanvas->editSelection.clear();
        mCanvas->update();
    }

    void toggleEditing() { setEditing(!editing()); }
    void frame() { mCanvas->frame(); }

    std::vector<std::string> editIds() const { return sortedIds(mCanvas->editSelection); }

    void addShape(const char* shape, std::optional<QPointF> at = {})
    {
        const pk::Page* p = doc ? doc->page(mPage) : nullptr;
        if (!p)
            return;
        QPointF pos = at.value_or(QPointF(p->width * 0.5 - 12, p->height * 0.5 - 12));
        json item = {{"shape", shape}, {"x", pk::snap(pos.x(), mCanvas->grid)}, {"y", pk::snap(pos.y(), mCanvas->grid)}, {"w", 24}, {"h", 24}};
        if (std::string(shape) == "text")
        {
            item["label"] = "Label";
            item["w"] = 64;
            item["color"] = "#d0d0d0";
        }
        else
        {
            std::vector<std::string> targets;
            for (NodeId id : scene::Graph::current().selection())
                if (scene::Node* n = scene::Graph::current().node(id))
                    targets.push_back(scene::poses::stripNamespace(n->name()));
            item["targets"] = targets;
            if (!targets.empty())
            {
                item["color"] = pk::sideColor(targets.front());
                item["label"] = targets.size() == 1 ? targets.front() : std::string();
            }
        }
        addItems(json::array({item}));
    }

    void addItems(const json& items)
    {
        if (!ensureEditable())
            return;
        json r;
        if (runUi("editPicker", pageCall({{"addItem", items}}), &r))
            selectAdded(r);
    }

    void addFromSelection()
    {
        if (!doc)
        {
            newPicker(true);
            return;
        }
        if (!ensureEditable())
            return;
        json r;
        if (runUi("editPicker", pageCall({{"addFromSelection", true}}), &r))
            selectAdded(r);
    }

    void removeItems()
    {
        if (!mCanvas->editSelection.empty())
            edit({{"removeItems", editIds()}});
        mCanvas->editSelection.clear();
    }

    void mirrorItems()
    {
        if (mCanvas->editSelection.empty() || !ensureEditable())
            return;
        json r;
        if (runUi("editPicker", pageCall({{"mirrorItems", editIds()}}), &r))
            selectAdded(r);
    }

    void alignItems(const char* mode)
    {
        if (mCanvas->editSelection.size() > 1)
            edit({{"items", editIds()}, {"align", mode}});
    }

    void distributeItems(const char* axis)
    {
        if (mCanvas->editSelection.size() > 2)
            edit({{"items", editIds()}, {"distribute", axis}});
    }

    void setItems(const json& patches) { edit({{"setItems", patches}}); }

    void patchSelected(const json& fields)
    {
        json patches = json::array();
        for (const std::string& id : editIds())
        {
            json p = fields;
            p["id"] = id;
            patches.push_back(std::move(p));
        }
        if (!patches.empty())
            setItems(patches);
    }

    void pickColor()
    {
        if (mCanvas->editSelection.empty() || !doc)
            return;
        const pk::Item* first = doc->item(*mCanvas->editSelection.begin());
        QColor c = colorPopup::getColor(itemColor(first ? first->color : std::string()), this, "Item Colour");
        if (c.isValid())
            patchSelected({{"color", c.name().toStdString()}});
    }

    void editLabel(const pk::Item& item)
    {
        bool ok = false;
        QString label = QInputDialog::getText(this, "Item Label", "Label:", QLineEdit::Normal, qs(item.label), &ok);
        if (ok)
            setItems(json::array({{{"id", item.id}, {"label", label.toStdString()}}}));
    }

    void targetsFromSelection()
    {
        std::vector<std::string> targets;
        for (NodeId id : scene::Graph::current().selection())
            if (scene::Node* n = scene::Graph::current().node(id))
                targets.push_back(scene::poses::stripNamespace(n->name()));
        patchSelected({{"targets", targets}});
    }

    void setAction(const char* kind)
    {
        if (mCanvas->editSelection.empty())
            return;
        json action = {{"kind", kind}};
        bool ok = true;
        if (std::string(kind) == "toggleAttr")
            action["attr"] = QInputDialog::getText(this, "Toggle Attribute", "Attribute to toggle between 0 and 1:", QLineEdit::Normal, "visibility", &ok)
                                 .trimmed()
                                 .toStdString();
        else if (std::string(kind) == "applyPose")
            action["pose"] = QInputDialog::getText(this, "Apply Pose", "Pose name:", QLineEdit::Normal, "bind", &ok).trimmed().toStdString();
        else if (std::string(kind) == "runCommand")
        {
            action["command"] = QInputDialog::getText(this, "Run Command", "Command (its args default to the targets):", QLineEdit::Normal,
                                                      "select", &ok)
                                    .trimmed()
                                    .toStdString();
        }
        if (ok)
            patchSelected({{"action", action}});
    }

    void setShape(const char* shape)
    {
        const std::string s = shape;
        if (!doc || mCanvas->editSelection.empty())
            return;
        std::string image;
        if (s == "image")
        {
            QString file = ui::fileDialog::getOpenFileName(this, "Item Image", QString(), "Images (*.png *.jpg *.jpeg *.bmp *.svg)");
            if (file.isEmpty())
                return;
            image = file.toStdString();
        }
        json patches = json::array();
        for (const std::string& id : editIds())
        {
            const pk::Item* i = doc->item(id);
            if (!i)
                continue;
            json p = {{"id", id}, {"shape", shape}};
            if (s == "polygon" && i->points.size() < 3)
                p["points"] = {{i->w * 0.5, 0.0}, {i->w, i->h * 0.5}, {i->w * 0.5, i->h}, {0.0, i->h * 0.5}};
            if (!image.empty())
                p["image"] = image;
            patches.push_back(std::move(p));
        }
        setItems(patches);
    }

    /** Replaces the edit selection with the items an editPicker call added. */
    void selectAdded(const json& r)
    {
        if (!r.contains("added") || r["added"].empty())
            return;
        mCanvas->editSelection.clear();
        for (const json& id : r["added"])
            mCanvas->editSelection.insert(id.get<std::string>());
        if (!editing())
            setEditing(true);
        mCanvas->update();
    }

    void newPicker(bool fromSelection)
    {
        bool ok = false;
        QString name = QInputDialog::getText(this, "New Picker", "Picker name:", QLineEdit::Normal, "picker", &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        json c = {{"args", {name.toStdString()}}};
        scene::Graph& g = scene::Graph::current();
        if (!g.selection().empty())
        {
            scene::poses::Character ch = scene::poses::characterOf(g, g.selection().front());
            c["character"] = ch.id;
            c["namespace"] = ch.nameSpace;
        }
        if (!runUi("createPicker", c))
            return;
        mName = name.toStdString();
        reload();
        if (fromSelection)
            addFromSelection();
    }

    void fillMenu(QMenu* menu)
    {
        menu->clear();
        menu->addAction(icon("plus"), "New Picker...", this, [this] { newPicker(false); });
        menu->addAction(icon("hand-click"), "New Picker from Selection...", this, [this] { newPicker(true); });
        QMenu* fromTemplate = menu->addMenu("New from Template");
        for (const pk::Document& t : pk::templatePickers())
            fromTemplate->addAction(qs(t.name), this, [this, name = t.name] {
                bool ok = false;
                QString to = QInputDialog::getText(this, "New Picker", "Picker name:", QLineEdit::Normal, qs(name), &ok).trimmed();
                if (ok && !to.isEmpty() && runUi("createPicker", {{"args", {to.toStdString()}}, {"fromTemplate", name}, {"replace", to.toStdString() == name}}))
                {
                    mName = to.toStdString();
                    reload();
                }
            });
        fromTemplate->setEnabled(!fromTemplate->isEmpty());
        menu->addAction(icon("file-import"), "Import...", this, [this] {
            QString file = ui::fileDialog::getOpenFileName(this, "Import Picker", QString(), "Lila picker (*.lilapicker);;All files (*)");
            json r;
            if (!file.isEmpty() && runUi("importPicker", {{"args", {file.toStdString()}}, {"replace", true}}, &r))
            {
                mName = r.value("name", std::string());
                reload();
            }
        });
        QAction* exportAction = menu->addAction(icon("file-export"), "Export...", this, [this] {
            QString file = ui::fileDialog::getSaveFileName(this, "Export Picker", qs(doc->name + ".lilapicker"), "Lila picker (*.lilapicker)");
            if (!file.isEmpty())
                runUi("exportPicker", call({{"file", file.toStdString()}}));
        });
        exportAction->setEnabled(doc.has_value());
        menu->addSeparator();
        QAction* rename = menu->addAction("Rename Picker...", this, [this] {
            bool ok = false;
            QString to = QInputDialog::getText(this, "Rename Picker", "New name:", QLineEdit::Normal, qs(doc->name), &ok).trimmed();
            if (ok && !to.isEmpty() && to.toStdString() != doc->name && ensureEditable() && runUi("editPicker", call({{"rename", to.toStdString()}})))
            {
                mName = to.toStdString();
                reload();
            }
        });
        rename->setEnabled(doc.has_value());
        QAction* del = menu->addAction(icon("trash"), "Delete Picker", this, [this] {
            if (QMessageBox::question(this, "Delete Picker", QString("Delete the picker '%1'?").arg(qs(doc->name))) == QMessageBox::Yes)
                runUi("deletePicker", call());
        });
        del->setEnabled(doc.has_value() && !isTemplate());
        menu->addSeparator();
        QAction* addPage = menu->addAction("Add Page...", this, [this] {
            bool ok = false;
            QString name = QInputDialog::getText(this, "Add Page", "Page name:", QLineEdit::Normal, "Page", &ok).trimmed();
            if (ok && !name.isEmpty())
            {
                edit({{"addPage", name.toStdString()}});
                mPage = name.toStdString();
                reload();
            }
        });
        QAction* renamePage = menu->addAction("Rename Page...", this, [this] {
            bool ok = false;
            QString to = QInputDialog::getText(this, "Rename Page", "New name:", QLineEdit::Normal, qs(mPage), &ok).trimmed();
            if (ok && !to.isEmpty() && to.toStdString() != mPage)
            {
                edit({{"renamePage", {mPage, to.toStdString()}}});
                mPage = to.toStdString();
                reload();
            }
        });
        QAction* removePage = menu->addAction("Remove Page", this, [this] {
            if (QMessageBox::question(this, "Remove Page", QString("Remove the page '%1' and its items?").arg(qs(mPage))) == QMessageBox::Yes)
                edit({{"removePage", mPage}});
        });
        QAction* background = menu->addAction(icon("photo"), "Page Background...", this, [this] {
            QString file = ui::fileDialog::getOpenFileName(this, "Page Background", QString(), "Images (*.png *.jpg *.jpeg *.bmp *.svg)");
            if (file.isEmpty())
                return;
            QPixmap pm(file);
            json fields = {{"background", file.toStdString()}};
            if (!pm.isNull())
                fields["width"] = pm.width(), fields["height"] = pm.height();
            edit({{"setPage", fields}});
        });
        QAction* clearBackground = menu->addAction("Clear Background", this, [this] { edit({{"setPage", {{"background", ""}}}}); });
        QAction* pageSize = menu->addAction("Page Size...", this, [this] {
            const pk::Page* p = doc->page(mPage);
            bool ok = false;
            QString text = QInputDialog::getText(this, "Page Size", "Width x height:", QLineEdit::Normal,
                                                 QString("%1 x %2").arg(p->width).arg(p->height), &ok);
            QStringList parts = text.split('x', Qt::SkipEmptyParts);
            if (ok && parts.size() == 2)
                edit({{"setPage", {{"width", parts[0].trimmed().toDouble()}, {"height", parts[1].trimmed().toDouble()}}}});
        });
        for (QAction* a : {addPage, renamePage, removePage, background, clearBackground, pageSize})
            a->setEnabled(doc.has_value());
        clearBackground->setEnabled(doc && doc->page(mPage) && !doc->page(mPage)->background.empty());
        removePage->setEnabled(doc && doc->pages.size() > 1);
        menu->addSeparator();
        menu->addAction(icon("zoom-reset"), "Frame Page", this, [this] { frame(); });
    }

protected:
    void focusInEvent(QFocusEvent* e) override
    {
        sActive = this;
        QWidget::focusInEvent(e);
    }
    void enterEvent(QEnterEvent* e) override
    {
        sActive = this;
        QWidget::enterEvent(e);
    }
    void showEvent(QShowEvent* e) override
    {
        QWidget::showEvent(e);
        reload();
    }

private:
    QToolButton* toolButton(const char* iconName, const char* tip)
    {
        auto* b = new QToolButton(this);
        b->setIcon(icon(iconName));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    }

    void scheduleReload()
    {
        if (mReloadQueued)
            return;
        mReloadQueued = true;
        QTimer::singleShot(0, this, [this] {
            mReloadQueued = false;
            if (isVisible())
                reload();
        });
    }

    /** Switches the namespace to the selection's when it is one this picker drives, so clicking a rig follows it. */
    void followSelection()
    {
        scene::Graph& g = scene::Graph::current();
        if (!doc || g.selection().empty())
            return;
        scene::Node* n = g.node(g.selection().back());
        if (!n)
            return;
        std::string ns = pk::namespaceOf(n->name());
        if (ns == nameSpace() || mNamespace->findData(qs(ns)) < 0)
            return;
        mNs = ns;
        mNamespace->setCurrentIndex(mNamespace->findData(qs(ns)));
        refreshResolved();
    }

    void reload()
    {
        // Pickers by name: scene ones, then templates no scene picker shadows.
        std::vector<std::string> names;
        for (const pk::Document& d : pk::scenePickers())
            names.push_back(d.name);
        std::vector<std::string> templates;
        for (const pk::Document& d : pk::templatePickers())
            if (!pk::findScene(d.name))
                templates.push_back(d.name);
        {
            QSignalBlocker block(mPicker);
            mPicker->clear();
            for (const std::string& n : names)
                mPicker->addItem(icon("hand-click"), qs(n), qs(n));
            for (const std::string& n : templates)
                mPicker->addItem(icon("lock"), qs(n), qs(n));
            int i = mPicker->findData(qs(mName));
            if (i < 0 && mPicker->count() > 0)
                i = 0;
            mPicker->setCurrentIndex(i);
            mName = i < 0 ? std::string() : mPicker->itemData(i).toString().toStdString();
        }
        doc = mName.empty() ? std::nullopt : pk::find(mName);
        if (doc && doc->pages.empty())
            doc->pages.push_back({});

        {
            QSignalBlocker block(mTabs);
            mSyncingTabs = true;
            while (mTabs->count() > 0)
                mTabs->removeTab(0);
            int current = 0;
            if (doc)
                for (size_t i = 0; i < doc->pages.size(); ++i)
                {
                    mTabs->addTab(qs(doc->pages[i].name));
                    if (doc->pages[i].name == mPage)
                        current = int(i);
                }
            mTabs->setCurrentIndex(doc ? current : -1);
            mPage = doc ? doc->pages[size_t(current)].name : std::string();
            mTabs->setVisible(doc && doc->pages.size() > 1);
            mSyncingTabs = false;
        }

        {
            QSignalBlocker block(mNamespace);
            mNamespace->clear();
            if (doc)
            {
                std::vector<std::string> spaces = pk::namespacesFor(scene::Graph::current(), *doc);
                if (!doc->nameSpace.empty() && std::find(spaces.begin(), spaces.end(), doc->nameSpace) == spaces.end())
                    spaces.insert(spaces.begin(), doc->nameSpace);
                for (const std::string& s : spaces)
                    mNamespace->addItem(s.empty() ? QString("(no namespace)") : qs(s), qs(s));
                if (mNamespace->count() == 0)
                    mNamespace->addItem("(no characters)", qs(doc->nameSpace));
                int i = mNamespace->findData(qs(nameSpace()));
                if (i < 0)
                {
                    i = 0;
                    mNs = mNamespace->itemData(0).toString().toStdString();
                }
                mNamespace->setCurrentIndex(i);
            }
            mNamespace->setEnabled(doc && mNamespace->count() > 1);
        }

        if (doc)
            if (const pk::Page* p = doc->page(mPage))
                std::erase_if(mCanvas->editSelection, [&](const std::string& id) { return !p->item(id); });
        mEditButton->setEnabled(doc.has_value());
        refreshResolved();
    }

    void refreshResolved()
    {
        mResolved.clear();
        if (doc)
            if (const pk::Page* p = doc->page(mPage))
                for (const pk::Item& i : p->items)
                    mResolved[i.id] = pk::resolveTargets(scene::Graph::current(), i, nameSpace());
        mCanvas->update();
    }

    QComboBox* mPicker = nullptr;
    QComboBox* mNamespace = nullptr;
    QToolButton* mEditButton = nullptr;
    QWidget* mEditBar = nullptr;
    QTabBar* mTabs = nullptr;
    PickerCanvas* mCanvas = nullptr;
    std::string mName;
    std::string mPage;
    std::optional<std::string> mNs;
    std::map<std::string, std::vector<NodeId>> mResolved;
    int mPickerListener = 0;
    int mGraphListener = 0;
    bool mReloadQueued = false;
    bool mSyncingTabs = false;
};

QPointer<ControlPickerPanel> ControlPickerPanel::sActive;

const pk::Page* PickerCanvas::page() const
{
    return mPanel->doc ? mPanel->doc->page(mPanel->pageName()) : nullptr;
}

pk::Page* PickerCanvas::editablePage()
{
    return mPanel->doc ? mPanel->doc->page(mPanel->pageName()) : nullptr;
}

QPainterPath PickerCanvas::shapePath(const pk::Item& i) const
{
    QPainterPath path;
    QRectF r = viewRect(i);
    switch (i.shape)
    {
    case pk::Shape::Circle: path.addEllipse(r); break;
    case pk::Shape::Polygon:
        if (i.points.size() >= 3)
        {
            QPolygonF poly;
            for (const pk::Point& p : i.points)
                poly << toView({i.x + p.x, i.y + p.y});
            path.addPolygon(poly);
            path.closeSubpath();
            break;
        }
        [[fallthrough]];
    default: path.addRoundedRect(r, std::min(3.0, r.width() * 0.2), std::min(3.0, r.height() * 0.2)); break;
    }
    return path;
}

std::optional<QRectF> PickerCanvas::handle() const
{
    const pk::Page* p = page();
    if (!mPanel->editing() || editSelection.size() != 1 || !p)
        return std::nullopt;
    const pk::Item* i = p->item(*editSelection.begin());
    if (!i)
        return std::nullopt;
    QRectF r = viewRect(*i);
    return QRectF(r.bottomRight() - QPointF(4, 4), QSizeF(8, 8));
}

void PickerCanvas::paintEvent(QPaintEvent*)
{
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.fillRect(rect(), gray(34));
    const pk::Page* p = page();
    if (!p)
    {
        g.setPen(gray(140));
        g.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   "No picker yet.\nSelect a rig's controls and use the menu's New Picker from Selection, or import a .lilapicker file.");
        return;
    }
    QRectF pageRect(toView({0, 0}), QSizeF(p->width * mZoom, p->height * mZoom));
    g.fillRect(pageRect, gray(44));
    if (QPixmap bg = loadImage(p->background); !bg.isNull())
    {
        g.setOpacity(p->backgroundOpacity);
        g.drawPixmap(pageRect, bg, QRectF(bg.rect()));
        g.setOpacity(1.0);
    }
    if (mPanel->editing() && grid * mZoom >= 6)
    {
        g.setPen(QPen(gray(70), 1.5));
        for (double x = 0; x <= p->width; x += grid)
            for (double y = 0; y <= p->height; y += grid)
                g.drawPoint(toView({x, y}));
    }
    g.setPen(QPen(gray(70), 1));
    g.setBrush(Qt::NoBrush);
    g.drawRect(pageRect);

    const std::vector<NodeId>& sel = scene::Graph::current().selection();
    for (const pk::Item& i : p->items)
    {
        const std::vector<NodeId>& ids = mPanel->resolved(i.id);
        size_t hit = 0;
        for (NodeId id : ids)
            if (std::find(sel.begin(), sel.end(), id) != sel.end())
                ++hit;
        const bool full = !ids.empty() && hit == ids.size(), partial = hit > 0 && !full;
        const bool missing = !i.targets.empty() && ids.empty();
        QColor fill = itemColor(i.color);
        QRectF r = viewRect(i);
        g.setOpacity(missing ? 0.35 : 1.0);
        if (i.shape == pk::Shape::Image)
        {
            if (QPixmap pm = loadImage(i.image); !pm.isNull())
                g.drawPixmap(r, pm, QRectF(pm.rect()));
            else
                g.fillRect(r, gray(60));
        }
        if (i.shape != pk::Shape::Text && i.shape != pk::Shape::Image)
        {
            QPainterPath path = shapePath(i);
            g.fillPath(path, full ? fill.lighter(160) : fill);
            QPen outline(full ? gray(250) : fill.darker(170), full ? 2.0 : 1.0);
            if (partial)
                outline = QPen(gray(235), 1.5, Qt::DashLine);
            g.strokePath(path, outline);
        }
        else if (full || partial)
            g.strokePath(shapePath(i), QPen(gray(235), 1.5, full ? Qt::SolidLine : Qt::DashLine));
        if (!i.label.empty())
        {
            const bool text = i.shape == pk::Shape::Text;
            double px = std::clamp((text ? r.height() * 0.6 : std::min(r.height() * 0.45, 11.0 * mZoom)), 1.0, 64.0);
            if (px >= 6.0)
            {
                QFont f = font();
                f.setPixelSize(int(px));
                g.setFont(f);
                g.setPen(text ? (full ? fill.lighter(150) : fill) : !i.textColor.empty() ? itemColor(i.textColor) : contrast(fill));
                QRectF tr = text ? r : r.adjusted(-40, 0, 40, 0);
                g.drawText(tr, Qt::AlignCenter | Qt::TextDontClip, qs(i.label));
            }
        }
        g.setOpacity(1.0);
        if (editSelection.contains(i.id))
        {
            g.setPen(QPen(gray(220), 1, Qt::DashLine));
            g.setBrush(Qt::NoBrush);
            g.drawRect(r.adjusted(-3, -3, 3, 3));
        }
    }
    if (std::optional<QRectF> h = handle())
        g.fillRect(*h, gray(220));
    if (mDrag == Drag::Marquee)
    {
        QRectF m = QRectF(mPress, mLast).normalized();
        g.setPen(QPen(gray(210), 1, Qt::DashLine));
        g.setBrush(gray(210, 30));
        g.drawRect(m);
    }
}

void PickerCanvas::selectFromClick(const std::vector<std::string>& ids, Qt::KeyboardModifiers mods)
{
    // Maya's modifiers: Shift toggles, Ctrl removes, Ctrl+Shift adds.
    const bool shift = mods.testFlag(Qt::ShiftModifier), ctrl = mods.testFlag(Qt::ControlModifier);
    const char* mode = shift && ctrl ? "add" : shift ? "toggle" : ctrl ? "remove" : "replace";
    mPanel->select(ids, mode);
}

void PickerCanvas::mousePressEvent(QMouseEvent* e)
{
    mPress = mLast = e->position();
    mPressMods = e->modifiers();
    const pk::Page* p = page();
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && e->modifiers().testFlag(Qt::AltModifier)))
    {
        mDrag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() != Qt::LeftButton || !p)
        return;
    QPointF at = toPage(e->position());
    const pk::Item* hit = p->itemAt(at.x(), at.y());
    if (!mPanel->editing())
    {
        if (hit && hit->action.kind != pk::ActionKind::Select && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
            mPanel->activate(*hit);
        else if (hit)
            selectFromClick({hit->id}, e->modifiers());
        else
            mDrag = Drag::Marquee;
        return;
    }
    if (std::optional<QRectF> h = handle(); h && h->adjusted(-3, -3, 3, 3).contains(e->position()))
        mDrag = Drag::Resize;
    else if (hit)
    {
        if (e->modifiers().testFlag(Qt::ShiftModifier))
        {
            if (!editSelection.erase(hit->id))
                editSelection.insert(hit->id);
        }
        else if (!editSelection.contains(hit->id))
            editSelection = {hit->id};
        mDrag = editSelection.contains(hit->id) ? Drag::Move : Drag::None;
    }
    else
    {
        if (!e->modifiers().testFlag(Qt::ShiftModifier))
            editSelection.clear();
        mDrag = Drag::Marquee;
    }
    mStart.clear();
    for (const std::string& id : editSelection)
        if (const pk::Item* i = p->item(id))
            mStart[id] = QRectF(i->x, i->y, i->w, i->h);
    update();
}

void PickerCanvas::mouseMoveEvent(QMouseEvent* e)
{
    QPointF prev = mLast;
    mLast = e->position();
    switch (mDrag)
    {
    case Drag::Pan:
        mPan += mLast - prev;
        mFramed = true;
        update();
        return;
    case Drag::Marquee: update(); return;
    case Drag::Move:
    case Drag::Resize:
    {
        pk::Page* p = editablePage();
        if (!p || mStart.empty())
            return;
        QPointF d = (mLast - mPress) / mZoom;
        if (mDrag == Drag::Move)
        {
            // Snap the item under the press, then move the rest by the same amount so spacing is kept.
            QRectF anchor = mStart.begin()->second;
            QPointF moved(anchor.x() + d.x(), anchor.y() + d.y());
            if (snapping)
                moved = QPointF(pk::snap(moved.x(), grid), pk::snap(moved.y(), grid));
            QPointF delta = moved - anchor.topLeft();
            for (auto& [id, r] : mStart)
                if (pk::Item* i = p->item(id))
                    i->x = r.x() + delta.x(), i->y = r.y() + delta.y();
        }
        else
        {
            auto& [id, r] = *mStart.begin();
            if (pk::Item* i = p->item(id))
            {
                double w = std::max(4.0, r.width() + d.x()), h = std::max(4.0, r.height() + d.y());
                if (e->modifiers().testFlag(Qt::ShiftModifier))
                    w = h = std::max(w, h);
                if (snapping)
                    w = std::max(grid, pk::snap(w, grid)), h = std::max(grid, pk::snap(h, grid));
                for (pk::Point& pt : i->points)
                    pt.x *= w / i->w, pt.y *= h / i->h;
                i->w = w;
                i->h = h;
            }
        }
        update();
        return;
    }
    case Drag::None:
        if (const pk::Page* p = page())
        {
            QPointF at = toPage(mLast);
            const pk::Item* hit = p->itemAt(at.x(), at.y());
            std::optional<QRectF> h = handle();
            setCursor(h && h->adjusted(-3, -3, 3, 3).contains(mLast) ? Qt::SizeFDiagCursor
                      : hit && mPanel->editing()                     ? Qt::SizeAllCursor
                      : hit                                          ? Qt::PointingHandCursor
                                                                     : Qt::ArrowCursor);
            QString tip;
            if (hit)
            {
                QStringList targets;
                for (const std::string& t : hit->targets)
                    targets << qs(t);
                tip = (hit->label.empty() ? qs(hit->id) : qs(hit->label)) + (targets.isEmpty() ? QString() : "\n" + targets.join(", ")) +
                      (hit->action.kind == pk::ActionKind::Select ? QString() : QString("\nAction: %1").arg(pk::actionName(hit->action.kind)));
            }
            setToolTip(tip);
        }
        return;
    }
}

void PickerCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    Drag drag = mDrag;
    mDrag = Drag::None;
    unsetCursor();
    const pk::Page* p = page();
    if (drag == Drag::Marquee && p)
    {
        QPointF a = toPage(mPress), b = toPage(e->position());
        bool click = (e->position() - mPress).manhattanLength() < 4;
        std::vector<std::string> ids = click ? std::vector<std::string>() : p->itemsIn(a.x(), a.y(), b.x(), b.y());
        if (mPanel->editing())
        {
            for (const std::string& id : ids)
                editSelection.insert(id);
        }
        else if (!ids.empty() || !(mPressMods & (Qt::ShiftModifier | Qt::ControlModifier)))
            selectFromClick(ids, mPressMods);
        update();
    }
    else if (drag == Drag::Move || drag == Drag::Resize)
        commitGeometry();
}

void PickerCanvas::commitGeometry()
{
    const pk::Page* p = page();
    if (!p)
        return;
    json patches = json::array();
    for (auto& [id, r] : mStart)
        if (const pk::Item* i = p->item(id); i && (i->x != r.x() || i->y != r.y() || i->w != r.width() || i->h != r.height()))
        {
            json patch = {{"id", id}, {"x", i->x}, {"y", i->y}, {"w", i->w}, {"h", i->h}};
            if (!i->points.empty())
                patch["points"] = i->toJson()["points"];
            patches.push_back(std::move(patch));
        }
    mStart.clear();
    if (!patches.empty())
        mPanel->setItems(patches);
}

void PickerCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    const pk::Page* p = page();
    if (!p || e->button() != Qt::LeftButton)
        return;
    QPointF at = toPage(e->position());
    const pk::Item* hit = p->itemAt(at.x(), at.y());
    if (mPanel->editing() && hit)
        mPanel->editLabel(*hit);
    else if (!hit && !mPanel->editing())
        frame();
}

void PickerCanvas::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_F)
        return frame();
    if (!mPanel->editing() || editSelection.empty())
        return QWidget::keyPressEvent(e);
    double step = e->modifiers().testFlag(Qt::ShiftModifier) ? 1.0 : grid;
    QPointF d;
    switch (e->key())
    {
    case Qt::Key_Delete:
    case Qt::Key_Backspace: mPanel->removeItems(); return;
    case Qt::Key_Left: d = {-step, 0}; break;
    case Qt::Key_Right: d = {step, 0}; break;
    case Qt::Key_Up: d = {0, -step}; break;
    case Qt::Key_Down: d = {0, step}; break;
    default: return QWidget::keyPressEvent(e);
    }
    mPanel->edit({{"moveItems", mPanel->editIds()}, {"offset", {d.x(), d.y()}}});
}

void PickerCanvas::contextMenuEvent(QContextMenuEvent* e)
{
    const pk::Page* p = page();
    QMenu menu(this);
    if (!p)
    {
        menu.addAction("New Picker from Selection...", this, [this] { mPanel->addFromSelection(); });
        menu.exec(e->globalPos());
        return;
    }
    QPointF at = toPage(e->pos());
    const pk::Item* hit = p->itemAt(at.x(), at.y());
    if (!mPanel->editing())
    {
        if (hit && hit->action.kind != pk::ActionKind::Select)
        {
            pk::Item item = *hit;
            menu.addAction("Run Action", this, [this, item] { mPanel->activate(item); });
            menu.addAction("Select Targets", this, [this, item] { mPanel->select({item.id}, "replace"); });
            menu.addSeparator();
        }
        menu.addAction("Select All on Page", this, [this, p] {
            std::vector<std::string> ids;
            for (const pk::Item& i : p->items)
                if (i.action.kind == pk::ActionKind::Select)
                    ids.push_back(i.id);
            mPanel->select(ids, "replace");
        });
        menu.addAction(icon("zoom-reset"), "Frame Page", this, [this] { frame(); });
        menu.addAction(icon("pencil"), "Edit Mode", this, [this] { mPanel->setEditing(true); });
        menu.exec(e->globalPos());
        return;
    }
    if (hit && !editSelection.contains(hit->id))
    {
        editSelection = {hit->id};
        update();
    }
    QPointF pos = at;
    QMenu* add = menu.addMenu(icon("plus"), "Add");
    add->addAction(icon("square"), "Rectangle", this, [this, pos] { mPanel->addShape("rect", pos); });
    add->addAction(icon("circle"), "Circle", this, [this, pos] { mPanel->addShape("circle", pos); });
    add->addAction(icon("typography"), "Text", this, [this, pos] { mPanel->addShape("text", pos); });
    add->addAction(icon("hand-click"), "From Selection", this, [this] { mPanel->addFromSelection(); });
    const bool any = !editSelection.empty();
    QList<QAction*> needSelection;
    needSelection << menu.addAction("Label...", this, [this, p] {
        if (const pk::Item* i = p->item(*editSelection.begin()))
            mPanel->editLabel(*i);
    });
    needSelection << menu.addAction(icon("palette"), "Colour...", this, [this] { mPanel->pickColor(); });
    needSelection << menu.addAction("Targets from Selection", this, [this] { mPanel->targetsFromSelection(); });
    QMenu* shape = menu.addMenu("Shape");
    for (const char* s : {"rect", "circle", "polygon", "text", "image"})
        shape->addAction(QString(s).replace(0, 1, QString(s).at(0).toUpper()), this, [this, s] { mPanel->setShape(s); });
    QMenu* action = menu.addMenu("Action");
    action->addAction("Select", this, [this] { mPanel->setAction("select"); });
    action->addAction("Toggle Attribute...", this, [this] { mPanel->setAction("toggleAttr"); });
    action->addAction("Apply Pose...", this, [this] { mPanel->setAction("applyPose"); });
    action->addAction("Run Command...", this, [this] { mPanel->setAction("runCommand"); });
    shape->setEnabled(any);
    action->setEnabled(any);
    menu.addSeparator();
    needSelection << menu.addAction(icon("flip-vertical"), "Mirror", this, [this] { mPanel->mirrorItems(); });
    QMenu* align = menu.addMenu("Align");
    align->addAction(icon("layout-align-left"), "Left", this, [this] { mPanel->alignItems("left"); });
    align->addAction(icon("layout-align-center"), "Centre", this, [this] { mPanel->alignItems("hcenter"); });
    align->addAction(icon("layout-align-right"), "Right", this, [this] { mPanel->alignItems("right"); });
    align->addAction(icon("layout-align-top"), "Top", this, [this] { mPanel->alignItems("top"); });
    align->addAction(icon("layout-align-middle"), "Middle", this, [this] { mPanel->alignItems("vcenter"); });
    align->addAction(icon("layout-align-bottom"), "Bottom", this, [this] { mPanel->alignItems("bottom"); });
    align->addSeparator();
    align->addAction(icon("layout-distribute-horizontal"), "Distribute Horizontally", this, [this] { mPanel->distributeItems("horizontal"); });
    align->addAction(icon("layout-distribute-vertical"), "Distribute Vertically", this, [this] { mPanel->distributeItems("vertical"); });
    align->setEnabled(editSelection.size() > 1);
    menu.addSeparator();
    needSelection << menu.addAction(icon("trash"), "Delete", this, [this] { mPanel->removeItems(); });
    for (QAction* a : needSelection)
        a->setEnabled(any);
    menu.addSeparator();
    menu.addAction("Leave Edit Mode", this, [this] { mPanel->setEditing(false); });
    menu.exec(e->globalPos());
}

void withActive(const std::function<void(ControlPickerPanel&)>& fn)
{
    panels::show(kControlPickerDock);
    if (ControlPickerPanel* p = ControlPickerPanel::sActive)
        fn(*p);
}
} // namespace

QWidget* createControlPickerPanel(QWidget* parent)
{
    return new ControlPickerPanel(parent);
}

void registerControlPickerUi()
{
    panels::add({.id = kControlPickerDock, .title = "Control Picker", .create = [](QWidget* parent) { return createControlPickerPanel(parent); },
                 .area = Qt::RightDockWidgetArea, .icon = "hand-click", .open = false, .toggleAction = "window.controlPicker"});
    actions::add({.id = "picker.editMode", .label = "Picker Edit Mode", .category = "Animation",
                  .run = [] { withActive([](ControlPickerPanel& p) { p.toggleEditing(); }); },
                  .doc = "Toggle the Control Picker's edit mode: drag items, resize, add, mirror, align and recolour."});
    actions::add({.id = "picker.addFromSelection", .label = "Add Selection to Picker", .category = "Animation",
                  .run = [] { withActive([](ControlPickerPanel& p) { p.addFromSelection(); }); },
                  .doc = "Add a Control Picker item for each selected node to the current page."});
    actions::add({.id = "picker.mirrorItems", .label = "Mirror Picker Items", .category = "Animation",
                  .run = [] { withActive([](ControlPickerPanel& p) { p.mirrorItems(); }); },
                  .doc = "Copy the Control Picker items selected in edit mode to the other side, with sided targets swapped."});
    actions::add({.id = "picker.frame", .label = "Frame Picker Page", .category = "Animation",
                  .run = [] { withActive([](ControlPickerPanel& p) { p.frame(); }); },
                  .doc = "Fit the Control Picker's page in its panel."});
}
} // namespace lila::ui
