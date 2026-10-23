#include "ui/MeshSequenceExporter.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <filesystem>

namespace lila::ui
{
namespace
{
constexpr const char* kPrefFile = "meshSequence";
constexpr const char* kLastPref = "/last";

json runUi(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
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

QDoubleSpinBox* frameBox(QWidget* parent, int decimals)
{
    auto* b = new QDoubleSpinBox(parent);
    b->setRange(-1e6, 1e6);
    b->setDecimals(decimals);
    return b;
}
} // namespace

MeshSequenceExporter::MeshSequenceExporter(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Export Mesh Sequence");
    setMinimumWidth(440);
    auto* v = new QVBoxLayout(this);

    mObjects = new QLabel(this);
    mObjects->setWordWrap(true);
    v->addWidget(mObjects);

    auto* range = new QGroupBox("Frames", this);
    auto* rv = new QVBoxLayout(range);
    auto* modes = new QHBoxLayout;
    mPlayback = new QRadioButton("Playback range", range);
    mCustom = new QRadioButton("Start / end", range);
    auto* group = new QButtonGroup(this);
    group->addButton(mPlayback);
    group->addButton(mCustom);
    modes->addWidget(mPlayback);
    modes->addWidget(mCustom);
    modes->addStretch(1);
    rv->addLayout(modes);
    auto* nums = new QHBoxLayout;
    mStart = frameBox(range, 0);
    mEnd = frameBox(range, 0);
    mStep = frameBox(range, 2);
    mStep->setRange(0.01, 1000.0);
    for (auto [label, box] : {std::pair{"Start", mStart}, {"End", mEnd}, {"Step", mStep}})
    {
        nums->addWidget(new QLabel(label, range));
        nums->addWidget(box, 1);
    }
    rv->addLayout(nums);
    v->addWidget(range);

    auto* files = new QGroupBox("Files", this);
    auto* ff = new QFormLayout(files);
    ff->setLabelAlignment(Qt::AlignRight);
    mFormat = new QComboBox(files);
    addChoices(mFormat, {{"FBX, a file per frame", "fbx"}, {"OBJ, a file per frame", "obj"}, {"Alembic, one file", "abc"}});
    ff->addRow("Format", mFormat);
    auto* folderRow = new QHBoxLayout;
    mFolder = new QLineEdit(files);
    mFolder->setToolTip("Output folder; a relative one is under the workspace.");
    folderRow->addWidget(mFolder, 1);
    auto* browse = new QToolButton(files);
    browse->setIcon(icon("folder-open"));
    browse->setToolTip("Choose the folder");
    folderRow->addWidget(browse);
    ff->addRow("Folder", folderRow);
    mName = new QLineEdit(files);
    mName->setToolTip("File name before the number. {name} is the first mesh's name.");
    ff->addRow("Name", mName);
    auto* numRow = new QHBoxLayout;
    mNumbering = new QComboBox(files);
    addChoices(mNumbering, {{"Index from 0", "index"}, {"Frame number", "frame"}});
    numRow->addWidget(mNumbering, 1);
    numRow->addWidget(new QLabel("Padding", files));
    mPadding = new QSpinBox(files);
    mPadding->setRange(0, 12);
    numRow->addWidget(mPadding);
    ff->addRow("Numbering", numRow);
    v->addWidget(files);

    auto* geom = new QGroupBox("Geometry", this);
    auto* gf = new QFormLayout(geom);
    gf->setLabelAlignment(Qt::AlignRight);
    mScale = new QDoubleSpinBox(geom);
    mScale->setRange(0.0001, 10000.0);
    mScale->setDecimals(4);
    mScale->setSingleStep(0.1);
    mScale->setToolTip("Multiplies every length: 0.1 writes a tenth of the size.");
    gf->addRow("Global scale", mScale);
    mProfile = new QComboBox(geom);
    addChoices(mProfile, {{"Unreal 5 (Z up)", "ue5"}, {"Unreal 6 (Y up)", "ue6"}, {"Scene axes", "generic"}});
    gf->addRow("Axes", mProfile);
    mSpace = new QComboBox(geom);
    addChoices(mSpace, {{"World", "world"}, {"Object", "object"}});
    gf->addRow("Space", mSpace);
    mRecenter = new QComboBox(geom);
    addChoices(mRecenter, {{"Off", "none"}, {"First frame's centre", "first"}, {"Every frame", "each"}});
    mRecenter->setToolTip("Move the bounding box centre to the origin: by the first frame's offset for all frames, or each frame on its own.");
    gf->addRow("Recentre", mRecenter);
    mModifiers = new QCheckBox("Apply modifiers", geom);
    mModifiers->setToolTip("Export the evaluated mesh. Off exports the base mesh without its modifier stack.");
    mTriangulate = new QCheckBox("Triangulate", geom);
    auto* checks = new QHBoxLayout;
    checks->addWidget(mModifiers);
    checks->addWidget(mTriangulate);
    checks->addStretch(1);
    gf->addRow("", checks);
    mNormals = new QCheckBox("Normals", geom);
    mUvs = new QCheckBox("UVs", geom);
    mColors = new QCheckBox("Vertex colors", geom);
    auto* attrs = new QHBoxLayout;
    attrs->addWidget(mNormals);
    attrs->addWidget(mUvs);
    attrs->addWidget(mColors);
    attrs->addStretch(1);
    gf->addRow("Include", attrs);
    v->addWidget(geom);

    auto* unreal = new QGroupBox("Unreal", this);
    auto* uf = new QFormLayout(unreal);
    uf->setLabelAlignment(Qt::AlignRight);
    mContentPath = new QLineEdit(unreal);
    mContentPath->setToolTip("FBX and OBJ send a static mesh per frame named <path>_000, ...; Alembic sends one Geometry Cache.");
    uf->addRow("Content path", mContentPath);
    mNiagara = new QLineEdit(unreal);
    mNiagara->setPlaceholderText("optional, like /Game/FX/NS_Spike");
    mNiagara->setToolTip("A Niagara system whose mesh renderer gets the frames, in order, as its meshes (for a mesh index flipbook).");
    uf->addRow("Niagara system", mNiagara);
    auto* send = new QPushButton(icon("send"), "Send to Unreal", unreal);
    send->setToolTip("Map the meshes with these settings and send them; saving the scene then sends them again.");
    uf->addRow("", send);
    v->addWidget(unreal);

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
    apply(last.is_object() ? last : json::object());

    auto changed = [this] {
        if (!mApplying)
            updatePreview();
    };
    for (QRadioButton* r : {mPlayback, mCustom})
        connect(r, &QRadioButton::toggled, this, changed);
    for (QDoubleSpinBox* b : {mStart, mEnd, mStep, mScale})
        connect(b, &QDoubleSpinBox::valueChanged, this, changed);
    connect(mPadding, &QSpinBox::valueChanged, this, changed);
    for (QComboBox* c : {mFormat, mNumbering, mProfile, mSpace, mRecenter})
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    for (QCheckBox* c : {mModifiers, mNormals, mUvs, mColors, mTriangulate})
        connect(c, &QCheckBox::toggled, this, changed);
    for (QLineEdit* e : {mFolder, mName})
        connect(e, &QLineEdit::textChanged, this, changed);
    connect(mFormat, &QComboBox::currentIndexChanged, this, [this] {
        const bool perFrame = choice(mFormat) != "abc";
        mNumbering->setEnabled(perFrame);
        mPadding->setEnabled(perFrame);
        mNiagara->setEnabled(perFrame);
    });
    auto ranged = [this] {
        for (QDoubleSpinBox* b : {mStart, mEnd})
            b->setEnabled(mCustom->isChecked());
    };
    connect(mCustom, &QRadioButton::toggled, this, ranged);
    connect(browse, &QToolButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, "Export Folder", mFolder->text());
        if (!dir.isEmpty())
            mFolder->setText(dir);
    });
    connect(go, &QPushButton::clicked, this, &MeshSequenceExporter::exportNow);
    connect(send, &QPushButton::clicked, this, &MeshSequenceExporter::sendToUnreal);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Value)
            scheduleRefresh();
    });
    ranged();
    emit mFormat->currentIndexChanged(mFormat->currentIndex());
    updatePreview();
}

