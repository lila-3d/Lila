#include <doctest/doctest.h>

#include "ui/Docs.h"

#include <miniz.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

using namespace lila::ui;
namespace fs = std::filesystem;

namespace
{
struct TempDir
{
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("lila_docs_test_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void writeFile(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void makeSite(const fs::path& dir, const std::string& home = "# Home\n")
{
    writeFile(dir / "docs" / "README.md", home);
}

/** A zip like GitHub's archives: entries under one top folder. */
void writeZip(const fs::path& zip, const std::map<std::string, std::string>& entries)
{
    mz_zip_archive ar{};
    REQUIRE(mz_zip_writer_init_heap(&ar, 0, 0));
    for (const auto& [name, data] : entries)
        REQUIRE(mz_zip_writer_add_mem(&ar, name.c_str(), data.data(), data.size(), mz_uint(MZ_DEFAULT_LEVEL)));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&ar, &buf, &size));
    std::ofstream(zip, std::ios::binary).write(static_cast<const char*>(buf), std::streamsize(size));
    mz_zip_writer_end(&ar);
}

std::map<std::string, std::string> siteEntries(const std::string& home)
{
    return {{"lila-3d.github.io-abc/docs/README.md", home},
            {"lila-3d.github.io-abc/docs/essentials.md", "# Essentials\n\n## Selection modes\n\nPress Shift+1 for vertex mode.\n"},
            {"lila-3d.github.io-abc/assets/vendor/docsify/docsify.min.js", "//js"}};
}

size_t leftovers(const fs::path& cacheRoot)
{
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(cacheRoot))
        if (e.path().filename().string().front() == '.')
            ++n;
    return n;
}

const std::string kShaA(40, 'a');
const std::string kShaB(40, 'b');
} // namespace

TEST_CASE("docs: a version maps to its docs tag, dev builds to main")
{
    CHECK(docs::refForVersion("0.1.0", false) == "v0.1");
    CHECK(docs::refForVersion("v1.12.3", false) == "v1.12");
    CHECK(docs::refForVersion("2.0", false) == "v2.0");
    CHECK(docs::refForVersion("0.2.0-dev", false) == "main");
    CHECK(docs::refForVersion("dev", false) == "main");
    CHECK(docs::refForVersion("", false) == "main");
    CHECK(docs::refForVersion("0.1.0", true) == "main");
    CHECK(docs::refsToTry("0.1.0", false) == std::vector<std::string>{"v0.1", "main"});
    CHECK(docs::refsToTry("0.1.0", true) == std::vector<std::string>{"main"});
}

TEST_CASE("docs: the override wins, then the cached tag, then cached main, then the bundled copy")
{
    TempDir t;
    docs::Sources s{t.path / "checkout", t.path / "cache", t.path / "bundled", {"v0.1", "main"}};
    CHECK_FALSE(docs::selectRoot(s));

    makeSite(s.bundledDir);
    CHECK(docs::selectRoot(s)->source == "bundled");

    makeSite(docs::cacheDirFor(s.cacheRoot, "main"));
    CHECK(docs::selectRoot(s)->ref == "main");

    makeSite(docs::cacheDirFor(s.cacheRoot, "v0.1"));
    auto tag = docs::selectRoot(s);
    CHECK(tag->source == "cache");
    CHECK(tag->ref == "v0.1");

    // An override that isn't a site is ignored.
    fs::create_directories(s.overrideDir);
    CHECK(docs::selectRoot(s)->source == "cache");
    makeSite(s.overrideDir);
    auto over = docs::selectRoot(s);
    CHECK(over->source == "override");
    CHECK(over->dir == s.overrideDir);
}

