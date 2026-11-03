#include "ui/Docs.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Version.h"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace lila::ui::docs
{
namespace
{
using json = nlohmann::json;

std::string trim(std::string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return std::string(s.substr(b, e - b));
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(std::string_view s, std::string_view p)
{
    return s.substr(0, p.size()) == p;
}

bool endsWith(std::string_view s, std::string_view p)
{
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

fs::path envDir(const char* name)
{
#ifdef _WIN32
    std::wstring wide(name, name + std::strlen(name));
    const wchar_t* v = _wgetenv(wide.c_str());
    return v && *v ? fs::path(v) : fs::path();
#else
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
#endif
}

std::string randomSuffix()
{
    std::random_device rd;
    std::ostringstream s;
    s << std::hex << rd() << rd();
    return s.str();
}

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool isHexSha(std::string_view s)
{
    return s.size() == 40 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

/** A zip entry's path below the stripped top folder, or empty when it would land outside the target. */
fs::path safeRelative(std::string_view name)
{
    if (name.empty() || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos)
        return {};
    fs::path rel = paths::fromUtf8(name).lexically_normal();
    for (const fs::path& part : rel)
        if (part == "..")
            return {};
    return rel;
}

/** Markdown reduced to the words a reader sees, for search snippets. */
std::string plainText(std::string_view md)
{
    std::string out;
    out.reserve(md.size());
    for (size_t i = 0; i < md.size(); ++i)
    {
        char c = md[i];
        if (startsWith(md.substr(i), "<!--"))
        {
            size_t end = md.find("-->", i);
            i = end == std::string_view::npos ? md.size() : end + 2;
            continue;
        }
        if (c == '!' && i + 1 < md.size() && md[i + 1] == '[')
            continue;
        if (c == ']' && i + 1 < md.size() && md[i + 1] == '(')
        {
            size_t end = md.find(')', i);
            i = end == std::string_view::npos ? md.size() : end;
            continue;
        }
        if (c == '*' || c == '`' || c == '[' || c == '|' || c == '#' || c == '\\')
            continue;
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && !out.empty() && out.back() == ' ')
            continue;
        out += c;
    }
    return trim(out);
}

std::string pageIdFor(const fs::path& docsDir, const fs::path& file)
{
    std::string rel = paths::toUtf8(file.lexically_relative(docsDir));
    if (endsWith(rel, ".md"))
        rel.resize(rel.size() - 3);
    if (rel == "README")
        return {};
    if (endsWith(rel, "/README"))
        rel.resize(rel.size() - 6);
    return rel;
}
} // namespace

std::string refForVersion(std::string_view version, bool devBuild)
{
    if (devBuild)
        return "main";
    std::string v = trim(version);
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.erase(0, 1);
    size_t dot = v.find('.');
    if (dot == std::string::npos || dot == 0)
        return "main";
    size_t end = dot + 1;
    while (end < v.size() && std::isdigit(static_cast<unsigned char>(v[end])))
        ++end;
    std::string major = v.substr(0, dot), minor = v.substr(dot + 1, end - dot - 1);
    bool digits = !minor.empty() && std::all_of(major.begin(), major.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    // A pre-release (0.2.0-dev) documents the line it leads to, which only main has yet.
    if (!digits || v.find('-') != std::string::npos)
        return "main";
    return "v" + major + "." + minor;
}

std::vector<std::string> refsToTry(std::string_view version, bool devBuild)
{
    std::string ref = refForVersion(version, devBuild);
    if (ref == "main")
        return {"main"};
    return {ref, "main"};
}

bool isSiteRoot(const fs::path& dir)
{
    std::error_code ec;
    return !dir.empty() && fs::is_regular_file(dir / "docs" / "README.md", ec);
}

Sources defaultSources()
{
    Sources s;
    s.overrideDir = envDir("LILA_DOCS_DIR");
    s.cacheRoot = paths::userConfigDir() / "docs";
    s.bundledDir = paths::executableDir() / "docs";
    std::error_code ec;
    // Installs carry resources/ next to the exe; dev builds read the source tree's.
    bool dev = !fs::is_directory(paths::executableDir() / "resources", ec);
    s.refs = refsToTry(version::number(), dev);
    return s;
}

std::optional<Root> selectRoot(const Sources& s)
{
    if (isSiteRoot(s.overrideDir))
        return Root{s.overrideDir, "override", {}, {}};
    for (const std::string& ref : s.refs)
    {
        fs::path dir = cacheDirFor(s.cacheRoot, ref);
        if (isSiteRoot(dir))
            return Root{dir, "cache", ref, stampSha(dir)};
    }
    if (isSiteRoot(s.bundledDir))
        return Root{s.bundledDir, "bundled", {}, stampSha(s.bundledDir)};
    return std::nullopt;
}

fs::path cacheDirFor(const fs::path& cacheRoot, std::string_view ref)
{
    std::string safe;
    for (char c : ref)
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_';
    if (safe.empty() || safe.front() == '.')
        safe.insert(0, "_");
    return cacheRoot / safe;
}

std::string stampSha(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_regular_file(dir / kStampFile, ec))
        return {};
    json j = json::parse(readFile(dir / kStampFile), nullptr, false);
    return j.is_object() ? j.value("sha", std::string()) : std::string();
}

bool installArchive(const fs::path& zip, const fs::path& cacheRoot, std::string_view ref, std::string_view sha, std::string* error)
{
    LILA_ZONE();

    auto fail = [error](std::string why) {
        if (error)
            *error = std::move(why);
        return false;
    };
    std::error_code ec;
    fs::create_directories(cacheRoot, ec);
    const fs::path finalDir = cacheDirFor(cacheRoot, ref);
    const fs::path tmp = cacheRoot / (".tmp-" + paths::toUtf8(finalDir.filename()) + "-" + randomSuffix());

    std::string bytes = readFile(zip);
    mz_zip_archive ar{};
    if (bytes.empty() || !mz_zip_reader_init_mem(&ar, bytes.data(), bytes.size(), 0))
        return fail("not a zip archive: " + paths::toUtf8(zip));

    const mz_uint count = mz_zip_reader_get_num_files(&ar);
    std::vector<std::string> names(count);
    // GitHub archives hold everything under one "<repo>-<ref>/" folder.
    std::string top;
    bool shared = count > 0;
    for (mz_uint i = 0; i < count; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&ar, i, &st))
        {
            mz_zip_reader_end(&ar);
            return fail("unreadable zip entry");
        }
        names[i] = st.m_filename;
        std::replace(names[i].begin(), names[i].end(), '\\', '/');
        std::string first = names[i].substr(0, names[i].find('/'));
        if (i == 0)
            top = first;
        if (first != top || names[i].find('/') == std::string::npos)
            shared = false;
    }

    bool ok = true;
    std::string why;
    for (mz_uint i = 0; i < count && ok; ++i)
    {
        if (mz_zip_reader_is_file_a_directory(&ar, i))
            continue;
        std::string_view name = names[i];
        if (shared)
            name.remove_prefix(top.size() + 1);
        if (name.empty())
            continue;
        fs::path rel = safeRelative(name);
        if (rel.empty())
        {
            ok = false;
            why = "unsafe path in archive: " + std::string(name);
            break;
        }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&ar, i, &size, 0);
        if (!data)
        {
            ok = false;
            why = "cannot extract " + std::string(name);
            break;
        }
        fs::path out = tmp / rel;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream f(out, std::ios::binary);
        f.write(static_cast<const char*>(data), std::streamsize(size));
        mz_free(data);
        if (!f)
        {
            ok = false;
            why = "cannot write " + paths::toUtf8(out);
        }
    }
    mz_zip_reader_end(&ar);

    if (ok && !isSiteRoot(tmp))
    {
        ok = false;
        why = "archive has no docs/README.md";
    }
    if (ok)
    {
        std::ofstream stamp(tmp / kStampFile, std::ios::binary);
        stamp << json{{"ref", std::string(ref)}, {"sha", std::string(sha)}}.dump(2);
        ok = bool(stamp);
        if (!ok)
            why = "cannot write the stamp";
    }
    if (!ok)
    {
        fs::remove_all(tmp, ec);
        return fail(why);
    }

    const fs::path old = cacheRoot / (".old-" + paths::toUtf8(finalDir.filename()) + "-" + randomSuffix());
    bool hadOld = fs::exists(finalDir, ec);
    if (hadOld)
    {
        fs::rename(finalDir, old, ec);
        if (ec)
        {
            fs::remove_all(tmp, ec);
            return fail("the current copy is in use: " + paths::toUtf8(finalDir));
        }
    }
    fs::rename(tmp, finalDir, ec);
    if (ec)
    {
        std::string msg = ec.message();
        if (hadOld)
            fs::rename(old, finalDir, ec);
        fs::remove_all(tmp, ec);
        return fail("cannot move the new copy in place: " + msg);
    }
    fs::remove_all(old, ec);
    return true;
}

