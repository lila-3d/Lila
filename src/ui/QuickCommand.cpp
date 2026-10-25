#include "ui/QuickCommand.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/CommandHistory.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace lila::ui::quickCommand
{
namespace
{
constexpr const char* kPrefs = "commandLine";
constexpr int kHistoryCap = 200;

CommandHistory& history()
{
    static CommandHistory h = [] {
        CommandHistory loaded(kHistoryCap);
        QStringList entries;
        for (const std::string& s : PrefStore::get().value<std::vector<std::string>>(kPrefs, "/history", {}))
            entries.append(QString::fromStdString(s));
        loaded.setEntries(entries);
        return loaded;
    }();
    return h;
}

void saveHistory()
{
    std::vector<std::string> out;
    for (const QString& s : history().entries())
        out.push_back(s.toStdString());
    PrefStore::get().set(kPrefs, "/history", out);
}

void runLine(const QString& code)
{
    if (!python::initialized())
    {
        log::error("Python is not available");
        return;
    }
    history().add(code);
    saveHistory();
    log::write(LogLevel::Echo, code.toStdString() + "\n");
    python::run(code.toStdString(), "<command>");
}

class CommandBox : public QFrame
{
public:
    explicit CommandBox(QWidget* over) : QFrame(over, Qt::Popup)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        mEdit = new QLineEdit(this);
        mEdit->setFont(monoFont());
        mEdit->setPlaceholderText("Python");
        mEdit->installEventFilter(this);
        lay->addWidget(mEdit);
        setFocusProxy(mEdit);

        history().resetWalk();
        mEdit->setText(history().last());
        mEdit->selectAll();

        int w = std::min(640, over->width() - 40);
        resize(w, sizeHint().height());
        move(over->mapToGlobal(QPoint((over->width() - w) / 2, over->height() - height() - 48)));
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj != mEdit || e->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key())
        {
        case Qt::Key_Up: mEdit->setText(history().older()); return true;
        case Qt::Key_Down: mEdit->setText(history().newer()); return true;
        case Qt::Key_Escape: close(); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        {
            QString code = mEdit->text();
            close();
            if (!code.trimmed().isEmpty())
                runLine(code);
            return true;
        }
        default: return false;
        }
    }

private:
    QLineEdit* mEdit = nullptr;
};

QPointer<CommandBox>& instance()
{
    static QPointer<CommandBox> box;
    return box;
}
} // namespace

void toggle(QWidget* over)
{
    QPointer<CommandBox>& box = instance();
    if (box && box->isVisible())
    {
        box->close();
        return;
    }
    box = new CommandBox(over);
    box->show();
    box->setFocus();
}

void registerActions(QWidget* over)
{
    QPointer<QWidget> window(over);
    actions::add({.id = "window.commandLine", .label = "Command Line", .category = "Windows", .run = [window] {
                      if (window)
                          toggle(window);
                  },
                  .doc = "A one-line Python field: Enter runs the line like the script editor does and closes, Esc closes. "
                         "Opens on the last command; Up and Down walk the history.",
                  .python = "lila.ui.run_action(\"window.commandLine\")"});
}
} // namespace lila::ui::quickCommand
