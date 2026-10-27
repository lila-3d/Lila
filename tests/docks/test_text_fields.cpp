#include <doctest/doctest.h>

#include "ops/CommandOptions.h"
#include "ui/FlagFields.h"
#include "ui/FontPicker.h"
#include "ui/MultilineEdit.h"

#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>

using namespace lila;
namespace opt = ops::options;

TEST_CASE("text fields: a multiline editor commits several lines, a font editor picks family and style")
{
    opt::Field f;
    f.flag = "text";
    f.label = "Text";
    f.kind = opt::FieldKind::Text;
    f.editor = "multiline";
    json got;
    ui::FlagFieldWidgets w = ui::makeFlagField(f, "a", [&](const json& v) { got = v; }, nullptr);
    auto* box = qobject_cast<ui::MultilineEdit*>(w.field);
    REQUIRE(box);
    CHECK(box->toPlainText() == "a");
    box->setPlainText("one\ntwo");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
    QApplication::sendEvent(box, &enter);
    CHECK(got == "one\ntwo");
    delete w.label;
    delete w.field;

    f.flag = "font";
    f.editor = "font";
    got = nullptr;
    w = ui::makeFlagField(f, "Inter SemiBold", [&](const json& v) { got = v; }, nullptr);
    auto* picker = qobject_cast<ui::FontPicker*>(w.field);
    REQUIRE(picker);
    CHECK(picker->family() == "Inter");
    CHECK(picker->style() == "SemiBold");
    CHECK(picker->styleWith(false, true) == "Italic");
    picker->setStyle("Regular");
    CHECK(got == "Inter");
    picker->setStyle("Italic");
    CHECK(got == "Inter Italic");
    delete w.label;
    delete w.field;
}
