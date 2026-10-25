#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The user docs (the lila-3d.github.io site, docsify pages under docs/) kept offline: which copy to read, how a copy is
 * fetched from GitHub and swapped in, and page lookup and search over it. No Qt here; DocsUi supplies the network and the
 * panel.
 */
namespace lila::ui::docs
{
namespace fs = std::filesystem;

inline constexpr const char* kRepo = "lila-3d/lila-3d.github.io";
inline constexpr const char* kPublicSite = "https://lila-3d.github.io/docs/";
/** Written into each cached copy: {"ref", "sha"}. */
inline constexpr const char* kStampFile = ".lila-docs.json";

/** "v<major>.<minor>" for a release, "main" for a dev build or a version that does not parse. */
LILA_UI_API std::string refForVersion(std::string_view version, bool devBuild);
/** Refs to fetch, best first: the release tag, then main. */
LILA_UI_API std::vector<std::string> refsToTry(std::string_view version, bool devBuild);

/** True for a site checkout: it has docs/README.md. */
LILA_UI_API bool isSiteRoot(const fs::path& dir);

struct Sources
{
    fs::path overrideDir; // LILA_DOCS_DIR: a local checkout, used as is and never updated
    fs::path cacheRoot;   // <user config>/docs, one folder per ref
    fs::path bundledDir;  // <exe>/docs, shipped with an install
    std::vector<std::string> refs;
};

struct Root
{
    fs::path dir; // the site root; pages are under dir/docs
    std::string source; // override | cache | bundled
    std::string ref;
    std::string sha;
};

/** LILA_DOCS_DIR, the cache under the user config dir, the bundled copy next to the exe, and this build's refs. */
LILA_UI_API Sources defaultSources();
/** The override when it is a site, else the cached copy of the first ref that has one, else the bundled copy. */
LILA_UI_API std::optional<Root> selectRoot(const Sources& s);

LILA_UI_API fs::path cacheDirFor(const fs::path& cacheRoot, std::string_view ref);
LILA_UI_API std::string stampSha(const fs::path& dir);

/**
 * Unpacks a GitHub archive zip into cacheRoot/<ref>: into a temp folder first (the zip's single top folder stripped),
 * checked to be a site, stamped, then renamed over the old copy. Any failure leaves the old copy as it was.
 */
LILA_UI_API bool installArchive(const fs::path& zip, const fs::path& cacheRoot, std::string_view ref, std::string_view sha,
                                std::string* error = nullptr);

struct Response
{
    int status = 0; // 0 when the request never got an answer
    std::string body;
    std::string error;
};
/** GET url. With saveTo set the body goes to that file instead of Response::body. */
using Fetch = std::function<Response(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                                     const fs::path& saveTo)>;

struct SyncResult
{
    bool ok = false;
    bool changed = false;
    std::string ref;
    std::string sha;
    std::string message;
};

/**
 * Brings the cache up to date for the first ref GitHub knows: asks for the ref's commit sha, and downloads and installs
 * the archive only when it differs from the cached copy's.
 */
LILA_UI_API SyncResult sync(const Fetch& fetch, const fs::path& cacheRoot, const std::vector<std::string>& refs,
                            const std::atomic<bool>* cancel = nullptr);

/** A page id as docsify routes it: "essentials", "api/" (its README), "" (home). Accepts .md names, "#/..." and anchors. */
LILA_UI_API std::string normalizePageId(std::string_view page, std::string* anchor = nullptr);

struct Page
{
    fs::path file;
    std::string id;
    std::string anchor;
    bool found = false;
};
/** The page's .md under docsDir (the site's docs folder); the home page, found false, when it is missing. */
LILA_UI_API Page resolvePage(const fs::path& docsDir, std::string_view page);
/** The same page on the public site. */
LILA_UI_API std::string publicUrl(std::string_view pageId, std::string_view anchor = {});
/** A link in a page, as the public site would open it; `currentPage` resolves "#heading" links. */
LILA_UI_API std::string publicUrlForLink(std::string_view href, std::string_view currentPage);
/**
 * The page spec ("curves?id=pipe", "" for home) of a docs URL: the public site with or without a scheme, a local server
 * copy on 127.0.0.1 or localhost, lila://docs/..., or a bare "#/page". Nothing for any other text.
 */
LILA_UI_API std::optional<std::string> pageSpecFromUrl(std::string_view text);
/** docsify's heading id. */
LILA_UI_API std::string slug(std::string_view heading);

/**
 * The API page for a Python symbol: api/<dotted name>, else the closest enclosing module or class page, else the API
 * home. Empty when the docs have no api/ section.
 */
LILA_UI_API std::string apiPageFor(const fs::path& docsDir, std::string_view symbol);

struct HtmlOptions
{
    /** An image's own size in pixels, from its src as the page writes it; nullopt when it can't be read. */
    std::function<std::optional<std::pair<int, int>>(std::string_view src)> imageSize;
    /** Wider images are scaled down to this, keeping their aspect; 0 keeps every image at its own size. */
    int maxImageWidth = 0;
};

/**
 * A docs page's markdown (GFM: tables, fences, strikethrough) as the HTML subset QTextDocument renders. Headings carry
 * <a name> anchors with docsify's ids, h2 sections get the site's rule above them, blockquotes and code blocks become
 * one-row tables (Qt has no block borders or padding), and paragraphs carry classes for the stylesheet: "img" for a
 * paragraph that is one image, "q" inside a blockquote.
 */
LILA_UI_API std::string markdownToHtml(std::string_view md, const HtmlOptions& options = {});

struct SidebarEntry
{
    std::string title;
    std::string target; // page id, or a URL when external
    int depth = 0;
    bool external = false;
};
LILA_UI_API std::vector<SidebarEntry> readSidebar(const fs::path& docsDir);

struct Hit
{
    std::string pageId;
    std::string anchor;
    std::string title;
    std::string snippet;
};
/** Sections whose heading or text contain every word of query, heading matches first. */
LILA_UI_API std::vector<Hit> search(const fs::path& docsDir, std::string_view query, size_t limit = 50);

struct Bookmark
{
    std::string title;
    std::string page;
    std::string anchor;

    bool operator==(const Bookmark&) const = default;
};

/** Bookmarks as kept in prefs: [{"title", "page", "anchor"}]. Entries without a title or page are dropped. */
LILA_UI_API std::vector<Bookmark> bookmarksFromJson(const nlohmann::json& j);
LILA_UI_API nlohmann::json bookmarksToJson(const std::vector<Bookmark>& list);
/** Index of the bookmark for page and anchor, or -1. */
LILA_UI_API int findBookmark(const std::vector<Bookmark>& list, std::string_view page, std::string_view anchor);
/** Adds b at the end, or removes the bookmark for its page and anchor. True when it added. */
LILA_UI_API bool toggleBookmark(std::vector<Bookmark>& list, const Bookmark& b);
/** Moves the bookmark at `from` to index `to`, both clamped. */
LILA_UI_API void moveBookmark(std::vector<Bookmark>& list, int from, int to);
} // namespace lila::ui::docs
