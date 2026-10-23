// Terrain Tools' panel, actions and menu. The panel's values live in the preferences, so the actions run with them
// from a menu, hotkey or shelf whether the panel is open or not.

#include "BladeExtrude.h"
#include "TerrainTools.h"

#include <lila/Command.h>
#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <random>
#include <vector>

using namespace lila;
using namespace lila::ui;

namespace terraintools
{
namespace
{
const char* const kUpChoices[] = {"Auto", "X", "Y", "Z"};

json defaults()
{
    return {{"up", "Auto"},
            {"seed", 0},
            {"extrudeBias", 0.0},
            {"extrudeSplit", false},
            {"extrude", json::object()},
            {"deformer", "Bend"},
            {"deformerBias", 0.0},
            {"deformerBase", true},
            {"deformerRegions", true},
            {"deformerSpin", true},
            {"amounts", json::object()},
            {"newExtrude", {{"thickness", 1.0}, {"offset", 0.0}, {"divisions", 1}, {"keepFacesTogether", false}}}};
}

/** The stored values over the defaults, with unknown choices put back. */
json load()
{
    json v = defaults();
    json stored = PrefStore::get().value(kPrefsFile, kPrefsKey, json::object());
    if (stored.is_object())
        for (auto& [k, x] : stored.items())
            if (k == "extrude" || k == "amounts" || k == "newExtrude")
            {
                if (x.is_object())
                    v[k].update(x);
            }
            else
                v[k] = x;
    if (!v["deformer"].is_string() || !findSpec(v["deformer"].get<std::string>()))
        v["deformer"] = "Bend";
    const std::string up = v["up"].is_string() ? v["up"].get<std::string>() : "";
    if (up != "Auto" && up != "X" && up != "Y" && up != "Z")
        v["up"] = "Auto";
    return v;
}

void save(const json& v) { PrefStore::get().set(kPrefsFile, kPrefsKey, v); }

double num(const json& j, const char* key, double fallback)
{
    const json& x = j.is_object() ? j.value(key, json()) : json();
    return x.is_number() ? x.get<double>() : fallback;
}

const DeformerSpec& currentSpec(const json& v) { return *findSpec(v["deformer"].get<std::string>()); }

glm::dvec2 amountRange(const json& v)
{
    const DeformerSpec& s = currentSpec(v);
    const json& stored = v["amounts"].value(s.kind, json::object());
    return {num(stored, "low", s.low), num(stored, "high", s.high)};
}

glm::dvec2 extrudeRange(const json& v, int field)
{
    const json& stored = v["extrude"].value(fieldName(field), json::object());
    const glm::dvec2 d = fieldDefaultRange(field);
    return {num(stored, "low", d.x), num(stored, "high", d.y)};
}

bool extrudeEnabled(const json& v, int field)
{
    const json& stored = v["extrude"].value(fieldName(field), json::object());
    const json& e = stored.is_object() ? stored.value("enabled", json()) : json();
    return e.is_boolean() ? e.get<bool>() : field == kThickness;
}

QPointer<QLabel> gStatus;
std::function<void()> gRefresh;

void status(const std::string& text)
{
    if (gStatus)
        gStatus->setText(QString::fromStdString(text));
}

/** Runs a command from the UI; a failure is reported in the status line and the log rather than thrown. */
json runCommand(const char* name, const json& call)
{
    try
    {
        return ops::execute(name, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        status(e.what());
        log::write(LogLevel::Warning, e.what());
        return nullptr;
    }
}

int64_t seedOf(const json& v) { return v["seed"].is_number_integer() ? v["seed"].get<int64_t>() : 0; }

void extrudeBlades()
{
    const json v = load();
    const json& n = v["newExtrude"];
    json r = runCommand("bladeExtrude", {{"thickness", num(n, "thickness", 1.0)},
                                         {"offset", num(n, "offset", 0.0)},
                                         {"divisions", int(num(n, "divisions", 1))},
                                         {"keepFacesTogether", n.value("keepFacesTogether", false)}});
    if (r.is_object())
    {
        size_t count = 0;
        for (auto& [mesh, ids] : r.items())
            count += ids.size();
        status("Extruded " + std::to_string(count) + " blades on " + std::to_string(r.size()) + " mesh(es).");
    }
}

void randomizeExtrude()
{
    const json v = load();
    json fields = json::object();
    for (int f = 0; f < kFieldCount; ++f)
        if (extrudeEnabled(v, f))
        {
            const glm::dvec2 r = extrudeRange(v, f);
            fields[fieldName(f)] = {r.x, r.y};
        }
    json r = runCommand("randomizeBladeExtrude", {{"fields", fields}, {"bias", num(v, "extrudeBias", 0.0)}, {"up", v["up"]}, {"seed", seedOf(v)}});
    if (r.is_object())
        status("Randomized " + std::to_string(r["blades"].get<int>()) + " blades across " + std::to_string(r["meshes"].get<int>()) +
               " mesh(es).");
}

void readExtrude()
{
    json r = runCommand("bladeExtrudeRanges", json::object());
    if (!r.is_object())
        return;
    json v = load();
    for (int f = 0; f < kFieldCount; ++f)
        if (r.contains(fieldName(f)))
        {
            v["extrude"][fieldName(f)]["low"] = r[fieldName(f)][0];
            v["extrude"][fieldName(f)]["high"] = r[fieldName(f)][1];
        }
    save(v);
    if (gRefresh)
        gRefresh();
    status("Ranges read from the selection.");
}

void resetExtrude()
{
    json r = runCommand("resetBladeExtrude", json::object());
    if (r.is_number())
        status("Reset " + std::to_string(r.get<int>()) + " blades to their extruded values.");
}

void selectExtruded(bool stems)
{
    json r = runCommand("selectBladeFaces", {{"stems", stems}});
    if (r.is_number())
        status("Selected " + std::to_string(r.get<int>()) + (stems ? " stem faces." : " extruded faces."));
}

void addDeformer()
{
    const json v = load();
    const DeformerSpec& s = currentSpec(v);
    const glm::dvec2 range = amountRange(v);
    json r = runCommand("addBladeDeformer", {{"kind", s.kind},
                                             {"perBlade", v.value("deformerRegions", true)},
                                             {"fromBase", v.value("deformerBase", true)},
                                             {"spin", v.value("deformerSpin", true)},
                                             {"up", v["up"]},
                                             {"seed", seedOf(v)},
                                             {"low", range.x},
                                             {"high", range.y},
                                             {"bias", num(v, "deformerBias", 0.0)}});
    if (r.is_array())
        status("Added " + std::to_string(r.size()) + " " + s.kind + " deformer(s).");
}

void randomizeDeformer()
{
    const json v = load();
    const DeformerSpec& s = currentSpec(v);
    const glm::dvec2 range = amountRange(v);
    json r = runCommand("randomizeBladeDeformers", {{"kind", s.kind},
                                                    {"low", range.x},
                                                    {"high", range.y},
                                                    {"bias", num(v, "deformerBias", 0.0)},
                                                    {"up", v["up"]},
                                                    {"seed", seedOf(v)},
                                                    {"spin", v.value("deformerSpin", true)}});
    if (r.is_number())
        status("Randomized " + std::to_string(r.get<int>()) + " " + s.kind + " deformer(s).");
}

void deleteDeformer()
{
    const DeformerSpec& s = currentSpec(load());
    json r = runCommand("deleteBladeDeformers", {{"kind", s.kind}});
    if (r.is_number())
        status("Deleted " + std::to_string(r.get<int>()) + " " + s.kind + " deformer(s).");
}

/** Spin box for a value at a JSON pointer in the stored values. */
QDoubleSpinBox* numberField(QWidget* parent, const std::string& pointer, double value, double low, double high, int decimals,
                            const char* tip)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(low, high);
    box->setDecimals(decimals);
    box->setSingleStep(decimals > 1 ? 0.1 : 1.0);
    box->setValue(value);
    box->setToolTip(tip);
    QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [pointer](double x) {
        json v = load();
        v[json::json_pointer(pointer)] = x;
        save(v);
    });
    return box;
}

QCheckBox* checkField(QWidget* parent, const char* label, const std::string& pointer, bool value, const char* tip)
{
    auto* box = new QCheckBox(label, parent);
    box->setChecked(value);
    box->setToolTip(tip);
    QObject::connect(box, &QCheckBox::toggled, box, [pointer](bool on) {
        json v = load();
        v[json::json_pointer(pointer)] = on;
        save(v);
    });
    return box;
}

/** -1..1 bias: a slider and a spin box kept together. */
QWidget* biasField(QWidget* parent, const std::string& pointer, double value)
{
    auto* w = new QWidget(parent);
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    auto* slider = new QSlider(Qt::Horizontal, w);
    slider->setRange(-100, 100);
    slider->setValue(int(std::lround(value * 100.0)));
    auto* box = numberField(w, pointer, value, -1.0, 1.0, 2,
                            "1 pushes the high end of the range to the blades near the top, -1 to the ones near the bottom, 0 is pure random.");
    box->setSingleStep(0.05);
    QObject::connect(slider, &QSlider::valueChanged, box, [box](int x) { box->setValue(x / 100.0); });
    QObject::connect(box, &QDoubleSpinBox::valueChanged, slider, [slider](double x) {
        const QSignalBlocker block(slider);
        slider->setValue(int(std::lround(x * 100.0)));
    });
    w->setToolTip(box->toolTip());
    row->addWidget(box);
    row->addWidget(slider, 1);
    return w;
}

QPushButton* button(QWidget* parent, const char* label, const char* tip, std::function<void()> fn)
{
    auto* b = new QPushButton(label, parent);
    b->setToolTip(tip);
    QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
    return b;
}

QLabel* note(QWidget* parent, const char* text)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setItalic(true);
    l->setFont(f);
    return l;
}

