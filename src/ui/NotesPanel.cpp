#include "ui/NotesPanel.h"

#include "core/Events.h"
#include "ops/Undo.h"
#include "scene/SceneData.h"
#include "ui/AchievementRules.h"
#include "ui/Deletion.h"
#include "ui/Icons.h"
#include "ui/Panels.h"

#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
using json = nlohmann::json;

constexpr const char* kNotesKey = "notes";

QToolButton* notesButton(QWidget* parent, const char* iconName, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    return b;
}

bool isTodoPage(const json& page)
{
    return page.value("kind", "note") == "todo";
}

std::string uniquePageName(const json& pages, const std::string& base, const json* except = nullptr)
{
    auto taken = [&](const std::string& n) {
        for (const json& p : pages)
            if (&p != except && p.value("name", "") == n)
                return true;
        return false;
    };
    if (!taken(base))
        return base;
    for (int i = 2;; ++i)
        if (std::string n = base + " " + std::to_string(i); !taken(n))
            return n;
}

void styleTodoItem(QListWidgetItem* item, bool done)
{
    QFont f = item->font();
    f.setStrikeOut(done);
    item->setFont(f);
    item->setForeground(done ? item->listWidget()->palette().color(QPalette::Disabled, QPalette::Text)
                             : item->listWidget()->palette().color(QPalette::Text));
}

/** Commits typing on focus loss and ends the typing session, so the next edit is a step of its own. */
class NotesFocusOut : public QObject
{
public:
    NotesFocusOut(QObject* parent, std::function<void()> fn) : QObject(parent), mFn(std::move(fn)) {}

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::FocusOut)
            mFn();
        return QObject::eventFilter(o, e);
    }

private:
    std::function<void()> mFn;
};

/** Ctrl+V of several lines adds each as an item (a line edit would keep only the first); Ctrl+C on the list copies. */
class TodoClipboard : public QObject
{
public:
    TodoClipboard(QObject* parent, std::function<bool(const QString&)> paste, std::function<void()> copy)
        : QObject(parent), mPaste(std::move(paste)), mCopy(std::move(copy))
    {
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::ShortcutOverride)
            return QObject::eventFilter(o, e);
        auto* k = static_cast<QKeyEvent*>(e);
        const bool list = qobject_cast<QListWidget*>(o);
        if (k->matches(QKeySequence::Paste))
        {
            const QString text = QGuiApplication::clipboard()->text();
            if (!list && !text.contains('\n'))
                return false;
            if (e->type() == QEvent::KeyPress)
                mPaste(text);
            e->accept();
            return true;
        }
        if (list && k->matches(QKeySequence::Copy))
        {
            if (e->type() == QEvent::KeyPress)
                mCopy();
            e->accept();
            return true;
        }
        return QObject::eventFilter(o, e);
    }

private:
    std::function<bool(const QString&)> mPaste;
    std::function<void()> mCopy;
};
} // namespace

