// The Tree Generator panel: the schema's fields by section, a button per step, and a bindable action behind every
// button. The panel edits one shared state, so the actions run from hotkeys and menus with the panel closed too.

#include "TreeGenerator.h"
#include "TreeSchema.h"
#include "lila/FileDialog.h"

#include <lila/Command.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Prefs.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

using namespace lila;
using namespace lila::ui;

namespace treegen
{
namespace
{
/** What the panel shows and the actions build from: settings in metres, the preset and the mesh fields. */
struct State
{
    json values;
    std::string preset;
    std::string leaf, canopy;
    bool loaded = false;
};

State& state()
{
    static State s;
    if (!s.loaded)
    {
        s.loaded = true;
        json p = PrefStore::get().merged(kPrefsFile);
        std::string remembered = p.is_object() && p.contains("Preset") && p["Preset"].is_string() ? p["Preset"].get<std::string>() : "";
        const auto names = presetNames(userPresets());
        if (std::find(names.begin(), names.end(), remembered) == names.end())
            remembered = builtInPresets().front();
        s.preset = remembered;
        s.values = presetSettings(remembered, userPresets());
        if (p.is_object() && p.contains("Export") && p["Export"].is_object())
            for (const auto& [k, v] : p["Export"].items())
                if (field(k))
                    s.values[k] = coerce(k, v);
    }
    return s;
}

/** Repaints the live panel from the state; empty while no panel is open. */
std::function<void()> gRefresh;
std::function<void(const QString&)> gStatus;

void refresh()
{
    if (gRefresh)
        gRefresh();
}

void status(const std::string& text)
{
    if (gStatus)
        gStatus(QString::fromStdString(text));
    log::info("Tree Generator: {}", text);
}

json call(json extra = json::object())
{
    const State& s = state();
    extra["preset"] = s.preset;
    extra["settings"] = s.values;
    if (!s.leaf.empty())
        extra["leaf"] = s.leaf;
    if (!s.canopy.empty())
        extra["canopy"] = s.canopy;
    return extra;
}

/** Runs a command from the UI; failures go to the status line and the log rather than out of a Qt slot. */
json run(const char* name, const json& args)
{
    try
    {
        return ops::execute(name, args, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        if (gStatus)
            gStatus(QString::fromStdString(e.what()));
        log::write(LogLevel::Warning, e.what());
        return nullptr;
    }
}

std::string shortText(const json& r)
{
    if (r.is_string())
        return r.get<std::string>();
    return r.dump();
}

// ------------------------------------------------------------------------------------------------ actions

void applyPreset()
{
    State& s = state();
    const json user = userPresets();
    const json& listed = user.contains(s.preset) ? user[s.preset] : presetOverrides(s.preset);
    json values = presetSettings(s.preset, user);
    for (const auto& [k, v] : values.items())
        if ((k.starts_with("global.") || k.starts_with("export.")) && !listed.contains(k))
            values[k] = s.values[k];
    s.values = values;
    PrefStore::get().set(kPrefsFile, "/Preset", s.preset);
    refresh();
}

void savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(nullptr, "Save Preset", "Preset name:", QLineEdit::Normal, QString::fromStdString(state().preset), &ok).trimmed();
    if (!ok)
        return;
    const json r = run("treeSavePreset", call({{"args", {name.toStdString()}}}));
    if (r.is_string())
    {
        state().preset = r.get<std::string>();
        refresh();
        status("Saved preset '" + state().preset + "'.");
    }
}

void deletePreset()
{
    const std::string name = state().preset;
    if (isBuiltIn(name))
    {
        status("'" + name + "' is a built in preset.");
        return;
    }
    if (QMessageBox::question(nullptr, "Delete Preset", QString::fromStdString("Delete the preset '" + name + "'?")) != QMessageBox::Yes)
        return;
    run("treeDeletePreset", {{"args", {name}}});
    state().preset = builtInPresets().front();
    applyPreset();
}

void randomizeSeed()
{
    for (const char* k : {"trunk.seed", "leaf.seed"})
        state().values[k] = int64_t(QRandomGenerator::global()->bounded(100000));
    refresh();
}

void loadFromSelection()
{
    const json s = run("treeSettings", json::object());
    if (!s.is_object())
    {
        status("Select a tree built by this tool.");
        return;
    }
    State& st = state();
    st.values = s["values"];
    auto join = [](const json& list) {
        std::string out;
        if (list.is_array())
            for (const json& n : list)
                if (n.is_string())
                    out += (out.empty() ? "" : " ") + n.get<std::string>();
        return out;
    };
    st.leaf = join(s["sources"].value("leaf", json()));
    st.canopy = join(s["sources"].value("canopy", json()));
    if (const std::string preset = s["preset"].get<std::string>(); !preset.empty())
        st.preset = preset;
    refresh();
    status("Loaded settings from " + s["tree"].get<std::string>() + ".");
}

void report(const char* what, const json& r)
{
    if (!r.is_null())
        status(std::string(what) + " " + shortText(r) + ".");
}

void rememberExport()
{
    json remembered = json::object();
    for (const auto& [k, v] : state().values.items())
        if (k.starts_with("export."))
            remembered[k] = v;
    PrefStore::get().set(kPrefsFile, "/Export", remembered);
}

void leafPlane()
{
    const json r = run("treeLeafPlane", call());
    if (r.is_string())
    {
        state().leaf = r.get<std::string>();
        refresh();
        status("Made " + state().leaf + ".");
    }
}

void exportPair()
{
    rememberExport();
    const json r = run("treeExport", call());
    if (r.is_object())
        status("Wrote " + r["high"].get<std::string>() + " and " + r["low"].get<std::string>() + ".");
}

void exportLeaf()
{
    rememberExport();
    const json r = run("treeExportLeaf", call());
    if (r.is_object())
        status("Wrote " + r["high"].get<std::string>() + " and " + r["low"].get<std::string>() + ".");
}

void measure()
{
    const json r = run("treeMeasureBake", call());
    if (!r.is_object())
        return;
    std::string text;
    for (const auto& [part, s] : r.items())
        text += (text.empty() ? "" : ", ") + part + " out " + std::to_string(s["out99"].get<double>()).substr(0, 6) + " in " +
                std::to_string(s["in99"].get<double>()).substr(0, 6);
    status(text.empty() ? "Nothing to measure." : "99% rays: " + text + " cm. Full list in the log.");
}

struct ActionDef
{
    const char* id;
    const char* label;
    const char* doc;
    std::function<void()> fn;
};

const std::vector<ActionDef>& actionDefs()
{
    static const std::vector<ActionDef> defs = {
        {"treeGenerator.applyPreset", "Reapply Preset", "Load the current preset's settings, keeping the up axis, style and export block unless the preset sets them.", applyPreset},
        {"treeGenerator.savePreset", "Save Tree Preset As", "Save the current settings as a preset in the preferences.", savePreset},
        {"treeGenerator.deletePreset", "Delete Tree Preset", "Delete the current saved preset.", deletePreset},
        {"treeGenerator.randomizeSeed", "Randomize Tree Seeds", "Pick new trunk and leaf seeds.", randomizeSeed},
        {"treeGenerator.loadFromSelection", "Load Tree From Selection", "Put the selected tree's stored settings and meshes back in the panel.", loadFromSelection},
        {"treeGenerator.stampSelection", "Stamp Tree Settings Onto Selection", "Store the panel's settings on the selected generated nodes.",
         [] { report("Stamped", run("treeStamp", call())); }},
        {"treeGenerator.rebuild", "Rebuild Selected Tree", "Rebuild the selected tree from the settings it carries.", [] { report("Rebuilt as", run("treeRebuild", json::object())); }},
        {"treeGenerator.growTrunk", "Grow Trunk", "Grow a trunk with recursive branches from the trunk settings.", [] { report("Grew", run("treeGrowTrunk", call())); }},
        {"treeGenerator.canopyBlobs", "Blobs At Branch Tips", "Drop a canopy blob on each branch tip of the selected (or last grown) trunk.",
         [] { report("Made", run("treeCanopyBlobs", call())); }},
        {"treeGenerator.scatterLeaves", "Scatter Leaves", "Scatter the leaf mesh over the canopy meshes (or the selection, leaf last).",
         [] { report("Scattered", run("treeScatterLeaves", call())); }},
        {"treeGenerator.leafPlane", "Make Leaf Plane", "Make a segmented leaf card and put it in the Leaf Mesh field.", leafPlane},
        {"treeGenerator.export", "Export Tree FBX Pair", "Write the selected tree as <name>_Lo.fbx and <name>_Hi.fbx with its json.", exportPair},
        {"treeGenerator.exportLeaf", "Export Single Leaf", "Write the leaf mesh on its own as <name>_Leaf_Lo.fbx and _Hi.fbx with its json and opacity map.", exportLeaf},
        {"treeGenerator.measureBake", "Measure Tree Bake Distances", "Report how far the high poly stands off the low poly per part.", measure},
        {"treeGenerator.opacityMap", "Write Leaf Opacity Map", "Rasterise the leaf mesh's UV shells into <name>_opacity.png.", [] { report("Wrote", run("treeOpacityMap", call())); }},
        {"treeGenerator.splitMasks", "Split Leaf Colour Masks", "Split the baked leaf colour map in the export folder into R, G and B masks.",
         [] { report("Wrote", run("treeSplitMasks", call())); }},
        {"treeGenerator.build", "Build Whole Tree", "Trunk, canopy blobs and the leaf scattered over them, in one go.", [] { report("Built", run("treeBuild", call())); }},
    };
    return defs;
}

void trigger(const char* id)
{
    for (const ActionDef& d : actionDefs())
        if (std::string(d.id) == id)
            d.fn();
}

// ------------------------------------------------------------------------------------------------ panel

QPushButton* button(const char* actionId, QWidget* parent)
{
    for (const ActionDef& d : actionDefs())
        if (std::string(d.id) == actionId)
        {
            QString label = d.label;
            auto* b = new QPushButton(label, parent);
            b->setToolTip(d.doc);
            QObject::connect(b, &QPushButton::clicked, b, [id = std::string(actionId)] { trigger(id.c_str()); });
            return b;
        }
    return new QPushButton(actionId, parent);
}

/** A titled section whose body folds away under its header. */
QWidget* section(QVBoxLayout* into, const QString& title, bool collapsed, QFormLayout*& form)
{
    auto* head = new QToolButton;
    head->setText(title);
    head->setCheckable(true);
    head->setChecked(!collapsed);
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
    head->setAutoRaise(true);
    auto* body = new QWidget;
    form = new QFormLayout(body);
    form->setContentsMargins(8, 2, 2, 6);
    body->setVisible(!collapsed);
    QObject::connect(head, &QToolButton::toggled, body, [head, body](bool on) {
        body->setVisible(on);
        head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    into->addWidget(head);
    into->addWidget(body);
    return body;
}

QDoubleSpinBox* spin(double lo, double hi, int decimals)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setKeyboardTracking(false);
    return s;
}

/** The editor for one schema field, writing into the state and registering how to show the state again. */
QWidget* editor(const Field& f, std::vector<std::function<void()>>& shows)
{
    const std::string key = f.key();
    const double unit = (f.kind == Kind::Length || f.kind == Kind::LengthRange) ? kUnitsPerMetre : 1.0;
    auto values = [] () -> json& { return state().values; };
    switch (f.kind)
    {
    case Kind::Range:
    case Kind::LengthRange:
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        QDoubleSpinBox* a = spin(f.fieldMin * unit, f.fieldMax * unit, 3);
        QDoubleSpinBox* b = spin(f.fieldMin * unit, f.fieldMax * unit, 3);
        h->addWidget(a);
        h->addWidget(b);
        auto write = [key, a, b, unit, values] { values()[key] = json::array({a->value() / unit, b->value() / unit}); };
        QObject::connect(a, &QDoubleSpinBox::valueChanged, w, write);
        QObject::connect(b, &QDoubleSpinBox::valueChanged, w, write);
        shows.push_back([key, a, b, unit, values] {
            const QSignalBlocker ba(a), bb(b);
            a->setValue(values()[key][0].get<double>() * unit);
            b->setValue(values()[key][1].get<double>() * unit);
        });
        return w;
    }
    case Kind::Length:
    case Kind::Float:
    {
        QDoubleSpinBox* s = spin(f.fieldMin * unit, f.fieldMax * unit, 3);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [key, unit, values](double v) { values()[key] = v / unit; });
        shows.push_back([key, s, unit, values] {
            const QSignalBlocker b(s);
            s->setValue(values()[key].get<double>() * unit);
        });
        return s;
    }
    case Kind::Int:
    {
        auto* s = new QSpinBox;
        s->setRange(int(f.fieldMin), int(f.fieldMax));
        s->setKeyboardTracking(false);
        QObject::connect(s, &QSpinBox::valueChanged, s, [key, values](int v) { values()[key] = int64_t(v); });
        shows.push_back([key, s, values] {
            const QSignalBlocker b(s);
            s->setValue(int(values()[key].get<int64_t>()));
        });
        return s;
    }
    case Kind::Bool:
    {
        auto* c = new QCheckBox;
        QObject::connect(c, &QCheckBox::toggled, c, [key, values](bool on) { values()[key] = on; });
        shows.push_back([key, c, values] {
            const QSignalBlocker b(c);
            c->setChecked(values()[key].get<bool>());
        });
        return c;
    }
    case Kind::Slider:
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        QDoubleSpinBox* s = spin(f.fieldMin, f.fieldMax, 2);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 1000);
        h->addWidget(s);
        h->addWidget(slider, 1);
        const double lo = f.sliderMin, hi = f.sliderMax;
        QObject::connect(slider, &QSlider::valueChanged, w, [s, lo, hi](int v) { s->setValue(lo + (hi - lo) * v / 1000.0); });
        QObject::connect(s, &QDoubleSpinBox::valueChanged, w, [key, slider, lo, hi, values](double v) {
            values()[key] = v;
            const QSignalBlocker b(slider);
            slider->setValue(int(std::lround(std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 1000.0)));
        });
        shows.push_back([key, s, slider, lo, hi, values] {
            const QSignalBlocker b(s), bs(slider);
            const double v = values()[key].get<double>();
            s->setValue(v);
            slider->setValue(int(std::lround(std::clamp((v - lo) / (hi - lo), 0.0, 1.0) * 1000.0)));
        });
        return w;
    }
    case Kind::String:
    case Kind::Folder:
    {
        auto* line = new QLineEdit;
        QObject::connect(line, &QLineEdit::editingFinished, line, [key, line, values] { values()[key] = line->text().trimmed().toStdString(); });
        shows.push_back([key, line, values] {
            const QSignalBlocker b(line);
            line->setText(QString::fromStdString(values()[key].get<std::string>()));
        });
        if (f.kind == Kind::String)
            return line;
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(line, 1);
        auto* browse = new QPushButton("...");
        browse->setToolTip("Browse for the export folder.");
        h->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, w, [key, line, values] {
            const QString folder = ui::fileDialog::getExistingDirectory(nullptr, "Export Folder", line->text());
            if (folder.isEmpty())
                return;
            line->setText(folder);
            values()[key] = folder.toStdString();
        });
        return w;
    }
    case Kind::Enum:
    {
        auto* c = new QComboBox;
        for (const std::string& o : f.options)
            c->addItem(QString::fromStdString(o));
        QObject::connect(c, &QComboBox::currentTextChanged, c, [key, values](const QString& t) { values()[key] = t.toStdString(); });
        shows.push_back([key, c, values] {
            const QSignalBlocker b(c);
            c->setCurrentText(QString::fromStdString(values()[key].get<std::string>()));
        });
        return c;
    }
    }
    return new QWidget;
}

