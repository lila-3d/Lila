// The Terrain Generator panel: presets, the source list, every setting grouped as the schema sections them, and the
// generate, bake plane, ID map and export buttons, each also an action for menus, hotkeys and shelves.

#include "TerrainGenerator.h"

#include <lila/Command.h>
#include <lila/Events.h>
#include <lila/Graph.h>
#include <lila/Icons.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/UI.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <map>
#include <random>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace terrain
{
namespace
{
constexpr const char* kPanelId = "terrainGenerator";
constexpr int kSliderSteps = 1000;

QString qs(const std::string& s) { return QString::fromStdString(s); }

json run(const char* name, const json& call = json::object(), bool echo = true, std::string* error = nullptr)
{
    try
    {
        return ops::execute(name, call, echo ? CallSource::UI : CallSource::Script);
    }
    catch (const std::exception& e)
    {
        if (error)
            *error = e.what();
        log::write(LogLevel::Error, e.what());
        return nullptr;
    }
}

/** A titled block whose contents fold away under its header. */
struct Section
{
    QWidget* root = nullptr;
    QWidget* body = nullptr;
    QVBoxLayout* layout = nullptr;
};

Section makeSection(QWidget* parent, const QString& title, bool collapsed)
{
    Section s;
    s.root = new QWidget(parent);
    auto* v = new QVBoxLayout(s.root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton(s.root);
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(!collapsed);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(collapsed ? Qt::RightArrow : Qt::DownArrow);
    header->setAutoRaise(true);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont f = header->font();
    f.setBold(true);
    header->setFont(f);
    s.body = new QWidget(s.root);
    s.layout = new QVBoxLayout(s.body);
    s.layout->setContentsMargins(8, 2, 4, 4);
    s.layout->setSpacing(2);
    s.body->setVisible(!collapsed);
    QObject::connect(header, &QToolButton::toggled, s.body, [header, body = s.body](bool open) {
        body->setVisible(open);
        header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    v->addWidget(header);
    v->addWidget(s.body);
    return s;
}

QString swatchStyle(const glm::dvec3& c)
{
    // The ID colour is data, so the swatch shows it as is.
    return QString("QToolButton { background-color: rgb(%1, %2, %3); border: 1px solid palette(mid); }")
        .arg(int(std::lround(std::clamp(c.r, 0.0, 1.0) * 255.0)))
        .arg(int(std::lround(std::clamp(c.g, 0.0, 1.0) * 255.0)))
        .arg(int(std::lround(std::clamp(c.b, 0.0, 1.0) * 255.0)));
}

/** One setting's editor: a field, plus a slider for numbers, kept in step. */
struct Control
{
    const Param* param = nullptr;
    QWidget* row = nullptr;
    QDoubleSpinBox* real = nullptr;
    QSpinBox* integer = nullptr;
    QSlider* slider = nullptr;
    QCheckBox* check = nullptr;
    QComboBox* combo = nullptr;
};

class Panel : public QWidget
{
public:
    explicit Panel(QWidget* parent) : QWidget(parent)
    {
        mPrefs = loadPrefs();
        mAuto["name"] = mPrefs["Export"].value("autoName", true);
        mAuto["material"] = mPrefs["Export"].value("autoMaterial", true);
        mPreset = mPrefs["_default"].get<std::string>();
        if (!hasPreset(mPreset))
            mPreset = defaultPreset();
        build();
        applyValues(presetSettings(mPreset, mPrefs["Presets"]));
        applyPresetNaming();
        mSelectionSub = events::subscribe(events::Event::SelectionChanged, [this](const events::EventData&) { onSelectionChanged(); }, "TerrainGenerator");
    }

    ~Panel() override { events::unsubscribe(mSelectionSub); }

    // ------------------------------------------------------------------------------------------------ actions

    void generate()
    {
        if (mSources.empty())
        {
            status("Add at least one source mesh before generating.");
            return;
        }
        report(run("terrainGenerate", generateCall()));
    }

    void regenerate()
    {
        const json s = run("terrainSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated terrain group to regenerate.");
            return;
        }
        json call = generateCall();
        call["args"] = {s["group"]};
        report(run("terrainRegenerate", call));
    }

    void deleteSelected()
    {
        const json name = run("terrainDelete");
        status(name.is_string() ? "Deleted " + name.get<std::string>() + "." : "Select a generated terrain group to delete.");
    }

    void createBakePlane()
    {
        const json s = run("terrainSettings", json::object(), false);
        json call = {{"values", readValues()}, {"preset", mPreset}};
        if (s.is_object())
            call["args"] = {s["group"]};
        const json plane = run("terrainCreateBakePlane", call);
        if (plane.is_string())
            status("Created " + plane.get<std::string>() + ".");
    }

    void bakeIdMap()
    {
        const json s = run("terrainSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated terrain group to bake.");
            return;
        }
        if (!s.contains("bakePlane"))
        {
            status(s["group"].get<std::string>() + " has no bake plane. Click Create Bake Plane first.");
            return;
        }
        const json exp = exportSettings();
        std::string target;
        if (!exp["path"].get<std::string>().empty() && !exp["name"].get<std::string>().empty())
            target = exp["path"].get<std::string>() + "/" + exp["name"].get<std::string>() + kIdMapSuffix + ".png";
        else
        {
            const QString chosen = QFileDialog::getSaveFileName(this, "Bake ID Map", QString(), "PNG (*.png);;TIFF (*.tif);;Targa (*.tga)");
            if (chosen.isEmpty())
                return;
            target = chosen.toStdString();
        }
        std::string error;
        const json path = run("terrainBakeIdMap", {{"args", {s["group"]}}, {"path", target}, {"resolution", mControls["bakeResolution"].integer->value()}}, true, &error);
        status(path.is_string() ? "Baked ID map to " + path.get<std::string>() + "." : error);
    }

    void exportBakes()
    {
        const json s = run("terrainSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated terrain group to export.");
            return;
        }
        const json exp = exportSettings();
        std::string error;
        const json r = run("terrainExport", {{"args", {s["group"]}}, {"path", exp["path"]}, {"name", exp["name"]}, {"material", exp["material"]}}, true, &error);
        if (!r.is_object())
        {
            status(error);
            return;
        }
        status("Exported " + r["high"].get<std::string>() + (r["low"].is_string() ? " and " + r["low"].get<std::string>() : " (no bake plane)") + ".");
    }

    void measureHeight()
    {
        std::string error;
        const json r = run("terrainMeasureHeight", json::object(), true, &error);
        if (!r.is_object())
        {
            status(error);
            return;
        }
        const json& b = r["bake"];
        char buf[256];
        std::snprintf(buf, sizeof buf, "Cage %.2f / %.2f, height outer %.2f inner %.2f. Full list in the script editor.", b["cageMax"].get<double>(),
                      b["cageMin"].get<double>(), b["heightOuter"].get<double>(), b["heightInner"].get<double>());
        status(buf);
    }

    void splitMasks()
    {
        const json exp = exportSettings();
        const json found = run("terrainFindVertexMap", {{"args", {exp["path"]}}, {"name", exp["name"]}}, false);
        if (!found.is_string())
        {
            status("No vertex colour map in " + exp["path"].get<std::string>() + ". Bake one first.");
            return;
        }
        const json written = run("terrainSplitMasks", {{"args", {found}}});
        status("Split " + found.get<std::string>() + " into " + std::to_string(written.is_array() ? written.size() : 0) + " mask(s).");
    }

    void addSelectedSources()
    {
        Graph& g = Graph::current();
        if (g.selection().empty())
        {
            status("Nothing is selected. Select the meshes you want to scatter.");
            return;
        }
        int added = 0;
        for (NodeId id : g.selection())
        {
            Node* n = g.node(id);
            if (!n)
                continue;
            const std::string name = n->path();
            if (std::any_of(mSources.begin(), mSources.end(), [&](const json& s) { return s["name"] == name; }))
                continue;
            if (!n->type().isA("mesh"))
            {
                log::warn("Terrain Generator: '{}' is not a polygon mesh, skipping.", name);
                continue;
            }
            mSources.push_back({{"name", name}, {"weight", 1.0}, {"objectId", 0}, {"heightOffset", 0.0}, {"enabled", true}, {"lengthAxis", "Auto"}, {"baseEnd", "Auto"}});
            ++added;
        }
        if (added)
        {
            rebuildSources();
            status("Added " + std::to_string(added) + " source mesh(es).");
        }
    }

    void randomizeSeed()
    {
        static std::mt19937 rng{std::random_device{}()};
        writeValue("seed", int64_t(std::uniform_int_distribution<int>(0, 999999)(rng)));
    }

    void loadFromSelection()
    {
        const json s = run("terrainSettings", json::object(), false);
        if (!s.is_object())
        {
            status("Select a generated terrain group first.");
            return;
        }
        loadGroup(s);
    }

private:
    // ------------------------------------------------------------------------------------------------ building

    void build()
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(4, 4, 4, 4);

        auto* bar = new QHBoxLayout;
        mPresetMenu = new QComboBox(this);
        mPresetMenu->setToolTip("Preset");
        bar->addWidget(mPresetMenu, 1);
        auto tool = [&](const char* iconName, const char* tip, std::function<void()> fn) {
            auto* b = new QToolButton(this);
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            bar->addWidget(b);
            return b;
        };
        tool("device-floppy", "Save Current Settings As Preset", [this] { savePreset(); });
        tool("trash", "Delete Preset", [this] { deletePreset(); });
        tool("star", "Set Default Preset", [this] {
            run("terrainSetDefaultPreset", {{"args", {mPreset}}}, false);
            mPrefs = loadPrefs();
            status("Default preset set to " + mPreset + ".");
        });
        tool("arrow-back-up", "Reset To Preset", [this] {
            applyValues(presetSettings(mPreset, mPrefs["Presets"]));
            status("Reset to '" + mPreset + "'.");
        });
        tool("download", "Load Settings From Selected Group", [this] { loadFromSelection(); });
        QToolButton* settingsButton = tool("settings", "Settings", [] {});
        auto* settingsMenu = new QMenu(settingsButton);
        QAction* autoLoad = settingsMenu->addAction("Load Settings From Selected Group");
        autoLoad->setCheckable(true);
        autoLoad->setChecked(mPrefs["Window"].value("autoLoadSelection", true));
        connect(autoLoad, &QAction::toggled, this, [this](bool on) {
            mPrefs["Window"]["autoLoadSelection"] = on;
            savePrefs(mPrefs);
        });
        settingsButton->setMenu(settingsMenu);
        settingsButton->setPopupMode(QToolButton::InstantPopup);
        tool("info-circle", "About", [this] {
            QMessageBox::about(this, "Terrain Generator",
                               QString("Terrain Generator v%1\nScatters tiling terrain for baking to seamless textures.\nCreated by: Jared Taylor").arg(kVersion));
        });
        v->addLayout(bar);
        connect(mPresetMenu, &QComboBox::activated, this, [this](int i) { changePreset(mPresetMenu->itemData(i).toString().toStdString()); });

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget(scroll);
        mBody = new QVBoxLayout(body);
        mBody->setContentsMargins(0, 0, 0, 0);
        mBody->setSpacing(2);
        scroll->setWidget(body);
        v->addWidget(scroll, 1);

        buildSources(body);
        for (const auto& [id, label] : sections())
            buildSection(body, id, label);
        buildExport(body);
        mBody->addStretch(1);

        mStatus = new QLabel("Ready.", this);
        mStatus->setWordWrap(true);
        v->addWidget(mStatus);
        auto* row = new QHBoxLayout;
        auto button = [&](QBoxLayout* into, const char* label, const char* tip, std::function<void()> fn, int stretch) {
            auto* b = new QPushButton(label, this);
            b->setToolTip(tip);
            connect(b, &QPushButton::clicked, this, std::move(fn));
            into->addWidget(b, stretch);
            return b;
        };
        button(row, "Generate", "Scatter a new tiling terrain group using the current settings.", [this] { generate(); }, 1)->setMinimumHeight(30);
        button(row, "Regenerate", "Delete the selected generated group and rebuild it with the current settings.", [this] { regenerate(); }, 0)->setMinimumHeight(30);
        button(row, "Delete", "Delete the selected generated group.", [this] { deleteSelected(); }, 0)->setMinimumHeight(30);
        v->addLayout(row);
        auto* bakeRow = new QHBoxLayout;
        button(bakeRow, "Create Bake Plane",
               "Make a plane matching the tile size, with 0-1 UVs, to bake the terrain onto. Parented into the selected generated group when there is one.",
               [this] { createBakePlane(); }, 1);
        button(bakeRow, "Bake ID Map", "Bake the Object ID colours of the selected generated group down onto its bake plane, as a flat ID map.",
               [this] { bakeIdMap(); }, 0);
        v->addLayout(bakeRow);
        updatePresetMenu();
    }

    void buildSources(QWidget* body)
    {
        Section s = makeSection(body, "Source Meshes", false);
        for (const char* line : {"Pivot at the base. Longest axis is the length, thinnest is the bend direction.",
                                 "Right click a source to override what was detected.",
                                 "Weight, Object ID, up axis offset. Sources sharing an ID bake as one colour."})
        {
            auto* l = new QLabel(line, s.body);
            l->setWordWrap(true);
            QFont f = l->font();
            f.setItalic(true);
            l->setFont(f);
            s.layout->addWidget(l);
        }
        auto* row = new QHBoxLayout;
        auto* add = new QPushButton("Add Selected", s.body);
        add->setToolTip("Add every selected polygon mesh as a scatter source.");
        auto* clear = new QPushButton("Remove All", s.body);
        clear->setToolTip("Clear the source list.");
        connect(add, &QPushButton::clicked, this, [this] { addSelectedSources(); });
        connect(clear, &QPushButton::clicked, this, [this] {
            mSources.clear();
            rebuildSources();
        });
        row->addStretch(1);
        row->addWidget(add);
        row->addWidget(clear);
        s.layout->addLayout(row);
        mSourceHolder = new QWidget(s.body);
        mSourceRows = new QVBoxLayout(mSourceHolder);
        mSourceRows->setContentsMargins(0, 0, 0, 0);
        mSourceRows->setSpacing(1);
        s.layout->addWidget(mSourceHolder);
        mBody->addWidget(s.root);
        rebuildSources();
    }

    void rebuildSources()
    {
        while (QLayoutItem* item = mSourceRows->takeAt(0))
        {
            if (QWidget* w = item->widget())
                w->deleteLater();
            delete item;
        }
        if (mSources.empty())
        {
            auto* l = new QLabel("No sources. Select your meshes and click Add Selected.", mSourceHolder);
            l->setAlignment(Qt::AlignCenter);
            mSourceRows->addWidget(l);
            return;
        }
        for (size_t i = 0; i < mSources.size(); ++i)
        {
            json& src = mSources[i];
            auto* w = new QWidget(mSourceHolder);
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            auto* on = new QCheckBox(w);
            on->setChecked(src.value("enabled", true));
            on->setToolTip("Include this source in the scatter.");
            connect(on, &QCheckBox::toggled, this, [this, i](bool v) { mSources[i]["enabled"] = v; });
            h->addWidget(on);

            const json info = run("terrainSourceInfo", {{"args", {src["name"]}}, {"lengthAxis", src.value("lengthAxis", "Auto")}, {"baseEnd", src.value("baseEnd", "Auto")}}, false);
            const bool valid = info.is_object() && info.value("valid", false);
            const std::string name = src.value("name", "");
            auto* label = new QLabel(qs(valid ? name + "   [" + info["describe"].get<std::string>() + "]" : name + "  (missing)"), w);
            label->setToolTip(valid ? qs(name + "\nLength " + info["length"].get<std::string>() + ", width " + info["width"].get<std::string>() +
                                         ", thickness " + info["thickness"].get<std::string>() + ".\nBend, twist and sine pivot from the " +
                                         (info["baseSign"].get<double>() > 0 ? "low" : "high") + " end.\nRight click to override.")
                                    : qs(name + " is not in this scene."));
            label->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(label, &QLabel::customContextMenuRequested, this, [this, i, label](const QPoint& at) { sourceMenu(i, label->mapToGlobal(at)); });
            h->addWidget(label, 1);

            auto* weight = new QDoubleSpinBox(w);
            weight->setRange(0.0, 1e6);
            weight->setDecimals(4);
            weight->setSingleStep(0.01);
            weight->setValue(src.value("weight", 1.0));
            weight->setToolTip("Relative chance this source is picked, against the other weights.");
            weight->setFixedWidth(70);
            connect(weight, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { mSources[i]["weight"] = std::max(0.0, v); });
            h->addWidget(weight);

            auto* id = new QSpinBox(w);
            id->setRange(0, 9999);
            id->setValue(int(src.value("objectId", 0)));
            id->setToolTip("Object ID. Sources sharing an ID are one material and one colour in the baked ID map, so related meshes can be masked together.");
            id->setFixedWidth(44);
            connect(id, &QSpinBox::editingFinished, this, [this, i, id] {
                mSources[i]["objectId"] = std::max(0, id->value());
                QTimer::singleShot(0, this, [this] { rebuildSources(); });
            });
            h->addWidget(id);

            auto* offset = new QDoubleSpinBox(w);
            offset->setRange(-1e5, 1e5);
            offset->setDecimals(2);
            offset->setSingleStep(0.1);
            offset->setValue(src.value("heightOffset", 0.0));
            offset->setToolTip("Offset along the up axis for this source only, on top of its height layer. Lift a source clear of the others so nothing scatters over it.");
            offset->setFixedWidth(60);
            connect(offset, &QDoubleSpinBox::valueChanged, this, [this, i](double v) { mSources[i]["heightOffset"] = v; });
            h->addWidget(offset);

            auto* swatch = new QToolButton(w);
            swatch->setFixedSize(22, 18);
            swatch->setStyleSheet(swatchStyle(idColour(src.value("objectId", 0), mPrefs["Palette"])));
            swatch->setToolTip("Colour this Object ID bakes as. Click to change it.");
            connect(swatch, &QToolButton::clicked, this, [this, i] { editIdColour(mSources[i].value("objectId", 0)); });
            h->addWidget(swatch);

            auto* remove = new QToolButton(w);
            remove->setIcon(icon("x"));
            remove->setAutoRaise(true);
            remove->setToolTip("Remove this source.");
            connect(remove, &QToolButton::clicked, this, [this, i] {
                mSources.erase(mSources.begin() + std::ptrdiff_t(i));
                QTimer::singleShot(0, this, [this] { rebuildSources(); });
            });
            h->addWidget(remove);
            mSourceRows->addWidget(w);
        }
    }

    void sourceMenu(size_t i, const QPoint& at)
    {
        QMenu menu(this);
        auto choices = [&](const char* title, const char* key, std::initializer_list<const char*> options) {
            QMenu* sub = menu.addMenu(title);
            for (const char* c : options)
            {
                QAction* a = sub->addAction(c);
                a->setCheckable(true);
                a->setChecked(mSources[i].value(key, "Auto") == c);
                connect(a, &QAction::triggered, this, [this, i, key, c] {
                    mSources[i][key] = c;
                    QTimer::singleShot(0, this, [this] { rebuildSources(); });
                });
            }
        };
        choices("Length Axis", "lengthAxis", {"Auto", "X", "Y", "Z"});
        choices("Base End", "baseEnd", {"Auto", "Min", "Max"});
        menu.addSeparator();
        QAction* select = menu.addAction("Select In Scene");
        connect(select, &QAction::triggered, this, [this, i] { run("select", {{"args", {mSources[i]["name"]}}}); });
        menu.exec(at);
    }

    void buildSection(QWidget* body, const std::string& id, const std::string& label)
    {
        Section s = makeSection(body, qs(label), id == "output");
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(1);
        grid->setColumnStretch(2, 1);
        int row = 0;
        for (const Param& p : parameters())
        {
            if (p.section != id)
                continue;
            mControls[p.name] = buildControl(s.body, grid, row++, p);
        }
        s.layout->addLayout(grid);
        if (id == "output")
        {
            auto* w = new QWidget(s.body);
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(new QLabel("Ground Colour", w));
            mGroundSwatch = new QToolButton(w);
            mGroundSwatch->setFixedSize(22, 18);
            mGroundSwatch->setToolTip("Colour the ground plane's Object ID bakes as. Click to change it.");
            connect(mGroundSwatch, &QToolButton::clicked, this, [this] { editIdColour(mControls["groundId"].integer->value()); });
            h->addWidget(mGroundSwatch);
            h->addStretch(1);
            mGroundRow = w;
            s.layout->addWidget(w);
        }
        if (id == "distribution")
        {
            mClusterReadout = new QLabel(s.body);
            mClusterReadout->setAlignment(Qt::AlignCenter);
            mClusterReadout->setWordWrap(true);
            mClusterReadout->setToolTip("Cluster discs that add up to more than the tile area cover it evenly, which reads as uniform scatter rather than clumps.");
            QFont f = mClusterReadout->font();
            f.setItalic(true);
            mClusterReadout->setFont(f);
            s.layout->addWidget(mClusterReadout);
        }
        if (id == "tile")
        {
            auto* seed = new QPushButton("Randomize Seed", s.body);
            seed->setToolTip("Pick a new random seed.");
            connect(seed, &QPushButton::clicked, this, [this] { randomizeSeed(); });
            auto* h = new QHBoxLayout;
            h->addStretch(1);
            h->addWidget(seed);
            s.layout->addLayout(h);
        }
        mBody->addWidget(s.root);
    }

    Control buildControl(QWidget* parent, QGridLayout* grid, int row, const Param& p)
    {
        Control c;
        c.param = &p;
        const QString tip = qs(p.help);
        auto* label = new QLabel(qs(p.label), parent);
        label->setToolTip(tip);
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(label, row, 0);
        // Row widgets show and hide together, so they share a holder that stands in for the row.
        c.row = label;
        const std::string name = p.name;
        if (p.type == "float" || p.type == "int")
        {
            c.slider = new QSlider(Qt::Horizontal, parent);
            c.slider->setRange(0, kSliderSteps);
            c.slider->setToolTip(tip);
            if (p.type == "float")
            {
                c.real = new QDoubleSpinBox(parent);
                c.real->setDecimals(3);
                c.real->setRange(p.min, p.max);
                c.real->setSingleStep(std::max((p.max - p.min) / 200.0, 0.001));
                c.real->setToolTip(tip);
                c.real->setFixedWidth(76);
                grid->addWidget(c.real, row, 1);
                connect(c.real, &QDoubleSpinBox::valueChanged, this, [this, name](double) { syncSlider(name); });
            }
            else
            {
                c.integer = new QSpinBox(parent);
                c.integer->setRange(int(p.min), int(p.max));
                c.integer->setToolTip(tip);
                c.integer->setFixedWidth(76);
                grid->addWidget(c.integer, row, 1);
                connect(c.integer, &QSpinBox::valueChanged, this, [this, name](int) { syncSlider(name); });
            }
            grid->addWidget(c.slider, row, 2);
            connect(c.slider, &QSlider::valueChanged, this, [this, name](int) { syncField(name); });
        }
        else if (p.type == "bool")
        {
            c.check = new QCheckBox(parent);
            c.check->setToolTip(tip);
            grid->addWidget(c.check, row, 1, 1, 2);
            connect(c.check, &QCheckBox::toggled, this, [this](bool) { onDriverChanged(); });
        }
        else
        {
            c.combo = new QComboBox(parent);
            c.combo->setToolTip(tip);
            for (const std::string& o : p.options)
                c.combo->addItem(qs(o));
            grid->addWidget(c.combo, row, 1, 1, 2);
            connect(c.combo, &QComboBox::currentIndexChanged, this, [this](int) { onDriverChanged(); });
        }
        return c;
    }

    void buildExport(QWidget* body)
    {
        Section s = makeSection(body, "Export", true);
        auto* grid = new QGridLayout;
        grid->setColumnStretch(1, 1);
        auto field = [&](int row, const char* label, const std::string& value, const char* tip) {
            auto* l = new QLabel(label, s.body);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            auto* e = new QLineEdit(qs(value), s.body);
            e->setToolTip(tip);
            grid->addWidget(e, row, 1);
            return e;
        };
        const json& exp = mPrefs["Export"];
        mExportPath = field(0, "Export Path", exp.value("path", ""), "Folder the FBX pair is written to.");
        auto* browse = new QToolButton(s.body);
        browse->setIcon(icon("folder-open"));
        browse->setToolTip("Browse for the export folder.");
        grid->addWidget(browse, 0, 2);
        mExportName = field(1, "File Name", exp.value("name", ""), "Written as <name>_Hi.fbx and <name>_Lo.fbx.");
        mExportMaterial = field(2, "Bake Plane Material", exp.value("material", ""),
                                "The bake plane's material is renamed to this for the export, since that is what the baker names its outputs after. Renamed back afterwards.");
        s.layout->addLayout(grid);
        connect(browse, &QToolButton::clicked, this, [this] {
            const QString chosen = QFileDialog::getExistingDirectory(this, "Export Folder", mExportPath->text());
            if (chosen.isEmpty())
                return;
            mExportPath->setText(chosen);
            setExportPref("path", chosen.toStdString());
        });
        connect(mExportPath, &QLineEdit::editingFinished, this, [this] { setExportPref("path", mExportPath->text().trimmed().toStdString()); });
        connect(mExportName, &QLineEdit::editingFinished, this, [this] { setExportPref("name", mExportName->text().trimmed().toStdString()); });
        connect(mExportMaterial, &QLineEdit::editingFinished, this, [this] { setExportPref("material", mExportMaterial->text().trimmed().toStdString()); });
        mExportReadout = new QLabel(s.body);
        QFont f = mExportReadout->font();
        f.setItalic(true);
        mExportReadout->setFont(f);
        s.layout->addWidget(mExportReadout);
        auto button = [&](const char* label, const char* tip, std::function<void()> fn) {
            auto* b = new QPushButton(label, s.body);
            b->setToolTip(tip);
            connect(b, &QPushButton::clicked, this, std::move(fn));
            s.layout->addWidget(b);
        };
        button("Export FBX Pair", "Write the selected generated group as <name>_Hi.fbx, and its bake plane as <name>_Lo.fbx.", [this] { exportBakes(); });
        button("Measure Height Range",
               "Sample the top surface over the tile and report where it actually sits. The bounding box max is one outlying tip, so it is the wrong number to scale a height bake by.",
               [this] { measureHeight(); });
        button("Split Vertex Colour Masks",
               "Split the baked vertex colour map in the export path into three greyscale masks, R root to tip, G per instance, B height layer. Painter greys an RGB image before any effect sees it, so the channels have to be split here.",
               [this] { splitMasks(); });
        mBody->addWidget(s.root);
    }

    // ------------------------------------------------------------------------------------------------ values

    json readValue(const Control& c) const
    {
        if (c.real)
            return c.real->value();
        if (c.integer)
            return int64_t(c.integer->value());
        if (c.check)
            return c.check->isChecked();
        return c.combo->currentText().toStdString();
    }

    json readValues() const
    {
        json v = json::object();
        for (const auto& [name, c] : mControls)
            v[name] = readValue(c);
        return sanitize(v);
    }

    void writeValue(const std::string& name, const json& value)
    {
        auto it = mControls.find(name);
        if (it == mControls.end())
            return;
        Control& c = it->second;
        if (c.real)
            c.real->setValue(value.is_number() ? value.get<double>() : 0.0);
        else if (c.integer)
            c.integer->setValue(value.is_number() ? int(value.get<double>()) : 0);
        else if (c.check)
            c.check->setChecked(value.is_boolean() && value.get<bool>());
        else if (c.combo)
            c.combo->setCurrentText(qs(value.is_string() ? value.get<std::string>() : ""));
    }

    void syncSlider(const std::string& name)
    {
        Control& c = mControls[name];
        if (!c.slider || mSyncing)
            return;
        mSyncing = true;
        const double v = c.real ? c.real->value() : double(c.integer->value());
        const double span = c.param->max - c.param->min;
        c.slider->setValue(span > 0.0 ? int(std::lround((v - c.param->min) / span * kSliderSteps)) : 0);
        mSyncing = false;
        afterNumber(name);
    }

    void syncField(const std::string& name)
    {
        Control& c = mControls[name];
        if (mSyncing)
            return;
        mSyncing = true;
        const double v = c.param->min + (c.param->max - c.param->min) * double(c.slider->value()) / kSliderSteps;
        if (c.real)
            c.real->setValue(v);
        else
            c.integer->setValue(int(std::lround(v)));
        mSyncing = false;
        afterNumber(name);
    }

    void afterNumber(const std::string& name)
    {
        if (name == "clusterCount" || name == "clusterRadius" || name == "clusterStrength" || name == "tileSize")
            updateClusterReadout();
        if (name == "groundId")
            refreshGroundSwatch();
    }

    void applyValues(const json& values)
    {
        mLoading = true;
        for (const auto& [name, value] : sanitize(values).items())
            writeValue(name, value);
        mLoading = false;
        refreshVisibility();
        updateClusterReadout();
    }

    void onDriverChanged()
    {
        if (mLoading)
            return;
        refreshVisibility();
        updateClusterReadout();
    }

    void refreshVisibility()
    {
        const json values = readValues();
        for (auto& [name, c] : mControls)
        {
            const bool show = terrain::isVisible(*c.param, values);
            for (QWidget* w : {static_cast<QWidget*>(c.row), static_cast<QWidget*>(c.real), static_cast<QWidget*>(c.integer), static_cast<QWidget*>(c.slider),
                               static_cast<QWidget*>(c.check), static_cast<QWidget*>(c.combo)})
                if (w)
                    w->setVisible(show);
        }
        if (mGroundRow)
            mGroundRow->setVisible(flag(values, "groundPlane"));
        refreshGroundSwatch();
    }

    void updateClusterReadout()
    {
        if (!mClusterReadout)
            return;
        const json values = readValues();
        const bool clustered = text(values, "distribution") == "Clustered";
        mClusterReadout->setVisible(clustered);
        if (clustered)
            mClusterReadout->setText(qs(describeClustering(values)));
    }

    void refreshGroundSwatch()
    {
        if (mGroundSwatch && mControls.count("groundId"))
            mGroundSwatch->setStyleSheet(swatchStyle(idColour(mControls["groundId"].integer->value(), mPrefs["Palette"])));
    }

    void editIdColour(int64_t id)
    {
        const glm::dvec3 c = idColour(id, mPrefs["Palette"]);
        const QColor chosen = QColorDialog::getColor(QColor::fromRgbF(float(c.r), float(c.g), float(c.b)), this, "Object ID " + QString::number(id));
        if (!chosen.isValid())
            return;
        run("terrainSetIdColour", {{"args", {id}}, {"colour", {chosen.redF(), chosen.greenF(), chosen.blueF()}}}, false);
        mPrefs = loadPrefs();
        refreshGroundSwatch();
        rebuildSources();
    }

    void status(const std::string& message)
    {
        if (mStatus)
            mStatus->setText(qs(message));
    }

    void report(const json& r)
    {
        if (!r.is_object() || !r["group"].is_string())
        {
            const json& w = r.is_object() ? r["warnings"] : json();
            status(w.is_array() && !w.empty() ? w.back().get<std::string>() : "Nothing was generated.");
            return;
        }
        const json s = run("terrainSettings", {{"args", {r["group"]}}}, false);
        const json m = s.is_object() ? s["metrics"] : json::object();
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s: %d instances, height %.3f to %.3f, %.3f above the plane.", r["group"].get<std::string>().c_str(), r["instances"].get<int>(),
                      m.value(kAttrMinHeight, 0.0), m.value(kAttrMaxHeight, 0.0), m.value(kAttrAbovePlane, 0.0));
        status(buf);
    }

    json generateCall() const
    {
        return {{"values", readValues()}, {"sources", mSources}, {"preset", mPreset}, {"palette", mPrefs["Palette"]}, {"export", exportSettings()}};
    }

    // ------------------------------------------------------------------------------------------------ presets

    std::vector<std::string> presetNames() const
    {
        std::vector<std::string> names = builtInPresets();
        std::vector<std::string> user;
        for (auto it = mPrefs["Presets"].begin(); it != mPrefs["Presets"].end(); ++it)
            if (std::find(names.begin(), names.end(), it.key()) == names.end())
                user.push_back(it.key());
        std::sort(user.begin(), user.end());
        names.insert(names.end(), user.begin(), user.end());
        return names;
    }

    bool hasPreset(const std::string& name) const
    {
        const std::vector<std::string> names = presetNames();
        return std::find(names.begin(), names.end(), name) != names.end();
    }

    void updatePresetMenu()
    {
        QSignalBlocker block(mPresetMenu);
        mPresetMenu->clear();
        for (const std::string& n : presetNames())
            mPresetMenu->addItem(qs(builtInOverrides(n) ? n : n + " *"), qs(n));
        mPresetMenu->setCurrentIndex(std::max(0, mPresetMenu->findData(qs(mPreset))));
    }

    void changePreset(const std::string& name)
    {
        mPreset = name;
        applyValues(presetSettings(name, mPrefs["Presets"]));
        applyPresetNaming();
        status("Loaded preset '" + name + "'.");
    }

    void savePreset()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Save Preset", "Preset Name:", QLineEdit::Normal, qs(mPreset), &ok).trimmed();
        if (!ok)
            return;
        if (name.isEmpty())
        {
            status("A preset needs a name.");
            return;
        }
        run("terrainSavePreset", {{"args", {name.toStdString()}}, {"values", readValues()}}, false);
        mPrefs = loadPrefs();
        mPreset = name.toStdString();
        updatePresetMenu();
        status("Saved preset '" + mPreset + "'.");
    }

    void deletePreset()
    {
        if (builtInOverrides(mPreset))
        {
            status("Built-in presets cannot be deleted.");
            return;
        }
        if (QMessageBox::question(this, "Delete Preset", qs("Delete the preset '" + mPreset + "'?")) != QMessageBox::Yes)
            return;
        run("terrainDeletePreset", {{"args", {mPreset}}}, false);
        mPrefs = loadPrefs();
        mPreset = defaultPreset();
        updatePresetMenu();
        applyValues(presetSettings(mPreset, mPrefs["Presets"]));
    }

    // ------------------------------------------------------------------------------------------------ export fields

    json exportSettings() const
    {
        return {{"path", mExportPath->text().trimmed().toStdString()},
                {"name", mExportName->text().trimmed().toStdString()},
                {"material", mExportMaterial->text().trimmed().toStdString()}};
    }

    void setExportPref(const std::string& key, const std::string& value)
    {
        mPrefs["Export"][key] = value;
        if (mAuto.count(key))
        {
            // An empty field goes back to following the preset.
            mAuto[key] = value.empty();
            mPrefs["Export"][key == "name" ? "autoName" : "autoMaterial"] = mAuto[key];
            if (mAuto[key])
                applyPresetNaming();
        }
        savePrefs(mPrefs);
        updateExportReadout();
    }

    /** File name and material follow the preset until they are typed over. */
    void applyPresetNaming()
    {
        const std::string stem = presetStem(mPreset);
        if (mAuto["name"])
        {
            mExportName->setText(qs(stem));
            mPrefs["Export"]["name"] = stem;
        }
        if (mAuto["material"])
        {
            mExportMaterial->setText(qs(stem));
            mPrefs["Export"]["material"] = stem;
        }
        updateExportReadout();
    }

    void updateExportReadout()
    {
        const json e = exportSettings();
        const std::string name = e["name"].get<std::string>();
        mExportReadout->setText(!e["path"].get<std::string>().empty() && !name.empty() ? qs(name + kHighSuffix + ".fbx and " + name + kLowSuffix + ".fbx")
                                                                                       : "Set a path and a name.");
    }

    // ------------------------------------------------------------------------------------------------ groups

    void loadGroup(const json& stored)
    {
        applyValues(stored["values"]);
        json palette = mPrefs["Palette"];
        palette.update(stored.value("palette", json::object()));
        mPrefs["Palette"] = palette;
        const json& exp = stored["export"];
        if (!exp.value("path", "").empty())
        {
            mExportPath->setText(qs(exp.value("path", "")));
            mPrefs["Export"]["path"] = exp.value("path", "");
        }
        const std::string storedStem = presetStem(stored.value("preset", ""));
        for (const auto& [key, field] : {std::pair{std::string("name"), mExportName}, std::pair{std::string("material"), mExportMaterial}})
            if (!exp.value(key, "").empty())
            {
                field->setText(qs(exp.value(key, "")));
                mPrefs["Export"][key] = exp.value(key, "");
                // A stored name that matches its preset was never typed over.
                mAuto[key] = exp.value(key, "") == storedStem;
            }
        mSources.clear();
        for (const json& s : stored["sources"])
            if (s.is_object() && !s.value("name", "").empty())
                mSources.push_back({{"name", s.value("name", "")},
                                    {"weight", s.value("weight", 1.0)},
                                    {"objectId", s.value("objectId", 0)},
                                    {"heightOffset", s.value("heightOffset", 0.0)},
                                    {"enabled", s.value("enabled", true)},
                                    {"lengthAxis", s.value("lengthAxis", "Auto")},
                                    {"baseEnd", s.value("baseEnd", "Auto")}});
        rebuildSources();
        const std::string preset = stored.value("preset", "");
        if (!preset.empty() && hasPreset(preset))
        {
            mPreset = preset;
            updatePresetMenu();
        }
        applyPresetNaming();
        status("Loaded settings from " + stored.value("group", std::string()) + ".");
    }

    void onSelectionChanged()
    {
        if (!mPrefs["Window"].value("autoLoadSelection", true) || !isVisible())
            return;
        const json s = run("terrainSettings", json::object(), false);
        if (s.is_object())
            loadGroup(s);
    }

    json mPrefs;
    std::map<std::string, bool> mAuto;
    std::string mPreset;
    std::vector<json> mSources;
    std::map<std::string, Control> mControls;
    QComboBox* mPresetMenu = nullptr;
    QVBoxLayout* mBody = nullptr;
    QWidget* mSourceHolder = nullptr;
    QVBoxLayout* mSourceRows = nullptr;
    QLabel* mClusterReadout = nullptr;
    QToolButton* mGroundSwatch = nullptr;
    QWidget* mGroundRow = nullptr;
    QLineEdit* mExportPath = nullptr;
    QLineEdit* mExportName = nullptr;
    QLineEdit* mExportMaterial = nullptr;
    QLabel* mExportReadout = nullptr;
    QLabel* mStatus = nullptr;
    bool mLoading = false;
    bool mSyncing = false;
    events::SubscriptionId mSelectionSub = 0;
};

