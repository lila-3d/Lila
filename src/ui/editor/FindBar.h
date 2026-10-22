#pragma once

#include <QPointer>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

namespace lila::ui
{
class CodeEditor;

/** Find and replace over the current editor: case, whole word and regex toggles, all matches highlighted. */
class FindBar : public QWidget
{
    Q_OBJECT

public:
    explicit FindBar(QWidget* parent = nullptr);

    void setEditor(CodeEditor* editor);
    /** Shows the bar seeded from the editor's selection and focuses the find field. */
    void activate(bool replace);
    void close();

    void findNext(bool backwards = false);
    void replaceOne();
    void replaceAll();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QRegularExpression pattern() const;
    QList<QTextCursor> matches() const;
    void refresh();

    QPointer<CodeEditor> mEditor;
    QLineEdit* mFind;
    QLineEdit* mReplace;
    QWidget* mReplaceRow;
    QToolButton* mCase;
    QToolButton* mWord;
    QToolButton* mRegex;
    QLabel* mCount;
    QTimer mRefresh;
};
} // namespace lila::ui
