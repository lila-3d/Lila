#include "ui/GameExporter.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <set>

namespace lila::ui
{
namespace
{
constexpr const char* kPrefFile = "gameExporter";
constexpr const char* kLastPref = "/last";

json runUi(const char* command, const json& call)
{
    try
    {
        return cmds::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

void addChoices(QComboBox* box, std::initializer_list<std::pair<const char*, const char*>> items)
{
    for (auto [label, value] : items)
        box->addItem(label, QString(value));
}

void setChoice(QComboBox* box, const json& value)
{
    if (!value.is_string())
        return;
    int i = box->findData(QString::fromStdString(value.get<std::string>()));
    if (i >= 0)
        box->setCurrentIndex(i);
}

std::string choice(const QComboBox* box) { return box->currentData().toString().toStdString(); }
} // namespace

GameExporter::GameExporter(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Game Exporter");
    setMinimumWidth(420);
    auto* v = new QVBoxLayout(this);

    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel("Preset", this));
    mPreset = new QComboBox(this);
    mPreset->setToolTip("Load saved settings. Presets marked (scene) are kept in this scene file.");
    presetRow->addWidget(mPreset, 1);
    auto* save = new QToolButton(this);
    save->setIcon(icon("device-floppy"));
    save->setToolTip("Save these settings as a preset");
    presetRow->addWidget(save);
    auto* del = new QToolButton(this);
    del->setIcon(icon("trash"));
    del->setToolTip("Delete the chosen preset");
    presetRow->addWidget(del);
    v->addLayout(presetRow);

    auto* what = new QGroupBox("Export", this);
    auto* wv = new QVBoxLayout(what);
    auto* modes = new QHBoxLayout;
    mAll = new QRadioButton("All", what);
    mSelection = new QRadioButton("Selection", what);
    mSets = new QRadioButton("Object Sets", what);
    auto* group = new QButtonGroup(this);
    for (QRadioButton* r : {mAll, mSelection, mSets})
    {
        group->addButton(r);
        modes->addWidget(r);
    }
    modes->addStretch(1);
    wv->addLayout(modes);
    mSetList = new QListWidget(what);
    mSetList->setMaximumHeight(120);
    mSetList->setToolTip("Tick the sets to export. Their objects export whole, also for component members.");
    wv->addWidget(mSetList);
    auto* wf = new QFormLayout;
    wf->setLabelAlignment(Qt::AlignRight);
    mSeparate = new QComboBox(what);
    addChoices(mSeparate, {{"One file", "none"}, {"A file per set", "perSet"}, {"A file per object", "perMember"}});
    wf->addRow("Files", mSeparate);
    mMoveToOrigin = new QCheckBox(what);
    mMoveToOrigin->setToolTip("Drop each exported root's own transform so it sits at the origin.");
    wf->addRow("Move to origin", mMoveToOrigin);
    wv->addLayout(wf);
    v->addWidget(what);

    auto* opts = new QGroupBox("File", this);
    auto* of = new QFormLayout(opts);
    of->setLabelAlignment(Qt::AlignRight);
    auto* pathRow = new QHBoxLayout;
    mPath = new QLineEdit(opts);
    mPath->setPlaceholderText("the scene's folder");
    pathRow->addWidget(mPath, 1);
    auto* browse = new QToolButton(opts);
    browse->setIcon(icon("folder-open"));
    browse->setToolTip("Choose the folder");
    pathRow->addWidget(browse);
    of->addRow("Path", pathRow);
    mFilename = new QLineEdit(opts);
    mFilename->setToolTip("File name for a single-file export. Per-set and per-object files are named after them.");
    of->addRow("File name", mFilename);
    mFileType = new QComboBox(opts);
    addChoices(mFileType, {{"FBX", "fbx"}, {"OBJ", "obj"}});
    of->addRow("Type", mFileType);
    mProfile = new QComboBox(opts);
    addChoices(mProfile, {{"Unreal 5 (Z up)", "ue5"}, {"Unreal 6 (Y up)", "ue6"}, {"Scene axes", "generic"}});
    of->addRow("Up axis", mProfile);
    mUnits = new QComboBox(opts);
    addChoices(mUnits, {{"Centimetres", "cm"}, {"Metres", "m"}, {"Millimetres", "mm"}, {"Inches", "in"}, {"Feet", "ft"}});
    of->addRow("Units", mUnits);
    mTriangulate = new QCheckBox(opts);
    of->addRow("Triangulate", mTriangulate);
    mSmoothing = new QCheckBox(opts);
    mSmoothing->setToolTip("Write hard and soft edges as smoothing groups.");
    of->addRow("Smoothing groups", mSmoothing);
    mTangents = new QCheckBox(opts);
    of->addRow("Tangents", mTangents);
    mEmbedMedia = new QCheckBox(opts);
    mEmbedMedia->setEnabled(false);
    mEmbedMedia->setToolTip("The FBX writer doesn't write textures yet.");
    of->addRow("Embed media", mEmbedMedia);
    v->addWidget(opts);

    mPreview = new QLabel(this);
    mPreview->setWordWrap(true);
    mPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(mPreview);

    auto* bottom = new QHBoxLayout;
    bottom->addStretch(1);
    auto* go = new QPushButton(icon("file-export"), "Export", this);
    go->setDefault(true);
    bottom->addWidget(go);
    auto* close = new QPushButton("Close", this);
    bottom->addWidget(close);
    v->addLayout(bottom);

    json last = PrefStore::get().value(kPrefFile, kLastPref, json::object());
    json defaults = cmds::execute("gameExportPresets", json::object())["defaults"];
    if (last.is_object())
        for (auto& [k, val] : last.items())
            defaults[k] = val;
    refreshSets();
    apply(defaults);
    refreshPresets();

    auto changed = [this] {
        if (!mApplying)
            updatePreview();
    };
    for (QRadioButton* r : {mAll, mSelection, mSets})
        connect(r, &QRadioButton::toggled, this, changed);
    connect(mSets, &QRadioButton::toggled, mSetList, &QWidget::setEnabled);
    connect(mSetList, &QListWidget::itemChanged, this, changed);
    for (QComboBox* c : {mSeparate, mFileType, mProfile, mUnits})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    for (QCheckBox* c : {mMoveToOrigin, mTriangulate, mSmoothing, mTangents})
        connect(c, &QCheckBox::toggled, this, changed);
    for (QLineEdit* e : {mPath, mFilename})
        connect(e, &QLineEdit::textChanged, this, changed);
    connect(mFileType, &QComboBox::currentIndexChanged, this, [this] {
        const bool fbx = choice(mFileType) == "fbx";
        for (QWidget* w : std::initializer_list<QWidget*>{mProfile, mUnits, mTriangulate, mSmoothing, mTangents})
            w->setEnabled(fbx);
    });
    connect(browse, &QToolButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, "Export Folder", mPath->text());
        if (!dir.isEmpty())
            mPath->setText(dir);
    });
    connect(mPreset, &QComboBox::activated, this, [this](int i) {
        json p = json::parse(mPreset->itemData(i).toString().toStdString(), nullptr, false);
        if (p.is_object() && p.contains("settings"))
        {
            json s = cmds::execute("gameExportPresets", json::object())["defaults"];
            for (auto& [k, val] : p["settings"].items())
                s[k] = val;
            apply(s);
        }
    });
    connect(save, &QToolButton::clicked, this, &GameExporter::savePreset);
    connect(del, &QToolButton::clicked, this, &GameExporter::deletePreset);
    connect(go, &QPushButton::clicked, this, &GameExporter::exportNow);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Value || mSets->isChecked())
            scheduleRefresh();
    });
    mSetList->setEnabled(mSets->isChecked());
    updatePreview();
}

