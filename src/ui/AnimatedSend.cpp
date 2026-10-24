#include "ui/AnimatedSend.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/SceneData.h"
#include "ui/Actions.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace lila::ui::animatedSend
{
using json = nlohmann::json;

namespace
{
constexpr const char* kDataKey = "animatedSend";

const char* kindId(Kind k)
{
    switch (k)
    {
    case Kind::Frames: return "frames";
    case Kind::Cache: return "cache";
    case Kind::Vat: return "vat";
    }
    return "cache";
}

Kind kindFrom(const std::string& s)
{
    return s == "frames" ? Kind::Frames : s == "vat" ? Kind::Vat : Kind::Cache;
}

/** The file export behind a kind; the Unreal sync samples through the same code. */
const char* exportCommand(Kind k)
{
    switch (k)
    {
    case Kind::Frames: return "exportMeshSequence";
    case Kind::Cache: return "exportAlembic";
    case Kind::Vat: return "exportVertexAnimation";
    }
    return "exportAlembic";
}

const char* mappingKind(Kind k)
{
    switch (k)
    {
    case Kind::Frames: return "meshSequence";
    case Kind::Cache: return "geometryCache";
    case Kind::Vat: return "vertexAnimation";
    }
    return "geometryCache";
}

scene::Node* findMesh(const std::string& mesh)
{
    try
    {
        return scene::Graph::current().find(mesh);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

std::string storeKey(const std::string& mesh)
{
    scene::Node* n = findMesh(mesh);
    return n ? n->fullPath() : mesh;
}

json defaults(const std::string& mesh)
{
    scene::Node* n = findMesh(mesh);
    const std::string name = n ? n->name() : mesh.substr(mesh.rfind('|') + 1);
    return {
        {"dest", "unreal"},
        {"kind", "cache"},
        {"playbackRange", true},
        {"start", 1.0},
        {"end", 24.0},
        {"step", 1.0},
        {"scale", 1.0},
        {"folder", "Export/" + name},
        {"frames", {{"contentPath", "/Game/Meshes/" + name + "/SM_" + name}, {"niagaraSystem", ""}}},
        {"cache", {{"contentPath", "/Game/Meshes/GC_" + name}}},
        {"vat", {{"contentPath", "/Game/Meshes/SM_" + name + "_VAT"}, {"format", "exr"}, {"materialParent", ""}}},
    };
}

void store(const std::string& mesh, const json& s)
{
    json all = scene::data::get(kDataKey, json::object());
    if (!all.is_object())
        all = json::object();
    if (all.value(storeKey(mesh), json()) == s)
        return;
    all[storeKey(mesh)] = s;
    scene::data::set(kDataKey, all);
}

json sampling(const json& s)
{
    json out = {{"scale", s.value("scale", 1.0)}};
    if (s.value("playbackRange", true))
        out["playbackRange"] = true;
    else
    {
        out["start"] = s.value("start", 1.0);
        out["end"] = s.value("end", 24.0);
        out["step"] = s.value("step", 1.0);
    }
    return out;
}

json runUi(const char* cmd, const json& call)
{
    return ops::execute(cmd, call, CallSource::UI);
}

/** Returns true when the mesh was mapped (Unreal) or written (files). */
bool sendOne(const std::string& mesh, Dest dest, Kind kind, const json& s)
{
    scene::Node* n = findMesh(mesh);
    const std::string name = n ? n->name() : mesh;
    const json per = s.value(kindId(kind), json::object());
    if (dest == Dest::Unreal)
    {
        json call = {{"args", {mesh}}, {"contentPath", per.value("contentPath", std::string())}, {"kind", mappingKind(kind)}, {"sequence", sampling(s)}};
        if (kind == Kind::Frames && !per.value("niagaraSystem", std::string()).empty())
            call["niagaraSystem"] = per["niagaraSystem"];
        if (kind == Kind::Vat)
        {
            call["vat"] = {{"format", per.value("format", "exr")}};
            if (!per.value("materialParent", std::string()).empty())
                call["vat"]["materialParent"] = per["materialParent"];
        }
        runUi("unrealMap", call);
        return true;
    }
    json call = sampling(s);
    call["args"] = {mesh};
    const std::string folder = s.value("folder", std::string("Export/") + name);
    json files;
    switch (kind)
    {
    case Kind::Frames:
        call["format"] = "fbx";
        call["folder"] = folder;
        files = runUi("exportMeshSequence", call);
        break;
    case Kind::Cache:
        call["path"] = folder + "/" + name + ".abc";
        files = runUi("exportAlembic", call);
        break;
    case Kind::Vat:
        call["folder"] = folder;
        call["name"] = name;
        call["format"] = per.value("format", "exr");
        files = runUi("exportVertexAnimation", call);
        break;
    }
    log::info("{}: {} written to {}", name, kindLabel(kind), folder);
    return !files.is_null();
}

QDoubleSpinBox* numberBox(QWidget* parent, double lo, double hi, int decimals, double value)
{
    auto* b = new QDoubleSpinBox(parent);
    b->setRange(lo, hi);
    b->setDecimals(decimals);
    b->setValue(value);
    return b;
}
} // namespace

const char* kindLabel(Kind k)
{
    switch (k)
    {
    case Kind::Frames: return "Static Mesh Frames";
    case Kind::Cache: return "Geometry Cache";
    case Kind::Vat: return "Vertex Animation Texture";
    }
    return "";
}

bool available(Kind k)
{
    return ops::find(exportCommand(k)) != nullptr;
}

json settings(const std::string& mesh)
{
    json s = defaults(mesh);
    json all = scene::data::get(kDataKey, json::object());
    if (all.is_object())
        if (auto it = all.find(storeKey(mesh)); it != all.end() && it->is_object())
            s.merge_patch(*it);
    return s;
}

void send(const std::vector<std::string>& meshes, Dest dest, Kind kind)
{
    if (meshes.empty())
    {
        log::warn("Select a mesh with a geometry nodes modifier (or other animation) to send");
        return;
    }
    if (!available(kind))
    {
        log::error("{} export isn't available in this build", kindLabel(kind));
        return;
    }
    bool mapped = false;
    for (const std::string& mesh : meshes)
    {
        json s = settings(mesh);
        s["dest"] = dest == Dest::Unreal ? "unreal" : "files";
        s["kind"] = kindId(kind);
        try
        {
            mapped |= sendOne(mesh, dest, kind, s) && dest == Dest::Unreal;
            store(mesh, s);
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    }
    if (mapped)
    {
        try
        {
            runUi("unrealSync", json::object());
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
    }
}

void sendAgain(const std::vector<std::string>& meshes)
{
    for (const std::string& mesh : meshes)
    {
        json s = settings(mesh);
        send({mesh}, s.value("dest", "unreal") == "files" ? Dest::Files : Dest::Unreal, kindFrom(s.value("kind", "cache")));
    }
}

std::string lastLabel(const std::vector<std::string>& meshes)
{
    if (meshes.empty())
        return "Send";
    json s = settings(meshes.front());
    return std::string(s.value("dest", "unreal") == "files" ? "Files: " : "Unreal: ") + kindLabel(kindFrom(s.value("kind", "cache")));
}

void openOptions(QWidget* parent, const std::vector<std::string>& meshes)
{
    if (meshes.empty())
    {
        log::warn("Select a mesh with a geometry nodes modifier (or other animation) first");
        return;
    }
    json s = settings(meshes.front());

    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Send Animated Mesh");
    dialog->setMinimumWidth(460);
    auto* v = new QVBoxLayout(dialog);
    auto* info = new QLabel(QString::fromStdString(meshes.size() == 1 ? meshes.front() : meshes.front() + " and " + std::to_string(meshes.size() - 1) + " more"), dialog);
    info->setWordWrap(true);
    v->addWidget(info);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    auto* dest = new QComboBox(dialog);
    dest->addItem("Unreal (updates on every save)", "unreal");
    dest->addItem("Files", "files");
    dest->setCurrentIndex(s.value("dest", "unreal") == "files" ? 1 : 0);
    form->addRow("Destination", dest);
    auto* kind = new QComboBox(dialog);
    for (Kind k : {Kind::Frames, Kind::Cache, Kind::Vat})
        kind->addItem(kindLabel(k), kindId(k));
    kind->setCurrentIndex(kind->findData(QString::fromStdString(s.value("kind", "cache"))));
    form->addRow("As", kind);

    auto* playback = new QCheckBox("Playback range", dialog);
    playback->setChecked(s.value("playbackRange", true));
    auto* start = numberBox(dialog, -1e6, 1e6, 0, s.value("start", 1.0));
    auto* end = numberBox(dialog, -1e6, 1e6, 0, s.value("end", 24.0));
    auto* step = numberBox(dialog, 0.01, 1000.0, 2, s.value("step", 1.0));
    auto* range = new QHBoxLayout;
    range->addWidget(playback);
    for (auto [label, box] : {std::pair{"Start", start}, {"End", end}, {"Step", step}})
    {
        range->addWidget(new QLabel(label, dialog));
        range->addWidget(box, 1);
    }
    form->addRow("Frames", range);
    auto* scale = numberBox(dialog, 0.0001, 10000.0, 4, s.value("scale", 1.0));
    form->addRow("Scale", scale);
    auto* folder = new QLineEdit(QString::fromStdString(s.value("folder", std::string())), dialog);
    folder->setToolTip("Output folder for files; a relative one is under the workspace.");
    form->addRow("Folder", folder);
    auto* contentPath = new QLineEdit(dialog);
    form->addRow("Content path", contentPath);
    auto* niagara = new QLineEdit(QString::fromStdString(s["frames"].value("niagaraSystem", std::string())), dialog);
    niagara->setPlaceholderText("/Game/FX/NS_Flipbook (optional)");
    niagara->setToolTip("Static Mesh Frames only: the Niagara system whose mesh renderer gets the frames, in order.");
    form->addRow("Niagara system", niagara);
    auto* vatFormat = new QComboBox(dialog);
    vatFormat->addItem("EXR offsets (HDR 1)", "exr");
    vatFormat->addItem("16-bit PNG + bounds (HDR 0)", "png16");
    vatFormat->addItem("8-bit PNG + bounds (HDR 0)", "png8");
    vatFormat->setCurrentIndex(std::max(0, vatFormat->findData(QString::fromStdString(s["vat"].value("format", "exr")))));
    vatFormat->setToolTip("Vertex Animation Texture only: the position texture. PNGs are normalised to BoundsMin/BoundsMax, "
                          "which the export reports with FrameCount and the texture names.");
    form->addRow("VAT positions", vatFormat);
    auto* vatParent = new QLineEdit(QString::fromStdString(s["vat"].value("materialParent", std::string())), dialog);
    vatParent->setPlaceholderText("Generated VAT material (default)");
    vatParent->setToolTip("Vertex Animation Texture to Unreal: your own parent material. Its HDR, BoundsMin, BoundsMax, "
                          "PositionTexture, NormalTexture and FrameCount parameters get filled in on the instance.");
    form->addRow("VAT material parent", vatParent);
    v->addLayout(form);

    // Content paths are kept per kind, so switching kind shows that kind's asset.
    auto paths = std::make_shared<json>(json{{"frames", s["frames"]}, {"cache", s["cache"]}, {"vat", s["vat"]}});
    auto shownKind = std::make_shared<std::string>(kind->currentData().toString().toStdString());
    auto sync = [=] {
        const bool files = dest->currentData().toString() == "files";
        const bool frames = kind->currentData().toString() == "frames";
        const bool custom = !playback->isChecked();
        for (QWidget* w : std::initializer_list<QWidget*>{start, end, step})
            w->setEnabled(custom);
        folder->setEnabled(files);
        contentPath->setEnabled(!files);
        niagara->setEnabled(!files && frames);
        const bool vat = kind->currentData().toString() == "vat";
        vatFormat->setEnabled(vat);
        vatParent->setEnabled(vat && !files);
    };
    QObject::connect(kind, &QComboBox::currentIndexChanged, dialog, [=] {
        (*paths)[*shownKind]["contentPath"] = contentPath->text().trimmed().toStdString();
        *shownKind = kind->currentData().toString().toStdString();
        contentPath->setText(QString::fromStdString((*paths)[*shownKind].value("contentPath", std::string())));
        sync();
    });
    contentPath->setText(QString::fromStdString((*paths)[*shownKind].value("contentPath", std::string())));
    QObject::connect(dest, &QComboBox::currentIndexChanged, dialog, sync);
    QObject::connect(playback, &QCheckBox::toggled, dialog, sync);
    sync();

    auto collect = [=] {
        (*paths)[*shownKind]["contentPath"] = contentPath->text().trimmed().toStdString();
        (*paths)["frames"]["niagaraSystem"] = niagara->text().trimmed().toStdString();
        (*paths)["vat"]["format"] = vatFormat->currentData().toString().toStdString();
        (*paths)["vat"]["materialParent"] = vatParent->text().trimmed().toStdString();
        json out = {
            {"dest", dest->currentData().toString().toStdString()},
            {"kind", kind->currentData().toString().toStdString()},
            {"playbackRange", playback->isChecked()},
            {"start", start->value()},
            {"end", end->value()},
            {"step", step->value()},
            {"scale", scale->value()},
            {"folder", folder->text().trimmed().toStdString()},
        };
        out.update(*paths);
        return out;
    };

    auto* buttons = new QDialogButtonBox(dialog);
    QPushButton* sendBtn = buttons->addButton("Send", QDialogButtonBox::AcceptRole);
    QPushButton* saveBtn = buttons->addButton("Save", QDialogButtonBox::ApplyRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    sendBtn->setDefault(true);
    v->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(saveBtn, &QPushButton::clicked, dialog, [=] {
        json out = collect();
        for (const std::string& m : meshes)
            store(m, out);
        dialog->accept();
    });
    QObject::connect(sendBtn, &QPushButton::clicked, dialog, [=] {
        json out = collect();
        for (const std::string& m : meshes)
            store(m, out);
        dialog->accept();
        sendAgain(meshes);
    });
    dialog->show();
}

void fillMenu(QMenu* menu, QWidget* parent, std::function<std::vector<std::string>()> meshes)
{
    for (Dest d : {Dest::Unreal, Dest::Files})
    {
        menu->addSection(d == Dest::Unreal ? "Send to Unreal" : "Export Files");
        for (Kind k : {Kind::Frames, Kind::Cache, Kind::Vat})
        {
            QAction* a = menu->addAction(kindLabel(k), menu, [meshes, d, k] { send(meshes(), d, k); });
            a->setEnabled(available(k));
        }
    }
    menu->addSeparator();
    QPointer<QWidget> owner(parent);
    menu->addAction("Options...", menu, [owner, meshes] { openOptions(owner, meshes()); });
}

void registerActions(QWidget* parent)
{
    auto selectedMeshes = [] {
        std::vector<std::string> out;
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
                out.push_back(n->path());
        return out;
    };
    QPointer<QWidget> owner(parent);
    actions::add({.id = "animated.sendAgain", .label = "Send Animated Mesh Again", .category = "Unreal",
                  .run = [selectedMeshes] { sendAgain(selectedMeshes()); },
                  .doc = "Sends the selected animated meshes where they went last, as the same kind (static mesh frames, "
                         "Geometry Cache or vertex animation texture), to Unreal or to files.",
                  .python = "lila.ops.unrealMap(\"Spike\", contentPath=\"/Game/FX/GC_Spike\", kind=\"geometryCache\"); lila.ops.unrealSync()"});
    actions::add({.id = "animated.sendOptions", .label = "Send Animated Mesh Options...", .category = "Unreal",
                  .run = [owner, selectedMeshes] { openOptions(owner, selectedMeshes()); },
                  .doc = "Destination, kind, frame range, scale, folder and Unreal content path for the selected animated meshes."});
    for (Dest d : {Dest::Unreal, Dest::Files})
        for (Kind k : {Kind::Frames, Kind::Cache, Kind::Vat})
        {
            std::string id = std::string("animated.") + (d == Dest::Unreal ? "unreal." : "files.") + kindId(k);
            std::string label = std::string(d == Dest::Unreal ? "Send to Unreal as " : "Export as ") + kindLabel(k);
            actions::add({.id = id, .label = label, .category = "Unreal",
                          .run = [selectedMeshes, d, k] { send(selectedMeshes(), d, k); },
                          .doc = label + ": samples the selected animated meshes over their frame range with the options they last used."});
        }
}
} // namespace lila::ui::animatedSend