NotesPanel::NotesPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(1);
    mPicker = new QComboBox(this);
    mPicker->setEditable(true);
    mPicker->setInsertPolicy(QComboBox::NoInsert);
    mPicker->lineEdit()->setPlaceholderText("Notes");
    mPicker->setToolTip("The note or list shown; edit the text to rename it");
    mPicker->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bar->addWidget(mPicker, 1);
    auto add = [&](const char* iconName, const QString& tip, auto fn) {
        QToolButton* b = notesButton(this, iconName, tip);
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    add("notes", "New note", [this] { addPage("note"); });
    add("checklist", "New todo list", [this] { addPage("todo"); });
    mClearDone = add("clear-all", "Remove the ticked items", [this] { clearDone(); });
    add("trash", "Delete this note or list", [this] { deleteCurrent(); });
    v->addLayout(bar);

    mStack = new QStackedWidget(this);
    mText = new QPlainTextEdit(mStack);
    mText->setPlaceholderText("Type a note. It saves with the scene.");
    mStack->addWidget(mText);

    auto* todo = new QWidget(mStack);
    auto* tv = new QVBoxLayout(todo);
    tv->setContentsMargins(0, 0, 0, 0);
    tv->setSpacing(2);
    mList = new QListWidget(todo);
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->setDragDropMode(QAbstractItemView::InternalMove);
    mList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    mList->setWordWrap(true);
    tv->addWidget(mList, 1);
    mNewItem = new QLineEdit(todo);
    mNewItem->setPlaceholderText("Add an item and press Enter, or paste a list");
    tv->addWidget(mNewItem);
    mStack->addWidget(todo);
    v->addWidget(mStack, 1);

    deletion::claim(mList, [this] {
        const QList<QListWidgetItem*> sel = mList->selectedItems();
        if (sel.isEmpty())
            return;
        mFilling = true;
        for (QListWidgetItem* it : sel)
            delete it;
        mFilling = false;
        commitItems();
    });

    auto* typing = new QTimer(this);
    typing->setSingleShot(true);
    typing->setInterval(700);
    connect(typing, &QTimer::timeout, this, [this] { commitText(); });
    connect(mText, &QPlainTextEdit::textChanged, this, [this, typing] {
        if (!mFilling)
            typing->start();
    });
    mText->installEventFilter(new NotesFocusOut(this, [this, typing] {
        if (typing->isActive())
        {
            typing->stop();
            commitText();
        }
        mTypingStep = 0;
    }));

    connect(mPicker, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (mFilling || i < 0)
            return;
        mTypingStep = 0;
        showPage(i);
    });
    connect(mPicker->lineEdit(), &QLineEdit::editingFinished, this, [this] { renameCurrent(); });
    connect(mList, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        if (mFilling)
            return;
        mFilling = true;
        styleTodoItem(it, it->checkState() == Qt::Checked);
        mFilling = false;
        commitItems();
        if (it->checkState() == Qt::Checked)
        {
            int items = 0, ticked = 0;
            for (int r = 0; r < mList->count(); ++r)
                if (mList->item(r)->flags() & Qt::ItemIsUserCheckable)
                {
                    ++items;
                    ticked += mList->item(r)->checkState() == Qt::Checked;
                }
            achievementRules::todoListTicked(items, ticked);
        }
    });
    connect(mList->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (!mFilling)
            commitItems();
    });
    connect(mNewItem, &QLineEdit::returnPressed, this, [this] { addItem(); });
    auto* clipboard = new TodoClipboard(this, [this](const QString& t) { return addLines(t); }, [this] { copyItems(); });
    mNewItem->installEventFilter(clipboard);
    mList->installEventFilter(clipboard);

    for (events::Event e : {events::Event::UndoStackChanged, events::Event::SceneNew, events::Event::SceneOpened})
        mSubs.push_back(events::subscribe(e, [this](const events::EventData&) { scheduleRefresh(); }));
    refresh();
}

NotesPanel::~NotesPanel()
{
    for (uint64_t s : mSubs)
        events::unsubscribe(s);
}