SyncResult sync(const Fetch& fetch, const fs::path& cacheRoot, const std::vector<std::string>& refs, const std::atomic<bool>* cancel)
{
    LILA_ZONE();

    const std::vector<std::pair<std::string, std::string>> ua = {{"User-Agent", "Lila"}};
    SyncResult res;
    for (const std::string& ref : refs)
    {
        if (cancel && *cancel)
            return {.message = "cancelled"};
        auto headers = ua;
        headers.emplace_back("Accept", "application/vnd.github.sha");
        Response r = fetch("https://api.github.com/repos/" + std::string(kRepo) + "/commits/" + ref, headers, {});
        // Unknown ref: a release whose docs were never tagged falls through to main.
        if (r.status == 404 || r.status == 422)
            continue;
        if (r.status != 200)
            return {.ref = ref, .message = r.status ? "GitHub answered " + std::to_string(r.status) : "offline (" + r.error + ")"};
        std::string sha = trim(r.body);
        if (!isHexSha(sha))
            return {.ref = ref, .message = "unexpected answer from GitHub"};
        if (stampSha(cacheDirFor(cacheRoot, ref)) == sha)
            return {.ok = true, .ref = ref, .sha = sha, .message = "up to date"};
        if (cancel && *cancel)
            return {.message = "cancelled"};

        std::error_code ec;
        fs::create_directories(cacheRoot, ec);
        fs::path zip = cacheRoot / (".download-" + paths::toUtf8(cacheDirFor(cacheRoot, ref).filename()) + "-" + randomSuffix() + ".zip");
        Response z = fetch("https://codeload.github.com/" + std::string(kRepo) + "/zip/" + sha, ua, zip);
        if (z.status != 200)
        {
            fs::remove(zip, ec);
            return {.ref = ref, .message = z.status ? "download failed (" + std::to_string(z.status) + ")" : "download failed (" + z.error + ")"};
        }
        std::string error;
        bool installed = installArchive(zip, cacheRoot, ref, sha, &error);
        fs::remove(zip, ec);
        if (!installed)
            return {.ref = ref, .message = error};
        return {.ok = true, .changed = true, .ref = ref, .sha = sha, .message = "updated"};
    }
    res.message = "no docs published for this version";
    return res;
}

std::string normalizePageId(std::string_view page, std::string* anchor)
{
    std::string s = trim(page);
    if (anchor)
        anchor->clear();
    if (startsWith(s, "#"))
        s.erase(0, 1);
    while (startsWith(s, "/"))
        s.erase(0, 1);
    if (size_t q = s.find("?id="); q != std::string::npos)
    {
        std::string a = s.substr(q + 4);
        a = a.substr(0, a.find('&'));
        if (anchor)
            *anchor = a;
        s.resize(q);
    }
    else if (size_t h = s.find('#'); h != std::string::npos)
    {
        if (anchor)
            *anchor = s.substr(h + 1);
        s.resize(h);
    }
    if (size_t q = s.find('?'); q != std::string::npos)
        s.resize(q);
    if (startsWith(s, "./"))
        s.erase(0, 2);
    if (endsWith(s, ".md"))
        s.resize(s.size() - 3);
    if (s == "README")
        s.clear();
    else if (endsWith(s, "/README"))
        s.resize(s.size() - 6);
    return s;
}

