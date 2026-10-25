#include "ui/TokenCompleter.h"

#include "ui/OutlinerFilter.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QStringListModel>
#include <QTimer>

#include <algorithm>
#include <cctype>

namespace lila::ui::completion
{
namespace
{
std::string lowered(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

class TokenCompleter : public QCompleter
{
public:
    TokenCompleter(QLineEdit* edit, Source source, QString separators)
        : QCompleter(edit), mEdit(edit), mSource(std::move(source)), mSeparators(std::move(separators))
    {
        mModel = new QStringListModel(this);
        setModel(mModel);
        setWidget(edit);
        setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        setCaseSensitivity(Qt::CaseInsensitive);
        setMaxVisibleItems(12);
        // Installed after QCompleter's own filter, so it sees Tab first.
        popup()->installEventFilter(this);
        connect(edit, &QLineEdit::textEdited, this, [this] { refresh(); });
        connect(this, qOverload<const QString&>(&QCompleter::activated), this, [this](const QString& s) { accept(s); });
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (obj == popup() && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Tab)
        {
            QModelIndex i = popup()->currentIndex();
            if (!i.isValid())
                i = mModel->index(0);
            popup()->hide();
            if (i.isValid())
                accept(i.data().toString());
            return true;
        }
        return QCompleter::eventFilter(obj, e);
    }

private:
    /** The token around the cursor, without the spaces padding it. */
    std::pair<int, int> span() const
    {
        QString text = mEdit->text();
        int cursor = mEdit->cursorPosition();
        int start = cursor, end = cursor;
        while (start > 0 && !mSeparators.contains(text[start - 1]))
            --start;
        while (end < text.size() && !mSeparators.contains(text[end]))
            ++end;
        while (start < end && text[start].isSpace())
            ++start;
        while (end > start && text[end - 1].isSpace())
            --end;
        return {start, end};
    }

    void refresh()
    {
        if (mAccepting)
            return;
        auto [start, end] = span();
        QString token = mEdit->text().mid(start, end - start);
        QStringList options = mSource(token);
        if (options.size() == 1 && options.front().compare(token, Qt::CaseInsensitive) == 0)
            options.clear();
        if (options.isEmpty())
        {
            popup()->hide();
            return;
        }
        mModel->setStringList(options);
        complete();
        popup()->setCurrentIndex(QModelIndex());
    }

    void accept(const QString& s)
    {
        auto [start, end] = span();
        mAccepting = true;
        mEdit->setSelection(start, end - start);
        mEdit->insert(s);
        mAccepting = false;
        if (s.endsWith(':'))
            QTimer::singleShot(0, this, [this] { refresh(); });
    }

    QLineEdit* mEdit;
    Source mSource;
    QString mSeparators;
    QStringListModel* mModel = nullptr;
    bool mAccepting = false;
};
} // namespace

std::vector<std::string> rank(const std::vector<std::string>& options, std::string_view typed)
{
    std::string t = lowered(typed);
    std::vector<std::string> prefix, inside;
    for (const std::string& o : options)
    {
        std::string l = lowered(o);
        if (l.starts_with(t))
            prefix.push_back(o);
        else if (l.find(t) != std::string::npos)
            inside.push_back(o);
    }
    prefix.insert(prefix.end(), inside.begin(), inside.end());
    return prefix;
}

QStringList rank(const QStringList& options, const QString& typed)
{
    QStringList prefix, inside;
    for (const QString& o : options)
        if (o.startsWith(typed, Qt::CaseInsensitive))
            prefix << o;
        else if (o.contains(typed, Qt::CaseInsensitive))
            inside << o;
    return prefix + inside;
}

QCompleter* attach(QLineEdit* edit, Source source, const QString& separators)
{
    return new TokenCompleter(edit, std::move(source), separators);
}

QCompleter* attachValues(QLineEdit* edit, std::function<QStringList()> values, bool list)
{
    return attach(edit, [values = std::move(values)](const QString& token) { return rank(values(), token); }, list ? "," : "");
}

QCompleter* attachSceneFilter(QLineEdit* edit)
{
    return attach(edit, [](const QString& token) {
        QStringList out;
        for (const std::string& s : outliner::filterCompletions(token.toStdString(), outliner::nodeTypeNames()))
            out << QString::fromStdString(s);
        return out;
    });
}
} // namespace lila::ui::completion