QPointer<Panel> gPanel;

/** Runs fn on the panel, showing it first: its fields are what the buttons act on. */
void onPanel(void (Panel::*fn)())
{
    panels::show(kPanelId);
    if (gPanel)
        (gPanel.data()->*fn)();
    else
        log::warn("Terrain Generator: open the panel (Windows > Terrain Generator) first");
}
} // namespace

void registerUi(ext::Context& ctx)
{
    panels::add(ctx, {.id = kPanelId,
                      .title = "Terrain Generator",
                      .create = [](QWidget* parent) -> QWidget* {
                          auto* p = new Panel(parent);
                          gPanel = p;
                          return p;
                      },
                      .icon = "mountain",
                      .open = false});

    struct Entry
    {
        const char* id;
        const char* label;
        const char* doc;
        void (Panel::*fn)();
    };
    const Entry entries[] = {
        {"terrainGenerator.generate", "Generate Terrain", "Scatter a new tiling terrain group with the Terrain Generator panel's settings.", &Panel::generate},
        {"terrainGenerator.regenerate", "Regenerate Terrain", "Rebuild the selected generated terrain group with the panel's settings.", &Panel::regenerate},
        {"terrainGenerator.delete", "Delete Terrain", "Delete the selected generated terrain group.", &Panel::deleteSelected},
        {"terrainGenerator.addSources", "Add Selected Terrain Sources", "Add the selected meshes to the Terrain Generator's sources.", &Panel::addSelectedSources},
        {"terrainGenerator.bakePlane", "Create Terrain Bake Plane", "A tile-sized plane with 0-1 UVs to bake onto, inside the selected generated group.", &Panel::createBakePlane},
        {"terrainGenerator.bakeIdMap", "Bake Terrain ID Map", "Bake the Object ID colours of the selected generated group onto its bake plane.", &Panel::bakeIdMap},
        {"terrainGenerator.export", "Export Terrain FBX Pair", "Write the selected generated group and its bake plane as <name>_Hi.fbx and <name>_Lo.fbx with the ID sidecars.", &Panel::exportBakes},
        {"terrainGenerator.measureHeight", "Measure Terrain Height Range", "Sample the selected group's top surface and stamp its p50, p99 and over-tile heights.", &Panel::measureHeight},
        {"terrainGenerator.splitMasks", "Split Vertex Colour Masks", "Split the baked vertex colour map in the export path into R, G and B greyscale masks.", &Panel::splitMasks},
        {"terrainGenerator.randomizeSeed", "Randomize Terrain Seed", "Pick a new random seed in the Terrain Generator panel.", &Panel::randomizeSeed},
        {"terrainGenerator.loadSelected", "Load Terrain Settings", "Load the selected generated group's settings into the Terrain Generator panel.", &Panel::loadFromSelection},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id, .label = e.label, .category = "Terrain", .run = [fn = e.fn] { onPanel(fn); }, .doc = e.doc});

    menus::addItem(ctx, {.id = "terrainGenerator.menu", .menu = "Create", .kind = menus::Kind::Submenu, .label = "Terrain Generator"});
    const char* const items[] = {"window.terrainGenerator", "", "terrainGenerator.addSources", "terrainGenerator.generate", "terrainGenerator.regenerate",
                                 "terrainGenerator.delete", "", "terrainGenerator.bakePlane", "terrainGenerator.bakeIdMap", "terrainGenerator.measureHeight",
                                 "terrainGenerator.export", "terrainGenerator.splitMasks"};
    int separators = 0;
    for (const char* a : items)
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("terrainGenerator.menu.") + a, .menu = "Create/Terrain Generator", .action = a});
        else
            menus::addItem(ctx, {.id = "terrainGenerator.menu.sep" + std::to_string(separators++), .menu = "Create/Terrain Generator", .kind = menus::Kind::Separator});
    }
}
} // namespace terrain