void addFields(QFormLayout* form, const std::string& sectionId, std::vector<std::function<void()>>& shows)
{
    for (const Field& f : fields())
        if (f.section == sectionId)
        {
            QWidget* e = editor(f, shows);
            const QString label = QString::fromStdString(f.label);
            if (f.kind == Kind::Length || f.kind == Kind::LengthRange)
                e->setToolTip("In cm.");
            form->addRow(label, e);
        }
}

/** A text field filled from the selection by its Set button. */
QWidget* meshField(std::string State::*member, std::vector<std::function<void()>>& shows)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    auto* line = new QLineEdit;
    auto* set = new QPushButton("Set");
    set->setToolTip("Fill from the selection.");
    h->addWidget(line, 1);
    h->addWidget(set);
    QObject::connect(line, &QLineEdit::editingFinished, line, [member, line] { state().*member = line->text().trimmed().toStdString(); });
    QObject::connect(set, &QPushButton::clicked, w, [member, line] {
        scene::Graph& g = scene::Graph::current();
        std::string names;
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id))
                names += (names.empty() ? "" : " ") + n->path();
        if (names.empty())
            return;
        state().*member = names;
        line->setText(QString::fromStdString(names));
    });
    shows.push_back([member, line] {
        const QSignalBlocker b(line);
        line->setText(QString::fromStdString(state().*member));
    });
    return w;
}

