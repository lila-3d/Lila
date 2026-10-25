#include "ui/PreferencesDialog.h"

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/TextMatch.h"
#include "scene/Anim.h"
#include "scene/Ghosts.h"
#include "scene/Naming.h"
#include "scene/ComponentPick.h"
#include "scene/Transform.h"
#include "ui/Icons.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"
#include "ui/TexturePicker.h"
#include "ui/ThemeEditor.h"
#include "ui/ThemeModel.h"
#include "ui/ViewportDisplay.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/Format.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>
#include <functional>
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

/** One pref's row in a page's form, which the search lifts into its flat results and puts back. */
struct PrefRow
{
    QFormLayout* form;
    int index;
    std::shared_ptr<Section> section;
    QString category;
    QWidget* label;
    QWidget* field;
    std::vector<std::string> searchText;
    bool lifted = false;
    /** Spans both columns on its page; the label only shows in search results. */
    bool wide = false;
};

struct Page
{
    QWidget* widget;
    QFormLayout* form;
    const char* file;
    std::string title;
    std::shared_ptr<Section> section;
    std::vector<PrefRow>* rows;
};

/**
 * Filters every page's rows into one flat list. The rows' own label and editor widgets move into the results and back,
 * so an edit there is the same edit as in the tab.
 */
class PrefSearch
{
public:
    PrefSearch(QTabWidget* tabs, QWidget* resultsView, QGridLayout* grid, QLabel* empty)
        : mTabs(tabs), mResultsView(resultsView), mGrid(grid), mEmpty(empty)
    {
    }

    std::vector<PrefRow> rows;

    void apply(const QString& text)
    {
        restore();
        textMatch::Query query(text.toStdString());
        mTabs->setVisible(query.empty());
        mResultsView->setVisible(!query.empty());
        if (query.empty())
            return;
        // Taken bottom-up so each row's recorded index still holds while the rows above it are in place.
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
        {
            if (!query.matches(it->searchText))
                continue;
            QFormLayout::TakeRowResult taken = it->form->takeRow(it->index);
            delete taken.labelItem;
            delete taken.fieldItem;
            it->lifted = true;
        }
        int line = 0;
        for (PrefRow& r : rows)
        {
            if (!r.lifted)
                continue;
            auto* category = new QLabel(r.category);
            QPalette pal = category->palette();
            pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
            category->setPalette(pal);
            mCategories.push_back(category);
            mGrid->addWidget(category, line, 0);
            if (r.wide)
                mGrid->addWidget(r.field, line, 1, 1, 2);
            else
            {
                mGrid->addWidget(r.label, line, 1, Qt::AlignRight | Qt::AlignVCenter);
                mGrid->addWidget(r.field, line, 2);
                r.label->show();
            }
            r.field->show();
            ++line;
        }
        mEmpty->setVisible(line == 0);
    }

private:
    void restore()
    {
        for (PrefRow& r : rows)
        {
            if (!r.lifted)
                continue;
            mGrid->removeWidget(r.label);
            mGrid->removeWidget(r.field);
            if (r.wide)
            {
                r.form->insertRow(r.index, r.field);
                r.label->hide();
            }
            else
                r.form->insertRow(r.index, r.label, r.field);
            r.form->setRowVisible(r.index, !r.section || r.section->header->isChecked());
            r.lifted = false;
        }
        for (QLabel* c : mCategories)
            delete c;
        mCategories.clear();
    }

    QTabWidget* mTabs;
    QWidget* mResultsView;
    QGridLayout* mGrid;
    QLabel* mEmpty;
    std::vector<QLabel*> mCategories;
};

/** Escape clears a non-empty search instead of closing the dialog. */
class ClearOnEscape : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        auto* edit = static_cast<QLineEdit*>(obj);
        bool escape = (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress) &&
                      static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape;
        if (!escape || edit->text().isEmpty())
            return false;
        if (e->type() == QEvent::ShortcutOverride)
        {
            e->accept();
            return true;
        }
        edit->clear();
        return true;
    }
};

Page page(QTabWidget* tabs, std::vector<PrefRow>* rows, const char* title, const char* file)
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
    return {w, form, file, title, nullptr, rows};
}

