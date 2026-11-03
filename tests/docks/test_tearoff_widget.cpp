#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "ui/ViewportHooks.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenuBar>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
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

void settleBriefly()
{
    for (int i = 0; i < 3; ++i)
        QApplication::processEvents();
}

void wait(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}
/** Records the action id of a drop from a menu drag. */
class DropCatcher : public QWidget
{
public:
    DropCatcher() { setAcceptDrops(true); }
    QString dropped;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override { e->setAccepted(e->mimeData()->hasFormat(tearoff::kActionMime)); }
    void dragMoveEvent(QDragMoveEvent* e) override { e->accept(); }
    void dropEvent(QDropEvent* e) override { dropped = QString::fromUtf8(e->mimeData()->data(tearoff::kActionMime)); }
};
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
    settleBriefly();

    CHECK(mesh->isTearOffEnabled());
    CHECK_FALSE(symmetry->isTearOffEnabled());
    // Submenus get the row as their parent opens, so ones added later are covered too.
    Q_EMIT mesh->aboutToShow();
    CHECK(symmetry->isTearOffEnabled());
    REQUIRE(tearoff::pathOf(symmetry));
    CHECK(*tearoff::pathOf(symmetry) == std::vector<std::string>{"Mesh", "Symmetry"});
    CHECK(tearoff::findMenu(&window, {"Mesh", "Symmetry"}) == symmetry);
    CHECK(tearoff::findMenu(&window, {"Mesh", "Nope"}) == nullptr);

    QDockWidget* dock = tearoff::tearOff(mesh, QPoint(200, 200));
    REQUIRE(dock);
    settleBriefly();
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
        settleBriefly();
        CHECK(rows(dock).size() == before + 1);
        CHECK(row(dock, "Bridge"));
        mesh->removeAction(toggle);
        settleBriefly();
        CHECK_FALSE(row(dock, "Symmetry On"));
    }

    SUBCASE("clicking the dashed row tears the menu off instead of Qt's floating menu")
    {
        mesh->popup(QPoint(50, 50));
        settleBriefly();
        QPoint at(mesh->width() / 2, mesh->actionGeometry(extrude).top() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(mesh, &release);
        settleBriefly();
        CHECK_FALSE(mesh->isTearOffMenuVisible());
        CHECK_FALSE(mesh->isVisible());
        QDockWidget* sym = nullptr;
        symmetry->popup(QPoint(50, 50));
        settleBriefly();
        QPoint symAt(symmetry->width() / 2, symmetry->actionGeometry(symmetry->actions().front()).top() / 2);
        QMouseEvent p2(QEvent::MouseButtonPress, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &p2);
        QMouseEvent r2(QEvent::MouseButtonRelease, symAt, symmetry->mapToGlobal(symAt), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(symmetry, &r2);
        settleBriefly();
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

TEST_CASE("clicking a section title tears off only that section")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* mesh = tearoff::enable(window.menuBar()->addMenu("Mesh"));
    mesh->addSection("Combine");
    mesh->addAction("Combine");
    mesh->addAction("Separate");
    mesh->addSeparator();
    mesh->addAction("Extract");
    QAction* normals = mesh->addSection("Normals");
    mesh->addAction("Soften Normals");
    window.show();
    settleBriefly();

    mesh->popup(QPoint(50, 50));
    settleBriefly();
    QPoint at = mesh->actionGeometry(normals).center();
    QMouseEvent press(QEvent::MouseButtonPress, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mesh, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, mesh->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(mesh, &release);
    settleBriefly();
    QDockWidget* dock = window.findChild<QDockWidget*>("tearoff:Mesh/#Normals");
    REQUIRE(dock);
    CHECK(dock->windowTitle() == "Normals");
    CHECK(row(dock, "Soften Normals"));
    CHECK_FALSE(row(dock, "Combine"));

    QDockWidget* combine = tearoff::tearOff(mesh, QPoint(200, 200), "Combine");
    REQUIRE(combine);
    settleBriefly();
    CHECK(rows(combine).size() == 3);
    CHECK(row(combine, "Extract"));
    CHECK_FALSE(row(combine, "Soften Normals"));

    delete combine;
    tearoff::recreate(&window, {"tearoff:Mesh/#Combine"});
    QDockWidget* again = window.findChild<QDockWidget*>("tearoff:Mesh/#Combine");
    REQUIRE(again);
    CHECK(row(again, "Separate"));
}

TEST_CASE("a dragged Create row creates where it is dropped, as one undo step")
{
    lila::ops::registerBuiltins();
    lila::scene::types::registerBuiltins();
    lila::scene::registerCommands();
    actions::registerBuiltins();
    tearoff::registerDropHandler();
    lila::ops::execute("newScene", nlohmann::json::object());
    lila::scene::Graph& g = lila::scene::Graph::current();

    CHECK(tearoff::draggable("create.cube"));
    CHECK_FALSE(tearoff::draggable("edit.undo"));
    QMimeData mime;
    mime.setData(tearoff::kActionMime, "create.cube");
    CHECK(viewportHooks::handleDrop(mime, {}, false));

    const size_t before = g.nodes().size();
    std::vector<std::string> made = tearoff::runAt("create.cube", {100.0, 0.0, -50.0});
    REQUIRE(made.size() == 1);
    lila::scene::Node* cube = g.find(made[0]);
    REQUIRE(cube);
    const glm::dvec3 t = cube->as<glm::dvec3>(lila::scene::Transform::kTranslate);
    CHECK(t.x == doctest::Approx(100.0));
    CHECK(t.y == doctest::Approx(0.0));
    CHECK(t.z == doctest::Approx(-50.0));

    lila::ops::execute("undo", nlohmann::json::object());
    CHECK(g.nodes().size() == before);
    CHECK_FALSE(g.find(made[0]));
}

TEST_CASE("pressing a Create item and dragging out of its menu closes the menus")
{
    actions::registerBuiltins();
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    window.resize(800, 600);
    QMenu* create = tearoff::enable(window.menuBar()->addMenu("Create"));
    QAction* locator = create->addAction("Locator");
    locator->setData(QString("create.locator"));
    create->addAction("Other");
    window.show();
    create->popup(window.mapToGlobal(QPoint(10, 30)));
    settleBriefly();
    REQUIRE(QApplication::activePopupWidget() == create);

    const QPoint on = create->actionGeometry(locator).center();
    QMouseEvent press(QEvent::MouseButtonPress, on, create->mapToGlobal(on), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(create, &press);
    const QPoint out(create->width() + 200, on.y());
    QMouseEvent move(QEvent::MouseMove, out, create->mapToGlobal(out), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(create, &move);
    settleBriefly();
    CHECK(QApplication::activePopupWidget() == nullptr);
    CHECK_FALSE(create->isVisible());

    // The ghost carries the drag to the widget under the cursor and drops it there on release.
    QLabel* ghost = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == "lilaMenuDrag")
            ghost = static_cast<QLabel*>(w);
    REQUIRE(ghost);
    DropCatcher catcher;
    catcher.setGeometry(300, 300, 200, 200);
    catcher.show();
    settleBriefly();
    const QPoint over = catcher.mapToGlobal(QPoint(100, 100));
    QMouseEvent hover(QEvent::MouseMove, ghost->mapFromGlobal(over), over, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(ghost, &hover);
    QMouseEvent release(QEvent::MouseButtonRelease, ghost->mapFromGlobal(over), over, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(ghost, &release);
    settleBriefly();
    CHECK(catcher.dropped == "create.locator");
}

TEST_CASE("a tear-off placed from the search follows the cursor until a click drops it")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QMainWindow window;
    window.setCentralWidget(new QWidget);
    QMenu* lights = tearoff::enable(window.menuBar()->addMenu("Lights"));
    lights->addAction("Point Light");
    lights->addSection("Sky");
    lights->addAction("Sun");
    window.setGeometry(0, 0, 800, 600);
    window.show();
    settleBriefly();

    QCursor::setPos(200, 200);
    QDockWidget* dock = tearoff::tearOffToPlace(lights);
    REQUIRE(dock);
    settleBriefly();
    CHECK(dock->isFloating());
    CHECK(dock->property(tearoff::kPlacingProperty).toBool());

    QCursor::setPos(400, 300);
    for (int i = 0; i < 10 && dock->pos() != QPoint(380, 288); ++i)
    {
        wait(20);
        settleBriefly();
    }
    CHECK(dock->pos() == QPoint(380, 288));

    // The dropping click is the placer's: the row under it does not run.
    QPushButton* point = row(dock, "Point Light");
    REQUIRE(point);
    bool ran = false;
    QObject::connect(lights->actions().first(), &QAction::triggered, [&ran] { ran = true; });
    const QPoint at = point->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, at, point->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(point, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, point->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(point, &release);
    settleBriefly();
    CHECK_FALSE(ran);
    CHECK_FALSE(dock->property(tearoff::kPlacingProperty).toBool());
    QCursor::setPos(100, 100);
    wait(50);
    settleBriefly();
    CHECK(dock->pos() == QPoint(380, 288));

    // Esc puts back a section made for the placing.
    QDockWidget* sky = tearoff::tearOffToPlace(lights, "Sky");
    REQUIRE(sky);
    settleBriefly();
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(sky, &esc);
    settleBriefly();
    CHECK_FALSE(sky->isVisible());
}