QWidget* makePanel(QWidget* parent)
{
    auto* outer = new QWidget(parent);
    auto* outerLayout = new QVBoxLayout(outer);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(outer);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setSpacing(2);
    auto shows = std::make_shared<std::vector<std::function<void()>>>();

    QFormLayout* form = nullptr;
    section(v, "Preset", false, form);
    auto* presetMenu = new QComboBox;
    form->addRow("Preset", presetMenu);
    auto* presetButtons = new QWidget;
    auto* pb = new QHBoxLayout(presetButtons);
    pb->setContentsMargins(0, 0, 0, 0);
    pb->addWidget(button("treeGenerator.applyPreset", presetButtons));
    pb->addWidget(button("treeGenerator.savePreset", presetButtons));
    pb->addWidget(button("treeGenerator.deletePreset", presetButtons));
    form->addRow(presetButtons);
    auto* selectionButtons = new QWidget;
    auto* sb = new QHBoxLayout(selectionButtons);
    sb->setContentsMargins(0, 0, 0, 0);
    for (const char* id : {"treeGenerator.randomizeSeed", "treeGenerator.loadFromSelection", "treeGenerator.stampSelection", "treeGenerator.rebuild"})
        sb->addWidget(button(id, selectionButtons));
    form->addRow(selectionButtons);
    auto fillPresets = [presetMenu] {
        const QSignalBlocker b(presetMenu);
        presetMenu->clear();
        for (const std::string& n : presetNames(userPresets()))
            presetMenu->addItem(QString::fromStdString(n));
        presetMenu->setCurrentText(QString::fromStdString(state().preset));
    };
    shows->push_back(fillPresets);
    QObject::connect(presetMenu, &QComboBox::currentTextChanged, presetMenu, [](const QString& t) {
        state().preset = t.toStdString();
        applyPreset();
    });

    auto* globals = new QWidget;
    auto* globalForm = new QFormLayout(globals);
    globalForm->setContentsMargins(8, 2, 2, 2);
    addFields(globalForm, "global", *shows);
    v->addWidget(globals);
    auto* note = new QLabel("Sizes are in cm. Presets assume a leaf cluster mesh.");
    note->setWordWrap(true);
    v->addWidget(note);

    section(v, "Trunk And Branches", false, form);
    addFields(form, "trunk", *shows);
    form->addRow(button("treeGenerator.growTrunk", nullptr));

    section(v, "Canopy Blobs", false, form);
    addFields(form, "blob", *shows);
    form->addRow(button("treeGenerator.canopyBlobs", nullptr));

    section(v, "Leaves", false, form);
    form->addRow("Leaf Mesh(es), Hi First", meshField(&State::leaf, *shows));
    form->addRow("Canopy Mesh(es)", meshField(&State::canopy, *shows));
    addFields(form, "leaf", *shows);
    form->addRow(button("treeGenerator.scatterLeaves", nullptr));

    section(v, "Leaf Plane", true, form);
    addFields(form, "plane", *shows);
    form->addRow(button("treeGenerator.leafPlane", nullptr));

    section(v, "Export", true, form);
    addFields(form, "export", *shows);
    for (const char* id : {"treeGenerator.export", "treeGenerator.exportLeaf", "treeGenerator.measureBake", "treeGenerator.opacityMap", "treeGenerator.splitMasks"})
        form->addRow(button(id, nullptr));
    v->addStretch(1);
    scroll->setWidget(body);
    outerLayout->addWidget(scroll, 1);

    auto* footer = new QVBoxLayout;
    footer->setContentsMargins(6, 4, 6, 6);
    auto* statusLine = new QLabel("Ready.");
    statusLine->setWordWrap(true);
    footer->addWidget(statusLine);
    QPushButton* build = button("treeGenerator.build", outer);
    build->setMinimumHeight(32);
    footer->addWidget(build);
    outerLayout->addLayout(footer);

    QPointer<QLabel> statusGuard(statusLine);
    gStatus = [statusGuard](const QString& t) {
        if (statusGuard)
            statusGuard->setText(t);
    };
    gRefresh = [shows] {
        for (const auto& show : *shows)
            show();
    };
    refresh();
    QObject::connect(outer, &QObject::destroyed, [] {
        gRefresh = nullptr;
        gStatus = nullptr;
    });
    return outer;
}
} // namespace