TEST_CASE("docs: an archive installs atomically and a bad one keeps the last good copy")
{
    TempDir t;
    fs::path cache = t.path / "cache";
    fs::path zip = t.path / "a.zip";
    writeZip(zip, siteEntries("# First\n"));
    std::string error;
    REQUIRE(docs::installArchive(zip, cache, "main", kShaA, &error));
    fs::path dir = docs::cacheDirFor(cache, "main");
    CHECK(readFile(dir / "docs" / "README.md") == "# First\n");
    CHECK(fs::exists(dir / "assets" / "vendor" / "docsify" / "docsify.min.js"));
    CHECK(docs::stampSha(dir) == kShaA);

    SUBCASE("a newer archive replaces it")
    {
        writeZip(zip, siteEntries("# Second\n"));
        REQUIRE(docs::installArchive(zip, cache, "main", kShaB, &error));
        CHECK(readFile(dir / "docs" / "README.md") == "# Second\n");
        CHECK(docs::stampSha(dir) == kShaB);
    }
    SUBCASE("an archive without docs is refused")
    {
        writeZip(zip, {{"top/other/file.txt", "x"}});
        CHECK_FALSE(docs::installArchive(zip, cache, "main", kShaB, &error));
        CHECK_FALSE(error.empty());
    }
    SUBCASE("a path escaping the folder is refused")
    {
        auto entries = siteEntries("# Evil\n");
        entries["lila-3d.github.io-abc/../../evil.txt"] = "x";
        writeZip(zip, entries);
        CHECK_FALSE(docs::installArchive(zip, cache, "main", kShaB, &error));
        CHECK_FALSE(fs::exists(t.path / "evil.txt"));
    }
    SUBCASE("a file that isn't a zip is refused")
    {
        writeFile(zip, "<html>rate limited</html>");
        CHECK_FALSE(docs::installArchive(zip, cache, "main", kShaB, &error));
    }
    if (docs::stampSha(dir) == kShaA)
        CHECK(readFile(dir / "docs" / "README.md") == "# First\n");
    CHECK(leftovers(cache) == 0);
}

TEST_CASE("docs: sync asks for the ref's sha and downloads only when it changed")
{
    TempDir t;
    fs::path cache = t.path / "cache";
    fs::path fixture = t.path / "fixture.zip";
    writeZip(fixture, siteEntries("# Synced\n"));

    std::string mainSha = kShaA;
    bool online = true;
    int downloads = 0;
    std::vector<std::string> asked;
    docs::Fetch fetch = [&](const std::string& url, const auto&, const fs::path& saveTo) -> docs::Response {
        asked.push_back(url);
        if (!online)
            return {.error = "Host not found"};
        if (url.find("/commits/v0.1") != std::string::npos)
            return {.status = 422, .body = "{}"};
        if (url.find("/commits/main") != std::string::npos)
            return {.status = 200, .body = mainSha + "\n"};
        if (url.find("codeload.github.com") != std::string::npos && url.ends_with(mainSha))
        {
            ++downloads;
            fs::copy_file(fixture, saveTo, fs::copy_options::overwrite_existing);
            return {.status = 200};
        }
        return {.status = 404};
    };

    docs::SyncResult r = docs::sync(fetch, cache, {"v0.1", "main"});
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(r.ref == "main");
    CHECK(downloads == 1);
    CHECK(readFile(docs::cacheDirFor(cache, "main") / "docs" / "README.md") == "# Synced\n");

    r = docs::sync(fetch, cache, {"v0.1", "main"});
    CHECK(r.ok);
    CHECK_FALSE(r.changed);
    CHECK(downloads == 1);

    mainSha = kShaB;
    r = docs::sync(fetch, cache, {"v0.1", "main"});
    CHECK(r.changed);
    CHECK(downloads == 2);
    CHECK(docs::stampSha(docs::cacheDirFor(cache, "main")) == kShaB);

    online = false;
    mainSha = std::string(40, 'c');
    r = docs::sync(fetch, cache, {"v0.1", "main"});
    CHECK_FALSE(r.ok);
    CHECK(docs::stampSha(docs::cacheDirFor(cache, "main")) == kShaB);
    CHECK(leftovers(cache) == 0);
}

