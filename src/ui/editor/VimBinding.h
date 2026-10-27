#pragma once

#include "core/Export.h"

#include "ui/editor/VimEngine.h"

#include <QObject>
#include <QPointer>

class QKeyEvent;

namespace lila::ui
{
class CodeEditor;

/**
 * Vim keybindings on a CodeEditor: feeds its keys to a vim::Engine, draws a block cursor outside insert mode and
 * keeps each vim change, insert-mode typing included, as one undo step. Deleting it restores the plain editor.
 */
class LILA_UI_API VimBinding : public QObject, private vim::Buffer
{
    Q_OBJECT

public:
    explicit VimBinding(CodeEditor* editor);
    ~VimBinding() override;

    vim::Mode mode() const { return mEngine.mode(); }
    QString status() const { return mEngine.status(); }
    /** Leaves insert, visual or command-line mode. */
    void reset();

    /** The vim token for a key press, or empty for keys vim leaves to the editor (F-keys, Alt, Ctrl+Enter). */
    static QString token(const QKeyEvent* e);

Q_SIGNALS:
    void statusChanged(const QString& status);
    void writeRequested();
    void closeRequested();
    void messageShown(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    bool handleKey(QKeyEvent* e);
    void refresh();
    void moveToChange(const QString& before);
    template <class Fn> void inGroup(Fn&& fn);

    QString text() const override;
    int cursor() const override;
    void setCursor(int pos) override;
    void replace(int pos, int length, const QString& with) override;
    void beginChange() override;
    void endChange() override;
    void undo() override;
    void redo() override;
    void showSelection(int from, int to) override;
    QString clipboard() const override;
    void setClipboard(const QString& text) override;
    void write() override { Q_EMIT writeRequested(); }
    void close() override { Q_EMIT closeRequested(); }
    void message(const QString& text) override { Q_EMIT messageShown(text); }
    int halfPage() const override;

    QPointer<CodeEditor> mEditor;
    vim::Engine mEngine;
    int mSelFrom = -1;
    int mSelTo = -1;
    int mCursor = 0;
    int mChangeDepth = 0;
    bool mChangeHasEdits = false;
    bool mPassing = false;
};
} // namespace lila::ui