MeshSequenceExporter::~MeshSequenceExporter() { scene::Graph::current().removeListener(mListener); }

void MeshSequenceExporter::open(QWidget* parent)
{
    static QPointer<MeshSequenceExporter> dialog;
    if (!dialog)
    {
        dialog = new MeshSequenceExporter(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MeshSequenceExporter::registerActions(QWidget* parent)
{
    if (actions::find("file.exportMeshSequence"))
        return;
    QPointer<QWidget> owner(parent);
    actions::add({.id = "file.exportMeshSequence", .label = "Export Mesh Sequence...", .category = "File",
                  .run = [owner] {
                      if (!closeShownWindow<MeshSequenceExporter>())
                          MeshSequenceExporter::open(owner);
                  },
                  .doc = "Exports the selected animated meshes frame by frame: an FBX or OBJ per frame (for Niagara mesh "
                         "flipbooks) or one Alembic file (for a Geometry Cache), with scale, axes and recentring.",
                  .python = "lila.ops.exportMeshSequence(\"Spike\", start=32, end=58, scale=0.1, folder=\"Export/Spike\")"});
}

json MeshSequenceExporter::settings() const
{
    return {
        {"playbackRange", mPlayback->isChecked()},
        {"start", mStart->value()},
        {"end", mEnd->value()},
        {"step", mStep->value()},
        {"format", choice(mFormat)},
        {"folder", mFolder->text().trimmed().toStdString()},
        {"name", mName->text().trimmed().toStdString()},
        {"numbering", choice(mNumbering)},
        {"padding", mPadding->value()},
        {"scale", mScale->value()},
        {"profile", choice(mProfile)},
        {"worldSpace", choice(mSpace) == "world"},
        {"recenter", choice(mRecenter)},
        {"applyModifiers", mModifiers->isChecked()},
        {"normals", mNormals->isChecked()},
        {"uvs", mUvs->isChecked()},
        {"colors", mColors->isChecked()},
        {"triangulate", mTriangulate->isChecked()},
        {"contentPath", mContentPath->text().trimmed().toStdString()},
        {"niagaraSystem", mNiagara->text().trimmed().toStdString()},
    };
}

void MeshSequenceExporter::apply(const json& s)
{
    mApplying = true;
    const scene::anim::Playback& pb = scene::anim::playback();
    (s.value("playbackRange", true) ? mPlayback : mCustom)->setChecked(true);
    mStart->setValue(s.value("start", std::round(pb.minTime * pb.fps)));
    mEnd->setValue(s.value("end", std::round(pb.maxTime * pb.fps)));
    mStep->setValue(s.value("step", 1.0));
    setChoice(mFormat, s.value("format", json("fbx")));
    mFolder->setText(QString::fromStdString(s.value("folder", std::string("sequences"))));
    mName->setText(QString::fromStdString(s.value("name", std::string("{name}"))));
    setChoice(mNumbering, s.value("numbering", json("index")));
    mPadding->setValue(s.value("padding", 3));
    mScale->setValue(s.value("scale", 1.0));
    setChoice(mProfile, s.value("profile", json("ue5")));
    setChoice(mSpace, json(s.value("worldSpace", true) ? "world" : "object"));
    setChoice(mRecenter, s.value("recenter", json("none")));
    mModifiers->setChecked(s.value("applyModifiers", true));
    mNormals->setChecked(s.value("normals", true));
    mUvs->setChecked(s.value("uvs", true));
    mColors->setChecked(s.value("colors", true));
    mTriangulate->setChecked(s.value("triangulate", true));
    mContentPath->setText(QString::fromStdString(s.value("contentPath", std::string())));
    mNiagara->setText(QString::fromStdString(s.value("niagaraSystem", std::string())));
    mApplying = false;
}

json MeshSequenceExporter::call(bool dryRun) const
{
    json s = settings();
    std::string name = s["name"].get<std::string>();
    json c = {{"format", s["format"]},   {"folder", s["folder"]},   {"pattern", (name.empty() ? "{name}" : name) + "_{" + s["numbering"].get<std::string>() + "}"},
              {"padding", s["padding"]}, {"step", s["step"]},       {"scale", s["scale"]},
              {"profile", s["profile"]}, {"worldSpace", s["worldSpace"]}, {"recenter", s["recenter"]},
              {"applyModifiers", s["applyModifiers"]}, {"normals", s["normals"]}, {"uvs", s["uvs"]},
              {"colors", s["colors"]},   {"triangulate", s["triangulate"]}, {"dryRun", dryRun}};
    if (s["playbackRange"].get<bool>())
        c["playbackRange"] = true;
    else
    {
        c["start"] = s["start"];
        c["end"] = s["end"];
    }
    return c;
}

void MeshSequenceExporter::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        updatePreview();
    });
}

