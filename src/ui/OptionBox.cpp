#include "ui/OptionBox.h"

#include "cmds/CommandOptions.h"
#include "core/ExtensionContext.h"
#include "ui/Actions.h"
#include "ui/FlagFields.h"
#include "ui/Icons.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
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
#include <map>

namespace lila::ui::optionBox
{
namespace opt = cmds::options;

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
    OptionsDialog(const Action& a, QWidget* parent) : QDialog(parent), mCommand(a.command), mCall(a.call.is_object() ? a.call : json::object())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle(QString::fromStdString(title(a)) + " Options");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 8);
        root->setSpacing(6);

        auto* bar = new QMenuBar(this);
        QMenu* edit = bar->addMenu("Edit");
        edit->addAction("Reset Settings", this, [this] { resetSettings(); });
        QMenu* help = bar->addMenu("Help");
        help->addAction("Copy Python", this, [this] {
            QGuiApplication::clipboard()->setText(QString::fromStdString(cmds::toPython(mCommand, opt::callWith(mCommand, mCall))));
        });
        root->setMenuBar(bar);

        if (!a.doc.empty())
        {
            auto* doc = new QLabel(QString::fromStdString(a.doc), this);
            doc->setWordWrap(true);
            doc->setStyleSheet("color: #9a9a9a;");
            doc->setContentsMargins(10, 4, 10, 0);
            root->addWidget(doc);
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

        json values = opt::values(mCommand, mCall);
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
                    opt::setValue(mCommand, flag, v);
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
        // A line edit commits on focus loss, which a button click may not have caused yet.
        if (QWidget* f = focusWidget())
            f->clearFocus();
        actions::runWithOptions(mCommand, mCall);
    }

    void resetSettings()
    {
        opt::reset(mCommand);
        json values = opt::values(mCommand, mCall);
        for (auto& [flag, set] : mSetters)
            if (set && values.contains(flag))
                set(values[flag]);
    }

    std::string mCommand;
    json mCall;
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
    if (!onBox && !shift)
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
                               {"rig.orientConstraint", "orientConstraint"}, {"rig.aimConstraint", "aimConstraint"}})
        actions::bindCommand(id, command);
    actions::bindCommand("rig.bindSkinHeatmap", "skinCluster", {{"method", "heatmap"}});
}
} // namespace lila::ui::optionBox
