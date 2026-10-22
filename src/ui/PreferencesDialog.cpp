#include "ui/PreferencesDialog.h"

#include "cmds/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Anim.h"
#include "scene/ComponentPick.h"
#include "scene/Transform.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"
#include "ui/ViewportDisplay.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <tuple>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

constexpr const char* kCollapsedPref = "/preferences/collapsed";

/** A collapsible run of rows under a header; the rows stay in the page's form so labels line up across sections. */
struct Section
{
    QToolButton* header;
    std::vector<int> rows;
};

struct Page
{
    QWidget* widget;
    QFormLayout* form;
    const char* file;
    std::string title;
    std::shared_ptr<Section> section;
};

Page page(QTabWidget* tabs, const char* title, const char* file)
{
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->setLabelAlignment(Qt::AlignRight);
    scroll->setWidget(w);
    tabs->addTab(scroll, title);
    return {w, form, file, title, nullptr};
}

template <typename Label>
void addRow(Page& p, Label label, QWidget* field)
{
    p.form->addRow(label, field);
    if (!p.section)
        return;
    int row = p.form->rowCount() - 1;
    p.section->rows.push_back(row);
    if (!p.section->header->isChecked())
        p.form->setRowVisible(row, false);
}

/** Starts a section: rows added after it sit under its header. The collapsed state is remembered per page and title. */
void section(Page& p, const char* title)
{
    auto* header = new QToolButton;
    header->setText(title);
    header->setCheckable(true);
    header->setAutoRaise(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet(QString("QToolButton { text-align: left; font-weight: 600; padding: 3px; margin-top: %1px; }")
                              .arg(p.form->rowCount() ? 8 : 0));
    std::string key = std::string(kCollapsedPref) + "/" + p.title + "/" + title;
    bool open = !PrefStore::get().value<bool>("general", key, false);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    p.form->addRow(header);
    auto s = std::make_shared<Section>(Section{header, {}});
    QFormLayout* form = p.form;
    QObject::connect(header, &QToolButton::toggled, header, [s, form, key](bool on) {
        s->header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        for (int row : s->rows)
            form->setRowVisible(row, on);
        PrefStore::get().set("general", key, !on);
    });
    p.section = std::move(s);
}

void choice(Page& p, const char* label, const char* key, std::vector<std::pair<QString, std::string>> options, const std::string& fallback)
{
    auto* box = new QComboBox;
    std::string current = PrefStore::get().value<std::string>(p.file, key, fallback);
    for (auto& [text, value] : options)
    {
        box->addItem(text, QString::fromStdString(value));
        if (value == current)
            box->setCurrentIndex(box->count() - 1);
    }
    std::string file = p.file, k = key;
    QObject::connect(box, &QComboBox::currentIndexChanged, box,
                     [box, file, k](int) { PrefStore::get().set(file, k, box->currentData().toString().toStdString()); });
    setComboDefault(box, box->findData(QString::fromStdString(fallback)));
    addRow(p, label, box);
}

void toggle(Page& p, const char* label, const char* key, bool fallback)
{
    auto* c = new QCheckBox;
    c->setChecked(PrefStore::get().value<bool>(p.file, key, fallback));
    std::string file = p.file, k = key;
    QObject::connect(c, &QCheckBox::toggled, c, [file, k](bool on) { PrefStore::get().set(file, k, on); });
    // Toolbar buttons and hotkeys flip some of these prefs while the dialog is open.
    int listener = PrefStore::get().onChanged([c, file, k, fallback](const std::string& changed) {
        bool on = PrefStore::get().value<bool>(file, k, fallback);
        if (changed == file && c->isChecked() != on)
        {
            QSignalBlocker block(c);
            c->setChecked(on);
        }
    });
    QObject::connect(c, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
    setCheckDefault(c, fallback);
    addRow(p, label, c);
}

void number(Page& p, const char* label, const char* key, double fallback, double lo, double hi, double step, int decimals = 2)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setValue(PrefStore::get().value<double>(p.file, key, fallback));
    std::string file = p.file, k = key;
    QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [file, k](double v) { PrefStore::get().set(file, k, v); });
    setSpinDefault(s, fallback);
    addRow(p, label, s);
}

