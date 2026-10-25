#include <doctest/doctest.h>

#include "core/Paths.h"
#include "ui/DocsUi.h"
#include "ui/Panels.h"

#include <QApplication>
#include <QFontDatabase>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QTextBrowser>
#include <QToolBar>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

namespace
{
void writeFile(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

void setDocsDir(const fs::path& dir)
{
#ifdef _WIN32
    _wputenv_s(L"LILA_DOCS_DIR", dir.wstring().c_str());
#else
    setenv("LILA_DOCS_DIR", dir.string().c_str(), 1);
#endif
}
} // namespace

TEST_CASE("docs panel reads a local checkout, follows links, goes back and searches")
{
    fs::path site = fs::temp_directory_path() / "lila_docs_panel_test";
    fs::remove_all(site);
    writeFile(site / "docs" / "README.md", "# Lila Docs\n\nSee [Essentials](essentials.md).\n");
    writeFile(site / "docs" / "essentials.md", "# Essentials\n\n## Selection modes\n\nPress **Shift+1** for vertex mode.\n");
    writeFile(site / "docs" / "_sidebar.md", "- [Home](/)\n- [Essentials](essentials.md)\n");
    setDocsDir(site);

    lila::ui::docs::registerDocsUi();
    const lila::ui::panels::Panel* p = lila::ui::panels::registry().find(lila::ui::docs::kDocsDock);
    REQUIRE(p);
    std::unique_ptr<QWidget> panel(p->create(nullptr));
    panel->resize(800, 600);
    panel->show();
    QApplication::processEvents();

    auto* browser = panel->findChild<QTextBrowser*>();
    REQUIRE(browser);
    CHECK(browser->toPlainText().contains("Lila Docs"));

    emit browser->anchorClicked(QUrl("essentials.md"));
    CHECK(browser->toPlainText().contains("Selection modes"));
    // Headings keep their level through the HTML, which anchors and bookmarks find them by.
    bool heading = false;
    for (QTextBlock b = browser->document()->begin(); b.isValid(); b = b.next())
        heading = heading || (b.blockFormat().headingLevel() == 2 && b.text() == "Selection modes");
    CHECK(heading);

    QAction* back = nullptr;
    for (QAction* a : panel->findChild<QToolBar*>()->actions())
        if (a->text() == "Back")
            back = a;
    REQUIRE(back);
    CHECK(back->isEnabled());
    back->trigger();
    CHECK(browser->toPlainText().contains("Lila Docs"));

    // Browser zoom keys re-style the page in whole steps; Ctrl+0 goes back to 100%.
    const qreal size = browser->document()->defaultFont().pointSizeF();
    QKeyEvent zoomIn(QEvent::KeyPress, Qt::Key_Equal, Qt::ControlModifier);
    QApplication::sendEvent(browser, &zoomIn);
    CHECK(browser->document()->defaultFont().pointSizeF() > size);
    QKeyEvent reset(QEvent::KeyPress, Qt::Key_0, Qt::ControlModifier);
    QApplication::sendEvent(browser, &reset);
    CHECK(browser->document()->defaultFont().pointSizeF() == doctest::Approx(size));

    auto* search = panel->findChild<QLineEdit*>();
    REQUIRE(search);
    search->setText("vertex");
    emit search->returnPressed();
    CHECK(browser->toPlainText().contains("Shift+1"));

    panel.reset();
    setDocsDir({});
    fs::remove_all(site);
}

// Renders a real docs page for eyeballing against the site: LILA_DOCS_DIR=<site checkout> LILA_DOCS_RENDER_PNG=<out.png>
// [LILA_DOCS_RENDER_PAGE=essentials]. Skipped when LILA_DOCS_RENDER_PNG is not set.
TEST_CASE("docs panel renders a page to an image")
{
    const char* out = std::getenv("LILA_DOCS_RENDER_PNG");
    if (!out || !*out)
        return;
    const char* page = std::getenv("LILA_DOCS_RENDER_PAGE");
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(lila::paths::resourcesDir() / "fonts", ec))
        if (entry.path().extension() == ".ttf")
            QFontDatabase::addApplicationFont(QString::fromStdWString(entry.path().wstring()));

    lila::ui::docs::registerDocsUi();
    std::unique_ptr<QWidget> panel(lila::ui::panels::registry().find(lila::ui::docs::kDocsDock)->create(nullptr));
    panel->resize(1000, 750);
    panel->show();
    QApplication::processEvents();
    auto* search = panel->findChild<QLineEdit*>();
    REQUIRE(search);
    search->setText(QString("#/") + (page ? page : "essentials"));
    emit search->returnPressed();
    QApplication::processEvents();

    auto* browser = panel->findChild<QTextBrowser*>();
    REQUIRE(browser);
    QTextDocument* doc = browser->document();
    // The top of a long page is enough to look at.
    const QSize size(browser->viewport()->width(), std::min(12000, int(std::ceil(doc->size().height()))));
    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor("#0c0d0f"));
    {
        QPainter painter(&image);
        doc->drawContents(&painter);
    }
    CHECK(image.save(QString::fromLocal8Bit(out)));
    // The whole panel, scrolled into the page, shows the sidebar following the section at the top.
    browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum() / 3);
    QApplication::processEvents();
    CHECK(panel->grab().save(QString::fromStdWString(fs::path(out).replace_extension().wstring()) + "_panel.png"));
}