/** The panel's content; rebuilt in place when the stored values change under it (Read Selection). */
void fill(QWidget* body)
{
    const json v = load();
    auto* layout = new QVBoxLayout(body);

    auto* common = new QGroupBox("Common", body);
    auto* cf = new QFormLayout(common);
    auto* up = new QComboBox(common);
    for (const char* c : kUpChoices)
        up->addItem(c);
    up->setCurrentText(QString::fromStdString(v["up"].get<std::string>()));
    up->setToolTip("Which axis counts as up when biasing toward the top. Auto follows the scene.");
    QObject::connect(up, &QComboBox::currentTextChanged, up, [](const QString& t) {
        json x = load();
        x["up"] = t.toStdString();
        save(x);
    });
    cf->addRow("Up Axis", up);
    auto* seedRow = new QWidget(common);
    auto* sr = new QHBoxLayout(seedRow);
    sr->setContentsMargins(0, 0, 0, 0);
    auto* seed = new QSpinBox(seedRow);
    seed->setRange(0, 999999);
    seed->setValue(int(seedOf(v)));
    seed->setToolTip("0 gives a different result every run. Any other number repeats.");
    QObject::connect(seed, &QSpinBox::valueChanged, seed, [](int s) {
        json x = load();
        x["seed"] = s;
        save(x);
    });
    sr->addWidget(seed, 1);
    sr->addWidget(button(seedRow, "New", "Pick a new random seed.", [seed] {
        static std::mt19937 pick{std::random_device{}()};
        seed->setValue(std::uniform_int_distribution<int>(1, 999999)(pick));
    }));
    cf->addRow("Seed", seedRow);
    layout->addWidget(common);

    auto* ex = new QGroupBox("Extrude", body);
    auto* ev = new QVBoxLayout(ex);
    ev->addWidget(note(ex, "Grow blades from the selected faces, then vary them. Every blade keeps its own values."));
    auto* nf = new QFormLayout;
    const json& n = v["newExtrude"];
    nf->addRow("Thickness", numberField(ex, "/newExtrude/thickness", num(n, "thickness", 1.0), -1e5, 1e5, 3,
                                        "How far new blades grow along their face normal, cm."));
    nf->addRow("Offset", numberField(ex, "/newExtrude/offset", num(n, "offset", 0.0), -1e5, 1e5, 3,
                                     "How much new blades narrow as they grow, cm."));
    auto* divisions = numberField(ex, "/newExtrude/divisions", num(n, "divisions", 1), 1, 64, 0, "Segments up each new blade.");
    nf->addRow("Divisions", divisions);
    ev->addLayout(nf);
    ev->addWidget(checkField(ex, "Keep Faces Together", "/newExtrude/keepFacesTogether", n.value("keepFacesTogether", false),
                             "Connected faces grow as one blade. Off, every face is its own blade."));
    ev->addWidget(button(ex, "Extrude Blades", "Grow blades from the selected faces.", extrudeBlades));

    auto* grid = new QFormLayout;
    for (int f = 0; f < kFieldCount; ++f)
    {
        auto* row = new QWidget(ex);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        const std::string base = std::string("/extrude/") + fieldName(f);
        const glm::dvec2 r = extrudeRange(v, f);
        h->addWidget(checkField(row, "", base + "/enabled", extrudeEnabled(v, f), "Randomize this value."));
        h->addWidget(numberField(row, base + "/low", r.x, -1e5, 1e5, 3, "Lowest value to pick."), 1);
        h->addWidget(numberField(row, base + "/high", r.y, -1e5, 1e5, 3, "Highest value to pick."), 1);
        grid->addRow(fieldLabel(f), row);
    }
    grid->addRow("Bias To Top", biasField(ex, "/extrudeBias", num(v, "extrudeBias", 0.0)));
    ev->addLayout(grid);
    auto* er = new QHBoxLayout;
    er->addWidget(button(ex, "Randomize Extrude", "Randomize the blades of every selected mesh.", randomizeExtrude), 1);
    er->addWidget(button(ex, "Read Selection", "Fill the ranges with give or take 30% of what the selected blades use now.", readExtrude));
    er->addWidget(button(ex, "Reset", "Put the blades back to the values they were extruded with.", resetExtrude));
    ev->addLayout(er);
    auto* sel = new QHBoxLayout;
    sel->addWidget(button(ex, "Select Extruded Faces",
                          "Select the faces the blades grew from, which are their caps now. Worked out from the mesh each time.",
                          [] { selectExtruded(false); }),
                   1);
    sel->addWidget(button(ex, "Select Stems", "Select every face a blade made, caps and walls, down to the ring it stands on.",
                          [] { selectExtruded(true); }));
    ev->addLayout(sel);
    layout->addWidget(ex);

    auto* de = new QGroupBox("Deformers", body);
    auto* dv = new QVBoxLayout(de);
    dv->addWidget(note(de, "Adds a deformer to the selected faces, one per blade."));
    auto* df = new QFormLayout;
    auto* type = new QComboBox(de);
    for (const DeformerSpec& s : deformerSpecs())
        type->addItem(s.label);
    type->setCurrentText(QString::fromStdString(v["deformer"].get<std::string>()));
    df->addRow("Type", type);
    const glm::dvec2 amount = amountRange(v);
    const std::string kind = currentSpec(v).kind;
    auto* amountRow = new QWidget(de);
    auto* ar = new QHBoxLayout(amountRow);
    ar->setContentsMargins(0, 0, 0, 0);
    ar->addWidget(numberField(amountRow, "/amounts/" + kind + "/low", amount.x, -1e5, 1e5, 3, "Lowest value to pick."), 1);
    ar->addWidget(numberField(amountRow, "/amounts/" + kind + "/high", amount.y, -1e5, 1e5, 3, "Highest value to pick."), 1);
    df->addRow(currentSpec(v).amount, amountRow);
    df->addRow("Bias To Top", biasField(de, "/deformerBias", num(v, "deformerBias", 0.0)));
    dv->addLayout(df);
    dv->addWidget(checkField(de, "One Per Blade", "/deformerRegions", v.value("deformerRegions", true),
                             "Give each blade its own deformer. Off puts one deformer on the whole selection."));
    dv->addWidget(checkField(de, "From Base", "/deformerBase", v.value("deformerBase", true),
                             "Anchor the handle at the bottom of each blade so it deforms root to tip. Off centres the handle."));
    dv->addWidget(checkField(de, "Random Direction", "/deformerSpin", v.value("deformerSpin", true),
                             "Spin each handle around the up axis, so blades lean different ways."));
    auto* dr = new QHBoxLayout;
    dr->addWidget(button(de, "Add Deformer", "Add the deformer to the selected faces and randomize it.", addDeformer), 1);
    dr->addWidget(button(de, "Randomize", "Randomize the deformers of this type already on the selection.", randomizeDeformer));
    dr->addWidget(button(de, "Delete", "Delete the deformers of this type on the selection.", deleteDeformer));
    dv->addLayout(dr);
    layout->addWidget(de);

    auto* statusLine = new QLabel("Ready.", body);
    statusLine->setWordWrap(true);
    gStatus = statusLine;
    layout->addWidget(statusLine);
    layout->addStretch(1);

    QObject::connect(type, &QComboBox::currentTextChanged, body, [](const QString& t) {
        json x = load();
        x["deformer"] = t.toStdString();
        save(x);
        if (gRefresh)
            gRefresh();
    });
}