TEST_CASE("docs: page ids, public URLs and heading ids follow docsify")
{
    std::string anchor;
    CHECK(docs::normalizePageId("essentials.md") == "essentials");
    CHECK(docs::normalizePageId("#/curves?id=pipe", &anchor) == "curves");
    CHECK(anchor == "pipe");
    CHECK(docs::normalizePageId("/") == "");
    CHECK(docs::normalizePageId("README.md") == "");
    CHECK(docs::normalizePageId("python/README.md") == "python/");
    CHECK(docs::normalizePageId("essentials#selection-modes", &anchor) == "essentials");
    CHECK(anchor == "selection-modes");

    CHECK(docs::publicUrl("essentials") == "https://lila-3d.github.io/docs/#/essentials");
    CHECK(docs::publicUrl("curves", "pipe") == "https://lila-3d.github.io/docs/#/curves?id=pipe");
    CHECK(docs::publicUrl("") == "https://lila-3d.github.io/docs/#/");

    CHECK(docs::slug("Create a primitive in place") == "create-a-primitive-in-place");
    CHECK(docs::slug("Find any menu command") == "find-any-menu-command");
    CHECK(docs::slug("Make a mesh (from curves)") == "make-a-mesh-from-curves");
    CHECK(docs::slug("3D cursor") == "_3d-cursor");
    CHECK(docs::slug("Vector.normalize") == "vectornormalize");
    CHECK(docs::slug("polyCube") == "polycube");
}

TEST_CASE("docs: pages resolve, API symbols find their page, the sidebar and search read the site")
{
    TempDir t;
    fs::path d = t.path / "docs";
    writeFile(d / "README.md", "# Lila Docs\n");
    writeFile(d / "essentials.md", "# Essentials\n\n## Selection modes\n\nPress **Shift+1** for vertex mode.\n\n## Floating shelf\n\nPress the backtick.\n");
    writeFile(d / "_sidebar.md", "- [Home](/)\n- [Essentials](essentials.md)\n  - [Modes](essentials.md?id=selection-modes)\n- [Site](https://lila-3d.github.io/ ':ignore')\n");

    CHECK(docs::resolvePage(d, "essentials").found);
    docs::Page missing = docs::resolvePage(d, "nowhere");
    CHECK_FALSE(missing.found);
    CHECK(missing.file == d / "README.md");
    CHECK_FALSE(docs::resolvePage(d, "../secret").found);

    CHECK(docs::apiPageFor(d, "lila.ops.polyCube").empty());
    writeFile(d / "python" / "README.md", "# Python\n");
    writeFile(d / "python" / "lila.ops.md", "# lila.ops\n");
    writeFile(d / "python" / "lila.math.md", "# lila.math\n");
    CHECK(docs::resolvePage(d, "python").id == "python/");
    CHECK(docs::apiPageFor(d, "lila.ops") == "python/lila.ops");
    CHECK(docs::apiPageFor(d, "lila.ops.polyCube") == "python/lila.ops?id=polycube");
    CHECK(docs::apiPageFor(d, "lila.math.Vector.normalize") == "python/lila.math?id=vectornormalize");
    CHECK(docs::apiPageFor(d, "lila.nodes.Node") == "python/");

    std::vector<docs::SidebarEntry> side = docs::readSidebar(d);
    REQUIRE(side.size() == 4);
    CHECK(side[0].target == "");
    CHECK(side[1].target == "essentials");
    CHECK(side[2].depth == 1);
    CHECK(side[3].external);

    std::vector<docs::Hit> hits = docs::search(d, "vertex mode");
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].pageId == "essentials");
    CHECK(hits[0].anchor == "selection-modes");
    CHECK(hits[0].snippet.find("Shift+1") != std::string::npos);
    CHECK(docs::search(d, "floating")[0].title == "Essentials > Floating shelf");
    CHECK(docs::search(d, "no such words").empty());
}