Page resolvePage(const fs::path& docsDir, std::string_view page)
{
    Page p;
    p.id = normalizePageId(page, &p.anchor);
    std::error_code ec;
    auto fileFor = [&](const std::string& id) {
        if (id.empty() || endsWith(id, "/"))
            return docsDir / paths::fromUtf8(id) / "README.md";
        return docsDir / paths::fromUtf8(id + ".md");
    };
    if (p.id.find("..") == std::string::npos)
    {
        p.file = fileFor(p.id);
        p.found = fs::is_regular_file(p.file, ec);
        if (!p.found && !p.id.empty() && !endsWith(p.id, "/") && fs::is_regular_file(fileFor(p.id + "/"), ec))
        {
            p.id += "/";
            p.file = fileFor(p.id);
            p.found = true;
        }
        else if (!p.found && endsWith(p.id, "/") && p.id.size() > 1 && fs::is_regular_file(fileFor(p.id.substr(0, p.id.size() - 1)), ec))
        {
            p.id.pop_back();
            p.file = fileFor(p.id);
            p.found = true;
        }
    }
    if (!p.found)
    {
        p.id.clear();
        p.anchor.clear();
        p.file = fileFor({});
    }
    return p;
}

std::string publicUrl(std::string_view pageId, std::string_view anchor)
{
    std::string url = std::string(kPublicSite) + "#/" + std::string(pageId);
    if (!anchor.empty())
        url += "?id=" + std::string(anchor);
    return url;
}

std::string publicUrlForLink(std::string_view href, std::string_view currentPage)
{
    std::string h = trim(href);
    std::string l = lower(h);
    if (startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "mailto:"))
        return h;
    if (startsWith(h, "#") && !startsWith(h, "#/"))
        return publicUrl(currentPage, h.substr(1));
    if (startsWith(h, "?id="))
        return publicUrl(currentPage, h.substr(4));
    std::string anchor;
    std::string id = normalizePageId(h, &anchor);
    return publicUrl(id, anchor);
}

std::optional<std::string> pageSpecFromUrl(std::string_view text)
{
    std::string s = trim(text);
    std::string l = lower(s);
    std::string route;
    auto afterDocs = [](std::string rest) {
        // rest is the path after the host: "/docs/#/page", "/docs/index.html#/page", "/#/page", "/docs".
        std::string path = rest.substr(0, rest.find('#'));
        std::string hash = rest.find('#') == std::string::npos ? std::string() : rest.substr(rest.find('#'));
        while (startsWith(path, "/"))
            path.erase(0, 1);
        if (std::string lp = lower(path); lp == "docs" || startsWith(lp, "docs/") || startsWith(lp, "docs?"))
            path.erase(0, 4);
        while (startsWith(path, "/"))
            path.erase(0, 1);
        if (endsWith(lower(path), "index.html"))
            path.resize(path.size() - 10);
        return hash.empty() ? path : hash;
    };
    if (startsWith(l, "#/"))
        route = s;
    else if (startsWith(l, "lila://docs"))
        route = afterDocs(s.substr(std::string_view("lila://").size()));
    else
    {
        for (std::string_view scheme : {"https://", "http://"})
            if (startsWith(l, scheme))
            {
                s.erase(0, scheme.size());
                l.erase(0, scheme.size());
                break;
            }
        size_t slash = l.find_first_of("/#");
        std::string host = l.substr(0, slash);
        host = host.substr(0, host.find(':'));
        if (host != "lila-3d.github.io" && host != "127.0.0.1" && host != "localhost")
            return std::nullopt;
        route = slash == std::string::npos ? std::string() : afterDocs(s.substr(slash));
    }
    std::string anchor;
    std::string id = normalizePageId(route, &anchor);
    while (endsWith(anchor, "/"))
        anchor.pop_back();
    if (id == "/")
        id.clear();
    return anchor.empty() ? id : id + "?id=" + anchor;
}

std::vector<Bookmark> bookmarksFromJson(const json& j)
{
    std::vector<Bookmark> out;
    if (!j.is_array())
        return out;
    for (const json& e : j)
    {
        if (!e.is_object())
            continue;
        Bookmark b{e.value("title", std::string()), e.value("page", std::string()), e.value("anchor", std::string())};
        if (!b.title.empty() && e.contains("page"))
            out.push_back(std::move(b));
    }
    return out;
}

json bookmarksToJson(const std::vector<Bookmark>& list)
{
    json out = json::array();
    for (const Bookmark& b : list)
        out.push_back({{"title", b.title}, {"page", b.page}, {"anchor", b.anchor}});
    return out;
}

int findBookmark(const std::vector<Bookmark>& list, std::string_view page, std::string_view anchor)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].page == page && list[i].anchor == anchor)
            return int(i);
    return -1;
}

bool toggleBookmark(std::vector<Bookmark>& list, const Bookmark& b)
{
    if (int i = findBookmark(list, b.page, b.anchor); i >= 0)
    {
        list.erase(list.begin() + i);
        return false;
    }
    list.push_back(b);
    return true;
}

void moveBookmark(std::vector<Bookmark>& list, int from, int to)
{
    if (list.empty())
        return;
    int last = int(list.size()) - 1;
    from = std::clamp(from, 0, last);
    to = std::clamp(to, 0, last);
    Bookmark b = std::move(list[size_t(from)]);
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, std::move(b));
}

std::string slug(std::string_view heading)
{
    std::string s = lower(trim(heading));
    std::string out;
    bool inTag = false;
    static const std::string_view punct = "\\'!\"#$%&()*+,./:;<=>?@[]^`{|}~";
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '<')
            inTag = true;
        if (inTag)
        {
            inTag = c != '>';
            continue;
        }
        if (punct.find(c) != std::string_view::npos)
            continue;
        // U+2000-U+206F (general punctuation, the dashes and quotes) is dropped as docsify does.
        if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(s[i + 1]) <= 0x81)
        {
            i += 2;
            continue;
        }
        // U+2E00-U+2E7F (supplemental punctuation) too.
        if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < s.size() && (static_cast<unsigned char>(s[i + 1]) == 0xB8 ||
                                                                          static_cast<unsigned char>(s[i + 1]) == 0xB9))
        {
            i += 2;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)))
            c = '-';
        if (c == '-' && !out.empty() && out.back() == '-')
            continue;
        out += c;
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out.insert(0, "_");
    return out;
}

std::string apiPageFor(const fs::path& docsDir, std::string_view symbol)
{
    std::error_code ec;
    if (!fs::is_regular_file(docsDir / "python" / "README.md", ec))
        return {};
    const std::string full = trim(symbol);
    std::string name = full;
    while (!name.empty())
    {
        if (name.find("..") == std::string::npos && fs::is_regular_file(docsDir / "python" / paths::fromUtf8(name + ".md"), ec))
        {
            std::string page = "python/" + name;
            if (full.size() > name.size() + 1)
                page += "?id=" + slug(full.substr(name.size() + 1));
            return page;
        }
        size_t dot = name.rfind('.');
        if (dot == std::string::npos)
            break;
        name.resize(dot);
    }
    return "python/";
}

