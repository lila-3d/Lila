#include "ui/QuickCommand.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/CommandHistory.h"
#include "ui/Theme.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
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
    // A bare command name runs it, as a command line should, rather than printing the function.
    static const QRegularExpression bareName(R"(^\s*([A-Za-z_][\w.]*)\s*$)");
    if (QRegularExpressionMatch m = bareName.match(code); m.hasMatch())
    {
        std::string name = m.captured(1).toStdString();
        python::run(name + "() if callable(" + name + ") else " + name, "<command>");
        return;
    }
    python::run(code.toStdString(), "<command>");
}

class CommandBox : public QFrame
{
public:
    explicit CommandBox(QWidget* over) : QFrame(over, Qt::Tool | Qt::FramelessWindowHint), mOver(over)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        mEdit = new CodeEditor(this);
        mEdit->setGutterVisible(false);
        mEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        mEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setUntitledName("Command");
        mEdit->setPlaceholderText("Python");
        QFont font = monoFont();
        if (auto* editor = over->window()->findChild<ScriptEditor*>())
        {
            font = editor->editorFont();
            mEdit->setRuffExe(editor->ruffExe());
            mEdit->setLanguageClient(editor->languageClient());
        }
        mEdit->setEditorFont(font);
        mEdit->hostKeyHandler = [this](QKeyEvent* k) { return hostKey(k); };
        lay->addWidget(mEdit);
        setFocusProxy(mEdit);

        history().resetWalk();
        mEdit->setPlainText(history().last());
        mEdit->selectAll();

        int line = QFontMetrics(font).lineSpacing() + 2 * int(mEdit->document()->documentMargin()) + 2 * mEdit->frameWidth();
        mEdit->setFixedHeight(line);
        resize(width(), line + 8);
        mLayout = over->window()->findChild<ViewportLayout*>();
        // Follows the viewport as the window moves or the layout, docks or quad view change its size.
        over->window()->installEventFilter(this);
        if (mLayout)
        {
            mLayout->installEventFilter(this);
            for (ViewportWidget* v : mLayout->views())
                v->installEventFilter(this);
        }
        place();

        // Closes like a popup when focus goes elsewhere; the completion and doc popups never take focus.
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (now && now != this && !isAncestorOf(now))
                close();
        });
        connect(qApp, &QApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive)
                close();
        });
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            // After the event, once every widget in the layout has its new geometry.
            QTimer::singleShot(0, this, [this] { place(); });
            break;
        default: break;
        }
        return QFrame::eventFilter(obj, e);
    }

private:
    /** Centred just above the bottom edge of the active viewport (above its bottom bar), or of the window without one. */
    void place()
    {
        QWidget* anchor = mLayout && mLayout->active() && mLayout->active()->isVisible() ? mLayout->active() : mOver.data();
        if (!anchor)
            return;
        const int margin = anchor == mOver ? 48 : 8;
        int w = std::max(160, std::min(640, anchor->width() - 40));
        resize(w, height());
        move(anchor->mapToGlobal(QPoint((anchor->width() - w) / 2, anchor->height() - height() - margin)));
    }

    bool hostKey(QKeyEvent* k)
    {
        Qt::KeyboardModifiers mods = k->modifiers() & ~Qt::KeypadModifier;
        switch (k->key())
        {
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (mods != Qt::NoModifier)
                return false;
            mEdit->setPlainText(k->key() == Qt::Key_Up ? history().older() : history().newer());
            mEdit->moveCursor(QTextCursor::End);
            return true;
        case Qt::Key_Escape: close(); return true;
        case Qt::Key_QuoteLeft:
            if (mods != Qt::ControlModifier)
                return false;
            close();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        {
            if (mods != Qt::NoModifier)
                return false;
            QString code = mEdit->toPlainText();
            close();
            if (!code.trimmed().isEmpty())
                runLine(code);
            return true;
        }
        default: return false;
        }
    }

    CodeEditor* mEdit = nullptr;
    QPointer<QWidget> mOver;
    QPointer<ViewportLayout> mLayout;
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
    box->raise();
    box->activateWindow();
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