TEST_CASE("docs: pasted URLs in every form open their page")
{
    using docs::pageSpecFromUrl;
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs/#/curves?id=pipe") == "curves?id=pipe");
    CHECK(pageSpecFromUrl("  lila-3d.github.io/docs/#/curves  ") == "curves");
    CHECK(pageSpecFromUrl("http://lila-3d.github.io/docs/#/curves/") == "curves/");
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs/#/curves?id=pipe/") == "curves?id=pipe");
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs/#/python/") == "python/");
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs/") == "");
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs") == "");
    CHECK(pageSpecFromUrl("lila-3d.github.io") == "");
    CHECK(pageSpecFromUrl("https://LILA-3D.github.io/docs/index.html#/essentials") == "essentials");
    CHECK(pageSpecFromUrl("https://lila-3d.github.io/docs/curves") == "curves");
    CHECK(pageSpecFromUrl("http://127.0.0.1:52011/docs/#/curves?id=pipe") == "curves?id=pipe");
    CHECK(pageSpecFromUrl("localhost:8000/docs/#/essentials") == "essentials");
    CHECK(pageSpecFromUrl("lila://docs/curves?id=pipe") == "curves?id=pipe");
    CHECK(pageSpecFromUrl("lila://docs/#/curves") == "curves");
    CHECK(pageSpecFromUrl("#/curves?id=pipe") == "curves?id=pipe");
    CHECK_FALSE(pageSpecFromUrl("pipe tool"));
    CHECK_FALSE(pageSpecFromUrl("https://example.com/docs/#/curves"));
    CHECK_FALSE(pageSpecFromUrl("curves"));
}

TEST_CASE("docs: links in a page become public site URLs")
{
    using docs::publicUrlForLink;
    CHECK(publicUrlForLink("#pipe", "curves") == "https://lila-3d.github.io/docs/#/curves?id=pipe");
    CHECK(publicUrlForLink("?id=pipe", "curves") == "https://lila-3d.github.io/docs/#/curves?id=pipe");
    CHECK(publicUrlForLink("essentials.md#select", "curves") == "https://lila-3d.github.io/docs/#/essentials?id=select");
    CHECK(publicUrlForLink("#/python/", "curves") == "https://lila-3d.github.io/docs/#/python/");
    CHECK(publicUrlForLink("https://example.com/x", "curves") == "https://example.com/x");
}

TEST_CASE("docs: a trailing slash on a page that is a file still finds it")
{
    TempDir t;
    makeSite(t.path);
    writeFile(t.path / "docs" / "curves.md", "# Curves\n");
    docs::Page p = docs::resolvePage(t.path / "docs", "curves/");
    CHECK(p.found);
    CHECK(p.id == "curves");
}

TEST_CASE("docs: bookmarks toggle, reorder and round-trip through prefs")
{
    using docs::Bookmark;
    std::vector<Bookmark> list;
    CHECK(docs::toggleBookmark(list, {"Curves", "curves", ""}));
    CHECK(docs::toggleBookmark(list, {"Curves > Pipe", "curves", "pipe"}));
    CHECK(docs::toggleBookmark(list, {"Home", "", ""}));
    CHECK(docs::findBookmark(list, "curves", "pipe") == 1);
    CHECK(docs::findBookmark(list, "curves", "loft") == -1);

    docs::moveBookmark(list, 2, 0);
    CHECK(list[0].title == "Home");
    docs::moveBookmark(list, 0, 99);
    CHECK(list.back().title == "Home");

    list[0].title = "My curves";
    auto back = docs::bookmarksFromJson(docs::bookmarksToJson(list));
    CHECK(back == list);

    CHECK_FALSE(docs::toggleBookmark(list, {"anything", "curves", "pipe"}));
    CHECK(docs::findBookmark(list, "curves", "pipe") == -1);
    CHECK(list.size() == 2);

    // Damaged prefs keep what they can.
    nlohmann::json j = nlohmann::json::parse(R"([{"title": "A", "page": "a"}, 3, {"page": "b"}, {"title": "C"}, "x"])");
    auto kept = docs::bookmarksFromJson(j);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0] == Bookmark{"A", "a", ""});
    CHECK(docs::bookmarksFromJson(nlohmann::json()).empty());
}