std::vector<SidebarEntry> readSidebar(const fs::path& docsDir)
{
    std::vector<SidebarEntry> out;
    std::istringstream in(readFile(docsDir / "_sidebar.md"));
    std::string line;
    while (std::getline(in, line))
    {
        size_t indent = line.find_first_not_of(' ');
        // A list item's marker is followed by a space; that keeps a "---" rule out.
        if (indent == std::string::npos || (line[indent] != '-' && line[indent] != '*') || indent + 1 >= line.size() || line[indent + 1] != ' ')
            continue;
        size_t open = line.find('[', indent), close = line.find("](", indent);
        if (open == std::string::npos || close == std::string::npos || close < open)
        {
            std::string text;
            bool inTag = false;
            for (char c : line.substr(indent + 1))
                if (c == '<' || c == '>')
                    inTag = c == '<';
                else if (!inTag)
                    text += c;
            if (text = trim(text); !text.empty())
                out.push_back({.title = text, .depth = int(indent / 2), .label = true});
            continue;
        }
        size_t end = line.find(')', close);
        std::string inner = line.substr(close + 2, end == std::string::npos ? std::string::npos : end - close - 2);
        std::string target = trim(inner.substr(0, inner.find(' ')));
        SidebarEntry e;
        e.title = line.substr(open + 1, close - open - 1);
        e.depth = int(indent / 2);
        e.external = target.find("://") != std::string::npos || inner.find(":ignore") != std::string::npos;
        e.target = e.external ? target : normalizePageId(target);
        out.push_back(std::move(e));
    }
    return out;
}

std::string pageTitle(const fs::path& docsDir, std::string_view page)
{
    std::string id = normalizePageId(page);
    if (!docsDir.empty())
        for (const SidebarEntry& e : readSidebar(docsDir))
            if (!e.external && e.target == id && !e.title.empty())
                return e.title;
    std::string out;
    bool start = true;
    for (char c : id)
    {
        if (c == '-' || c == '_' || c == '/' || c == '.')
        {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            start = true;
            continue;
        }
        out += start ? char(std::toupper(static_cast<unsigned char>(c))) : c;
        start = false;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out.empty() ? "Lila" : out;
}

std::string pageActionId(std::string_view page)
{
    std::string anchor;
    std::string id = normalizePageId(page, &anchor);
    while (endsWith(id, "/"))
        id.pop_back();
    std::ranges::replace(id, '/', '.');
    std::string out = "help.docs." + (id.empty() ? std::string("home") : id);
    if (!anchor.empty())
        out += "." + anchor;
    return out;
}

std::vector<Hit> search(const fs::path& docsDir, std::string_view query, size_t limit)
{
    LILA_ZONE();

    std::vector<std::string> words;
    {
        std::istringstream q{lower(std::string(query))};
        for (std::string w; q >> w;)
            words.push_back(w);
    }
    if (words.empty())
        return {};

    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(docsDir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".md" && !startsWith(paths::toUtf8(it->path().filename()), "_"))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());

    std::vector<Hit> headingHits, textHits;
    for (const fs::path& file : files)
    {
        std::string id = pageIdFor(docsDir, file);
        std::istringstream in(readFile(file));
        std::string line, pageTitle, heading, body;
        bool fence = false;
        auto flush = [&] {
            std::string h = lower(heading), text = lower(plainText(body));
            bool inHeading = true, all = true;
            for (const std::string& w : words)
            {
                bool hHas = h.find(w) != std::string::npos;
                inHeading = inHeading && hHas;
                all = all && (hHas || text.find(w) != std::string::npos);
            }
            if (!all || (heading.empty() && trim(body).empty()))
                return;
            Hit hit{id, heading == pageTitle ? std::string() : slug(heading), heading == pageTitle || pageTitle.empty() ? heading : pageTitle + " > " + heading, {}};
            std::string plain = plainText(body);
            size_t at = inHeading ? 0 : lower(plain).find(words.front());
            if (at == std::string::npos)
                at = 0;
            size_t from = at > 40 ? at - 40 : 0;
            hit.snippet = (from ? "..." : "") + plain.substr(from, 160) + (from + 160 < plain.size() ? "..." : "");
            (inHeading ? headingHits : textHits).push_back(std::move(hit));
        };
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (startsWith(trim(line), "```"))
                fence = !fence;
            if (!fence && startsWith(line, "#"))
            {
                flush();
                size_t text = line.find_first_not_of('#');
                heading = text == std::string::npos ? std::string() : trim(line.substr(text));
                if (pageTitle.empty() && startsWith(line, "# "))
                    pageTitle = heading;
                body.clear();
                continue;
            }
            body += line;
            body += '\n';
        }
        flush();
    }
    headingHits.insert(headingHits.end(), std::make_move_iterator(textHits.begin()), std::make_move_iterator(textHits.end()));
    if (headingHits.size() > limit)
        headingHits.resize(limit);
    return headingHits;
}

namespace
{
constexpr std::string_view kCodePad = "\xE2\x80\xAF"; // U+202F: the code chip's side padding, which Qt has no CSS for
constexpr std::string_view kCodeHyphen = "\xE2\x80\x91"; // U+2011: Qt wraps after a hyphen, which would split `[a-c]`

bool isBlank(std::string_view s)
{
    return s.find_first_not_of(" \t\r") == std::string_view::npos;
}

size_t indentOf(std::string_view s)
{
    size_t n = s.find_first_not_of(' ');
    return n == std::string_view::npos ? s.size() : n;
}

void appendEscaped(std::string& out, char c)
{
    switch (c)
    {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    default: out += c;
    }
}

std::string escaped(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        appendEscaped(out, c);
    return out;
}

bool isAsciiPunct(char c)
{
    return static_cast<unsigned char>(c) < 0x80 && std::ispunct(static_cast<unsigned char>(c));
}

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80;
}

