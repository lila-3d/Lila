// The Foliage Blades panel, actions and menu. The panel's values live in the preferences, so the actions run with them
// from a menu, hotkey or shelf whether the panel is open or not.

#include "BladeExtrude.h"
#include "Blades.h"

#include "PanelKit.h"

#include <lila/Command.h>
#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <random>
#include <vector>

using namespace lila;
using namespace lila::ui;

namespace blades
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
    json stored = PrefStore::get().value(kPrefsFile, kPrefsKey, nullptr);
    // With nothing saved here yet, the values saved under /Tools in terrainGenerator.json are used.
    if (stored.is_null())
        stored = PrefStore::get().value("terrainGenerator", "/Tools", json::object());
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

/** Value field for a value at a JSON pointer in the stored values. */
QDoubleSpinBox* numberField(QWidget* parent, const std::string& pointer, double value, double low, double high, int decimals,
                            const char* tip, const QString& unit = {})
{
    QDoubleSpinBox* box = panelkit::valueField(low, high, decimals, unit, parent);
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

/** -1..1 bias toward the top. */
QDoubleSpinBox* biasField(QWidget* parent, const std::string& pointer, double value)
{
    auto* box = panelkit::valueField(0, 1, 0, {}, parent);
    box->setRange(-1.0, 1.0);
    box->setDecimals(2);
    box->setSingleStep(0.05);
    box->setValue(value);
    box->setToolTip("Bias To Top: 1 pushes the high end of the range to the blades near the top, -1 to the ones near the bottom, 0 is pure random.");
    QObject::connect(box, &QDoubleSpinBox::valueChanged, box, [pointer](double x) {
        json v = load();
        v[json::json_pointer(pointer)] = x;
        save(v);
    });
    return box;
}

QFormLayout* bladesForm(QWidget* w)
{
    auto* f = new QFormLayout(w);
    f->setContentsMargins(0, 0, 0, 0);
    f->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->setVerticalSpacing(4);
    return f;
}

/** The panel's content; rebuilt in place when the stored values change under it (Read Selection). */
void fill(QWidget* body)
{
    constexpr const char* kSections = "foliageBlades";
    const json v = load();
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(4);

    auto* common = new QWidget(body);
    auto* cf = bladesForm(common);
    panelkit::Segmented up = panelkit::segmented(common);
    for (int i = 0; i < int(std::size(kUpChoices)); ++i)
        up.add(kUpChoices[i], i == 0 ? "Auto: up follows the scene" : QString("%1 is up").arg(kUpChoices[i]), i);
    for (int i = 0; i < int(std::size(kUpChoices)); ++i)
        if (v["up"].get<std::string>() == kUpChoices[i])
            up.setCurrent(i);
    up.widget->setToolTip("Up Axis: which axis counts as up when biasing toward the top. Auto follows the scene.");
    QObject::connect(up.group, &QButtonGroup::idClicked, up.group, [](int id) {
        json x = load();
        x["up"] = kUpChoices[std::clamp(id, 0, int(std::size(kUpChoices)) - 1)];
        save(x);
    });
    cf->addRow("Up Axis", up.widget);
    auto* seedRow = new QWidget(common);
    auto* sr = new QHBoxLayout(seedRow);
    sr->setContentsMargins(0, 0, 0, 0);
    sr->setSpacing(2);
    QDoubleSpinBox* seed = panelkit::valueField(0, 999999, 0, {}, seedRow);
    seed->setValue(double(seedOf(v)));
    seed->setToolTip("Seed: 0 gives a different result every run. Any other number repeats.");
    QObject::connect(seed, &QDoubleSpinBox::valueChanged, seed, [](double s) {
        json x = load();
        x["seed"] = int(std::lround(s));
        save(x);
    });
    sr->addWidget(seed, 1);
    auto* newSeed = new QToolButton(seedRow);
    newSeed->setText("New");
    newSeed->setToolTip("New Seed: pick a new random seed.");
    panelkit::setRole(newSeed, "push");
    QObject::connect(newSeed, &QToolButton::clicked, seed, [seed] {
        static std::mt19937 pick{std::random_device{}()};
        seed->setValue(std::uniform_int_distribution<int>(1, 999999)(pick));
    });
    sr->addWidget(newSeed);
    cf->addRow("Seed", seedRow);
    layout->addWidget(panelkit::section("Common", common, body, kSections));

    auto* ex = new QWidget(body);
    auto* ev = new QVBoxLayout(ex);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(4);
    auto* nfBox = new QWidget(ex);
    auto* nf = bladesForm(nfBox);
    const json& n = v["newExtrude"];
    nf->addRow("Thickness", numberField(nfBox, "/newExtrude/thickness", num(n, "thickness", 1.0), -1e5, 1e5, 3,
                                        "Thickness: how far new blades grow along their face normal.", "cm"));
    nf->addRow("Offset", numberField(nfBox, "/newExtrude/offset", num(n, "offset", 0.0), -1e5, 1e5, 3,
                                     "Offset: how much new blades narrow as they grow.", "cm"));
    auto* divisions = panelkit::valueField(0, 1, 0, {}, nfBox);
    divisions->setRange(1, 64);
    divisions->setDecimals(0);
    divisions->setValue(num(n, "divisions", 1));
    divisions->setToolTip("Divisions: segments up each new blade.");
    QObject::connect(divisions, &QDoubleSpinBox::valueChanged, divisions, [](double x) {
        json s = load();
        s["newExtrude"]["divisions"] = int(std::lround(x));
        save(s);
    });
    nf->addRow("Divisions", divisions);
    nf->addRow("", checkField(nfBox, "Keep Faces Together", "/newExtrude/keepFacesTogether", n.value("keepFacesTogether", false),
                              "Keep Faces Together: connected faces grow as one blade. Off, every face is its own blade."));
    ev->addWidget(nfBox);
    ev->addWidget(panelkit::push("foliageBlades.extrudeBlades", ex, "Extrude Blades"));
    layout->addWidget(panelkit::section("Extrude", ex, body, kSections));

    auto* rand = new QWidget(body);
    auto* rv = new QVBoxLayout(rand);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(4);
    auto* gridBox = new QWidget(rand);
    auto* grid = bladesForm(gridBox);
    for (int f = 0; f < kFieldCount; ++f)
    {
        auto* row = new QWidget(gridBox);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        const std::string base = std::string("/extrude/") + fieldName(f);
        const glm::dvec2 r = extrudeRange(v, f);
        QCheckBox* on = checkField(row, "", base + "/enabled", extrudeEnabled(v, f), "Randomize this value.");
        QDoubleSpinBox* low = numberField(row, base + "/low", r.x, -1e5, 1e5, 3, "Low: lowest value to pick.");
        QDoubleSpinBox* high = numberField(row, base + "/high", r.y, -1e5, 1e5, 3, "High: highest value to pick.");
        low->setEnabled(on->isChecked());
        high->setEnabled(on->isChecked());
        QObject::connect(on, &QCheckBox::toggled, low, &QWidget::setEnabled);
        QObject::connect(on, &QCheckBox::toggled, high, &QWidget::setEnabled);
        h->addWidget(on);
        h->addWidget(low, 1);
        h->addWidget(high, 1);
        grid->addRow(fieldLabel(f), row);
    }
    grid->addRow("Bias To Top", biasField(gridBox, "/extrudeBias", num(v, "extrudeBias", 0.0)));
    rv->addWidget(gridBox);
    rv->addWidget(panelkit::grid({panelkit::push("foliageBlades.randomizeExtrude", rand, "Randomize"),
                                  panelkit::push("foliageBlades.readExtrude", rand, "Read Selection")},
                                 2, rand));
    rv->addWidget(panelkit::overflow({"foliageBlades.resetExtrude", "foliageBlades.selectExtruded", "foliageBlades.selectStems"}, rand));
    layout->addWidget(panelkit::section("Randomize Extrude", rand, body, kSections));

    auto* de = new QWidget(body);
    auto* dv = new QVBoxLayout(de);
    dv->setContentsMargins(0, 0, 0, 0);
    dv->setSpacing(4);
    auto* dfBox = new QWidget(de);
    auto* df = bladesForm(dfBox);
    auto* type = new QComboBox(dfBox);
    for (const DeformerSpec& s : deformerSpecs())
        type->addItem(s.label);
    type->setCurrentText(QString::fromStdString(v["deformer"].get<std::string>()));
    type->setToolTip("Type: the deformer each blade gets");
    df->addRow("Type", type);
    const glm::dvec2 amount = amountRange(v);
    const std::string kind = currentSpec(v).kind;
    auto* amountRow = new QWidget(dfBox);
    auto* ar = new QHBoxLayout(amountRow);
    ar->setContentsMargins(0, 0, 0, 0);
    ar->setSpacing(2);
    ar->addWidget(numberField(amountRow, "/amounts/" + kind + "/low", amount.x, -1e5, 1e5, 3, "Low: lowest value to pick."), 1);
    ar->addWidget(numberField(amountRow, "/amounts/" + kind + "/high", amount.y, -1e5, 1e5, 3, "High: highest value to pick."), 1);
    df->addRow(currentSpec(v).amount, amountRow);
    df->addRow("Bias To Top", biasField(dfBox, "/deformerBias", num(v, "deformerBias", 0.0)));
    df->addRow("", checkField(dfBox, "One Per Blade", "/deformerRegions", v.value("deformerRegions", true),
                              "One Per Blade: give each blade its own deformer. Off puts one deformer on the whole selection."));
    df->addRow("", checkField(dfBox, "From Base", "/deformerBase", v.value("deformerBase", true),
                              "From Base: anchor the handle at the bottom of each blade so it deforms root to tip. Off centres the handle."));
    df->addRow("", checkField(dfBox, "Random Direction", "/deformerSpin", v.value("deformerSpin", true),
                              "Random Direction: spin each handle around the up axis, so blades lean different ways."));
    dv->addWidget(dfBox);
    dv->addWidget(panelkit::grid({panelkit::push("foliageBlades.addDeformer", de, "Add"), panelkit::push("foliageBlades.randomizeDeformer", de, "Randomize"),
                                  panelkit::push("foliageBlades.deleteDeformer", de, "Delete")},
                                 3, de));
    layout->addWidget(panelkit::section("Deformers", de, body, kSections));

    auto* statusLine = new QLabel(body);
    statusLine->setWordWrap(true);
    statusLine->setEnabled(false);
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
    const char* cat = "Foliage";
    struct Entry
    {
        const char* id;
        const char* label;
        const char* doc;
        std::function<void()> run;
    };
    const std::vector<Entry> entries = {
        {"foliageBlades.extrudeBlades", "Extrude Blades", "Grow blades from the selected faces with the Foliage Blades extrude settings.", extrudeBlades},
        {"foliageBlades.randomizeExtrude", "Randomize Blade Extrude", "Give every blade of the selected meshes random extrude values.", randomizeExtrude},
        {"foliageBlades.readExtrude", "Read Blade Extrude Ranges", "Fill the extrude ranges from what the selected blades use now.", readExtrude},
        {"foliageBlades.resetExtrude", "Reset Blade Extrude", "Put the selected meshes' blades back to the values they were extruded with.", resetExtrude},
        {"foliageBlades.selectExtruded", "Select Extruded Faces", "Select the caps of the blades on the selected meshes.", [] { selectExtruded(false); }},
        {"foliageBlades.selectStems", "Select Stems", "Select every face the blades made, caps and walls.", [] { selectExtruded(true); }},
        {"foliageBlades.addDeformer", "Add Blade Deformer", "Add the Foliage Blades deformer type to the selected faces, one per blade, and randomize it.", addDeformer},
        {"foliageBlades.randomizeDeformer", "Randomize Blade Deformers", "Randomize the deformers of the current type on the selection.", randomizeDeformer},
        {"foliageBlades.deleteDeformer", "Delete Blade Deformers", "Delete the deformers of the current type on the selection.", deleteDeformer},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id, .label = e.label, .category = cat, .run = e.run, .doc = e.doc});

    panels::add(ctx, {.id = "foliageBlades", .title = "Foliage Blades", .create = makePanel, .icon = "plant-2", .open = false});

    menus::addItem(ctx, {.id = "foliageBlades.menu", .menu = "Mesh", .kind = menus::Kind::Submenu, .label = "Foliage Blades"});
    int sep = 0;
    for (const char* a : {"foliageBlades.extrudeBlades", "foliageBlades.randomizeExtrude", "foliageBlades.resetExtrude", "",
                          "foliageBlades.selectExtruded", "foliageBlades.selectStems", "", "foliageBlades.addDeformer",
                          "foliageBlades.randomizeDeformer", "foliageBlades.deleteDeformer", "", "window.foliageBlades"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("foliageBlades.menu.") + a, .menu = "Mesh/Foliage Blades", .action = a});
        else
            menus::addItem(ctx, {.id = "foliageBlades.menu.sep" + std::to_string(sep++), .menu = "Mesh/Foliage Blades", .kind = menus::Kind::Separator});
    }
}
} // namespace blades
