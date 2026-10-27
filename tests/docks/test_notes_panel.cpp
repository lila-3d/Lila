#include <doctest/doctest.h>

#include "ops/Undo.h"
#include "scene/SceneData.h"
#include "ui/NotesPanel.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QToolButton>

using namespace lila;
using json = nlohmann::json;

namespace
{
QToolButton* button(QWidget& w, const QString& tip)
{
    for (QToolButton* b : w.findChildren<QToolButton*>())
        if (b->toolTip() == tip)
            return b;
    return nullptr;
}

json pages()
{
    return scene::data::get("notes", json::object()).value("pages", json::array());
}
} // namespace

TEST_CASE("notes panel: notes and todo lists save to scene data and undo")
{
    scene::data::clear();
    UndoStack::get().clear();
    ui::NotesPanel panel;
    panel.show();

    auto* text = panel.findChild<QPlainTextEdit*>();
    REQUIRE(text);
    text->setFocus();
    text->setPlainText("first");
    text->clearFocus(); // focus loss commits the pending typing
    QApplication::processEvents();
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 800)
        QApplication::processEvents();
    REQUIRE(pages().size() == 1);
    CHECK(pages()[0]["text"] == "first");
    CHECK(pages()[0]["kind"] == "note");

    button(panel, "New todo list")->click();
    REQUIRE(pages().size() == 2);
    CHECK(pages()[1]["kind"] == "todo");
    auto* entry = panel.findChildren<QLineEdit*>().back();
    for (QLineEdit* e : panel.findChildren<QLineEdit*>())
        if (e->placeholderText().startsWith("Add an item"))
            entry = e;
    entry->setText("buy milk");
    Q_EMIT entry->returnPressed();
    entry->setText("ship it");
    Q_EMIT entry->returnPressed();
    REQUIRE(pages()[1]["items"].size() == 2);

    auto* list = panel.findChild<QListWidget*>();
    list->item(0)->setCheckState(Qt::Checked);
    CHECK(pages()[1]["items"][0]["done"] == true);
    button(panel, "Remove the ticked items")->click();
    REQUIRE(pages()[1]["items"].size() == 1);
    CHECK(pages()[1]["items"][0]["text"] == "ship it");

    UndoStack::get().undo();
    QApplication::processEvents();
    CHECK(pages()[1]["items"].size() == 2);
    CHECK(list->count() == 2);

    auto* picker = panel.findChild<QComboBox*>();
    picker->lineEdit()->setText("Shopping");
    Q_EMIT picker->lineEdit()->editingFinished();
    CHECK(pages()[1]["name"] == "Shopping");

    QGuiApplication::clipboard()->setText("Radio callsigns\n\n- Bevel all cockpit\r\n[x] Bake cockpit\n");
    QKeyEvent paste(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QApplication::sendEvent(entry, &paste);
    REQUIRE(pages()[1]["items"].size() == 5);
    CHECK(pages()[1]["items"][2]["text"] == "Radio callsigns");
    CHECK(pages()[1]["items"][3]["text"] == "Bevel all cockpit");
    CHECK(pages()[1]["items"][4]["done"] == true);
    CHECK(entry->text().isEmpty());

    button(panel, "Delete this note or list")->click();
    CHECK(pages().size() == 1);
    scene::data::clear();
    UndoStack::get().clear();
}