/** Length of the entity (&amp; &#39; &#x2014;) at s[i], 0 when there is none. */
size_t entityAt(std::string_view s, size_t i)
{
    size_t j = i + 1;
    if (j < s.size() && s[j] == '#')
    {
        ++j;
        const bool hex = j < s.size() && (s[j] == 'x' || s[j] == 'X');
        j += hex;
        const size_t digits = j;
        while (j < s.size() && (hex ? std::isxdigit(static_cast<unsigned char>(s[j])) : std::isdigit(static_cast<unsigned char>(s[j]))))
            ++j;
        return j > digits && j - digits <= 7 && j < s.size() && s[j] == ';' ? j + 1 - i : 0;
    }
    const size_t start = j;
    while (j < s.size() && std::isalnum(static_cast<unsigned char>(s[j])) && j - start < 32)
        ++j;
    return j > start && std::isalpha(static_cast<unsigned char>(s[start])) && j < s.size() && s[j] == ';' ? j + 1 - i : 0;
}

/** One past the closing backticks of the code span opening at s[i], or npos. */
size_t codeSpanEnd(std::string_view s, size_t i, size_t* run = nullptr)
{
    size_t n = s.find_first_not_of('`', i);
    n = (n == std::string_view::npos ? s.size() : n) - i;
    if (run)
        *run = n;
    for (size_t j = i + n; j < s.size();)
    {
        j = s.find('`', j);
        if (j == std::string_view::npos)
            break;
        size_t k = s.find_first_not_of('`', j);
        k = k == std::string_view::npos ? s.size() : k;
        if (k - j == n)
            return k;
        j = k;
    }
    return std::string_view::npos;
}

/** Index of the bracket closing the one at s[open], skipping escapes and code spans; npos when unbalanced. */
size_t matchingBracket(std::string_view s, size_t open, char o, char c)
{
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '\\')
            ++i;
        else if (s[i] == '`' && o == '[')
        {
            if (size_t end = codeSpanEnd(s, i); end != std::string_view::npos)
                i = end - 1;
        }
        else if (s[i] == o)
            ++depth;
        else if (s[i] == c && --depth == 0)
            return i;
    }
    return std::string_view::npos;
}

struct LinkParts
{
    std::string_view text;
    std::string href;
    std::string title;
    size_t end = 0;
};

/** `[text](href "title")` with s[open] the '['. */
std::optional<LinkParts> parseLink(std::string_view s, size_t open)
{
    const size_t close = matchingBracket(s, open, '[', ']');
    if (close == std::string_view::npos || close + 1 >= s.size() || s[close + 1] != '(')
        return std::nullopt;
    const size_t paren = matchingBracket(s, close + 1, '(', ')');
    if (paren == std::string_view::npos)
        return std::nullopt;
    LinkParts p;
    p.text = s.substr(open + 1, close - open - 1);
    p.end = paren + 1;
    std::string dest = trim(s.substr(close + 2, paren - close - 2));
    if (size_t q = dest.find_first_of("\"'");
        q != std::string::npos && q > 0 && std::isspace(static_cast<unsigned char>(dest[q - 1])) && dest.size() > q + 1 && dest.back() == dest[q])
    {
        p.title = dest.substr(q + 1, dest.size() - q - 2);
        dest = trim(dest.substr(0, q));
    }
    if (dest.size() >= 2 && dest.front() == '<' && dest.back() == '>')
        dest = dest.substr(1, dest.size() - 2);
    p.href = dest;
    return p;
}

/** An inline HTML tag Qt renders, kept as written; anything else in angle brackets is shown as text. */
bool inlineTagAt(std::string_view s, size_t i, size_t* end)
{
    static constexpr std::string_view kTags[] = {"br", "kbd", "b", "i", "em", "strong", "code", "sub", "sup", "span", "u", "s", "a", "img"};
    const size_t j = i + 1 + (i + 1 < s.size() && s[i + 1] == '/');
    size_t nameEnd = j;
    while (nameEnd < s.size() && std::isalpha(static_cast<unsigned char>(s[nameEnd])))
        ++nameEnd;
    const std::string name = lower(std::string(s.substr(j, nameEnd - j)));
    if (std::find(std::begin(kTags), std::end(kTags), name) == std::end(kTags) || nameEnd >= s.size() ||
        !(s[nameEnd] == '>' || s[nameEnd] == '/' || std::isspace(static_cast<unsigned char>(s[nameEnd]))))
        return false;
    const size_t close = s.find('>', nameEnd);
    if (close == std::string_view::npos)
        return false;
    *end = close + 1;
    return true;
}

std::string expandTabs(std::string_view line)
{
    std::string out;
    for (char c : line)
        if (c == '\t')
            out.append(4 - out.size() % 4, ' ');
        else if (c != '\r')
            out += c;
    return out;
}

bool isFence(std::string_view line, std::string* marker = nullptr)
{
    const std::string t = trim(line);
    if (indentOf(line) > 3 || t.size() < 3 || (t[0] != '`' && t[0] != '~'))
        return false;
    size_t n = t.find_first_not_of(t[0]);
    n = n == std::string::npos ? t.size() : n;
    if (n < 3 || (t[0] == '`' && t.find('`', n) != std::string::npos))
        return false;
    if (marker)
        *marker = t.substr(0, n);
    return true;
}

bool isRule(std::string_view line)
{
    const std::string t = trim(line);
    if (indentOf(line) > 3 || t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_'))
        return false;
    size_t count = 0;
    for (char c : t)
        if (c == t[0])
            ++count;
        else if (c != ' ')
            return false;
    return count >= 3;
}

int headingLevel(std::string_view line)
{
    const size_t lead = indentOf(line);
    if (lead > 3)
        return 0;
    size_t n = line.find_first_not_of('#', lead);
    n = (n == std::string_view::npos ? line.size() : n) - lead;
    return n >= 1 && n <= 6 && (lead + n == line.size() || line[lead + n] == ' ') ? int(n) : 0;
}

bool isQuote(std::string_view line)
{
    const size_t lead = indentOf(line);
    return lead <= 3 && lead < line.size() && line[lead] == '>';
}

struct ListMarker
{
    bool ordered = false;
    char kind = '-'; // the bullet, or the '.' or ')' after the number
    int start = 1;
    size_t contentIndent = 0;
    std::string rest;
};

std::optional<ListMarker> listMarker(std::string_view line)
{
    ListMarker m;
    const size_t lead = indentOf(line);
    if (lead > 3 || lead >= line.size() || isRule(line))
        return std::nullopt;
    size_t j = lead;
    if (line[j] == '-' || line[j] == '*' || line[j] == '+')
        m.kind = line[j++];
    else
    {
        const size_t digits = j;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])) && j - digits < 9)
            ++j;
        if (j == digits || j >= line.size() || (line[j] != '.' && line[j] != ')'))
            return std::nullopt;
        m.ordered = true;
        m.start = std::atoi(std::string(line.substr(digits, j - digits)).c_str());
        m.kind = line[j++];
    }
    if (j < line.size() && line[j] != ' ')
        return std::nullopt;
    const size_t text = line.find_first_not_of(' ', j);
    if (text == std::string_view::npos)
        m.contentIndent = j + 1;
    else
    {
        // Five or more spaces after the marker start indented code, so the content starts after one.
        m.contentIndent = text - j > 4 ? j + 1 : text;
        m.rest = std::string(line.substr(m.contentIndent));
    }
    return m;
}