/** Adds a row and records it for the search; `key` is what it edits, matched like the label and tooltips. */
template <typename Label>
void addRow(Page& p, Label label, QWidget* field, const std::string& key)
{
    p.form->addRow(label, field);
    int row = p.form->rowCount() - 1;
    QWidget* labelWidget = p.form->itemAt(row, QFormLayout::LabelRole)->widget();
    QString labelText = labelWidget->property("text").toString();
    QString category = QString::fromStdString(p.title);
    if (p.section)
        category += " / " + p.section->header->text();
    p.rows->push_back({p.form, row, p.section, category, labelWidget, field,
                       {labelText.toStdString(), labelWidget->toolTip().toStdString(), field->toolTip().toStdString(),
                        std::string(p.file) + key, category.toStdString()}});
    if (!p.section)
        return;
    p.section->rows.push_back(row);
    if (!p.section->header->isChecked())
        p.form->setRowVisible(row, false);
}

/** A row spanning the page width, for an editor too big for the field column. */
void addWideRow(Page& p, const QString& label, QWidget* field, const std::string& key)
{
    p.form->addRow(field);
    int row = p.form->rowCount() - 1;
    auto* labelWidget = new QLabel(label, p.widget);
    labelWidget->hide();
    QString category = QString::fromStdString(p.title);
    if (p.section)
        category += " / " + p.section->header->text();
    p.rows->push_back({p.form, row, p.section, category, labelWidget, field,
                       {label.toStdString(), field->toolTip().toStdString(), std::string(p.file) + key, category.toStdString()}});
    p.rows->back().wide = true;
    if (!p.section)
        return;
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
    addRow(p, label, box, k);
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
    addRow(p, label, c, k);
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
    addRow(p, label, s, k);
}

/** A swatch that opens a color picker; the pref holds sRGB "#rrggbb". */
void color(Page& p, const char* label, const std::string& key, const std::string& fallback)
{
    auto* b = new QToolButton;
    b->setFixedSize(40, 18);
    b->setProperty("lilaRawColors", true);
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
    addRow(p, name, b, key);
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
    addRow(p, name, row, k);
}