GameExporter::~GameExporter() { scene::Graph::current().removeListener(mListener); }

void GameExporter::open(QWidget* parent)
{
    static QPointer<GameExporter> dialog;
    if (!dialog)
    {
        dialog = new GameExporter(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void GameExporter::registerActions(QWidget* parent)
{
    if (actions::find("file.gameExporter"))
        return;
    QPointer<QWidget> owner(parent);
    actions::add({.id = "file.gameExporter", .label = "Game Exporter...", .category = "File",
                  .run = [owner] {
                      if (!closeShownWindow<GameExporter>())
                          GameExporter::open(owner);
                  },
                  .doc = "Exports everything, the selection or object sets to FBX or OBJ for a game engine: one file, a file "
                         "per set or a file per object, with presets.",
                  .python = "lila.ops.gameExport(mode=\"sets\", sets=[\"Props\"], separate=\"perSet\", path=\"Export\")"});
}

json GameExporter::settings() const
{
    json sets = json::array();
    for (int i = 0; i < mSetList->count(); ++i)
        if (mSetList->item(i)->checkState() == Qt::Checked)
            sets.push_back(mSetList->item(i)->text().toStdString());
    return {
        {"path", mPath->text().trimmed().toStdString()},
        {"filename", mFilename->text().trimmed().toStdString()},
        {"fileType", choice(mFileType)},
        {"mode", mSets->isChecked() ? "sets" : mSelection->isChecked() ? "selection" : "all"},
        {"sets", sets},
        {"separate", choice(mSeparate)},
        {"moveToOrigin", mMoveToOrigin->isChecked()},
        {"profile", choice(mProfile)},
        {"triangulate", mTriangulate->isChecked()},
        {"smoothingGroups", mSmoothing->isChecked()},
        {"tangents", mTangents->isChecked()},
        {"units", choice(mUnits)},
        {"embedMedia", false},
    };
}

void GameExporter::apply(const json& s)
{
    mApplying = true;
    mPath->setText(QString::fromStdString(s.value("path", std::string())));
    mFilename->setText(QString::fromStdString(s.value("filename", std::string("export"))));
    setChoice(mFileType, s.value("fileType", json("fbx")));
    const std::string mode = s.value("mode", std::string("all"));
    (mode == "sets" ? mSets : mode == "selection" ? mSelection : mAll)->setChecked(true);
    const json sets = s.value("sets", json::array());
    for (int i = 0; i < mSetList->count(); ++i)
    {
        const std::string name = mSetList->item(i)->text().toStdString();
        const bool on = sets.is_array() && std::ranges::find(sets, json(name)) != sets.end();
        mSetList->item(i)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
    }
    setChoice(mSeparate, s.value("separate", json("none")));
    mMoveToOrigin->setChecked(s.value("moveToOrigin", false));
    setChoice(mProfile, s.value("profile", json("ue5")));
    setChoice(mUnits, s.value("units", json("cm")));
    mTriangulate->setChecked(s.value("triangulate", true));
    mSmoothing->setChecked(s.value("smoothingGroups", true));
    mTangents->setChecked(s.value("tangents", true));
    mApplying = false;
    updatePreview();
}

void GameExporter::refreshSets()
{
    std::set<std::string> ticked;
    for (int i = 0; i < mSetList->count(); ++i)
        if (mSetList->item(i)->checkState() == Qt::Checked)
            ticked.insert(mSetList->item(i)->text().toStdString());
    const bool wasApplying = mApplying;
    mApplying = true;
    mSetList->clear();
    scene::Graph& g = scene::Graph::current();
    for (const scene::Node* s : scene::objectSets::all(g))
    {
        auto* item = new QListWidgetItem(icon("list-details"), QString::fromStdString(s->name()), mSetList);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(ticked.contains(s->name()) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(QString::number(scene::objectSets::nodesOf(g, s->id()).size()) + " object(s)");
    }
    mApplying = wasApplying;
}

void GameExporter::refreshPresets()
{
    mPreset->clear();
    mPreset->addItem("(choose a preset)", QString());
    json all = cmds::execute("gameExportPresets", json::object());
    for (const char* where : {"scene", "prefs"})
        for (auto& [name, s] : all[where].items())
        {
            json entry = {{"name", name}, {"scene", std::string(where) == "scene"}, {"settings", s}};
            mPreset->addItem(QString::fromStdString(name + (std::string(where) == "scene" ? "  (scene)" : "")),
                             QString::fromStdString(entry.dump()));
        }
}

void GameExporter::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        refreshSets();
        refreshPresets();
        updatePreview();
    });
}

void GameExporter::updatePreview()
{
    json call = settings();
    call["dryRun"] = true;
    try
    {
        json jobs = cmds::execute("gameExport", call);
        if (jobs.empty())
        {
            mPreview->setText("Nothing to export.");
            return;
        }
        QStringList files;
        for (const json& j : jobs)
            files << QString::fromStdString(std::filesystem::path(j["file"].get<std::string>()).filename().string()) +
                         QString(" (%1)").arg(j["nodes"].size());
        mPreview->setText(QString("Writes %1 file(s): ").arg(jobs.size()) + files.join(", "));
    }
    catch (const CommandError& e)
    {
        mPreview->setText(QString::fromStdString(e.what()));
    }
}

void GameExporter::exportNow()
{
    const json s = settings();
    PrefStore::get().set(kPrefFile, kLastPref, s);
    json written = runUi("gameExport", s);
    if (written.is_array())
        log::info("Game export wrote {} file(s){}", written.size(), written.empty() ? "" : ": " + written.dump());
}

void GameExporter::savePreset()
{
    QDialog ask(this);
    ask.setWindowTitle("Save Preset");
    auto* f = new QFormLayout(&ask);
    auto* name = new QLineEdit(&ask);
    json current = json::parse(mPreset->currentData().toString().toStdString(), nullptr, false);
    name->setText(current.is_object() ? QString::fromStdString(current.value("name", std::string())) : QString());
    f->addRow("Name", name);
    auto* inScene = new QCheckBox(&ask);
    inScene->setToolTip("Keep the preset in this scene file instead of your preferences.");
    inScene->setChecked(current.is_object() && current.value("scene", false));
    f->addRow("Save in scene", inScene);
    auto* ok = new QPushButton("Save", &ask);
    connect(ok, &QPushButton::clicked, &ask, &QDialog::accept);
    f->addRow(ok);
    if (ask.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    runUi("gameExportSavePreset", {{"args", {name->text().trimmed().toStdString()}}, {"settings", settings()}, {"scene", inScene->isChecked()}});
    refreshPresets();
    int i = mPreset->findText(name->text().trimmed() + (inScene->isChecked() ? "  (scene)" : ""));
    if (i >= 0)
        mPreset->setCurrentIndex(i);
}

void GameExporter::deletePreset()
{
    json current = json::parse(mPreset->currentData().toString().toStdString(), nullptr, false);
    if (!current.is_object())
        return;
    runUi("gameExportDeletePreset", {{"args", {current["name"]}}, {"scene", current.value("scene", false)}});
    refreshPresets();
}
} // namespace lila::ui
