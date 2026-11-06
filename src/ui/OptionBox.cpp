#include "ui/OptionBox.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DocsUi.h"
#include "ui/FlagFields.h"
#include "ui/Icons.h"
#include "ui/Shelf.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <map>

namespace lila::ui::optionBox
{
namespace opt = ops::options;

namespace
{
constexpr const char* kBoxProperty = "lilaOptionBox";
constexpr const char* kDecorated = "lilaOptionBoxMenu";

/** Commands whose flags are switches of a mode or a selection, not settings worth a dialog. */
bool settingless(const std::string& command)
{
    static const QSet<QString> skip = {"undo", "redo", "select", "selectMode", "newScene", "setSymmetry", "smoothPreview", "show", "hide",
                                       "toggleVisibility", "convertSelection", "pickWalk", "symmetrize", "symmetryRebuild"};
    return skip.contains(QString::fromStdString(command));
}

/** Trailing space in the shortcut column that the square is painted over. */
QString padding()
{
    return QString(3, QChar(0x2003));
}

QWidget* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->inherits("QMainWindow") && w->isVisible())
            return w;
    return nullptr;
}

class OptionsDialog : public QDialog
{
public:
    OptionsDialog(const Action& a, QWidget* parent)
        : QDialog(parent), mAction(a.id), mCommand(a.command), mCall(a.call.is_object() ? a.call : json::object())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QString::fromStdString(title(a)) + " Options");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 8);
        root->setSpacing(6);

        auto* bar = new QMenuBar(this);
        QMenu* edit = bar->addMenu("Edit");
        edit->addAction("Save Settings", this, [this] { saveSettings(); })
            ->setToolTip("Remembers these values for the menu item, hotkeys and shelf buttons without running the command.");
        edit->addAction("Reset Settings", this, [this] { resetSettings(); });
        edit->setToolTipsVisible(true);
        QMenu* help = bar->addMenu("Help");
        help->addAction("Copy Python", this, [this] {
            QGuiApplication::clipboard()->setText(QString::fromStdString(ops::toPython(mCommand, pendingCall())));
        });
        root->setMenuBar(bar);

        const opt::Override* over = opt::findOverride(mCommand);
        std::string docsPage = over ? over->docs : std::string();
        if (!a.doc.empty() || !docsPage.empty())
        {
            auto* header = new QHBoxLayout();
            header->setContentsMargins(10, 4, 10, 0);
            auto* doc = new QLabel(QString::fromStdString(a.doc), this);
            doc->setWordWrap(true);
            doc->setStyleSheet("color: #9a9a9a;");
            header->addWidget(doc, 1);
            if (!docsPage.empty())
                header->addWidget(docs::helpButton(docsPage, this), 0, Qt::AlignTop);
            root->addLayout(header);
        }

        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* body = new QWidget(scroll);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(10, 4, 10, 4);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        scroll->setWidget(body);
        root->addWidget(scroll, 1);

        if (const opt::Override* o = opt::findOverride(mCommand); o && !o->fills.empty())
        {
            auto* fillRow = new QHBoxLayout();
            fillRow->setContentsMargins(10, 4, 10, 0);
            // A few fills are buttons; more are a menu, so the row doesn't widen the box.
            if (o->fills.size() > 3)
            {
                auto* menu = new QComboBox(this);
                menu->addItem(tr("Preset"));
                for (const opt::Override::Fill& fill : o->fills)
                {
                    menu->addItem(QString::fromStdString(fill.label));
                    menu->setItemData(menu->count() - 1, QString::fromStdString(fill.tip), Qt::ToolTipRole);
                }
                connect(menu, &QComboBox::activated, this, [this, menu, fills = o->fills](int index) {
                    if (index > 0)
                        fillFrom(fills[size_t(index - 1)].values);
                    menu->setCurrentIndex(0);
                });
                fillRow->addWidget(menu);
            }
            else
                for (const opt::Override::Fill& fill : o->fills)
                {
                    auto* b = new QPushButton(QString::fromStdString(fill.label), this);
                    b->setToolTip(QString::fromStdString(fill.tip));
                    connect(b, &QPushButton::clicked, this, [this, values = fill.values] { fillFrom(values); });
                    fillRow->addWidget(b);
                }
            fillRow->addStretch(1);
            root->addLayout(fillRow);
        }

        mValues = opt::values(mCommand, mCall);
        const json& values = mValues;
        std::vector<opt::Field> fields = opt::fields(mCommand, mCall);
        // Sections keep the order they first appear in; a field without one joins the untitled group at the top.
        std::vector<std::string> sections = {""};
        for (const opt::Field& f : fields)
            if (std::find(sections.begin(), sections.end(), f.section) == sections.end())
                sections.push_back(f.section);
        int row = 0;
        for (const std::string& section : sections)
        {
            bool any = std::any_of(fields.begin(), fields.end(), [&](const opt::Field& f) { return f.section == section; });
            if (!any)
                continue;
            if (!section.empty())
            {
                auto* header = new QLabel(QString::fromStdString(section), body);
                QFont hf = header->font();
                hf.setWeight(QFont::DemiBold);
                header->setFont(hf);
                header->setContentsMargins(0, row ? 8 : 0, 0, 2);
                grid->addWidget(header, row++, 0, 1, 2);
            }
            for (const opt::Field& f : fields)
            {
                if (f.section != section)
                    continue;
                std::string flag = f.flag;
                FlagFieldWidgets w = makeFlagField(f, values.value(flag, f.defaultValue), [this, flag](const json& v) {
                    mValues[flag] = v;
                }, body);
                mSetters[flag] = w.set;
                if (w.label)
                {
                    grid->addWidget(w.label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
                    grid->addWidget(w.field, row, 1);
                }
                else
                    grid->addWidget(w.field, row, 1);
                ++row;
                if (QLabel* note = noteFor(flag, body))
                    grid->addWidget(note, row++, 1);
            }
        }
        grid->setRowStretch(row, 1);

        auto* buttons = new QHBoxLayout();
        buttons->setContentsMargins(10, 0, 10, 0);
        auto* applyClose = new QPushButton("Apply and Close", this);
        auto* apply = new QPushButton("Apply", this);
        auto* closeButton = new QPushButton("Close", this);
        applyClose->setDefault(true);
        for (QPushButton* b : {applyClose, apply, closeButton})
            buttons->addWidget(b, 1);
        root->addLayout(buttons);
        connect(applyClose, &QPushButton::clicked, this, [this] {
            run();
            close();
        });
        connect(apply, &QPushButton::clicked, this, [this] { run(); });
        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

        body->adjustSize();
        resize(std::max(420, body->sizeHint().width() + 40), std::min(720, body->sizeHint().height() + 140));
    }

