#include <doctest/doctest.h>

#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyleFactory>

using namespace lila::ui;

namespace
{
/** Visible rows of a torn-off panel, by label. */
std::vector<QPushButton*> rows(QDockWidget* dock)
{
    std::vector<QPushButton*> out;
    for (QPushButton* b : dock->widget()->findChildren<QPushButton*>("tearOffRow"))
        if (!b->isHidden() && !b->parentWidget()->isHidden())
            out.push_back(b);
    return out;
}

QPushButton* row(QDockWidget* dock, const QString& label)
{
    for (QPushButton* b : rows(dock))
        if (b->text() == label)
            return b;
    return nullptr;
}

void settle()
{
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();
}
} // namespace

TEST_CASE("a torn-off menu is a dock of buttons bound to the menu's actions")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* mesh = tearoff::enable(window.menuBar()->addMenu("&Mesh"));
    int extrudes = 0;
    QAction* extrude = mesh->addAction("&Extrude\tCtrl+E");
    QObject::connect(extrude, &QAction::triggered, [&] { ++extrudes; });
    QAction* toggle = mesh->addAction("Symmetry On");
    toggle->setCheckable(true);
    QAction* disabled = mesh->addAction("Disabled");
    disabled->setEnabled(false);
    mesh->addSeparator();
    QMenu* symmetry = mesh->addMenu("Symmetry");
    symmetry->addAction("X");
    window.show();
    settle();

    CHECK(mesh->isTearOffEnabled());
    CHECK_FALSE(symmetry->isTearOffEnabled());
    // Submenus get the row as their parent opens, so ones added later are covered too.
    emit mesh->aboutToShow();
    CHECK(symmetry->isTearOffEnabled());
    REQUIRE(tearoff::pathOf(symmetry));
    CHECK(*tearoff::pathOf(symmetry) == std::vector<std::string>{"Mesh", "Symmetry"});
    CHECK(tearoff::findMenu(&window, {"Mesh", "Symmetry"}) == symmetry);
    CHECK(tearoff::findMenu(&window, {"Mesh", "Nope"}) == nullptr);

    QDockWidget* dock = tearoff::tearOff(mesh, QPoint(200, 200));
    REQUIRE(dock);
    settle();
    CHECK(dock->objectName() == "tearoff:Mesh");
    CHECK(dock->isFloating());
    CHECK(dock->windowTitle() == "Mesh");
    CHECK(tearoff::tearOff(mesh) == dock);

    REQUIRE(row(dock, "Extrude"));
    row(dock, "Extrude")->click();
    CHECK(extrudes == 1);
    REQUIRE(row(dock, "Disabled"));
    CHECK_FALSE(row(dock, "Disabled")->isEnabled());
    REQUIRE(row(dock, "Symmetry"));

    SUBCASE("rows follow the actions' state and the menu's contents")
    {
        disabled->setEnabled(true);
        CHECK(row(dock, "Disabled")->isEnabled());
        extrude->setText("Extrude Faces");
        CHECK(row(dock, "Extrude Faces"));
        disabled->setVisible(false);
        CHECK_FALSE(row(dock, "Disabled"));
        size_t before = rows(dock).size();
        mesh->addAction("Bridge");
        settle();
        CHECK(rows(dock).size() == before + 1);
        CHECK(row(dock, "Bridge"));
        mesh->removeAction(toggle);
        settle();
        CHECK_FALSE(row(dock, "Symmetry On"));
    }

    SUBCASE("clicking the dashed row tears the menu off instead of Qt's floating menu")
    {
        mesh->popup(QPoint(50, 50));
        settle();
        QPoint at(mesh->width() / 2, mesh->actionGeometry(extrude).top() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &release);
        settle();
        CHECK_FALSE(mesh->isTearOffMenuVisible());
        CHECK_FALSE(mesh->isVisible());
        QDockWidget* sym = nullptr;
        symmetry->popup(QPoint(50, 50));
        settle();
        QPoint symAt(symmetry->width() / 2, symmetry->actionGeometry(symmetry->actions().front()).top() / 2);
        QMouseEvent p2(QEvent::MouseButtonPress, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &p2);
        QMouseEvent r2(QEvent::MouseButtonRelease, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &r2);
        settle();
        sym = window.findChild<QDockWidget*>("tearoff:Mesh/Symmetry");
        REQUIRE(sym);
        CHECK(row(sym, "X"));
    }

    SUBCASE("a layout recreates torn-off docks by name")
    {
        tearoff::recreate(&window, {"tearoff:Mesh/Symmetry", "tearoff:Missing", "outliner"});
        QDockWidget* sym = window.findChild<QDockWidget*>("tearoff:Mesh/Symmetry");
        REQUIRE(sym);
        CHECK(row(sym, "X"));
        CHECK_FALSE(window.findChild<QDockWidget*>("tearoff:Missing"));
        tearoff::recreate(&window, {"tearoff:Mesh/Symmetry"});
        CHECK(window.findChildren<QDockWidget*>("tearoff:Mesh/Symmetry").size() == 1);
    }
}
