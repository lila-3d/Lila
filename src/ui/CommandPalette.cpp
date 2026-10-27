#include "ui/CommandPalette.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/CommandHistory.h"
#include "ui/Hotkeys.h"
#include "ui/OutlinerFilter.h"
#include "ui/Theme.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QListWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

namespace lila::ui::commandPalette
{
namespace
{
constexpr const char* kPrefs = "commandPalette";
constexpr int kHistoryCap = 200;
constexpr int kMaxResults = 8;

CommandHistory& history()
{
    static CommandHistory h = [] {
        CommandHistory loaded(kHistoryCap);
        auto saved = PrefStore::get().value<std::vector<std::string>>(kPrefs, "/history", {});
        if (saved.empty())
            saved = PrefStore::get().value<std::vector<std::string>>("commandLine", "/history", {});
        QStringList entries;
        for (const std::string& s : saved)
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
    // A bare command name runs it rather than printing the function.
    static const QRegularExpression bareName(R"(^\s*([A-Za-z_][\w.]*)\s*$)");
    if (QRegularExpressionMatch m = bareName.match(code); m.hasMatch())
    {
        std::string name = m.captured(1).toStdString();
        python::run(name + "() if callable(" + name + ") else " + name, "<command>");
        return;
    }
    python::run(code.toStdString(), "<command>");
}

struct PaletteResult
{
    std::string action;  // an action id, or empty for a command
    std::string command; // a command name
    QString title;
    QString detail;
    int score = 0;
};

/** A doc string's first sentence. */
QString firstSentence(const std::string& doc)
{
    QString s = QString::fromStdString(doc);
    if (int end = s.indexOf(". "); end > 0)
        s.truncate(end + 1);
    return s;
}

/** Calls, assignments, literals and statements are Python, not a search. */
bool looksLikePython(const QString& text)
{
    static const QRegularExpression code(R"([()=\[\]'"]|^\s*lila\.|^\s*(import|from|for|if|print)\b)");
    return code.match(text).hasMatch();
}

std::vector<PaletteResult> searchPalette(const QString& text)
{
    const std::string q = text.trimmed().toStdString();
    std::vector<PaletteResult> out;
    if (q.empty())
        return out;
    for (const Action* a : actions::all())
    {
        if (!a->run || a->label.empty() || (a->enabled && !a->enabled()))
            continue;
        // A label match beats one that needs the category to fit.
        const int score = std::max(outliner::fuzzyScore(q, a->label) * 2, outliner::fuzzyScore(q, a->category + " " + a->label));
        if (score <= 0)
            continue;
        QString detail = QString::fromStdString(a->category);
        if (QString key = Hotkeys::get().keyFor(a->id); !key.isEmpty())
            detail += "  " + key;
        out.push_back({a->id, {}, QString::fromStdString(actions::displayLabel(a->id)), detail, score});
    }
    for (const CommandDef* c : ops::all())
        if (int score = outliner::fuzzyScore(q, c->name); score > 0)
            out.push_back({{}, c->name, QString::fromStdString(c->name), firstSentence(c->doc), score});
    std::stable_sort(out.begin(), out.end(), [](const PaletteResult& a, const PaletteResult& b) { return a.score > b.score; });
    if (out.size() > size_t(kMaxResults))
        out.resize(kMaxResults);
    return out;
}

class PaletteBox : public QFrame
{
public:
    explicit PaletteBox(QWidget* over) : QFrame(over, Qt::Tool | Qt::FramelessWindowHint), mOver(over)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        lay->setSpacing(4);
        mResults = new QListWidget(this);
        mResults->setFocusPolicy(Qt::NoFocus);
        mResults->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mResults->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mResults->setUniformItemSizes(true);
        mResults->hide();
        connect(mResults, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
            mResults->setCurrentItem(it);
            runCurrent();
        });
        lay->addWidget(mResults);
        mEdit = new CodeEditor(this);
        mEdit->setGutterVisible(false);
        mEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        mEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        mEdit->setUntitledName("Command");
        mEdit->setPlaceholderText("Search actions and commands, or type Python");
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
        mFromHistory = true;
        connect(mEdit, &QPlainTextEdit::textChanged, this, [this] {
            if (!mWalking)
                mFromHistory = false;
            updateResults();
        });