private:
    static std::string title(const Action& a)
    {
        std::string t = a.label;
        if (t.size() > 3 && t.ends_with("..."))
            t.resize(t.size() - 3);
        return t;
    }

    void run()
    {
        saveSettings();
        // An action that gathers its own arguments (the selection, a pick order) runs as itself, with these settings.
        if (const Action* a = actions::find(mAction); a && a->run)
            actions::run(json(mAction));
        else
            actions::runWithOptions(mCommand, mCall);
    }

    QLabel* noteFor(const std::string& flag, QWidget* parent) const
    {
        const opt::Override* o = opt::findOverride(mCommand);
        if (!o)
            return nullptr;
        auto it = o->notes.find(flag);
        if (it == o->notes.end() || !it->second)
            return nullptr;
        std::string text;
        try
        {
            text = it->second();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
        }
        if (text.empty())
            return nullptr;
        auto* label = new QLabel(QString::fromStdString(text), parent);
        label->setWordWrap(true);
        label->setStyleSheet("color: #9a9a9a;");
        return label;
    }

    /** Stores the fields' values where Apply does, so the menu item, hotkeys and shelf run with them. */
    void saveSettings()
    {
        // A line edit commits on focus loss, which a button click or menu item may not have caused yet.
        if (QWidget* f = focusWidget())
            f->clearFocus();
        for (auto& [flag, v] : mValues.items())
            opt::setValue(mCommand, flag, v);
    }

    /** Sets the fields a fill button gives values for; the rest keep theirs. */
    void fillFrom(const std::function<json()>& values)
    {
        json given;
        try
        {
            given = values();
        }
        catch (const std::exception& e)
        {
            log::warn("{}", e.what());
            return;
        }
        if (!given.is_object())
            return;
        for (auto& [flag, v] : given.items())
        {
            mValues[flag] = v;
            if (auto it = mSetters.find(flag); it != mSetters.end() && it->second)
                it->second(v);
        }
    }

    void resetSettings()
    {
        opt::reset(mCommand);
        mValues = opt::values(mCommand, mCall);
        for (auto& [flag, set] : mSetters)
            if (set && mValues.contains(flag))
                set(mValues[flag]);
    }

    /** The call Apply would make with the fields as they stand. */
    json pendingCall() const
    {
        json call = mCall;
        for (const opt::Field& f : opt::fields(mCommand, mCall))
            if (mValues.contains(f.flag) && mValues[f.flag] != f.defaultValue && !call.contains(f.flag))
                call[f.flag] = mValues[f.flag];
        return call;
    }

    std::string mAction;
    std::string mCommand;
    json mCall;
    /** The fields' values, stored only by Apply or Save Settings so Close leaves the remembered ones alone. */
    json mValues;
    std::map<std::string, std::function<void(const json&)>> mSetters;
};