bool isHtmlBlock(std::string_view line)
{
    static constexpr std::string_view kTags[] = {"div", "p", "img", "table", "details", "summary", "br", "center", "figure"};
    const std::string t = lower(trim(line));
    if (t.size() < 2 || t[0] != '<')
        return false;
    const size_t j = 1 + (t[1] == '/');
    size_t e = j;
    while (e < t.size() && std::isalpha(static_cast<unsigned char>(t[e])))
        ++e;
    return std::find(std::begin(kTags), std::end(kTags), t.substr(j, e - j)) != std::end(kTags) &&
           (e == t.size() || t[e] == '>' || t[e] == ' ' || t[e] == '/');
}

/** A table row's cells, split on unescaped pipes; `\|` becomes `|` in every cell, code spans included, as GFM has it. */
std::vector<std::string> tableCells(std::string_view line)
{
    std::string t = trim(line);
    if (!t.empty() && t.front() == '|')
        t.erase(0, 1);
    if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\'))
        t.pop_back();
    std::vector<std::string> cells(1);
    for (size_t i = 0; i < t.size(); ++i)
    {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|')
        {
            cells.back() += '|';
            ++i;
        }
        else if (t[i] == '|')
            cells.emplace_back();
        else
            cells.back() += t[i];
    }
    for (std::string& c : cells)
        c = trim(c);
    return cells;
}

bool isDelimiterRow(std::string_view line)
{
    if (line.find('-') == std::string_view::npos)
        return false;
    for (const std::string& c : tableCells(line))
    {
        const size_t b = !c.empty() && c.front() == ':';
        const size_t e = c.size() - (c.size() > 1 && c.back() == ':');
        if (b >= e || c.find_first_not_of('-', b) < e)
            return false;
    }
    return true;
}

bool startsTable(const std::vector<std::string>& lines, size_t i)
{
    return i + 1 < lines.size() && lines[i].find('|') != std::string::npos && isDelimiterRow(lines[i + 1]) &&
           tableCells(lines[i]).size() == tableCells(lines[i + 1]).size();
}

/** A line that ends a paragraph by starting a block of its own. */
bool interruptsParagraph(const std::vector<std::string>& lines, size_t i)
{
    const std::string& line = lines[i];
    if (isBlank(line) || isFence(line) || isRule(line) || headingLevel(line) || isQuote(line) || isHtmlBlock(line) ||
        startsWith(trim(line), "<!--") || startsTable(lines, i))
        return true;
    std::optional<ListMarker> m = listMarker(line);
    return m && !m->rest.empty() && (!m->ordered || m->start == 1);
}

class Converter
{
public:
    explicit Converter(const HtmlOptions& options) : mOptions(options) {}

    std::string blocks(const std::vector<std::string>& lines, std::string_view paraClass = {}) const
    {
        std::string out;
        for (size_t i = 0; i < lines.size();)
        {
            const std::string& line = lines[i];
            if (isBlank(line))
                ++i;
            else if (startsWith(trim(line), "<!--"))
            {
                while (i < lines.size() && lines[i].find("-->") == std::string::npos)
                    ++i;
                ++i;
            }
            else if (isFence(line))
                i = codeBlock(lines, i, out);
            else if (int level = headingLevel(line))
            {
                heading(line, level, out);
                ++i;
            }
            else if (isRule(line))
            {
                out += "<hr/>\n";
                ++i;
            }
            else if (isQuote(line))
                i = quote(lines, i, out);
            else if (startsTable(lines, i))
                i = table(lines, i, out);
            else if (listMarker(line))
                i = list(lines, i, out);
            else if (isHtmlBlock(line))
                for (; i < lines.size() && !isBlank(lines[i]); ++i)
                    out += lines[i] + "\n";
            else
                i = paragraph(lines, i, paraClass, out);
        }
        return out;
    }

    std::string inlineHtml(std::string_view s) const
    {
        std::string out;
        out.reserve(s.size() + s.size() / 4);
        for (size_t i = 0; i < s.size();)
        {
            const char c = s[i];
            const char next = i + 1 < s.size() ? s[i + 1] : '\0';
            if (c == '\\' && next == '\n')
            {
                out += "<br/>";
                i += 2;
            }
            else if (c == '\\' && isAsciiPunct(next))
            {
                appendEscaped(out, next);
                i += 2;
            }
            else if (c == '`')
                i = codeSpan(s, i, out);
            else if (c == '!' && next == '[' && link(s, i + 1, true, out, i))
                ;
            else if (c == '[' && link(s, i, false, out, i))
                ;
            else if (c == '<' && angle(s, i, out))
                ;
            else if ((c == '*' || c == '_') && emphasis(s, i, out))
                ;
            else if (c == '~' && next == '~' && strike(s, i, out))
                ;
            else if (size_t n = c == '&' ? entityAt(s, i) : 0)
            {
                out += s.substr(i, n);
                i += n;
            }
            else if (c == '\n')
            {
                const bool hard = out.size() >= 2 && out[out.size() - 1] == ' ' && out[out.size() - 2] == ' ';
                while (!out.empty() && out.back() == ' ')
                    out.pop_back();
                out += hard ? "<br/>" : " ";
                ++i;
            }
            else
                appendEscaped(out, s[i++]);
        }
        return out;
    }

    /** The text a reader sees, for heading ids and alt text. */
    std::string plainOf(std::string_view md) const
    {
        const std::string html = inlineHtml(md);
        std::string out;
        bool tag = false;
        for (size_t i = 0; i < html.size(); ++i)
        {
            if (html[i] == '<')
                tag = true;
            else if (tag)
                tag = html[i] != '>';
            else if (std::string_view(html).substr(i, kCodePad.size()) == kCodePad)
                i += kCodePad.size() - 1;
            else if (std::string_view(html).substr(i, kCodeHyphen.size()) == kCodeHyphen)
            {
                out += '-';
                i += kCodeHyphen.size() - 1;
            }
            else
                out += html[i];
        }
        static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
            {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}};
        for (const auto& [from, to] : kEntities)
            for (size_t at = out.find(from); at != std::string::npos; at = out.find(from, at + to.size()))
                out.replace(at, from.size(), to);
        return out;
    }

