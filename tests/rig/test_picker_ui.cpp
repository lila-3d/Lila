#include "TestUtil.h"

#include "scene/Picker.h"
#include "ui/ControlPicker.h"

#include <QApplication>
#include <QMouseEvent>
#include <QWidget>

#include <memory>

using namespace rigtest;
using namespace lila::scene;

namespace
{
void click(QWidget* w, QPointF at, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(w, &release);
}

std::vector<std::string> selectedNames()
{
    std::vector<std::string> out;
    for (NodeId id : graph().selection())
        out.push_back(graph().node(id)->name());
    return out;
}
} // namespace

TEST_CASE("control picker panel selects through clicks and follows the scene selection")
{
    run("newScene");
    picker::clearScene();
    for (const char* ns : {"a", "b"})
    {
        std::string p = std::string(ns) + ":";
        run("select", {{"clear", true}});
        run("joint", {{"position", v3(0, 100, 0)}, {"name", p + "pelvis"}});
        run("joint", {{"position", v3(10, 120, 0)}, {"name", p + "hand_l"}});
    }
    run("select", {{"clear", true}});
    json layout = {{"name", "body"},
                   {"pages", {{{"name", "Body"}, {"width", 200}, {"height", 200},
                               {"items", {{{"target", "pelvis"}, {"x", 100}, {"y", 100}, {"size", 20}},
                                          {{"target", "hand_l"}, {"x", 150}, {"y", 60}, {"size", 20}}}}}}}};
    run("createPicker", {{"args", {"body"}}, {"layout", layout}, {"namespace", "a"}});

    std::unique_ptr<QWidget> panel(lila::ui::createControlPickerPanel(nullptr));
    panel->resize(300, 400);
    panel->show();
    QApplication::processEvents();
    QWidget* canvas = panel->findChild<QWidget*>("pickerCanvas");
    REQUIRE(canvas);
    // Fit the 200 unit page into a 224 pixel canvas: zoom 1, 12 pixel margin.
    canvas->resize(224, 224);
    QKeyEvent f(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier);
    QApplication::sendEvent(canvas, &f);

    click(canvas, {12 + 100, 12 + 100});
    CHECK(selectedNames() == std::vector<std::string>{"a:pelvis"});
    click(canvas, {12 + 150, 12 + 60}, Qt::ShiftModifier);
    CHECK(selectedNames() == std::vector<std::string>{"a:pelvis", "a:hand_l"});
    click(canvas, {12 + 100, 12 + 100}, Qt::ControlModifier);
    CHECK(selectedNames() == std::vector<std::string>{"a:hand_l"});
    // Empty space clears.
    click(canvas, {12 + 10, 12 + 190});
    CHECK(graph().selection().empty());

    // Selecting the other character switches the panel to its namespace, so the next click picks there.
    run("select", {{"args", {"b:hand_l"}}});
    QApplication::processEvents();
    click(canvas, {12 + 100, 12 + 100});
    CHECK(selectedNames() == std::vector<std::string>{"b:pelvis"});

    // Undoing a picker edit while the panel shows it reloads without trouble.
    run("editPicker", {{"args", {"body"}}, {"addItem", {{"x", 10}, {"y", 10}}}});
    run("undo");
    QApplication::processEvents();
    canvas->repaint();
    panel.reset();
}