QWidget* makePanel(QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QPointer<QScrollArea> guard(scroll);
    gRefresh = [guard] {
        if (!guard)
            return;
        auto* body = new QWidget;
        fill(body);
        // Deferred: the refresh can come from a widget inside the body being replaced.
        QMetaObject::invokeMethod(guard, [guard, body] {
            if (guard)
                guard->setWidget(body);
            else
                delete body;
        }, Qt::QueuedConnection);
    };
    auto* body = new QWidget;
    fill(body);
    scroll->setWidget(body);
    return scroll;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    const char* cat = "Terrain Tools";
    struct Entry
    {
        const char* id;
        const char* label;
        const char* doc;
        std::function<void()> run;
    };
    const std::vector<Entry> entries = {
        {"terrainTools.extrudeBlades", "Extrude Blades", "Grow blades from the selected faces with the Terrain Tools extrude settings.", extrudeBlades},
        {"terrainTools.randomizeExtrude", "Randomize Blade Extrude", "Give every blade of the selected meshes random extrude values.", randomizeExtrude},
        {"terrainTools.readExtrude", "Read Blade Extrude Ranges", "Fill the extrude ranges from what the selected blades use now.", readExtrude},
        {"terrainTools.resetExtrude", "Reset Blade Extrude", "Put the selected meshes' blades back to the values they were extruded with.", resetExtrude},
        {"terrainTools.selectExtruded", "Select Extruded Faces", "Select the caps of the blades on the selected meshes.", [] { selectExtruded(false); }},
        {"terrainTools.selectStems", "Select Stems", "Select every face the blades made, caps and walls.", [] { selectExtruded(true); }},
        {"terrainTools.addDeformer", "Add Blade Deformer", "Add the Terrain Tools deformer type to the selected faces, one per blade, and randomize it.", addDeformer},
        {"terrainTools.randomizeDeformer", "Randomize Blade Deformers", "Randomize the deformers of the current type on the selection.", randomizeDeformer},
        {"terrainTools.deleteDeformer", "Delete Blade Deformers", "Delete the deformers of the current type on the selection.", deleteDeformer},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id, .label = e.label, .category = cat, .run = e.run, .doc = e.doc});

    panels::add(ctx, {.id = "terrainTools", .title = "Terrain Tools", .create = makePanel, .icon = "plant-2", .open = false});

    menus::addItem(ctx, {.id = "terrainTools.menu", .menu = "Mesh", .kind = menus::Kind::Submenu, .label = "Terrain Tools"});
    int sep = 0;
    for (const char* a : {"terrainTools.extrudeBlades", "terrainTools.randomizeExtrude", "terrainTools.resetExtrude", "",
                          "terrainTools.selectExtruded", "terrainTools.selectStems", "", "terrainTools.addDeformer",
                          "terrainTools.randomizeDeformer", "terrainTools.deleteDeformer", "", "window.terrainTools"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("terrainTools.menu.") + a, .menu = "Mesh/Terrain Tools", .action = a});
        else
            menus::addItem(ctx, {.id = "terrainTools.menu.sep" + std::to_string(sep++), .menu = "Mesh/Terrain Tools", .kind = menus::Kind::Separator});
    }
}
} // namespace terraintools