private:
    std::string imageHtml(const LinkParts& link) const
    {
        std::string out = "<img src=\"" + escaped(link.href) + "\" alt=\"" + escaped(plainOf(link.text)) + "\"";
        if (!link.title.empty())
            out += " title=\"" + escaped(link.title) + "\"";
        std::optional<std::pair<int, int>> size = mOptions.imageSize ? mOptions.imageSize(link.href) : std::nullopt;
        if (size && size->first > 0 && size->second > 0)
        {
            const int w = mOptions.maxImageWidth > 0 ? std::min(size->first, mOptions.maxImageWidth) : size->first;
            const int h = int(std::lround(double(size->second) * w / size->first));
            out += " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
        }
        return out + " />";
    }

    size_t codeSpan(std::string_view s, size_t i, std::string& out) const
    {
        size_t run = 0;
        const size_t end = codeSpanEnd(s, i, &run);
        if (end == std::string_view::npos)
        {
            out.append(run, '`');
            return i + run;
        }
        std::string code(s.substr(i + run, end - run - (i + run)));
        std::replace(code.begin(), code.end(), '\n', ' ');
        if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && !isBlank(code))
            code = code.substr(1, code.size() - 2);
        std::string shown = escaped(code);
        for (size_t at = shown.find('-'); at != std::string::npos; at = shown.find('-', at + kCodeHyphen.size()))
            shown.replace(at, 1, kCodeHyphen);
        out += "<code>";
        out += kCodePad;
        out += shown;
        out += kCodePad;
        out += "</code>";
        return end;
    }

    bool link(std::string_view s, size_t open, bool image, std::string& out, size_t& i) const
    {
        std::optional<LinkParts> l = parseLink(s, open);
        if (!l)
            return false;
        if (image)
            out += imageHtml(*l);
        else
            out += "<a href=\"" + escaped(l->href) + "\"" + (l->title.empty() ? "" : " title=\"" + escaped(l->title) + "\"") + ">" +
                   inlineHtml(l->text) + "</a>";
        i = l->end;
        return true;
    }

    /** A comment (dropped), an autolink or an inline tag at s[i]. */
    bool angle(std::string_view s, size_t& i, std::string& out) const
    {
        if (s.substr(i, 4) == "<!--")
        {
            const size_t end = s.find("-->", i);
            i = end == std::string_view::npos ? s.size() : end + 3;
            return true;
        }
        const size_t close = s.find('>', i);
        const std::string_view inner = close == std::string_view::npos ? std::string_view() : s.substr(i + 1, close - i - 1);
        if (!inner.empty() && inner.find_first_of(" \t\n<") == std::string_view::npos &&
            (startsWith(inner, "http://") || startsWith(inner, "https://") || startsWith(inner, "mailto:")))
        {
            out += "<a href=\"" + escaped(inner) + "\">" + escaped(startsWith(inner, "mailto:") ? inner.substr(7) : inner) + "</a>";
            i = close + 1;
            return true;
        }
        if (size_t end = 0; inlineTagAt(s, i, &end))
        {
            out += s.substr(i, end - i);
            i = end;
            return true;
        }
        return false;
    }

    /** `*em*`, `**strong**` and their `_` forms at s[i]; false when the run opens nothing. */
    bool emphasis(std::string_view s, size_t& i, std::string& out) const
    {
        const char d = s[i];
        size_t runEnd = s.find_first_not_of(d, i);
        runEnd = runEnd == std::string_view::npos ? s.size() : runEnd;
        const size_t n = std::min<size_t>(runEnd - i, 3);
        if (runEnd >= s.size() || std::isspace(static_cast<unsigned char>(s[runEnd])) || (d == '_' && i > 0 && isWordChar(s[i - 1])))
            return false;
        for (size_t j = runEnd; j < s.size(); ++j)
        {
            if (s[j] == '\\')
            {
                ++j;
                continue;
            }
            if (s[j] == '`')
            {
                if (size_t end = codeSpanEnd(s, j); end != std::string_view::npos)
                    j = end - 1;
                continue;
            }
            if (s[j] != d)
                continue;
            size_t k = s.find_first_not_of(d, j);
            k = k == std::string_view::npos ? s.size() : k;
            if (k - j < n || std::isspace(static_cast<unsigned char>(s[j - 1])) || (d == '_' && k < s.size() && isWordChar(s[k])))
            {
                j = k - 1;
                continue;
            }
            static constexpr std::string_view kOpen[] = {"", "<i>", "<b>", "<b><i>"};
            static constexpr std::string_view kClose[] = {"", "</i>", "</b>", "</i></b>"};
            out += kOpen[n];
            out += inlineHtml(s.substr(runEnd, j - runEnd));
            out += kClose[n];
            i = j + n;
            return true;
        }
        return false;
    }

    bool strike(std::string_view s, size_t& i, std::string& out) const
    {
        const size_t end = s.find("~~", i + 2);
        if (end == std::string_view::npos || end == i + 2)
            return false;
        out += "<s>" + inlineHtml(s.substr(i + 2, end - i - 2)) + "</s>";
        i = end + 2;
        return true;
    }

    size_t paragraph(const std::vector<std::string>& lines, size_t i, std::string_view paraClass, std::string& out) const
    {
        std::string text = lines[i].substr(indentOf(lines[i]));
        for (++i; i < lines.size() && !interruptsParagraph(lines, i); ++i)
            text += "\n" + lines[i].substr(indentOf(lines[i]));
        // Trailing spaces make a hard break only between lines.
        while (!text.empty() && text.back() == ' ')
            text.pop_back();
        // A paragraph that is just an image is a figure, spaced as one.
        if (startsWith(text, "!["))
            if (std::optional<LinkParts> l = parseLink(text, 1); l && l->end == text.size())
            {
                out += "<p class=\"img\">" + imageHtml(*l) + "</p>\n";
                return i;
            }
        out += (paraClass.empty() ? std::string("<p>") : "<p class=\"" + std::string(paraClass) + "\">") + inlineHtml(text) + "</p>\n";
        return i;
    }

    size_t codeBlock(const std::vector<std::string>& lines, size_t i, std::string& out) const
    {
        std::string fence;
        isFence(lines[i], &fence);
        const size_t lead = indentOf(lines[i]);
        std::string code;
        for (++i; i < lines.size(); ++i)
        {
            const std::string t = trim(lines[i]);
            if (startsWith(t, fence) && t.find_first_not_of(fence[0]) == std::string::npos && indentOf(lines[i]) <= 3)
            {
                ++i;
                break;
            }
            code += escaped(std::string_view(lines[i]).substr(std::min(lead, indentOf(lines[i])))) + "\n";
        }
        if (!code.empty())
            code.pop_back();
        // A one-cell table: Qt draws no border or padding around a pre.
        out += "<table class=\"pre\" width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"0\"><tr><td><pre>" + code +
               "</pre></td></tr></table>\n";
        return i;
    }

    void heading(const std::string& line, int level, std::string& out) const
    {
        std::string text = trim(std::string_view(line).substr(line.find('#') + size_t(level)));
        while (!text.empty() && text.back() == '#')
            text.pop_back();
        text = trim(text);
        const std::string h = "h" + std::to_string(level);
        // The site rules a line above each section.
        if (level == 2 && !out.empty())
            out += "<hr/>\n";
        out += "<" + h + "><a name=\"" + escaped(slug(plainOf(text))) + "\"></a>" + inlineHtml(text) + "</" + h + ">\n";
    }

    size_t quote(const std::vector<std::string>& lines, size_t i, std::string& out) const
    {
        std::vector<std::string> inner;
        for (; i < lines.size() && !isBlank(lines[i]); ++i)
        {
            const std::string t = trim(lines[i]);
            if (t[0] == '>')
                inner.push_back(t.substr(t.size() > 1 && t[1] == ' ' ? 2 : 1));
            else if (!interruptsParagraph(lines, i))
                inner.push_back(t);
            else
                break;
        }
        // Paragraphs after the first are "qn", spaced from the one above; the first sits flush so the padding centres it.
        std::string body = blocks(inner, "q");
        const std::string_view q = "<p class=\"q\">";
        for (size_t at = body.find(q, body.find(q) + 1); at != std::string::npos && at > 0; at = body.find(q, at + 1))
            body.replace(at, q.size(), "<p class=\"qn\">");
        // A bar cell and a body cell: Qt draws no left border on a block.
        out += "<table class=\"quote\" width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td class=\"bar\" width=\"4\"></td><td class=\"body\">" +
               body + "</td></tr></table>\n";
        return i;
    }

    size_t table(const std::vector<std::string>& lines, size_t i, std::string& out) const
    {
        const std::vector<std::string> head = tableCells(lines[i]);
        std::vector<std::string> align;
        for (const std::string& d : tableCells(lines[i + 1]))
        {
            const bool l = d.front() == ':', r = d.back() == ':';
            align.push_back(l && r ? "center" : r ? "right" : l ? "left" : "");
        }
        out += "<table class=\"grid\" border=\"1\" cellspacing=\"0\" cellpadding=\"0\">\n<thead><tr>";
        for (size_t c = 0; c < head.size(); ++c)
            out += "<th align=\"" + (align[c].empty() ? std::string("center") : align[c]) + "\">" + inlineHtml(head[c]) + "</th>";
        out += "</tr></thead>\n";
        int row = 0;
        for (i += 2; i < lines.size() && !isBlank(lines[i]) && lines[i].find('|') != std::string::npos; ++i)
        {
            std::vector<std::string> cells = tableCells(lines[i]);
            cells.resize(head.size());
            const std::string alt = ++row % 2 == 0 ? " class=\"alt\"" : "";
            out += "<tr>";
            for (size_t c = 0; c < cells.size(); ++c)
                out += "<td" + alt + (align[c].empty() ? "" : " align=\"" + align[c] + "\"") + ">" + inlineHtml(cells[c]) + "</td>";
            out += "</tr>\n";
        }
        out += "</table>\n";
        return i;
    }

    size_t list(const std::vector<std::string>& lines, size_t i, std::string& out) const
    {
        const ListMarker first = *listMarker(lines[i]);
        std::vector<std::vector<std::string>> items{{first.rest}};
        size_t contentIndent = first.contentIndent;
        bool loose = false;
        bool blankBefore = false;
        for (++i; i < lines.size(); ++i)
        {
            const std::string& l = lines[i];
            if (isBlank(l))
            {
                blankBefore = true;
                items.back().emplace_back();
                continue;
            }
            if (indentOf(l) >= contentIndent)
            {
                items.back().push_back(l.substr(contentIndent));
                blankBefore = false;
                continue;
            }
            std::optional<ListMarker> m = listMarker(l);
            if (m && m->ordered == first.ordered && m->kind == first.kind)
            {
                loose = loose || blankBefore;
                contentIndent = m->contentIndent;
                items.push_back({m->rest});
                blankBefore = false;
                continue;
            }
            if (!blankBefore && !m && !interruptsParagraph(lines, i))
            {
                items.back().push_back(trim(l));
                continue;
            }
            break;
        }
        const std::string tag = first.ordered ? "ol" : "ul";
        out += "<" + tag + (first.ordered && first.start != 1 ? " start=\"" + std::to_string(first.start) + "\"" : "") + ">\n";
        for (const std::vector<std::string>& item : items)
        {
            std::string html = blocks(item, "li");
            // A tight item holds its text directly: Qt gives a paragraph inside an item a line of its own.
            if (!loose)
                for (size_t at = html.find("<p class=\"li\">"); at != std::string::npos; at = html.find("<p class=\"li\">", at))
                {
                    html.erase(html.find("</p>\n", at), 5);
                    html.erase(at, 14);
                }
            while (!html.empty() && html.back() == '\n')
                html.pop_back();
            out += "<li>" + html + "</li>\n";
        }
        out += "</" + tag + ">\n";
        return i;
    }

    const HtmlOptions& mOptions;
};
} // namespace

std::string markdownToHtml(std::string_view md, const HtmlOptions& options)
{
    LILA_ZONE();

    std::vector<std::string> lines;
    for (size_t at = 0; at <= md.size();)
    {
        size_t end = md.find('\n', at);
        end = end == std::string_view::npos ? md.size() : end;
        lines.push_back(expandTabs(md.substr(at, end - at)));
        at = end + 1;
    }
    return Converter(options).blocks(lines);
}
} // namespace lila::ui::docs