void registerUi(ext::Context& ctx)
{
    const char* cat = "Foliage";
    for (const ActionDef& d : actionDefs())
        actions::add(ctx, {.id = d.id, .label = d.label, .category = cat, .run = d.fn, .doc = d.doc});

    panels::add(ctx, {.id = "treeGenerator", .title = "Tree Generator", .create = makePanel, .icon = "tree", .open = false});
    levelToolkit::add(ctx, {.id = "foliage.trees",
                            .title = "Trees",
                            .items = {{"window.treeGenerator", "tree", "Generator"}, {"treeGenerator.growTrunk", "git-branch", "Trunk"},
                                      {"treeGenerator.scatterLeaves", "leaf", "Leaves"}, {"treeGenerator.rebuild", "refresh", "Rebuild"},
                                      {"treeGenerator.export", "file-export", "Export"}},
                            .order = 60});

    menus::addItem(ctx, {.id = "treeGenerator.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Tree Generator"});
    int separators = 0;
    for (const char* a : {"window.treeGenerator", "", "treeGenerator.build", "treeGenerator.growTrunk", "treeGenerator.canopyBlobs", "treeGenerator.scatterLeaves",
                          "treeGenerator.leafPlane", "treeGenerator.rebuild", "", "treeGenerator.export", "treeGenerator.exportLeaf", "treeGenerator.measureBake",
                          "treeGenerator.opacityMap", "treeGenerator.splitMasks"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("treeGenerator.menu.") + a, .menu = "Level/Tree Generator", .action = a});
        else
            menus::addItem(ctx, {.id = "treeGenerator.menu.sep" + std::to_string(separators++), .menu = "Level/Tree Generator", .kind = menus::Kind::Separator});
    }
}
} // namespace treegen