/** A swatch that opens a color picker; the pref holds sRGB "#rrggbb". */
void color(Page& p, const char* label, const std::string& key, const std::string& fallback)
{
    auto* b = new QToolButton;
    b->setFixedSize(40, 18);
    auto show = [b](const QString& hex) { b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid #262626; }").arg(hex)); };
    std::string file = p.file;
    auto current = [file, key, fallback] {
        QColor c(QString::fromStdString(PrefStore::get().value<std::string>(file, key, fallback)));
        return c.isValid() ? c : QColor(QString::fromStdString(fallback));
    };
    show(current().name());
    QObject::connect(b, &QToolButton::clicked, b, [b, show, current, file, key] {
        QColor c = QColorDialog::getColor(current(), b, "Choose Color");
        if (!c.isValid())
            return;
        PrefStore::get().set(file, key, c.name().toStdString());
        show(c.name());
    });
    auto* name = new QLabel(label);
    onMiddleClickReset(name, [show, file, key, fallback] {
        PrefStore::get().set(file, key, fallback);
        show(QString::fromStdString(fallback));
    }, QString::fromStdString(fallback), b);
    addRow(p, name, b);
}

void text(Page& p, const char* label, const char* key, const std::string& fallback, bool fileButton = false)
{
    auto* row = new QWidget;
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* e = new QLineEdit(QString::fromStdString(PrefStore::get().value<std::string>(p.file, key, fallback)));
    h->addWidget(e, 1);
    std::string file = p.file, k = key;
    QObject::connect(e, &QLineEdit::editingFinished, e, [e, file, k] { PrefStore::get().set(file, k, e->text().toStdString()); });
    if (fileButton)
    {
        auto* b = new QPushButton("...");
        b->setFixedWidth(28);
        QObject::connect(b, &QPushButton::clicked, e, [e, file, k] {
            QString f = QFileDialog::getOpenFileName(e, "Choose File", e->text());
            if (f.isEmpty())
                return;
            e->setText(f);
            PrefStore::get().set(file, k, f.toStdString());
        });
        h->addWidget(b);
    }
    auto* name = new QLabel(label);
    onMiddleClickReset(name, [e, file, k, fallback] {
        e->setText(QString::fromStdString(fallback));
        PrefStore::get().set(file, k, fallback);
    }, fallback.empty() ? "empty" : QString::fromStdString(fallback), e);
    addRow(p, name, row);
}
} // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
{
    namespace vd = viewportDisplay;
    setWindowTitle("Preferences");
    resize(840, 520);
    auto* v = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    v->addWidget(tabs, 1);

    Page general = page(tabs, "General", "general");
    section(general, "Scene");
    choice(general, "Up axis", "/scene/upAxis", {{"Y up", "y"}, {"Z up", "z"}}, "y");
    choice(general, "Save format", "/scene/defaultFormat", {{"Binary (.lila)", "binary"}, {"Text (.lilaa)", "ascii"}}, "binary");
    number(general, "Autosave every (seconds)", "/autosave/intervalSeconds", 30.0, 5.0, 3600.0, 5.0, 0);
    section(general, "Undo");
    toggle(general, "Infinite undo", "/undo/infinite", true);
    number(general, "Steps (when not infinite)", "/undo/steps", 500.0, 1.0, 100000.0, 50.0, 0);
    number(general, "Memory budget (MB)", "/undo/memoryMB", 2048.0, 0.0, 1048576.0, 256.0, 0);
    number(general, "Held key repeats after (ms)", "/undo/repeatDelayMs", 180.0, 0.0, 2000.0, 10.0, 0);
    number(general, "Held key repeats every (ms, 0 = off)", "/undo/repeatIntervalMs", 100.0, 0.0, 2000.0, 10.0, 0);
    section(general, "References");
    toggle(general, "Reload changed references", "/references/autoReload", true);
    section(general, "Terminal");
    text(general, "Shell", "/terminal/shell", "");
    number(general, "Scrollback (lines)", "/terminal/scrollback", 10000.0, 0.0, 1000000.0, 1000.0, 0);
    toggle(general, "Typing scrolls to bottom", "/terminal/scrollOnInput", true);
    toggle(general, "Output scrolls to bottom", "/terminal/scrollOnOutput", false);
    toggle(general, "Offer agent setup per workspace", "/agents/promptOnTerminal", true);

    Page ui = page(tabs, "Interface", "general");
    section(ui, "Chrome");
    toggle(ui, "Show menu bar (the hotbox still has the menus)", "/ui/showMenuBar", true);
    toggle(ui, "Show shelf (` still opens the floating shelf)", "/ui/showShelf", true);
    toggle(ui, "Badges at the viewport cursor for held keys", "/viewport/cursorBadges", true);
    section(ui, "Floating Shelf");
    choice(ui, "Opens at", "/floatingShelf/openAt",
           {{"Cursor", "cursor"}, {"Center of viewport", "viewport"}, {"Center of screen", "screen"}}, "viewport");
    number(ui, "Offset X (px)", "/floatingShelf/offsetX", 0.0, -2000.0, 2000.0, 10.0, 0);
    number(ui, "Offset Y (px)", "/floatingShelf/offsetY", 250.0, -2000.0, 2000.0, 10.0, 0);
    toggle(ui, "Close when clicking elsewhere", "/floatingShelf/closeOnClickAway", false);
    toggle(ui, "Close after running a button", "/floatingShelf/closeAfterRun", false);

    Page viewport = page(tabs, "Viewport", "general");
    section(viewport, "Environment");
    text(viewport, "HDRI", "/viewport/env/path", "", true);
    number(viewport, "Rotation", "/viewport/env/rotation", 0.0, -360.0, 360.0, 5.0, 0);
    number(viewport, "Intensity", "/viewport/env/intensity", 1.0, 0.0, 10.0, 0.1);
    toggle(viewport, "Show environment", "/viewport/env/background", true);
    number(viewport, "Background blur", "/viewport/env/blur", 0.6, 0.0, 1.0, 0.05);
    number(viewport, "Background exposure", "/viewport/env/backgroundExposure", 0.25, 0.0, 4.0, 0.1);
    section(viewport, "Lighting");
    toggle(viewport, "Shadows", vd::kShadows, true);
    number(viewport, "Ground shadow", "/viewport/groundShadow", 0.5, 0.0, 1.0, 0.05);
    toggle(viewport, "Ambient occlusion", "/viewport/ao/enabled", true);
    number(viewport, "AO radius (cm)", "/viewport/ao/radius", 30.0, 0.1, 10000.0, 1.0, 1);
    number(viewport, "AO intensity", "/viewport/ao/intensity", 1.0, 0.0, 4.0, 0.1);
    section(viewport, "Color");
    number(viewport, "Exposure (stops)", vd::kExposure, 0.0, -10.0, 10.0, 0.1);
    number(viewport, "Gamma", vd::kGamma, 1.0, 0.1, 4.0, 0.05);
    section(viewport, "Antialiasing");
    toggle(viewport, "Enabled", vd::kAaEnabled, true);
    {
        auto* box = new QComboBox;
        int current = PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples);
        for (int n : {2, 4, 8})
        {
            box->addItem(QString("%1x").arg(n), n);
            if (n == current)
                box->setCurrentIndex(box->count() - 1);
        }
        QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int) { PrefStore::get().set("general", vd::kAaSamples, box->currentData().toInt()); });
        setComboDefault(box, box->findData(vd::kDefaultSamples));
        addRow(viewport, "Samples", box);
    }
    section(viewport, "Grid");
    toggle(viewport, "Show grid", vd::kGrid, true);
    number(viewport, "Spacing (cm)", "/grid/spacing", 10.0, 0.1, 10000.0, 1.0, 1);
    section(viewport, "Display");
    toggle(viewport, "Poly count", "/viewport/polyCount", true);
    toggle(viewport, "Panel toolbar", vd::kToolbar, true);
    toggle(viewport, "Hiding a parent hides its children", scene::kInheritVisibilityPref, true);
    section(viewport, "Highlight Under Cursor");
    toggle(viewport, "Components", "/viewport/preselect/components", true);
    toggle(viewport, "Objects", "/viewport/preselect/objects", true);
    section(viewport, "Topology Under Cursor");
    {
        namespace cp = scene::componentPick;
        for (auto [t, label, colorLabel] : {std::tuple{cp::Topology::Ngon, "N-gons (5+ sides)", "N-gon color"},
                                            {cp::Topology::Triangle, "Triangles", "Triangle color"},
                                            {cp::Topology::Pole, "Poles (5+ edges)", "Pole color"}})
        {
            toggle(viewport, label, cp::topologyEnabledKey(t).c_str(), true);
            color(viewport, colorLabel, cp::topologyColorKey(t), cp::topologyDefaultColor(t));
        }
    }

    Page manip = page(tabs, "Manipulators", "general");
    section(manip, "Manipulators");
    number(manip, "Size (pixels)", "/manipulator/size", 100.0, 30.0, 400.0, 5.0, 0);
    choice(manip, "Move space", "/manipulator/moveSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}}, "world");
    choice(manip, "Rotate space", "/manipulator/rotateSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}}, "world");
    number(manip, "Rotate snap (degrees)", "/manipulator/rotateStep", 15.0, 0.1, 180.0, 1.0, 1);
    toggle(manip, "Tint axes (X/Y/Z colors)", "/manipulator/axisColors", false);
    section(manip, "Soft Select");
    toggle(manip, "Enabled", "/softSelect/enabled", false);
    number(manip, "Radius (cm)", "/softSelect/radius", 25.0, 0.01, 100000.0, 1.0);
    addRow(manip, "Falloff", new SoftSelectPanel);

    Page measure = page(tabs, "Measure", "general");
    section(measure, "Units");
    choice(measure, "Display units", "/units/linear",
           {{"Millimeters", "mm"}, {"Centimeters", "cm"}, {"Meters", "m"}, {"Inches", "in"}, {"Feet", "ft"}}, "cm");
    number(measure, "Decimal places", "/measure/precision", 2.0, 0.0, 8.0, 1.0, 0);
    section(measure, "Clipboard");
    choice(measure, "Measure Selection copies", "/measure/clipboard",
           {{"The main number", "value"}, {"The full report", "report"}, {"Nothing", "off"}}, "value");
    toggle(measure, "Include units when copying", "/measure/clipboardUnits", false);
    section(measure, "Measure Tool");
    toggle(measure, "Attach measure points to the object clicked", "/measure/attach", true);
    number(measure, "Result stays on screen (seconds)", "/measure/hudSeconds", 5.0, 0.5, 60.0, 0.5, 1);
    toggle(measure, "Show measurements", "/measure/show", true);

    Page timeSlider = page(tabs, "Time Slider", "anim");
    section(timeSlider, "Playback");
    {
        namespace anim = scene::anim;
        auto* box = new QComboBox;
        for (anim::PlaybackSpeed speed : anim::kPlaybackSpeeds)
        {
            box->addItem(QString::fromUtf8(anim::speedLabel(speed).data(), qsizetype(anim::speedLabel(speed).size())),
                         QString::fromUtf8(anim::speedName(speed).data(), qsizetype(anim::speedName(speed).size())));
            if (speed == anim::playback().speed)
                box->setCurrentIndex(box->count() - 1);
        }
        box->setToolTip("Real-time drops frames to keep pace with fps. Play every frame shows each one, as fast as the "
                        "scene evaluates or capped at fps.");
        QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int) {
            try
            {
                cmds::execute("playbackOptions", {{"playbackSpeed", box->currentData().toString().toStdString()}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
        int listener = anim::addPlaybackListener([box] {
            QSignalBlocker block(box);
            box->setCurrentIndex(box->findData(QString::fromUtf8(anim::speedName(anim::playback().speed).data())));
        });
        QObject::connect(box, &QObject::destroyed, [listener] { anim::removePlaybackListener(listener); });
        setComboDefault(box, box->findData("realTime"));
        addRow(timeSlider, "Playback speed", box);
    }
    {
        namespace anim = scene::anim;
        auto* spin = new QDoubleSpinBox;
        spin->setRange(anim::kMinCustomSpeed, anim::kMaxCustomSpeed);
        spin->setDecimals(2);
        spin->setSingleStep(0.25);
        spin->setSuffix("x");
        spin->setValue(anim::playback().customSpeed);
        spin->setEnabled(anim::playback().speed == anim::PlaybackSpeed::Custom);
        spin->setToolTip("Real-time multiplier used when Playback speed is Custom Speed, e.g. 0.25 or 1.5.");
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [](double v) {
            try
            {
                cmds::execute("playbackOptions", {{"playbackSpeed", "custom"}, {"speedMultiplier", v}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        });
        int listener = anim::addPlaybackListener([spin] {
            QSignalBlocker block(spin);
            spin->setValue(anim::playback().customSpeed);
            spin->setEnabled(anim::playback().speed == anim::PlaybackSpeed::Custom);
        });
        QObject::connect(spin, &QObject::destroyed, [listener] { anim::removePlaybackListener(listener); });
        setSpinDefault(spin, 1.0);
        addRow(timeSlider, "Custom speed multiplier", spin);
    }

    Page naming = page(tabs, "Naming", "naming");
    section(naming, "Conventions");
    toggle(naming, "Enforce conventions", "/enabled", false);
    choice(naming, "When a name breaks them", "/mode", {{"Fix it", "fix"}, {"Warn", "warn"}, {"Refuse", "block"}}, "fix");
    section(naming, "Case");
    choice(naming, "Joints", "/rules/joint", {{"snake_case", "snake_case"}, {"PascalCase", "PascalCase"}, {"camelCase", "camelCase"}}, "snake_case");
    choice(naming, "Everything else", "/rules/*", {{"PascalCase", "PascalCase"}, {"snake_case", "snake_case"}, {"camelCase", "camelCase"}}, "PascalCase");

    Page sc = page(tabs, "Source Control", "sourceControl");
    section(sc, "Provider");
    choice(sc, "Provider", "/provider", {{"Automatic", "auto"}, {"Perforce", "perforce"}, {"Git", "git"}, {"None", "none"}}, "auto");
    section(sc, "Saving");
    choice(sc, "Saving a read-only file", "/autoCheckout", {{"Ask first", "prompt"}, {"Check out", "always"}, {"Do nothing", "never"}}, "prompt");
    toggle(sc, "Mark new files for add", "/autoAdd", true);
    section(sc, "Perforce");
    text(sc, "P4PORT", "/perforce/port", "");
    text(sc, "P4USER", "/perforce/user", "");
    text(sc, "P4CLIENT", "/perforce/client", "");

    Page unreal = page(tabs, "Unreal", "unreal");
    section(unreal, "Sync");
    toggle(unreal, "Sync mapped assets on save", "/autoSync", true);
    text(unreal, "Export cache folder", "/cacheDir", "");

    Page script = page(tabs, "Scripting", "scriptEditor");
    section(script, "Script Editor");
    toggle(script, "Format with ruff on save", "/formatOnSave", true);
    toggle(script, "Relative line numbers", "/relativeLineNumbers", true);
    toggle(script, "Vim keybindings", "/vim", false);
    section(script, "Scene Scripts");
    choice(script, "Scripts set to run on open", "/sceneScriptsOnOpen", {{"Ask first", "ask"}, {"Never run", "never"}}, "ask");

    auto* bottom = new QHBoxLayout;
    auto* folder = new QPushButton("Open Prefs Folder", this);
    connect(folder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(PrefStore::get().userDir().wstring())));
    });
    bottom->addWidget(new QLabel("Changes apply immediately.", this));
    bottom->addStretch(1);
    bottom->addWidget(folder);
    auto* close = new QPushButton("Close", this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    bottom->addWidget(close);
    v->addLayout(bottom);
}

void PreferencesDialog::open(QWidget* parent)
{
    static QPointer<PreferencesDialog> dialog;
    if (!dialog)
    {
        dialog = new PreferencesDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
} // namespace lila::ui
