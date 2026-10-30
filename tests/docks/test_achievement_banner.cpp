#include <doctest/doctest.h>

#include "ui/AchievementBanner.h"

#include <QApplication>
#include <QMainWindow>
#include <QWidget>

using namespace lila;

namespace
{
QWidget* bannerWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->isVisible() && (w->windowFlags() & Qt::WindowTransparentForInput) && w->width() == 380)
            return w;
    return nullptr;
}
} // namespace

TEST_CASE("achievement banner shows in the main window's bottom right corner, one at a time")
{
    QMainWindow main;
    main.resize(1280, 720);
    main.show();
    QApplication::processEvents();

    ui::achievementBanner::show({"EDGE_LORD", "Edge Lord", "Add your first Auto Bevel.", 0, 0});
    ui::achievementBanner::show({"SEARCH_PARTY", "Search Party", "Run 50 commands from Ctrl+T.", 20, 50});
    QApplication::processEvents();

    QWidget* banner = bannerWindow();
    REQUIRE(banner);
    QPoint corner = main.mapToGlobal(QPoint(main.width(), main.height()));
    CHECK(banner->geometry().right() < corner.x());
    CHECK(banner->geometry().bottom() < corner.y());
    CHECK(corner.x() - banner->geometry().right() < 40);
    int shown = 0;
    for (QWidget* w : QApplication::topLevelWidgets())
        shown += w->isVisible() && (w->windowFlags() & Qt::WindowTransparentForInput) && w->width() == 380;
    CHECK(shown == 1);
    if (const char* out = std::getenv("LILA_BANNER_GRAB"))
        banner->grab().save(out);
}
