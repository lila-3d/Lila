#include <doctest/doctest.h>

#include "io/FileTranslators.h"
#include "ui/FileDrop.h"

#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

using namespace lila;
namespace fs = std::filesystem;

TEST_CASE("file drop: the first scene opens, the rest import, unknown files are skipped")
{
    io::translators::registerBuiltins();
    ui::fileDrop::Plan p = ui::fileDrop::plan({"a.fbx", "b.lila", "c.ma", "d.lilaa", "e.txt"}, false);
    CHECK(p.open == fs::path("b.lila"));
    REQUIRE(p.imports.size() == 2);
    CHECK(p.imports[0] == fs::path("a.fbx"));
    CHECK(p.imports[1] == fs::path("c.ma"));
    REQUIRE(p.skipped.size() == 2);
    CHECK(p.skipped[0] == fs::path("d.lilaa"));
    CHECK(p.skipped[1] == fs::path("e.txt"));
}

TEST_CASE("file drop: importOnly imports a Maya scene and skips a Lila one")
{
    io::translators::registerBuiltins();
    ui::fileDrop::Plan p = ui::fileDrop::plan({"a.ma", "b.lila"}, true);
    CHECK(p.open.empty());
    REQUIRE(p.imports.size() == 1);
    CHECK(p.imports[0] == fs::path("a.ma"));
    CHECK(p.skipped.size() == 1);
    CHECK(ui::fileDrop::plan({"x.png"}, false).empty());
}

TEST_CASE("file drop: only existing local files count")
{
    QTemporaryDir dir;
    const QString file = dir.filePath("mesh.obj");
    QFile f(file);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.close();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(file), QUrl::fromLocalFile(dir.filePath("missing.obj")), QUrl::fromLocalFile(dir.path()), QUrl("https://example.com/a.obj")});
    std::vector<fs::path> files = ui::fileDrop::localFiles(&mime);
    REQUIRE(files.size() == 1);
    CHECK(files[0] == fs::path(file.toStdWString()));
}