/** Shows the page's last row only while shown() holds (and its section is open), checked whenever the page's prefs change. */
void showLastRowWhen(Page& p, std::function<bool()> shown)
{
    QFormLayout* form = p.form;
    const int row = form->rowCount() - 1;
    std::shared_ptr<Section> section = p.section;
    auto update = [form, row, section, shown] { form->setRowVisible(row, (!section || section->header->isChecked()) && shown()); };
    update();
    QWidget* field = form->itemAt(row, QFormLayout::FieldRole)->widget();
    if (section)
        QObject::connect(section->header, &QToolButton::toggled, field, update);
    const std::string file = p.file;
    int listener = PrefStore::get().onChanged([update, file](const std::string& changed) {
        if (changed == file)
            update();
    });
    QObject::connect(field, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
}

/**
 * Disables a row while disabled() holds, with `reason` as its tooltip. Checked whenever the page's prefs or the general
 * prefs (theme, app-wide switches) change.
 */
void disableRowWhen(Page& p, int row, std::function<bool()> disabled, const QString& reason)
{
    QWidget* label = p.form->itemAt(row, QFormLayout::LabelRole)->widget();
    QWidget* field = p.form->itemAt(row, QFormLayout::FieldRole)->widget();
    QPointer<QWidget> labelPtr(label), fieldPtr(field);
    auto update = [labelPtr, fieldPtr, disabled, reason] {
        if (!labelPtr || !fieldPtr)
            return;
        bool off = disabled();
        for (QWidget* w : {labelPtr.data(), fieldPtr.data()})
        {
            w->setEnabled(!off);
            w->setToolTip(off ? reason : QString());
        }
    };
    update();
    const std::string file = p.file;
    int listener = PrefStore::get().onChanged([field, update, file](const std::string& changed) {
        // Deferred: the theme cache drops on this same change.
        if (changed == file || changed == "general")
            QTimer::singleShot(0, field, update);
    });
    QObject::connect(field, &QObject::destroyed, [listener] { PrefStore::get().removeOnChanged(listener); });
}

/** Whether a naming rule's case is one where underscores need allowing, and they are allowed. */
bool namingUnderscores(const std::string& type, const std::string& fallbackCase, bool needAllowed)
{
    PrefStore& prefs = PrefStore::get();
    return prefs.value<std::string>("naming", "/rules/" + type, fallbackCase) != "snake_case" &&
           (!needAllowed || prefs.value<bool>("naming", "/allowUnderscores/" + type, true));
}

Page& rowsOf(void* page)
{
    return *static_cast<Page*>(page);
}

void builtIn(Registry<prefPages::Page>& r, const char* id, const char* title, const char* file, std::function<void(Page&)> build)
{
    r.put(id, {id, title, file, [build](prefPages::PageBuilder& b) { build(*static_cast<Page*>(b.mPage)); }}, kLilaOwner);
}

/** Lila's own pages, in tab order. */
void addBuiltIns(Registry<prefPages::Page>& r)
{
    namespace vd = viewportDisplay;
    builtIn(r, "general", "General", "general", [](Page& general) {
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
        section(general, "Textures");
        {
            auto* edit = new QPushButton("Texture Folders...");
            edit->setToolTip("Which folders of the project the material editor's texture lists show, per workspace and per scene");
            QObject::connect(edit, &QPushButton::clicked, edit, [edit] { texturePicker::showFoldersDialog(edit->window()); });
            addRow(general, "Project texture folders", edit, "/textures");
        }
        section(general, "References");
        toggle(general, "Reload changed references", "/references/autoReload", true);
        section(general, "Terminal");
        text(general, "Shell", "/terminal/shell", "");
        number(general, "Scrollback (lines)", "/terminal/scrollback", 10000.0, 0.0, 1000000.0, 1000.0, 0);
        toggle(general, "Typing scrolls to bottom", "/terminal/scrollOnInput", true);
        toggle(general, "Output scrolls to bottom", "/terminal/scrollOnOutput", false);
        toggle(general, "Offer agent setup per workspace", "/agents/promptOnTerminal", true);
        section(general, "Docs");
        toggle(general, "Download docs updates at startup", "/docs/autoUpdate", true);
    });

    builtIn(r, "interface", "Interface", "general", [](Page& ui) {
        section(ui, "Theme");
        addWideRow(ui, "Theme", makeThemeEditor(), themes::kThemePref);
        section(ui, "Chrome");
        toggle(ui, "Show menu bar (the hotbox still has the menus)", "/ui/showMenuBar", true);
        toggle(ui, "Show shelf (` still opens the floating shelf)", "/ui/showShelf", true);
        toggle(ui, "Badges at the viewport cursor for held keys", "/viewport/cursorBadges", true);
        section(ui, "Color");
        toggle(ui, "Monotone (overrides the colors below)", vd::kMonotone, false);
        toggle(ui, "View axis indicator in X/Y/Z colors", vd::kViewAxisColors, true);
        toggle(ui, "Manipulator axes in X/Y/Z colors", vd::kManipulatorAxisColors, true);
        toggle(ui, "Time slider keys in red", vd::kTimeKeyColors, true);
        toggle(ui, "Graph editor X/Y/Z curves in axis colors", vd::kGraphAxisColors, true);
        toggle(ui, "Animation ghosts tinted before and after", vd::kGhostColors, true);
        toggle(ui, "Classic component colors (blue wire, pink vertices, red hover, yellow selection)", vd::kComponentColors, true);
        section(ui, "Component Thickness (px)");
        const vd::ComponentThickness thickness;
        number(ui, "Edge thickness", vd::kEdgeThickness, thickness.edge, 1.0, 16.0, 0.5, 1);
        number(ui, "Hovered edge thickness", vd::kEdgeThicknessHover, thickness.edgeHover, 0.5, 16.0, 0.5, 1);
        number(ui, "Selected edge thickness", vd::kEdgeThicknessSelected, thickness.edgeSelected, 1.0, 16.0, 0.5, 1);
        number(ui, "Vertex thickness", vd::kVertexThickness, thickness.vertex, 0.5, 32.0, 0.5, 1);
        number(ui, "Hovered vertex thickness", vd::kVertexThicknessHover, thickness.vertexHover, 0.5, 32.0, 0.5, 1);
        number(ui, "Selected vertex thickness", vd::kVertexThicknessSelected, thickness.vertexSelected, 0.5, 32.0, 0.5, 1);
        number(ui, "UV seam thickness", vd::kUvSeamThickness, thickness.uvSeam, 1.0, 16.0, 0.5, 1);
        toggle(ui, "Edges on the symmetry plane in red", vd::kSymmetryEdges, true);
        toggle(ui, "Symmetry plane edges on selected objects in object mode", vd::kSymmetryEdgesObjectMode, true);
        number(ui, "Symmetry plane edge thickness", vd::kSymmetryEdgeThickness, thickness.symmetryEdge, 1.0, 16.0, 0.5, 1);
        section(ui, "Floating Shelf");
        choice(ui, "Opens at", "/floatingShelf/openAt",
               {{"Cursor", "cursor"}, {"Center of viewport", "viewport"}, {"Center of screen", "screen"}}, "viewport");
        number(ui, "Offset X (px)", "/floatingShelf/offsetX", 250.0, -2000.0, 2000.0, 10.0, 0);
        number(ui, "Offset Y (px)", "/floatingShelf/offsetY", 0.0, -2000.0, 2000.0, 10.0, 0);
        toggle(ui, "Remember last position (overrides Opens at)", "/floatingShelf/rememberPosition", false);
        toggle(ui, "Close when clicking elsewhere", "/floatingShelf/closeOnClickAway", false);
        toggle(ui, "Close after running a button", "/floatingShelf/closeAfterRun", false);
    });

    builtIn(r, "viewport", "Viewport", "general", [](Page& viewport) {
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
            addRow(viewport, "Samples", box, vd::kAaSamples);
        }
        section(viewport, "Grid");
        toggle(viewport, "Show grid", vd::kGrid, true);
        number(viewport, "Spacing (cm)", "/grid/spacing", 10.0, 0.1, 10000.0, 1.0, 1);
        section(viewport, "Display");
        toggle(viewport, "Poly count", "/viewport/polyCount", true);
        toggle(viewport, "Panel toolbar", vd::kToolbar, true);
        toggle(viewport, "Hiding a parent hides its children", scene::kInheritVisibilityPref, true);
        section(viewport, "Selection");
        toggle(viewport, "Marquee selects edges and faces it touches (off: only those whose centre it covers)", "/select/marqueeTouch", true);
        section(viewport, "Highlight Under Cursor");
        toggle(viewport, "Components", "/viewport/preselect/components", true);
        toggle(viewport, "Objects", "/viewport/preselect/objects", false);
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

        section(viewport, "Animation Ghosts");
        {
            namespace gh = scene::ghosts;
            toggle(viewport, "Show ghosts", gh::kEnabled, false);
            choice(viewport, "Ghost at", gh::kMode, {{"Previous and next keys", "keys"}, {"Every N frames", "frames"}}, "keys");
            choice(viewport, "Ghosts show", gh::kShow, {{"Meshes", "mesh"}, {"Skeletons", "skeleton"}, {"Both", "both"}}, "mesh");
            number(viewport, "Ghosts before", gh::kBefore, 2.0, 0.0, 50.0, 1.0, 0);
            number(viewport, "Ghosts after", gh::kAfter, 2.0, 0.0, 50.0, 1.0, 0);
            number(viewport, "Step (frames, or every Nth key)", gh::kStep, 1.0, 1.0, 1000.0, 1.0, 0);
            number(viewport, "Ghost opacity", gh::kOpacity, 0.3, 0.0, 1.0, 0.05);
            color(viewport, "Before color", gh::kBeforeColor, gh::kDefaultBeforeColor);
            color(viewport, "After color", gh::kAfterColor, gh::kDefaultAfterColor);
        }
    });

    builtIn(r, "manipulators", "Manipulators", "general", [](Page& manip) {
        section(manip, "Manipulators");
        number(manip, "Size (pixels)", "/manipulator/size", 100.0, 30.0, 400.0, 5.0, 0);
        choice(manip, "Move space", "/manipulator/moveSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}, {"Normal", "normal"}}, "world");
        choice(manip, "Rotate space", "/manipulator/rotateSpace", {{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}, {"Component", "component"}}, "world");
        choice(manip, "Scale space (components)", "/manipulator/scaleSpace", {{"World", "world"}, {"Object", "object"}, {"Component", "component"}}, "world");
        number(manip, "Rotate snap (degrees)", "/manipulator/rotateStep", 15.0, 0.1, 180.0, 1.0, 1);
        section(manip, "Soft Select");
        toggle(manip, "Enabled", "/softSelect/enabled", false);
        number(manip, "Radius (cm)", "/softSelect/radius", 25.0, 0.01, 100000.0, 1.0);
        addRow(manip, "Falloff", new SoftSelectPanel, "/softSelect/falloff");
    });

    builtIn(r, "measure", "Measure", "general", [](Page& measure) {
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
    });

    builtIn(r, "timeSlider", "Time Slider", "anim", [](Page& timeSlider) {
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
                    ops::execute("playbackOptions", {{"playbackSpeed", box->currentData().toString().toStdString()}}, CallSource::UI);
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
            addRow(timeSlider, "Playback speed", box, "/playback/speed");
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
                    ops::execute("playbackOptions", {{"playbackSpeed", "custom"}, {"speedMultiplier", v}}, CallSource::UI);
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
            addRow(timeSlider, "Custom speed multiplier", spin, "/playback/speedMultiplier");
        }
    });

    builtIn(r, "naming", "Naming", "naming", [](Page& naming) {
        section(naming, "Conventions");
        toggle(naming, "Enforce conventions", "/enabled", false);
        choice(naming, "When a name breaks them", "/mode", {{"Fix it", "fix"}, {"Warn", "warn"}, {"Refuse", "block"}}, "fix");
        section(naming, "Uniqueness");
        // Off, names only need to be unique among siblings and paths (Parent|Child) tell the rest apart.
        toggle(naming, "Unique node names", "/uniqueNames", false);
        section(naming, "Case");
        choice(naming, "Joints", "/rules/joint", {{"snake_case", "snake_case"}, {"PascalCase", "PascalCase"}, {"camelCase", "camelCase"}}, "snake_case");
        choice(naming, "Everything else", "/rules/*", {{"PascalCase", "PascalCase"}, {"snake_case", "snake_case"}, {"camelCase", "camelCase"}}, "PascalCase");
        for (auto [label, type, fallback] : {std::tuple{"Allow underscores in joints", "joint", "snake_case"},
                                             {"Allow underscores in everything else", "*", "PascalCase"}})
        {
            toggle(naming, label, ("/allowUnderscores/" + std::string(type)).c_str(), true);
            naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
                ->widget()
                ->setToolTip("PascalCase and camelCase keep the underscores typed in a name: Arm_L, not ArmL.");
            showLastRowWhen(naming, [type, fallback] { return namingUnderscores(type, fallback, false); });
        }
        auto anyUnderscores = [] { return namingUnderscores("joint", "snake_case", true) || namingUnderscores("*", "PascalCase", true); };
        toggle(naming, "Underscores only before suffixes", "/underscoreSuffixesOnly", true);
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("An underscore must be followed by a listed suffix or a number: Arm_L and Wheel_LF stay, Dog_Leg_B becomes DogLeg_B.");
        showLastRowWhen(naming, anyUnderscores);
        text(naming, "Suffixes", "/underscoreSuffixes", scene::NamingPolicy::defaults()["underscoreSuffixes"].get<std::string>());
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("Comma-separated. Matched ignoring case and written as listed, so arm_l becomes Arm_L.");
        auto suffixesOnly = [anyUnderscores] { return anyUnderscores() && PrefStore::get().value<bool>("naming", "/underscoreSuffixesOnly", true); };
        showLastRowWhen(naming, suffixesOnly);
        toggle(naming, "Allow suffixes as prefixes", "/underscorePrefixes", false);
        naming.form->itemAt(naming.form->rowCount() - 1, QFormLayout::FieldRole)
            ->widget()
            ->setToolTip("A listed suffix may also lead the name before an underscore: L_Arm stays, rather than becoming LArm.");
        showLastRowWhen(naming, suffixesOnly);
    });

    builtIn(r, "sourceControl", "Source Control", "sourceControl", [](Page& sc) {
        section(sc, "Provider");
        choice(sc, "Provider", "/provider", {{"Automatic", "auto"}, {"Perforce", "perforce"}, {"Git", "git"}, {"None", "none"}}, "auto");
        section(sc, "Saving");
        choice(sc, "Saving a read-only file", "/autoCheckout", {{"Ask first", "prompt"}, {"Check out", "always"}, {"Do nothing", "never"}}, "prompt");
        toggle(sc, "Mark new files for add", "/autoAdd", true);
        section(sc, "Perforce");
        text(sc, "P4PORT", "/perforce/port", "");
        text(sc, "P4USER", "/perforce/user", "");
        text(sc, "P4CLIENT", "/perforce/client", "");
    });

    builtIn(r, "unreal", "Unreal", "unreal", [](Page& unreal) {
        section(unreal, "Sync");
        toggle(unreal, "Sync mapped assets on save", "/autoSync", true);
        text(unreal, "Export cache folder", "/cacheDir", "");
    });

    builtIn(r, "scripting", "Scripting", "scriptEditor", [](Page& script) {
        section(script, "Script Editor");
        toggle(script, "Relative line numbers", CodeEditor::kRelativeLineNumbersPref, CodeEditor::kRelativeLineNumbersDefault);
        toggle(script, "Vim keybindings", "/vim", false);
        toggle(script, "Monotone", palette::kMonotonePref, false);
        const int monotoneRow = script.form->rowCount() - 1;
        toggle(script, "App Monotone overrides script editor", palette::kFollowAppMonotonePref, true);
        disableRowWhen(script, monotoneRow, palette::monotoneForcedByApp,
                       "The app is Monotone and \"App Monotone overrides script editor\" is on, so the script editor stays grey");

        section(script, "Formatting");
        namespace fk = format::key;
        const format::Settings d;
        auto tip = [&script](const char* text) {
            script.form->itemAt(script.form->rowCount() - 1, QFormLayout::FieldRole)->widget()->setToolTip(text);
        };
        toggle(script, "Format on save", fk::onSave, d.onSave);
        toggle(script, "Organize imports on save", fk::organizeImportsOnSave, d.organizeImportsOnSave);
        tip("Sorts and groups imports with ruff's isort rules (Ctrl+Alt+O does it on demand).");
        toggle(script, "Fix all on save", fk::fixAllOnSave, d.fixAllOnSave);
        tip("Applies ruff's safe fixes, such as removing unused imports.");
        toggle(script, "Format on paste", fk::onPaste, d.onPaste);
        tip("Formats the statements the pasted text lands in.");
        toggle(script, "Format on type", fk::onType, d.onType);
        tip("Formats a statement once it is complete: on Enter at the end of a line, or on typing the bracket or colon that ends it.");
        toggle(script, "Format on focus loss", fk::onFocusLoss, d.onFocusLoss);
        toggle(script, "Trim trailing whitespace on save", fk::trimTrailingWhitespace, d.trimTrailingWhitespace);
        tip("Leaves strings and the caret's line alone.");
        toggle(script, "Ensure final newline on save", fk::finalNewline, d.finalNewline);
        toggle(script, "Use project ruff config", fk::useProjectConfig, d.useProjectConfig);
        tip("A ruff.toml, .ruff.toml or pyproject.toml with [tool.ruff] beside the script or above it overrides the style below.");
        number(script, "Line length", fk::lineLength, d.lineLength, 40, 320, 1, 0);
        choice(script, "Quote style", fk::quoteStyle, {{"Double", "double"}, {"Single", "single"}, {"Preserve", "preserve"}},
               d.quoteStyle.toStdString());
        number(script, "Indent width", fk::indentWidth, d.indentWidth, 1, 8, 1, 0);
        tip("Indentation ruff formats with. Tab in the editor always indents by 4.");

        section(script, "Scene Scripts");
        choice(script, "Scripts set to run on open", "/sceneScriptsOnOpen", {{"Ask first", "ask"}, {"Never run", "never"}}, "ask");
    });
}
} // namespace

