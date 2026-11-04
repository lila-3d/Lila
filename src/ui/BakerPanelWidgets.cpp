#include "ui/BakerPanelInternal.h"
#include "ui/UiCommands.h"

namespace lila::ui
{
using namespace bakerPanel;

namespace
{
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

/**
 * Results thumbnails drag out with the middle button (the left one selects), as their files plus the bake outputs, so a
 * drop on a mesh knows each one's map.
 */
class ThumbList : public QListWidget
{
public:
    using QListWidget::QListWidget;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::MiddleButton)
            return QListWidget::mousePressEvent(e);
        mPressed = itemAt(e->position().toPoint());
        mPressAt = e->position().toPoint();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::MiddleButton) || !mPressed)
            return QListWidget::mouseMoveEvent(e);
        if ((e->position().toPoint() - mPressAt).manhattanLength() < QApplication::startDragDistance())
            return;
        QList<QListWidgetItem*> items = mPressed->isSelected() ? selectedItems() : QList<QListWidgetItem*>{mPressed};
        mPressed = nullptr;
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData(items));
        drag->setPixmap(items.front()->icon().pixmap(iconSize() / 2));
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
            mPressed = nullptr;
        QListWidget::mouseReleaseEvent(e);
    }
    QStringList mimeTypes() const override { return {"text/uri-list", kOutputsMime}; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        auto* m = new QMimeData;
        QList<QUrl> urls;
        json outputs = json::array();
        for (const QListWidgetItem* it : items)
        {
            urls << QUrl::fromLocalFile(it->data(kFileRole).toString());
            outputs.push_back(json::parse(it->data(kOutputRole).toString().toStdString(), nullptr, false));
        }
        m->setUrls(urls);
        m->setData(kOutputsMime, QByteArray::fromStdString(outputs.dump()));
        return m;
    }

private:
    QListWidgetItem* mPressed = nullptr;
    QPoint mPressAt;
};