std::map<std::string, QPointer<OptionsDialog>>& dialogs()
{
    static std::map<std::string, QPointer<OptionsDialog>> d;
    return d;
}

Registry<std::function<void()>>& customs()
{
    static Registry<std::function<void()>> c;
    return c;
}

/** Where the square sits on a row: the right end of the shortcut column, clear of the submenu arrow margin. */
QRect boxRect(const QMenu* menu, QAction* a)
{
    QRect r = menu->actionGeometry(a);
    int side = std::min(r.height() - 6, menu->fontMetrics().height());
    int padW = menu->fontMetrics().horizontalAdvance(padding());
    int rightMargin = 15;
    QRect box(0, 0, side, side);
    box.moveCenter(QPoint(r.right() - rightMargin - padW / 2, r.center().y()));
    return box;
}

void closeMenus()
{
    while (QWidget* p = QApplication::activePopupWidget())
        p->close();
}

class MenuFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        auto* menu = qobject_cast<QMenu*>(obj);
        if (!menu)
            return false;
        switch (e->type())
        {
        case QEvent::Paint:
        {
            obj->event(e);
            QPainter p(menu);
            QPoint mouse = menu->mapFromGlobal(QCursor::pos());
            for (QAction* a : menu->actions())
                if (a->property(kBoxProperty).toBool() && a->isVisible())
                {
                    QRect box = boxRect(menu, a);
                    bool hot = box.adjusted(-4, -4, 4, 4).contains(mouse);
                    icon("app-window").paint(&p, box, Qt::AlignCenter, !a->isEnabled() ? QIcon::Disabled : hot ? QIcon::Active : QIcon::Normal);
                }
            return true;
        }
        case QEvent::MouseMove:
            menu->update();
            return false;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        {
            auto* me = static_cast<QMouseEvent*>(e);
            QAction* a = menu->actionAt(me->position().toPoint());
            // Any button: hotbox menus are opened and released with whichever button pressed the menu name.
            if (!a || !a->property(kBoxProperty).toBool() || !(me->button() & (Qt::LeftButton | Qt::MiddleButton | Qt::RightButton)))
                return false;
            std::string id = optionsAt(menu, a, int(me->globalPosition().x()), me->modifiers() & Qt::ShiftModifier);
            if (id.empty())
                return false;
            if (e->type() == QEvent::MouseButtonRelease)
            {
                closeMenus();
                if (shelves::addModifierHeld())
                    shelves::addToCurrent(json{{"optionBox", id}});
                else
                    QTimer::singleShot(0, [id] { open(id); });
            }
            return true;
        }
        default: return false;
        }
    }
};

MenuFilter* menuFilter()
{
    static QPointer<MenuFilter> f;
    if (!f)
        f = new MenuFilter(qApp);
    return f;
}

void markRows(QMenu* menu)
{
    QString pad = padding();
    for (QAction* a : menu->actions())
    {
        if (QMenu* sub = a->menu())
        {
            decorateMenu(sub);
            continue;
        }
        std::string id = a->data().toString().toStdString();
        bool box = !id.empty() && available(id);
        a->setProperty(kBoxProperty, box);
        if (!box)
            continue;
        QString text = a->text();
        if (!text.endsWith(pad))
            a->setText(text + (text.contains('\t') ? "" : "\t") + pad);
        QString hint = "Click the square at the right, or Shift+click, for options.";
        if (!a->toolTip().contains(hint))
            a->setToolTip(a->toolTip() + "\n" + hint);
    }
}
} // namespace

bool available(const std::string& actionId)
{
    if (customs().contains(actionId))
        return true;
    const Action* a = actions::find(actionId);
    return a && !a->command.empty() && !settingless(a->command) && opt::hasOptions(a->command, a->call);
}

std::string optionsAt(const QMenu* menu, QAction* a, int globalX, bool shift)
{
    if (!menu || !a || !a->property(kBoxProperty).toBool())
        return {};
    bool onBox = menu->mapFromGlobal(QPoint(globalX, 0)).x() >= boxRect(menu, a).left() - 6;
    // Ctrl+Shift on the item itself adds it to the shelf rather than opening its options.
    if (!onBox && (!shift || shelves::addModifierHeld()))
        return {};
    return a->data().toString().toStdString();
}

Registry<std::function<void()>>& customRegistry()
{
    return customs();
}

void addCustom(const std::string& actionId, std::function<void()> open, std::string owner)
{
    customs().put(actionId, std::move(open), std::move(owner));
}