namespace
{
bool has(const std::string& html, std::string_view part)
{
    return html.find(part) != std::string::npos;
}
} // namespace

TEST_CASE("docs html: escaped pipes in table cells, code spans included, render as pipes")
{
    std::string html = docs::markdownToHtml("| Type | To |\n| --- | --- |\n| `root\\|spine` | Any term with `\\|`. |\n| a \\| b | x |\n");
    CHECK(has(html, "<table class=\"grid\""));
    CHECK(has(html, "root|spine"));
    CHECK(has(html, "a | b"));
    CHECK_FALSE(has(html, "\\|"));
    // Four cells a row: the escaped pipes never split one.
    CHECK(has(html, "<th align=\"center\">Type</th><th align=\"center\">To</th></tr>"));
    CHECK(has(html, "<td class=\"alt\">a | b</td><td class=\"alt\">x</td></tr>"));
}

TEST_CASE("docs html: table rows alternate and follow the delimiter row's alignment")
{
    std::string html = docs::markdownToHtml("| A | B |\n| :-- | --: |\n| 1 | 2 |\n| 3 | 4 |\n| 5 | 6 |\n");
    CHECK(has(html, "<th align=\"left\">A</th><th align=\"right\">B</th>"));
    CHECK(has(html, "<td align=\"left\">1</td>"));
    CHECK(has(html, "<td class=\"alt\" align=\"left\">3</td>"));
    CHECK(has(html, "<td align=\"left\">5</td>"));
}

TEST_CASE("docs html: blockquotes become a bar cell beside a body cell")
{
    std::string html = docs::markdownToHtml("Intro.\n\n> **Note:** There is no type dropdown.\nlazy line\n\nAfter.\n");
    CHECK(has(html, "<table class=\"quote\""));
    CHECK(has(html, "<td class=\"bar\" width=\"4\"></td><td class=\"body\"><p class=\"q\"><b>Note:</b> There is no type dropdown. lazy line</p>"));
    CHECK(has(html, "<p>After.</p>"));
}

TEST_CASE("docs html: images are sized from their own size, scaled to the column keeping aspect")
{
    docs::HtmlOptions o;
    o.imageSize = [](std::string_view src) -> std::optional<std::pair<int, int>> {
        if (src == "images/big.png")
            return std::pair{1600, 900};
        if (src == "images/small.png")
            return std::pair{200, 100};
        return std::nullopt;
    };
    o.maxImageWidth = 800;
    std::string html = docs::markdownToHtml("![Big one](images/big.png)\n\n![Small](images/small.png)\n\n![Gone](images/none.png)\n", o);
    CHECK(has(html, "<p class=\"img\"><img src=\"images/big.png\" alt=\"Big one\" width=\"800\" height=\"450\" /></p>"));
    CHECK(has(html, "<img src=\"images/small.png\" alt=\"Small\" width=\"200\" height=\"100\" />"));
    CHECK(has(html, "<img src=\"images/none.png\" alt=\"Gone\" />"));
}

TEST_CASE("docs html: headings carry docsify ids and h2 sections a rule")
{
    std::string html = docs::markdownToHtml("# Essentials\n\nText.\n\n## Find any `menu` command\n\n### Ctrl+T & more ##\n");
    CHECK(has(html, "<h1><a name=\"essentials\"></a>Essentials</h1>"));
    CHECK(has(html, "<hr/>\n<h2><a name=\"find-any-menu-command\"></a>Find any <code>"));
    CHECK(has(html, "<h3><a name=\"ctrlt-more\"></a>Ctrl+T &amp; more</h3>"));
    // No rule above a page that opens on an h2.
    CHECK(docs::markdownToHtml("## First\n").rfind("<h2>", 0) == 0);
}