namespace prefPages
{
Registry<Page>& registry()
{
    static Registry<Page> r;
    static bool builtIns = [] {
        // Lila's own, even when an extension's init is what first asks for the registry.
        owners::OwnerScope lila({});
        addBuiltIns(r);
        return true;
    }();
    (void)builtIns;
    return r;
}

void add(Page page, std::string owner)
{
    std::string id = page.id;
    registry().put(std::move(id), std::move(page), std::move(owner));
}

uint64_t add(ext::Context& ctx, Page page)
{
    if (const std::string* owner = registry().ownerOf(page.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "preferences page '" + page.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = page.id;
    add(std::move(page), ctx.owner());
    return ctx.track("preferencesPage:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

bool remove(const std::string& id)
{
    return registry().remove(id);
}

std::string PageBuilder::file() const
{
    return rowsOf(mPage).file;
}

void PageBuilder::section(const std::string& title)
{
    lila::ui::section(rowsOf(mPage), title.c_str());
}

void PageBuilder::toggle(const std::string& label, const std::string& key, bool fallback)
{
    lila::ui::toggle(rowsOf(mPage), label.c_str(), key.c_str(), fallback);
}

void PageBuilder::number(const std::string& label, const std::string& key, double fallback, double lo, double hi, double step, int decimals)
{
    lila::ui::number(rowsOf(mPage), label.c_str(), key.c_str(), fallback, lo, hi, step, decimals);
}

void PageBuilder::choice(const std::string& label, const std::string& key, const std::vector<std::pair<std::string, std::string>>& options,
                         const std::string& fallback)
{
    std::vector<std::pair<QString, std::string>> opts;
    for (auto& [text, value] : options)
        opts.emplace_back(QString::fromStdString(text), value);
    lila::ui::choice(rowsOf(mPage), label.c_str(), key.c_str(), std::move(opts), fallback);
}

void PageBuilder::text(const std::string& label, const std::string& key, const std::string& fallback, bool fileButton)
{
    lila::ui::text(rowsOf(mPage), label.c_str(), key.c_str(), fallback, fileButton);
}

void PageBuilder::color(const std::string& label, const std::string& key, const std::string& fallback)
{
    lila::ui::color(rowsOf(mPage), label.c_str(), key, fallback);
}

void PageBuilder::row(const std::string& label, QWidget* field, const std::string& key)
{
    addRow(rowsOf(mPage), QString::fromStdString(label), field, key);
}
} // namespace prefPages

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Preferences");
    resize(840, 520);
    auto* v = new QVBoxLayout(this);
    auto* search = new QLineEdit(this);
    search->setPlaceholderText("Search preferences");
    search->setClearButtonEnabled(true);
    search->addAction(icon("search"), QLineEdit::LeadingPosition);
    search->installEventFilter(new ClearOnEscape(search));
    v->addWidget(search);
    auto* tabs = new QTabWidget(this);
    v->addWidget(tabs, 1);

    auto* resultsView = new QScrollArea(this);
    resultsView->setWidgetResizable(true);
    resultsView->setFrameShape(QFrame::NoFrame);
    resultsView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* results = new QWidget;
    auto* resultsLayout = new QVBoxLayout(results);
    auto* grid = new QGridLayout;
    grid->setColumnStretch(2, 1);
    resultsLayout->addLayout(grid);
    auto* empty = new QLabel("No matching preferences");
    resultsLayout->addWidget(empty);
    resultsLayout->addStretch(1);
    resultsView->setWidget(results);
    resultsView->hide();
    v->addWidget(resultsView, 1);
    auto prefSearch = std::make_shared<PrefSearch>(tabs, resultsView, grid, empty);
    std::vector<PrefRow>& rows = prefSearch->rows;
    connect(search, &QLineEdit::textChanged, resultsView, [prefSearch](const QString& text) { prefSearch->apply(text); });

    for (const prefPages::Page* def : prefPages::registry().allInOrder())
    {
        if (!def->build)
            continue;
        Page p = page(tabs, &rows, def->title.c_str(), def->file.c_str());
        prefPages::PageBuilder builder(&p);
        try
        {
            def->build(builder);
        }
        catch (const std::exception& e)
        {
            log::error("preferences page {}: {}", def->id, e.what());
        }
    }

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