uint64_t addCustom(ext::Context& ctx, const std::string& actionId, std::function<void()> open)
{
    if (const std::string* owner = customs().ownerOf(actionId); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "the option box of '" + actionId + "' already belongs to " + *owner);
        return 0;
    }
    addCustom(actionId, std::move(open), ctx.owner());
    return ctx.track("optionBox:" + actionId, [actionId, own = ctx.owner()] {
        if (const std::string* o = customs().ownerOf(actionId); o && *o == own)
            customs().remove(actionId);
    });
}

bool removeCustom(const std::string& actionId)
{
    return customs().remove(actionId);
}

void open(const std::string& actionId)
{
    if (const std::function<void()>* custom = customs().find(actionId))
    {
        std::function<void()> fn = *custom;
        fn();
        return;
    }
    const Action* a = actions::find(actionId);
    if (!a || a->command.empty())
        return;
    QPointer<OptionsDialog>& d = dialogs()[actionId];
    if (!d)
        d = new OptionsDialog(*a, mainWindow());
    d->show();
    d->raise();
    d->activateWindow();
    flashLocate(d);
}

/** The label of an action making this call (or running this command), else the command's name in words. */
std::string titleFor(const std::string& command, const nlohmann::json& call)
{
    std::string any;
    for (const Action* a : actions::all())
        if (a->command == command)
        {
            if (a->call == call)
                return a->label;
            if (any.empty())
                any = a->label;
        }
    if (!any.empty())
        return any;
    std::string words;
    for (char c : command)
    {
        if (std::isupper(static_cast<unsigned char>(c)) && !words.empty())
            words += ' ';
        words += words.empty() ? char(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    return words;
}

bool shiftOpens(const std::string& command, const nlohmann::json& call, const std::string& title)
{
    if (!(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier))
        return false;
    return openCommand(title.empty() ? titleFor(command, call) : title, command, call);
}

bool openCommand(const std::string& title, const std::string& command, const nlohmann::json& call)
{
    const nlohmann::json fixed = call.is_object() ? call : nlohmann::json::object();
    if (settingless(command) || !opt::hasOptions(command, fixed))
        return false;
    const std::string key = "command:" + command + fixed.dump();
    QPointer<OptionsDialog>& d = dialogs()[key];
    if (!d)
    {
        const CommandDef* def = ops::find(command);
        d = new OptionsDialog(Action{.id = key, .label = title, .doc = def ? def->doc : std::string(), .command = command, .call = fixed},
                              mainWindow());
    }
    d->show();
    d->raise();
    d->activateWindow();
    flashLocate(d);
    return true;
}

void decorateMenu(QMenu* menu)
{
    if (!menu || menu->property(kDecorated).toBool())
        return;
    menu->setProperty(kDecorated, true);
    menu->installEventFilter(menuFilter());
    // Connected after the hotkey refresh that rewrites each row's text, so the padding goes on last.
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { markRows(menu); });
}

QWidget* withOptionBox(QToolButton* button, const std::string& actionId, QWidget* parent)
{
    if (!button || !available(actionId))
        return button;
    auto* box = new QWidget(parent);
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    button->setParent(box);
    h->addWidget(button);
    auto* square = new QToolButton(box);
    square->setIcon(icon("app-window"));
    square->setIconSize(QSize(12, 12));
    square->setAutoRaise(true);
    square->setFixedWidth(16);
    square->setToolTip("Options for " + button->text() + " (or Shift+click the button)");
    QObject::connect(square, &QToolButton::clicked, square, [actionId] { open(actionId); });
    h->addWidget(square);
    return box;
}

void install()
{
    for (auto [id, command] : {std::pair{"rig.orientJoint", "orientJoint"}, {"rig.mirrorJoints", "mirrorJoints"}, {"rig.insertJoint", "insertJoint"},
                               {"rig.bindSkin", "skinCluster"}, {"rig.mirrorSkinWeights", "mirrorSkinWeights"},
                               {"rig.copySkinWeights", "copySkinWeights"}, {"rig.pruneWeights", "pruneWeights"},
                               {"rig.parentConstraint", "parentConstraint"}, {"rig.pointConstraint", "pointConstraint"},
                               {"rig.orientConstraint", "orientConstraint"}, {"rig.aimConstraint", "aimConstraint"}, {"rig.pin", "pin"},
                               {"rig.bakePin", "bakePin"}, {"rig.splineChain", "splineChain"}, {"rig.hoseLimb", "hoseLimb"},
                               {"rig.hoseFingers", "hoseFingers"}})
        actions::bindCommand(id, command);
    actions::bindCommand("rig.bindSkinHeatmap", "skinCluster", {{"method", "heatmap"}});
}
} // namespace lila::ui::optionBox