void NotesPanel::scheduleRefresh()
{
    if (mPending)
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

json* NotesPanel::current()
{
    return mCurrent >= 0 && mCurrent < int(mPages.size()) ? &mPages[size_t(mCurrent)] : nullptr;
}

void NotesPanel::refresh()
{
    json pages = scene::data::get(kNotesKey, json::object()).value("pages", json::array());
    if (!pages.is_array())
        pages = json::array();
    if (pages == mPages && mPicker->count() == int(mPages.size()))
        return;
    const std::string was = current() ? current()->value("name", "") : std::string();
    mPages = std::move(pages);
    int index = mPages.empty() ? -1 : std::clamp(mCurrent, 0, int(mPages.size()) - 1);
    for (size_t i = 0; i < mPages.size(); ++i)
        if (mPages[i].value("name", "") == was)
            index = int(i);
    showPage(index);
}

void NotesPanel::showPage(int index)
{
    mCurrent = index;
    mFilling = true;
    mPicker->clear();
    for (const json& p : mPages)
        mPicker->addItem(icon(isTodoPage(p) ? "checklist" : "notes"), QString::fromStdString(p.value("name", "")));
    mPicker->setCurrentIndex(index);

    const json* page = current();
    const bool todo = page && isTodoPage(*page);
    mStack->setCurrentIndex(todo ? 1 : 0);
    mClearDone->setVisible(todo);
    if (todo)
    {
        mList->clear();
        for (const json& item : page->value("items", json::array()))
        {
            auto* it = new QListWidgetItem(QString::fromStdString(item.value("text", "")), mList);
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
            const bool done = item.value("done", false);
            it->setCheckState(done ? Qt::Checked : Qt::Unchecked);
            styleTodoItem(it, done);
        }
    }
    else
    {
        const QString text = page ? QString::fromStdString(page->value("text", "")) : QString();
        if (mText->toPlainText() != text)
        {
            const int cursor = mText->textCursor().position();
            mText->setPlainText(text);
            QTextCursor c = mText->textCursor();
            c.setPosition(std::min(cursor, int(text.size())));
            mText->setTextCursor(c);
        }
    }
    mFilling = false;
}

void NotesPanel::store(const std::string& label, bool amend)
{
    UndoStack& undo = UndoStack::get();
    const bool amending = amend && mTypingStep && undo.newestId() == mTypingStep && undo.openAmend(label);
    if (!amending)
        undo.openChunk(label);
    scene::data::set(kNotesKey, mPages.empty() ? json(nullptr) : json{{"pages", mPages}});
    undo.closeChunk();
    mTypingStep = amend ? undo.newestId() : 0;
}

void NotesPanel::addPage(const std::string& kind)
{
    commitText();
    const bool todo = kind == "todo";
    json page = {{"name", uniquePageName(mPages, todo ? "Todo" : "Notes")}, {"kind", kind}};
    page[todo ? "items" : "text"] = todo ? json::array() : json("");
    mPages.push_back(std::move(page));
    store(todo ? "New Todo List" : "New Note");
    showPage(int(mPages.size()) - 1);
    (todo ? static_cast<QWidget*>(mNewItem) : mText)->setFocus();
}

void NotesPanel::renameCurrent()
{
    if (mFilling)
        return;
    const std::string name = mPicker->currentText().trimmed().toStdString();
    json* page = current();
    if (!page)
    {
        if (name.empty())
            return;
        mPages.push_back({{"name", name}, {"kind", "note"}, {"text", ""}});
        store("New Note");
        showPage(0);
        return;
    }
    if (name.empty() || name == page->value("name", ""))
    {
        showPage(mCurrent);
        return;
    }
    (*page)["name"] = uniquePageName(mPages, name, page);
    store("Rename Note");
    showPage(mCurrent);
}

void NotesPanel::deleteCurrent()
{
    if (!current())
        return;
    mPages.erase(mPages.begin() + mCurrent);
    store("Delete Note");
    showPage(mPages.empty() ? -1 : std::min(mCurrent, int(mPages.size()) - 1));
}

void NotesPanel::commitText()
{
    json* page = current();
    if (page && isTodoPage(*page))
        return;
    const std::string text = mText->toPlainText().toStdString();
    if (!page)
    {
        if (text.empty())
            return;
        mPages.push_back({{"name", "Notes"}, {"kind", "note"}, {"text", text}});
        mCurrent = 0;
        store("Edit Note", true);
        showPage(0);
        return;
    }
    if (page->value("text", "") == text)
        return;
    (*page)["text"] = text;
    store("Edit Note", true);
}

void NotesPanel::commitItems()
{
    json* page = current();
    if (!page || !isTodoPage(*page))
        return;
    json items = json::array();
    for (int i = 0; i < mList->count(); ++i)
    {
        QListWidgetItem* it = mList->item(i);
        items.push_back({{"text", it->text().toStdString()}, {"done", it->checkState() == Qt::Checked}});
    }
    if ((*page)["items"] == items)
        return;
    (*page)["items"] = std::move(items);
    store("Edit Todo List");
}

void NotesPanel::addItem()
{
    if (addLines(mNewItem->text()))
        mNewItem->clear();
}

bool NotesPanel::addLines(const QString& text)
{
    // Bullets and checkboxes from markdown or plain-text lists; "[x]" comes in ticked.
    static const QRegularExpression bullet(R"(^(?:[-*+\x{2022}]\s+|\d+[.)]\s+)?(\[([ xX])\]\s*)?)");
    json added = json::array();
    for (const QString& line : text.split(QRegularExpression("[\r\n]+")))
    {
        QString t = line.trimmed();
        const QRegularExpressionMatch m = bullet.match(t);
        const bool done = m.captured(2).compare("x", Qt::CaseInsensitive) == 0;
        t = t.mid(m.capturedLength()).trimmed();
        if (!t.isEmpty())
            added.push_back({{"text", t.toStdString()}, {"done", done}});
    }
    if (added.empty())
        return false;
    if (!current())
    {
        mPages.push_back({{"name", uniquePageName(mPages, "Todo")}, {"kind", "todo"}, {"items", json::array()}});
        mCurrent = int(mPages.size()) - 1;
    }
    json* page = current();
    if (!isTodoPage(*page))
        return false;
    for (json& item : added)
        (*page)["items"].push_back(std::move(item));
    store(added.size() == 1 ? "Add Todo Item" : "Add Todo Items");
    showPage(mCurrent);
    mList->scrollToBottom();
    return true;
}

void NotesPanel::copyItems()
{
    QStringList lines;
    for (int i = 0; i < mList->count(); ++i)
        if (QListWidgetItem* it = mList->item(i); it->isSelected())
            lines << (it->checkState() == Qt::Checked ? "[x] " : "") + it->text();
    if (!lines.isEmpty())
        QGuiApplication::clipboard()->setText(lines.join('\n'));
}

void NotesPanel::clearDone()
{
    json* page = current();
    if (!page || !isTodoPage(*page))
        return;
    json kept = json::array();
    for (const json& item : (*page)["items"])
        if (!item.value("done", false))
            kept.push_back(item);
    if (kept.size() == (*page)["items"].size())
        return;
    (*page)["items"] = std::move(kept);
    store("Clear Done Items");
    showPage(mCurrent);
}

void registerNotesPanel()
{
    panels::add({.id = kNotesDock,
                 .title = "Notes",
                 .create = [](QWidget* p) -> QWidget* { return new NotesPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "notes",
                 .open = false,
                 .toggleAction = "window.notes"});
}
} // namespace lila::ui