/** An issue row's edges or faces, selected on its mesh (ids of its outMesh). */
void selectIssueComponents(QTreeWidgetItem* it)
{
    const QString stored = it->data(0, kComponentsRole).toString();
    const std::string node = ss(it->data(0, kNodeRole).toString());
    scene::Node* n = stored.isEmpty() || node.empty() ? nullptr : findNode(node);
    if (!n || !n->type().isA("mesh"))
        return;
    const json c = json::parse(stored.toStdString(), nullptr, false);
    json call = {{"args", {n->path()}}};
    if (c.contains("edgePoints"))
    {
        scene::MeshPtr m;
        try
        {
            m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        }
        catch (const std::exception&)
        {
        }
        if (!m)
            return;
        json edges = json::array();
        for (const json& pr : c["edgePoints"])
        {
            const geo::Index a = pr[0].get<geo::Index>(), b = pr[1].get<geo::Index>();
            if (a >= m->pointCapacity() || b >= m->pointCapacity() || !m->pointValid(a))
                continue;
            m->forEachPointEdge(a, [&](geo::Index e) {
                if (m->edgeOtherPoint(e, a) == b)
                    edges.push_back(e);
            });
        }
        call["edges"] = std::move(edges);
    }
    else if (c.contains("faces"))
        call["faces"] = c["faces"];
    else
        return;
    UndoChunk chunk("Select Issue");
    runUi("select", {{"args", {n->path()}}});
    runUi("selectComponents", call);
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
    setSpinDefault(s, 0.0);
    return s;
}
} // namespace

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
    mTargets->setToolTip("Target profile: the maps, conventions and file names a downstream tool expects. Picking one applies them to the project.");
    connect(mTargets, &QComboBox::currentIndexChanged, this, [this](int) {
        if (mUpdating)
            return;
        mTarget = ss(mTargets->currentData().toString());
        PrefStore::get().set(kPrefs, "/target", mTarget);
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
    QToolButton* add = toolButton(w, "plus", "New group: the selected meshes and the meshes under them become its lows. Shift+click uses only the selected meshes.");
    connect(add, &QToolButton::clicked, this, [this] {
        json call = json::object();
        if (const std::vector<std::string> m = selectedMeshes(); !m.empty())
            call["lows"] = m;
        run("bakeGroup", call);
    });
    bar->addWidget(add);
    // No current group: bakeGroup without a name creates one.
    auto addSelected = [this](const char* flag) {
        const std::vector<std::string> meshes = selectedMeshes();
        if (meshes.empty())
            return;
        json call = {{flag, meshes}};
        if (const std::string g = currentGroup(); !g.empty())
            call["args"] = {g};
        run("bakeGroup", call);
    };
    QToolButton* low = toolButton(w, "arrow-down",
                                  "Add the selected meshes and the meshes under them to the current group (a new one if none) as "
                                  "lows. Shift+click adds only the selected meshes.",
                                  "Low");
    connect(low, &QToolButton::clicked, this, [addSelected] { addSelected("addLow"); });
    bar->addWidget(low);
    QToolButton* high = toolButton(w, "arrow-up",
                                   "Add the selected meshes and the meshes under them to the current group (a new one if none) as "
                                   "highs. Shift+click adds only the selected meshes.",
                                   "High");
    connect(high, &QToolButton::clicked, this, [addSelected] { addSelected("addHigh"); });
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
    bar->addWidget(docs::helpButton("baking", w));
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
    // Rebuilds run with updates off; only the user's folding is remembered.
    auto toggleFold = [this](QTreeWidgetItem* it) {
        const QString key = it->data(0, kFoldKeyRole).toString();
        if (key.isEmpty() || !mGroups->updatesEnabled())
            return;
        if (!mLintToggled.removeOne(key))
            mLintToggled << key;
    };
    connect(mGroups, &QTreeWidget::itemExpanded, this, toggleFold);
    connect(mGroups, &QTreeWidget::itemCollapsed, this, toggleFold);
    Hotkeys::passThrough(mGroups);
    mGroups->setProperty("hotkeyContext", "baker");
    deletion::claim(mGroups, [this] { removeSelectedRows(); });
    tree->dropped = [this](const QStringList& names, QTreeWidgetItem* at) { dropNodes(names, at); };
    tree->removeRows = [this] { removeSelectedRows(); };
    connect(mGroups, &QTreeWidget::currentItemChanged, this, [this] {
        if (!mUpdating)
            refreshDetails();
        updateButtons();
    });
    connect(mGroups, &QTreeWidget::itemClicked, this, [](QTreeWidgetItem* it, int) { selectIssueComponents(it); });
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
    setSpinDefault(mCageOffset, 1.0);
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
    QToolButton* resetPaint = toolButton(mCageRow, "eraser", "Reset the painted cage offsets and skew on the group's lows", "Reset");
    resetPaint->setObjectName("bakerResetCagePaint");
    connect(resetPaint, &QToolButton::clicked, this, [this] {
        scene::Node* n = findNode(currentGroup());
        if (!n || !bake::isGroup(*n) || bake::groupSettings(*n).lows.empty())
            return;
        const std::vector<std::string> lows = bake::groupSettings(*n).lows;
        UndoChunk chunk("Reset Bake Cage Paint");
        for (const char* attr : {"offset", "skew"})
            runUi("bakePaint", {{"args", lows}, {"attr", attr}, {"reset", true}});
        mCageStatus->clear();
    });
    mLivePreview = toolButton(mCageRow, "live-view", "Live preview: rebake a low resolution preview on the lows as the cage changes; nothing is written", "Live");
    mLivePreview->setObjectName("bakerLivePreview");
    mLivePreview->setCheckable(true);
    connect(mLivePreview, &QToolButton::clicked, this, [](bool on) { bakeCage::setLivePreview(on); });
    bakeCage::onChanged(this, [this] { scheduleRefresh(); });
    cageRow->addWidget(mShowCage);
    cageRow->addWidget(paint);
    cageRow->addWidget(estimate);
    cageRow->addWidget(resetPaint);
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
    setComboDefault(mAoScope, mAoScope->findData(qs(bake::GroupSettings{}.aoScope)));
    mAoScope->setToolTip("Which highs occlude AO rays: this group's (so overlapping groups never shadow each other), only the hit high, or every group's");
    connect(mAoScope, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"aoScope", ss(mAoScope->currentData().toString())}}, false);
    });
    form->addRow("AO Occluders", mAoScope);

    auto* unlinkBox = new QWidget(mDetails);
    auto* uv = new QVBoxLayout(unlinkBox);
    uv->setContentsMargins(0, 0, 0, 0);
    uv->setSpacing(1);
    mUnlinks = new QTreeWidget(unlinkBox);
    mUnlinks->setObjectName("bakerUnlinks");
    mUnlinks->setMaximumHeight(170);
    mUnlinks->setUniformRowHeights(true);
    mUnlinks->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mUnlinks->setHeaderLabels({"Low", "Skip Projection"});
    mUnlinks->headerItem()->setToolTip(0, "Meshes under a low don't occlude it");
    mUnlinks->headerItem()->setToolTip(1, "They don't project onto it either");
    mUnlinks->header()->setStretchLastSection(false);
    mUnlinks->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mUnlinks->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    connect(mUnlinks, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        scene::Node* n = findNode(currentGroup());
        const int row = mUnlinks->indexOfTopLevelItem(item);
        if (mUpdating || column != 1 || row < 0 || !n || !bake::isGroup(*n))
            return;
        std::vector<bake::Unlink> all = bake::groupSettings(*n).unlinks;
        if (row >= int(all.size()))
            return;
        all[size_t(row)].projection = item->checkState(1) == Qt::Checked;
        setUnlinks(currentGroup(), all);
    });
    connect(mUnlinks, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!mUpdating && !mUnlinksCollapsed.contains(item->text(0)))
            mUnlinksCollapsed << item->text(0);
    });
    connect(mUnlinks, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!mUpdating)
            mUnlinksCollapsed.removeAll(item->text(0));
    });
    uv->addWidget(mUnlinks);
    auto* ub = new QHBoxLayout;
    ub->setSpacing(1);
    QToolButton* unlinkAdd = toolButton(unlinkBox, "link-off",
                                        "The selected meshes stop occluding the current Low row, or every low when a group row is "
                                        "current. Shift+click: they also stop projecting onto it.",
                                        "Unlink Selected");
    connect(unlinkAdd, &QToolButton::clicked, this, [this] {
        QTreeWidgetItem* it = mGroups->currentItem();
        const std::string low = it && it->data(0, kKindRole).toInt() == kLowItem ? ss(it->data(0, kNodeRole).toString()) : std::string();
        unlinkSelected(currentGroup(), low, QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    ub->addWidget(unlinkAdd);
    QToolButton* unlinkRemove = toolButton(unlinkBox, "trash", "Remove the selected meshes, or every mesh of a selected low row");
    connect(unlinkRemove, &QToolButton::clicked, this, [this] { removeSelectedUnlinks(); });
    ub->addWidget(unlinkRemove);
    ub->addStretch(1);
    uv->addLayout(ub);
    mUnlinks->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mUnlinks, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QTreeWidgetItem* item = mUnlinks->itemAt(at);
        if (!item)
            return;
        if (!item->isSelected())
        {
            mUnlinks->clearSelection();
            item->setSelected(true);
        }
        QMenu menu(this);
        menu.addAction(icon("trash"), "Remove", this, [this] { removeSelectedUnlinks(); });
        menu.exec(mUnlinks->viewport()->mapToGlobal(at));
    });
    form->addRow("Unlinked", unlinkBox);
    mBackfaces = new QCheckBox("Ignore backfaces", mDetails);
    setCheckDefault(mBackfaces, bake::GroupSettings{}.ignoreBackfaces);
    mBackfaces->setToolTip("Rays only hit high faces facing them");
    connect(mBackfaces, &QCheckBox::toggled, this, [this](bool on) {
        if (!mUpdating && !currentGroup().empty())
            run("bakeGroup", {{"args", {currentGroup()}}, {"ignoreBackfaces", on}}, false);
    });
    form->addRow(QString(), mBackfaces);
    mSelf = new QCheckBox("Self as high", mDetails);
    setCheckDefault(mSelf, false);
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
    auto removeModifier = [this] {
        json s = currentStack();
        const int r = mStack->currentRow();
        if (r < 0 || r >= int(s.size()))
            return;
        s.erase(s.begin() + r);
        setStack(s);
    };
    deletion::claim(mStack, removeModifier);
    QToolButton* del = toolButton(mStackButtons, "trash", "Remove the modifier");
    connect(del, &QToolButton::clicked, this, removeModifier);
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
    connect(remove, &QToolButton::clicked, this, [this] { removeSelectedMaps(); });
    bar->addWidget(remove);
    v->addLayout(bar);

    mMaps = new QTreeWidget(w);
    mMaps->setObjectName("bakerMaps");
    mMaps->setColumnCount(4);
    mMaps->setHeaderLabels({"Map", "Suffix", "Bits", "Format"});
    mMaps->setRootIsDecorated(false);
    mMaps->setUniformRowHeights(true);
    mMaps->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    mMaps->setSelectionMode(QAbstractItemView::ExtendedSelection);
    Hotkeys::passThrough(mMaps);
    deletion::claim(mMaps, [this] { removeSelectedMaps(); });
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
        if (mUpdating)
            return;
        json call = {{flag, std::move(value)}};
        if (const std::string ts = mTextureSet ? ss(mTextureSet->currentData().toString()) : std::string(); !ts.empty())
            call["material"] = ts;
        run("setBakeOutput", call);
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
        const QString dir = ui::fileDialog::getExistingDirectory(this, "Bake Output Folder", start);
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
    // Word wrap in a form row hits Qt's height-for-width bug and stretches the row over the whole tab.
    mFolderResolved->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
    mPreview = new QTreeWidget(w);
    mPreview->setObjectName("bakerPreview");
    mPreview->setHeaderLabels({"File", "Size", "Format"});
    mPreview->setUniformRowHeights(true);
    mPreview->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mPreview->setMaximumHeight(180);
    mPreview->header()->setStretchLastSection(false);
    mPreview->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mPreview->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    mPreview->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    auto openSet = [this](QTreeWidgetItem* it, bool open) {
        if (!mPreview->updatesEnabled() || it->parent())
            return;
        const QString set = it->data(0, Qt::UserRole).toString();
        mPreviewOpen.removeAll(set);
        if (open)
            mPreviewOpen << set;
    };
    connect(mPreview, &QTreeWidget::itemExpanded, this, [openSet](QTreeWidgetItem* it) { openSet(it, true); });
    connect(mPreview, &QTreeWidget::itemCollapsed, this, [openSet](QTreeWidgetItem* it) { openSet(it, false); });
    form->addRow("Files", mPreview);

    auto* setRow = new QHBoxLayout;
    mTextureSet = new QComboBox(w);
    mTextureSet->setObjectName("bakerTextureSet");
    mTextureSet->setToolTip("Which texture set the settings below are for: every set, or one material ({material}) with its own "
                            "format, bits, resolution, supersampling and padding. Needs {material} in the name.");
    connect(mTextureSet, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!mUpdating)
            refreshOutput();
    });
    setRow->addWidget(mTextureSet, 1);
    mTextureSetReset = toolButton(w, "arrow-back-up", "Use the project's settings for this texture set again");
    connect(mTextureSetReset, &QToolButton::clicked, this, [this] {
        if (const std::string ts = ss(mTextureSet->currentData().toString()); !ts.empty())
            run("setBakeOutput", {{"material", ts}, {"clear", true}});
    });
    setRow->addWidget(mTextureSetReset);
    form->addRow("Texture Set", setRow);

    mFormat = new QComboBox(w);
    mFormat->addItem("PNG", "png");
    mFormat->addItem("OpenEXR (half)", "exr");
    mFormat->addItem("TGA", "tga");
    mFormat->addItem("TIFF", "tif");
    mFormat->addItem("PSD (layered)", "psd");
    const bake::OutputSettings defaults;
    setComboDefault(mFormat, mFormat->findData(qs(defaults.format)));
    connect(mFormat, &QComboBox::currentIndexChanged, this, [this, set](int) {
        set("format", ss(mFormat->currentData().toString()));
        updatePreview();
    });
    form->addRow("Format", mFormat);
    mBits = new QComboBox(w);
    mBits->addItem("8 bit", 8);
    mBits->addItem("16 bit", 16);
    mBits->addItem("32 bit float", 32);
    setComboDefault(mBits, mBits->findData(defaults.bits));
    mBits->setToolTip("PNG and PSD take 8 or 16, TIFF 8, 16 or 32 float, EXR 16 (half) or 32 float; TGA is always 8");
    connect(mBits, &QComboBox::currentIndexChanged, this, [this, set](int) { set("bits", mBits->currentData().toInt()); });
    form->addRow("Bits", mBits);
    mResolution = new QComboBox(w);
    mResolution->setObjectName("bakerResolution");
    for (int r : {256, 512, 1024, 2048, 4096, 8192})
        mResolution->addItem(QString("%1 x %1").arg(r), r);
    setComboDefault(mResolution, mResolution->findData(defaults.width));
    connect(mResolution, &QComboBox::currentIndexChanged, this, [this, set](int) {
        if (const int r = mResolution->currentData().toInt(); r > 0)
            set("resolution", r);
    });
    form->addRow("Resolution", mResolution);
    mSupersampling = new QComboBox(w);
    for (int s : {1, 4, 16, 64})
        mSupersampling->addItem(s == 1 ? QString("1 (off)") : QString("%1 (%2 x %2)").arg(s).arg(int(std::sqrt(s))), s);
    mSupersampling->setToolTip("Samples per texel");
    setComboDefault(mSupersampling, mSupersampling->findData(defaults.supersampling));
    connect(mSupersampling, &QComboBox::currentIndexChanged, this, [this, set](int) { set("supersampling", mSupersampling->currentData().toInt()); });
    form->addRow("Supersampling", mSupersampling);
    mPadding = new QSpinBox(w);
    mPadding->setRange(-1, 1024);
    mPadding->setSuffix(" px");
    mPadding->setSpecialValueText("Fill");
    mPadding->setKeyboardTracking(false);
    mPadding->setToolTip("Texels of dilation past island borders; Fill floods the whole texture");
    setSpinDefault(mPadding, defaults.padding);
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
    setCheckDefault(mAutoAssign, bake::ProjectSettings{}.autoAssign);
    form->addRow(QString(), mAutoAssign);
    mRebakeOnSave = new QCheckBox("Rebake on save", w);
    mRebakeOnSave->setObjectName("bakerRebakeOnSave");
    mRebakeOnSave->setToolTip("Saving the scene rebakes this project when its meshes or settings changed since the last bake");
    connect(mRebakeOnSave, &QCheckBox::toggled, this, pipeline("rebakeOnSave"));
    setCheckDefault(mRebakeOnSave, false);
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
    mSummary = new QTreeWidget(mSummaryBox);
    mSummary->setObjectName("bakerSummary");
    mSummary->setHeaderHidden(true);
    mSummary->setColumnCount(2);
    mSummary->setUniformRowHeights(true);
    mSummary->header()->setStretchLastSection(true);
    mSummary->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    connect(mSummary, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        if (const QString file = it->data(0, kFileRole).toString(); !file.isEmpty())
            openFolder(file);
    });
    row->addWidget(mSummary, 1);
    auto* buttons = new QVBoxLayout;
    buttons->setSpacing(1);
    auto* assign = toolButton(mSummaryBox, "link", "Assign the maps to the lows' materials", "Assign");
    assign->setObjectName("bakerAssign");
    connect(assign, &QToolButton::clicked, this, [this] { assignResult(); });
    buttons->addWidget(assign);
    row->addLayout(buttons);
    auto* openOutput = toolButton(mSummaryBox, "folder", "Open the output folder");
    openOutput->setObjectName("bakerOpenFolder");
    connect(openOutput, &QToolButton::clicked, this, [this] { openFolder(outputFolder()); });
    buttons->addWidget(openOutput);
    auto* clear = toolButton(mSummaryBox, "x", "Clear the bake messages");
    clear->setObjectName("bakerClearSummary");
    connect(clear, &QToolButton::clicked, this, [this] {
        mSummary->clear();
        mSummaryBox->hide();
    });
    buttons->addWidget(clear);
    buttons->addStretch(1);
    mThumbs = new ThumbList(split);
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
    // Baked files are left on disk: removing them can't be undone.
    deletion::claim(mThumbs, nullptr);
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
} // namespace lila::ui
