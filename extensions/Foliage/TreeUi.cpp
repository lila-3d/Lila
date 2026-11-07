// The Tree Generator panel: the schema's fields by section, a button per step, and a bindable action behind every
// button. The panel edits one shared state, so the actions run from hotkeys and menus with the panel closed too.

#include "TreeGenerator.h"
#include "TreeSchema.h"
#include "lila/FileDialog.h"

#include "PanelKit.h"

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
#include <QRandomGenerator>
#include <QScrollArea>
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

/** A push button running the action, labelled `label` or the action's own label. */
QToolButton* button(const char* actionId, QWidget* parent, const QString& label = {})
{
    return panelkit::push(actionId, parent, label);
}

/** A titled section whose form body folds away under its header. */
QWidget* treeSection(QVBoxLayout* into, QWidget* parent, const QString& title, bool collapsed, QFormLayout*& form)
{
    auto* body = new QWidget;
    form = new QFormLayout(body);
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setVerticalSpacing(4);
    into->addWidget(panelkit::section(title, body, parent, "treeGenerator", !collapsed));
    return body;
}

/** "packed" -> "Packed": a stored choice as the panel shows it. */
QString treeChoiceName(const std::string& value)
{
    QString s = QString::fromStdString(value);
    if (!s.isEmpty())
        s[0] = s[0].toUpper();
    return s;
}