        mLine = QFontMetrics(font).lineSpacing() + 2 * int(mEdit->document()->documentMargin()) + 2 * mEdit->frameWidth();
        mEdit->setFixedHeight(mLine);
        resize(width(), mLine + 8);
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
        const int rows = mResults->isVisibleTo(this) ? mResults->count() : 0;
        if (rows)
            mResults->setFixedHeight(rows * mResults->sizeHintForRow(0) + 2 * mResults->frameWidth());
        const int h = mLine + 8 + (rows ? mResults->height() + 4 : 0);
        int w = std::max(160, std::min(640, anchor->width() - 40));
        resize(w, h);
        move(anchor->mapToGlobal(QPoint((anchor->width() - w) / 2, anchor->height() - h - margin)));
    }

    /** Matches for the typed text, the best right above the field; none for a history recall or Python. */
    void updateResults()
    {
        const QString text = mEdit->toPlainText();
        mFound = mFromHistory || looksLikePython(text) ? std::vector<PaletteResult>{} : searchPalette(text);
        mResults->clear();
        for (auto it = mFound.rbegin(); it != mFound.rend(); ++it)
            mResults->addItem(it->title + "    " + it->detail);
        mResults->setVisible(!mFound.empty());
        if (!mFound.empty())
            mResults->setCurrentRow(mResults->count() - 1);
        place();
    }

    void runCurrent()
    {
        const int row = mResults->currentRow();
        if (row < 0 || row >= int(mFound.size()))
            return;
        const PaletteResult r = mFound[mFound.size() - 1 - size_t(row)];
        close();
        if (!r.action.empty())
            actions::run(nlohmann::json(r.action));
        else
            runLine(QString::fromStdString(r.command));
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
            if (mResults->isVisibleTo(this))
            {
                const int n = mResults->count();
                mResults->setCurrentRow((mResults->currentRow() + (k->key() == Qt::Key_Up ? -1 : 1) + n) % n);
                return true;
            }
            mWalking = true;
            mEdit->setPlainText(k->key() == Qt::Key_Up ? history().older() : history().newer());
            mWalking = false;
            mFromHistory = true;
            updateResults();
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
            // Shift+Enter runs the text as Python even when it matches something.
            if (mods != Qt::NoModifier && mods != Qt::ShiftModifier)
                return false;
            if (mods == Qt::NoModifier && mResults->isVisibleTo(this) && mResults->currentRow() >= 0)
            {
                runCurrent();
                return true;
            }
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
    QListWidget* mResults = nullptr;
    std::vector<PaletteResult> mFound;
    int mLine = 0;
    bool mFromHistory = false;
    bool mWalking = false;
    QPointer<QWidget> mOver;
    QPointer<ViewportLayout> mLayout;
};

QPointer<PaletteBox>& instance()
{
    static QPointer<PaletteBox> box;
    return box;
}
} // namespace

void toggle(QWidget* over)
{
    QPointer<PaletteBox>& box = instance();
    if (box && box->isVisible())
    {
        box->close();
        return;
    }
    box = new PaletteBox(over);
    box->show();
    box->raise();
    box->activateWindow();
    box->setFocus();
}

void registerActions(QWidget* over)
{
    QPointer<QWidget> window(over);
    actions::add({.id = "window.commandPalette", .label = "Command Palette", .category = "Windows", .run = [window] {
                      if (window)
                          toggle(window);
                  },
                  .doc = "Type to find any action or command and press Enter to run it, or type Python and press Enter to run "
                         "it like the script editor does. Shift+Enter always runs the text as Python. Opens on the last line; "
                         "Up and Down walk the history.",
                  .python = "lila.ui.run_action(\"window.commandPalette\")"});
}
} // namespace lila::ui::commandPalette