TEST_CASE("docs html: lists, inline markup, code blocks and comments")
{
    std::string html = docs::markdownToHtml("- **Enter** runs it.\n- Nested:\n  - inner *em* and _em_ but snake_case\n- [Link](curves.md?id=pipe)\n\n"
                                            "1. One\n2. Two\n\n```python\nif a < b:\n    pass\n```\n\n<!-- TODO image\nof it -->\n"
                                            "Text with `<driver>_x`, <driver> and ~~gone~~ \\*literal\\*.\n");
    CHECK(has(html, "<ul>\n<li><b>Enter</b> runs it.</li>\n<li>Nested:<ul>\n<li>inner <i>em</i> and <i>em</i> but snake_case</li>"));
    CHECK(has(html, "<li><a href=\"curves.md?id=pipe\">Link</a></li>"));
    CHECK(has(html, "<ol>\n<li>One</li>\n<li>Two</li>\n</ol>"));
    CHECK(has(html, "<table class=\"pre\""));
    CHECK(has(html, "<pre>if a &lt; b:\n    pass</pre>"));
    CHECK_FALSE(has(html, "TODO"));
    CHECK(has(html, "&lt;driver&gt;_x"));
    CHECK(has(html, ", &lt;driver&gt; and <s>gone</s> *literal*."));
}

TEST_CASE("inline code hyphens never wrap and read back as hyphens")
{
    const std::string html = docs::markdownToHtml("Match `[a-c]` here.");
    CHECK(html.find("[a\xE2\x80\x91" "c]") != std::string::npos);
    CHECK(html.find("[a-c]") == std::string::npos);
}

TEST_CASE("docs: page titles come from the sidebar, else the page id; each page has one action id")
{
    TempDir t;
    fs::path d = t.path / "docs";
    writeFile(d / "_sidebar.md", "- [Python API](python/)\n- [Node editor](node-editor.md)\n  - [Keys](node-editor.md?id=keys)\n- [Ext](https://x.y/ ':ignore')\n");
    CHECK(docs::pageTitle(d, "node-editor") == "Node editor");
    CHECK(docs::pageTitle(d, "node-editor?id=keys") == "Node editor");
    CHECK(docs::pageTitle(d, "python/") == "Python API");
    CHECK(docs::pageTitle(d, "stagehand") == "Stagehand");
    CHECK(docs::pageTitle({}, "node-editor") == "Node Editor");
    CHECK(docs::pageTitle({}, "python/lila.ops") == "Python Lila Ops");
    CHECK(docs::pageTitle({}, "") == "Lila");

    writeFile(d / "_sidebar.md", "---\n\n- <span class=\"nav-label\">Nodes</span>\n  - [Node editor](node-editor.md)\n    - [Geometry nodes](geometry-nodes.md)\n");
    std::vector<docs::SidebarEntry> side = docs::readSidebar(d);
    REQUIRE(side.size() == 3);
    CHECK(side[0].label);
    CHECK(side[0].title == "Nodes");
    CHECK(side[0].target.empty());
    CHECK(side[1].depth == 1);
    CHECK(side[2].target == "geometry-nodes");
    CHECK(docs::pageTitle(d, "geometry-nodes") == "Geometry nodes");

    CHECK(docs::pageActionId("curves") == "help.docs.curves");
    CHECK(docs::pageActionId("curves.md") == "help.docs.curves");
    CHECK(docs::pageActionId("essentials?id=filter-the-outliner") == "help.docs.essentials.filter-the-outliner");
    CHECK(docs::pageActionId("python/") == "help.docs.python");
    CHECK(docs::pageActionId("") == "help.docs.home");
}