void MeshSequenceExporter::updatePreview()
{
    json selection = ops::execute("ls", {{"selection", true}});
    mObjects->setText(selection.empty() ? QString("Select the animated meshes to export.")
                                        : "Exports: " + QString::fromStdString(selection.dump()).remove('[').remove(']').remove('"'));
    if (selection.empty())
    {
        mPreview->clear();
        return;
    }
    try
    {
        json files = ops::execute("exportMeshSequence", call(true));
        if (files.empty())
        {
            mPreview->setText("Nothing to export.");
            return;
        }
        QString first = QString::fromStdString(std::filesystem::path(files.front().get<std::string>()).filename().string());
        QString lastName = QString::fromStdString(std::filesystem::path(files.back().get<std::string>()).filename().string());
        mPreview->setText(files.size() == 1 ? "Writes " + first : QString("Writes %1 files: %2 ... %3").arg(files.size()).arg(first, lastName));
    }
    catch (const CommandError& e)
    {
        mPreview->setText(QString::fromStdString(e.what()));
    }
}

void MeshSequenceExporter::exportNow()
{
    PrefStore::get().set(kPrefFile, kLastPref, settings());
    json written = runUi("exportMeshSequence", call(false));
    if (written.is_array())
        log::info("Mesh sequence: wrote {} file(s)", written.size());
}

