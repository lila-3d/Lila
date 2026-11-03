#include "ui/editor/CodeEditor.h"
#include "ui/editor/VimBinding.h"

#include <doctest/doctest.h>

#include <QApplication>
#include <QKeyEvent>

using namespace lila::ui;

namespace
{
void typeKeys(CodeEditor& editor, const QString& keys)
{
    for (int i = 0; i < keys.size(); ++i)
    {
        int key = 0;
        QString text;
        if (keys.mid(i, 5) == "<Esc>")
        {
            key = Qt::Key_Escape;
            i += 4;
        }
        else
        {
            QChar c = keys[i];
            text = c;
            key = c.isLetter() ? Qt::Key_A + (c.toLower().unicode() - 'a') : c.unicode();
        }
        Qt::KeyboardModifiers mods = !text.isEmpty() && text[0].isUpper() ? Qt::ShiftModifier : Qt::NoModifier;
        QKeyEvent e(QEvent::KeyPress, key, mods, text);
        QApplication::sendEvent(&editor, &e);
    }
}
} // namespace

TEST_CASE("vim binding drives a code editor with one undo step per change")
{
    CodeEditor editor;
    editor.setPlainText("foo bar\nbaz");
    auto* vim = new VimBinding(&editor);
    CHECK(vim->mode() == vim::Mode::Normal);
    CHECK(editor.overwriteMode()); // the block cursor

    typeKeys(editor, "dw");
    CHECK(editor.toPlainText() == "bar\nbaz");
    typeKeys(editor, "u");
    CHECK(editor.toPlainText() == "foo bar\nbaz");

    typeKeys(editor, "ciwhello world<Esc>");
    CHECK(editor.toPlainText().toStdString() == "hello world bar\nbaz");
    CHECK(vim->mode() == vim::Mode::Normal);
    typeKeys(editor, "u");
    CHECK(editor.toPlainText() == "foo bar\nbaz");

    typeKeys(editor, "A!");
    CHECK(vim->mode() == vim::Mode::Insert);
    CHECK(!editor.overwriteMode());
    typeKeys(editor, "?<Esc>j.");
    CHECK(editor.toPlainText() == "foo bar!?\nbaz!?");

    delete vim;
    CHECK(!editor.overwriteMode());
    typeKeys(editor, "x");
    CHECK(editor.toPlainText().count('x') == 1); // typed as plain text again
}