/** The editor for one schema field, writing into the state and registering how to show the state again. */
QWidget* editor(const Field& f, std::vector<std::function<void()>>& shows)
{
    const std::string key = f.key();
    const bool length = f.kind == Kind::Length || f.kind == Kind::LengthRange;
    const double unit = length ? kUnitsPerMetre : 1.0;
    const QString suffix = length ? "cm" : "";
    auto values = [] () -> json& { return state().values; };
    switch (f.kind)
    {
    case Kind::Range:
    case Kind::LengthRange:
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        QDoubleSpinBox* a = panelkit::valueField(f.fieldMin * unit, f.fieldMax * unit, 3, suffix, w);
        QDoubleSpinBox* b = panelkit::valueField(f.fieldMin * unit, f.fieldMax * unit, 3, suffix, w);
        a->setToolTip("Low");
        b->setToolTip("High");
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
        QDoubleSpinBox* s = panelkit::valueField(f.fieldMin * unit, f.fieldMax * unit, 3, suffix, nullptr);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [key, unit, values](double v) { values()[key] = v / unit; });
        shows.push_back([key, s, unit, values] {
            const QSignalBlocker b(s);
            s->setValue(values()[key].get<double>() * unit);
        });
        return s;
    }
    case Kind::Int:
    {
        QDoubleSpinBox* s = panelkit::valueField(f.fieldMin, f.fieldMax, 0, {}, nullptr);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [key, values](double v) { values()[key] = int64_t(std::llround(v)); });
        shows.push_back([key, s, values] {
            const QSignalBlocker b(s);
            s->setValue(double(values()[key].get<int64_t>()));
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
        auto* s = new QDoubleSpinBox;
        s->setRange(f.fieldMin, f.fieldMax);
        s->setDecimals(2);
        s->setSingleStep((f.sliderMax - f.sliderMin) / 100.0);
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [key, values](double v) { values()[key] = v; });
        shows.push_back([key, s, values] {
            const QSignalBlocker b(s);
            s->setValue(values()[key].get<double>());
        });
        return s;
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
        h->setSpacing(2);
        h->addWidget(line, 1);
        auto* browse = new QToolButton(w);
        browse->setText("Browse");
        browse->setToolTip("Browse: pick the export folder.");
        panelkit::setRole(browse, "push");
        h->addWidget(browse);
        QObject::connect(browse, &QToolButton::clicked, w, [key, line, values] {
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
            c->addItem(treeChoiceName(o), QString::fromStdString(o));
        QObject::connect(c, &QComboBox::currentIndexChanged, c, [key, c, values](int) { values()[key] = c->currentData().toString().toStdString(); });
        shows.push_back([key, c, values] {
            const QSignalBlocker b(c);
            c->setCurrentIndex(std::max(0, c->findData(QString::fromStdString(values()[key].get<std::string>()))));
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
            form->addRow(label, e);
        }
}

/** A text field filled from the selection by its Set button. */
QWidget* meshField(std::string State::*member, std::vector<std::function<void()>>& shows)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    auto* line = new QLineEdit;
    line->setPlaceholderText("None");
    auto* set = new QToolButton(w);
    set->setText("Set");
    set->setToolTip("Set: fill from the selection.");
    panelkit::setRole(set, "push");
    h->addWidget(line, 1);
    h->addWidget(set);
    QObject::connect(line, &QLineEdit::editingFinished, line, [member, line] { state().*member = line->text().trimmed().toStdString(); });
    QObject::connect(set, &QToolButton::clicked, w, [member, line] {
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
    v->setContentsMargins(12, 8, 12, 8);
    v->setSpacing(4);
    auto shows = std::make_shared<std::vector<std::function<void()>>>();

    QFormLayout* form = nullptr;
    QWidget* presetBody = treeSection(v, body, "Preset", false, form);
    auto* presetMenu = new QComboBox;
    presetMenu->setToolTip("Preset: settings to start from. Presets assume a leaf cluster mesh.");
    form->addRow("Preset", presetMenu);
    form->addRow("", panelkit::grid({button("treeGenerator.applyPreset", presetBody, "Reapply"), button("treeGenerator.savePreset", presetBody, "Save As"),
                                     button("treeGenerator.randomizeSeed", presetBody, "New Seeds"),
                                     button("treeGenerator.loadFromSelection", presetBody, "Load Selection")},
                                    2, presetBody));
    form->addRow("", panelkit::overflow({"treeGenerator.stampSelection", "treeGenerator.rebuild", "treeGenerator.deletePreset"}, presetBody));
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
    addFields(form, "global", *shows);

    QWidget* section = treeSection(v, body, "Trunk and Branches", false, form);
    addFields(form, "trunk", *shows);
    form->addRow("", button("treeGenerator.growTrunk", section));

    section = treeSection(v, body, "Canopy Blobs", false, form);
    addFields(form, "blob", *shows);
    form->addRow("", button("treeGenerator.canopyBlobs", section));

    section = treeSection(v, body, "Leaves", false, form);
    form->addRow("Leaf Meshes", meshField(&State::leaf, *shows));
    form->itemAt(form->rowCount() - 1, QFormLayout::FieldRole)->widget()->setToolTip("Leaf Meshes: the leaf mesh or meshes, high poly first");
    form->addRow("Canopy Meshes", meshField(&State::canopy, *shows));
    addFields(form, "leaf", *shows);
    form->addRow("", button("treeGenerator.scatterLeaves", section));

    section = treeSection(v, body, "Leaf Plane", true, form);
    addFields(form, "plane", *shows);
    form->addRow("", button("treeGenerator.leafPlane", section));

    section = treeSection(v, body, "Export", true, form);
    addFields(form, "export", *shows);
    form->addRow("", button("treeGenerator.export", section, "Export FBX Pair"));
    form->addRow("", panelkit::overflow({"treeGenerator.exportLeaf", "treeGenerator.measureBake", "treeGenerator.opacityMap", "treeGenerator.splitMasks"},
                                        section));
    v->addStretch(1);
    scroll->setWidget(body);
    outerLayout->addWidget(scroll, 1);

    auto* footer = new QVBoxLayout;
    footer->setContentsMargins(12, 4, 12, 8);
    footer->setSpacing(4);
    auto* statusLine = new QLabel;
    statusLine->setWordWrap(true);
    statusLine->setEnabled(false);
    footer->addWidget(statusLine);
    footer->addWidget(button("treeGenerator.build", outer, "Build Whole Tree"));
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