void MeshSequenceExporter::sendToUnreal()
{
    const json s = settings();
    PrefStore::get().set(kPrefFile, kLastPref, s);
    json selection = ops::execute("ls", {{"selection", true}});
    if (selection.size() != 1)
    {
        log::error("Send to Unreal: select one mesh (or an object set or group holding the meshes)");
        return;
    }
    std::string path = s["contentPath"].get<std::string>();
    if (path.empty())
    {
        log::error("Send to Unreal: give a content path, like /Game/FX/SM_{}", selection[0].get<std::string>());
        return;
    }
    json sequence = {{"step", s["step"]},       {"scale", s["scale"]},           {"padding", s["padding"]},
                     {"worldSpace", s["worldSpace"]}, {"recenter", s["recenter"]}, {"applyModifiers", s["applyModifiers"]},
                     {"normals", s["normals"]}, {"uvs", s["uvs"]},               {"colors", s["colors"]}};
    if (!s["playbackRange"].get<bool>())
    {
        sequence["start"] = s["start"];
        sequence["end"] = s["end"];
    }
    const bool cache = s["format"] == "abc";
    json map = {{"args", selection}, {"contentPath", path}, {"kind", cache ? "geometryCache" : "meshSequence"}, {"sequence", sequence}};
    if (!cache && !s["niagaraSystem"].get<std::string>().empty())
        map["niagaraSystem"] = s["niagaraSystem"];
    if (runUi("unrealMap", map).is_null())
        return;
    runUi("unrealSync", json::object());
}
} // namespace lila::ui
