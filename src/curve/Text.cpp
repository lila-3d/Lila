#include "curve/Text.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "curve/CurveBoolean.h"
#include "curve/CurveMath.h"
#include "curve/Planar.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb/stb_truetype.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

namespace lila::curve
{
namespace
{
namespace fs = std::filesystem;

struct FontEntry
{
    std::string path;
    int index = 0;
    std::string family, style, full;
};

uint16_t be16(const unsigned char* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

bool readAt(std::ifstream& in, uint64_t offset, size_t size, std::vector<unsigned char>& out)
{
    out.resize(size);
    in.seekg(std::streamoff(offset));
    in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
    return in.gcount() == std::streamsize(size);
}

std::string encodeUtf8(uint32_t c);

std::string utf16be(const unsigned char* p, size_t bytes)
{
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2)
        out += encodeUtf8(be16(p + i));
    return out;
}

/** Family, style and full name of each font in a file, read from its name table without loading the rest. */
std::vector<FontEntry> readNames(const fs::path& path)
{
    std::vector<FontEntry> out;
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> head;
    if (!in || !readAt(in, 0, 12, head))
        return out;
    std::vector<uint32_t> offsets;
    if (be32(head.data()) == 0x74746366) // 'ttcf'
    {
        uint32_t count = std::min<uint32_t>(be32(head.data() + 8), 64);
        std::vector<unsigned char> table;
        if (!readAt(in, 12, size_t(count) * 4, table))
            return out;
        for (uint32_t k = 0; k < count; ++k)
            offsets.push_back(be32(table.data() + k * 4));
    }
    else
        offsets.push_back(0);
    for (size_t fi = 0; fi < offsets.size(); ++fi)
    {
        std::vector<unsigned char> dir;
        if (!readAt(in, offsets[fi], 12, dir))
            continue;
        uint16_t tables = be16(dir.data() + 4);
        std::vector<unsigned char> recs;
        if (!readAt(in, uint64_t(offsets[fi]) + 12, size_t(tables) * 16, recs))
            continue;
        uint32_t nameOff = 0, nameLen = 0;
        for (uint16_t t = 0; t < tables; ++t)
            if (be32(recs.data() + t * 16) == 0x6E616D65) // 'name'
            {
                nameOff = be32(recs.data() + t * 16 + 8);
                nameLen = be32(recs.data() + t * 16 + 12);
            }
        std::vector<unsigned char> name;
        if (!nameLen || nameLen > (1u << 22) || !readAt(in, nameOff, nameLen, name) || name.size() < 6)
            continue;
        uint16_t count = be16(name.data() + 2), strings = be16(name.data() + 4);
        std::map<int, std::pair<int, std::string>> best; // name id -> (rank, text)
        for (uint16_t r = 0; r < count && 6u + (r + 1u) * 12u <= name.size(); ++r)
        {
            const unsigned char* rec = name.data() + 6 + r * 12;
            uint16_t platform = be16(rec), encoding = be16(rec + 2), language = be16(rec + 4), id = be16(rec + 6);
            uint16_t len = be16(rec + 8), off = be16(rec + 10);
            if ((id != 1 && id != 2 && id != 4 && id != 16 && id != 17) || size_t(strings) + off + len > name.size())
                continue;
            const unsigned char* text = name.data() + strings + off;
            int rank = 0;
            std::string s;
            if (platform == 3 && (encoding == 1 || encoding == 10))
            {
                rank = language == 0x409 ? 3 : 2;
                s = utf16be(text, len);
            }
            else if (platform == 1 && encoding == 0)
            {
                rank = 1;
                s.assign(reinterpret_cast<const char*>(text), len);
            }
            else
                continue;
            auto it = best.find(id);
            if (it == best.end() || it->second.first < rank)
                best[id] = {rank, s};
        }
        auto get = [&](int id) { return best.count(id) ? best[id].second : std::string(); };
        FontEntry e;
        e.path = paths::toUtf8(path);
        e.index = int(fi);
        e.family = !get(16).empty() ? get(16) : get(1);
        e.style = !get(17).empty() ? get(17) : get(2);
        e.full = get(4);
        if (!e.family.empty())
            out.push_back(std::move(e));
    }
    return out;
}

std::string norm(std::string_view s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '-' && c != '_')
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<fs::path> systemFontDirs()
{
    std::vector<fs::path> dirs;
#ifdef _WIN32
    if (const char* win = std::getenv("WINDIR"))
        dirs.push_back(fs::path(win) / "Fonts");
    if (const char* local = std::getenv("LOCALAPPDATA"))
        dirs.push_back(fs::path(local) / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/Library/Fonts"};
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(fs::path(home) / "Library" / "Fonts");
#else
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* home = std::getenv("HOME"))
    {
        dirs.push_back(fs::path(home) / ".fonts");
        dirs.push_back(fs::path(home) / ".local" / "share" / "fonts");
    }
#endif
    return dirs;
}

std::vector<FontEntry> scan(const std::vector<fs::path>& dirs)
{
    std::vector<FontEntry> out;
    for (const fs::path& d : dirs)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (ext != ".ttf" && ext != ".otf" && ext != ".ttc")
                continue;
            for (FontEntry& e : readNames(it->path()))
                out.push_back(std::move(e));
        }
    }
    return out;
}

std::mutex& fontMutex()
{
    static std::mutex m;
    return m;
}

const std::vector<FontEntry>& bundledFonts()
{
    static const std::vector<FontEntry> fonts = scan({paths::resourcesDir() / "fonts"});
    return fonts;
}

struct SystemFonts
{
    std::mutex mutex;
    std::shared_ptr<const std::vector<FontEntry>> fonts;
    std::vector<fs::file_time_type> stamps;
    uint64_t version = 0;
};

SystemFonts& systemFontState()
{
    static SystemFonts s;
    return s;
}

/** The installed fonts, rescanned when a font folder changes: installing or removing a font touches its folder. */
std::shared_ptr<const std::vector<FontEntry>> systemFonts()
{
    SystemFonts& s = systemFontState();
    std::vector<fs::path> dirs = systemFontDirs();
    std::vector<fs::file_time_type> stamps;
    for (const fs::path& d : dirs)
    {
        std::error_code ec;
        stamps.push_back(fs::last_write_time(d, ec));
    }
    std::lock_guard lock(s.mutex);
    if (!s.fonts || stamps != s.stamps)
    {
        s.fonts = std::make_shared<const std::vector<FontEntry>>(scan(dirs));
        s.stamps = std::move(stamps);
        ++s.version;
    }
    return s.fonts;
}

const FontEntry* match(const std::vector<FontEntry>& fonts, const std::string& q)
{
    static const std::vector<std::string> regular = {"regular", "book", "normal", "roman", ""};
    for (const FontEntry& e : fonts)
        if (norm(e.full) == q || norm(e.family + e.style) == q || norm(paths::toUtf8(paths::fromUtf8(e.path).stem())) == q)
            return &e;
    for (const std::string& r : regular)
        for (const FontEntry& e : fonts)
            if (norm(e.family) == q && norm(e.style) == r)
                return &e;
    for (const FontEntry& e : fonts)
        if (norm(e.family) == q)
            return &e;
    return nullptr;
}

std::string entryPath(const FontEntry& e) { return e.index ? e.path + "#" + std::to_string(e.index) : e.path; }

/** A loaded font, shared between evaluations. */
struct Font
{
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

std::shared_ptr<const Font> loadFont(const std::string& spec, std::string* why)
{
    static std::map<std::string, std::shared_ptr<const Font>> cache;
    std::lock_guard lock(fontMutex());
    if (auto it = cache.find(spec); it != cache.end())
        return it->second;
    std::string file = spec;
    int index = 0;
    if (size_t hash = spec.rfind('#'); hash != std::string::npos && hash > 2)
    {
        index = std::atoi(spec.c_str() + hash + 1);
        file = spec.substr(0, hash);
    }
    std::ifstream in(paths::fromUtf8(file), std::ios::binary);
    if (!in)
    {
        *why = "cannot read " + file;
        return nullptr;
    }
    auto f = std::make_shared<Font>();
    f->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    int offset = stbtt_GetFontOffsetForIndex(f->data.data(), index);
    if (offset < 0 || !stbtt_InitFont(&f->info, f->data.data(), offset))
    {
        *why = file + " is not a font stb_truetype can read";
        return nullptr;
    }
    cache[spec] = f;
    return f;
}

std::vector<uint32_t> decodeUtf8(std::string_view s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 4;
        if (extra == 4 || i + extra >= s.size() + (extra ? 0 : 1))
        {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        for (size_t k = 1; k <= extra; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::string encodeUtf8(uint32_t c)
{
    std::string out;
    if (c < 0x80)
        out += char(c);
    else if (c < 0x800)
    {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    else
    {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
    return out;
}

/** One glyph's contours as closed Beziers, placed with its origin at pen. Outer contours come out counter-clockwise. */
Splines glyphCurves(const Font& f, int glyph, double scale, const D3& pen)
{
    Splines out;
    stbtt_vertex* v = nullptr;
    int n = stbtt_GetGlyphShape(&f.info, glyph, &v);
    auto at = [&](double x, double y) { return D4(pen + D3(x * scale, y * scale, 0.0), 1.0); };
    std::vector<Piece> ps;
    D4 start(0.0), cur(0.0);
    auto close = [&]() {
        if (!ps.empty() && glm::length(glm::dvec3(cur) - glm::dvec3(start)) > 1e-12)
            ps.push_back({{cur, start}});
        if (ps.size() >= 2)
        {
            Spline s = fromPieces(ps, true, Kind::Bezier);
            // TrueType winds outer contours clockwise; CFF the other way.
            if (!f.info.cff.size)
                reverse(s);
            vectorHandlesOnLines(s);
            if (isValid(s))
                out.push_back(std::move(s));
        }
        ps.clear();
    };
    for (int k = 0; k < n; ++k)
    {
        const stbtt_vertex& q = v[k];
        D4 p = at(q.x, q.y);
        switch (q.type)
        {
        case STBTT_vmove:
            close();
            start = cur = p;
            break;
        case STBTT_vline:
            if (glm::length(glm::dvec3(p) - glm::dvec3(cur)) > 1e-12)
                ps.push_back({{cur, p}});
            cur = p;
            break;
        case STBTT_vcurve:
            ps.push_back({{cur, at(q.cx, q.cy), p}});
            cur = p;
            break;
        case STBTT_vcubic:
            ps.push_back({{cur, at(q.cx, q.cy), at(q.cx1, q.cy1), p}});
            cur = p;
            break;
        default: break;
        }
    }
    close();
    stbtt_FreeShape(&f.info, v);
    return out;
}
} // namespace

std::string findFont(std::string_view nameOrPath)
{
    std::string name(nameOrPath);
    if (name.empty())
        name = "Inter";
    std::error_code ec;
    fs::path p = paths::fromUtf8(name);
    if (fs::is_regular_file(p, ec))
        return name;
    std::string q = norm(name);
    if (const FontEntry* e = match(bundledFonts(), q))
        return entryPath(*e);
    auto installed = systemFonts();
    if (const FontEntry* e = match(*installed, q))
        return entryPath(*e);
    return {};
}

std::vector<std::string> fontNames()
{
    std::vector<std::string> out;
    auto installed = systemFonts();
    for (const auto* list : {&bundledFonts(), installed.get()})
        for (const FontEntry& e : *list)
            out.push_back(e.family + (e.style.empty() ? "" : " " + e.style));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint64_t fontListVersion()
{
    systemFonts();
    std::lock_guard lock(systemFontState().mutex);
    return systemFontState().version;
}

std::vector<FontFamily> fontFamilies()
{
    std::map<std::string, std::vector<std::string>> byFamily;
    auto installed = systemFonts();
    for (const auto* list : {&bundledFonts(), installed.get()})
        for (const FontEntry& e : *list)
        {
            std::vector<std::string>& styles = byFamily[e.family];
            std::string style = e.style.empty() ? "Regular" : e.style;
            if (std::find(styles.begin(), styles.end(), style) == styles.end())
                styles.push_back(style);
        }
    std::vector<FontFamily> out;
    for (auto& [family, styles] : byFamily)
    {
        // Regular first, then by name.
        std::sort(styles.begin(), styles.end(), [](const std::string& a, const std::string& b) {
            bool ra = norm(a) == "regular", rb = norm(b) == "regular";
            return ra != rb ? ra : a < b;
        });
        out.push_back({family, std::move(styles)});
    }
    return out;
}

namespace
{
/** Upper or lower case for the Latin, Greek and Cyrillic letters that have one; anything else comes back as is. */
uint32_t changeCase(uint32_t c, bool upper)
{
    if (upper)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F))
            return c - 0x20;
        if (c == 0x3C2)
            return 0x3A3;
        if (c == 0xFF)
            return 0x178;
        if (c >= 0x450 && c <= 0x45F)
            return c - 0x50;
        if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && (c & 1))
            return c - 1;
        if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && !(c & 1))
            return c - 1;
        return c;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) || (c >= 0x410 && c <= 0x42F))
        return c + 0x20;
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x400 && c <= 0x40F)
        return c + 0x50;
    if (((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) && !(c & 1))
        return c + 1;
    if (((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) && (c & 1))
        return c + 1;
    return c;
}

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == '\r' || c == 0xA0; }

void applyCase(std::vector<uint32_t>& cps, TextCase mode)
{
    if (mode == TextCase::AsTyped)
        return;
    bool wordStart = true;
    for (uint32_t& c : cps)
    {
        bool letter = changeCase(c, true) != c || changeCase(c, false) != c;
        c = changeCase(c, mode == TextCase::Upper || (mode == TextCase::Title && wordStart));
        if (letter)
            wordStart = false;
        else if (isSpace(c) || c == '\n' || c == '-')
            wordStart = true;
    }
}

int16_t fontShort(const Font& f, const char* table, int offset, int16_t fallback)
{
    uint32_t at = stbtt__find_table(f.info.data, uint32_t(f.info.fontstart), table);
    if (!at || size_t(at) + size_t(offset) + 2 > f.data.size())
        return fallback;
    return int16_t(be16(f.info.data + at + offset));
}

Spline rectangle(double x0, double y0, double x1, double y1)
{
    D4 a(x0, y0, 0.0, 1.0), b(x1, y0, 0.0, 1.0), c(x1, y1, 0.0, 1.0), d(x0, y1, 0.0, 1.0);
    Spline s = fromPieces({{{a, b}}, {{b, c}}, {{c, d}}, {{d, a}}}, true, Kind::Bezier);
    vectorHandlesOnLines(s);
    return s;
}

struct TextLine
{
    size_t begin = 0, end = 0;
    bool paragraphEnd = true;
};
} // namespace

TextResult textCurves(const TextOptions& o)
{
    LILA_ZONE();

    TextResult res;
    std::string spec = findFont(o.font);
    if (spec.empty())
    {
        res.warnings.push_back("font '" + o.font + "' not found; using Inter");
        spec = findFont("Inter");
    }
    std::string why;
    std::shared_ptr<const Font> font = spec.empty() ? nullptr : loadFont(spec, &why);
    if (!font)
    {
        res.warnings.push_back(why.empty() ? "no font to draw with" : why);
        return res;
    }
    res.fontPath = spec;
    const double size = std::max(o.size, 1e-6);
    const double scale = stbtt_ScaleForMappingEmToPixels(&font->info, float(size));
    const double xScale = scale * std::max(o.width, 1e-3);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    const double lineHeight = (ascent - descent + gap) * scale * o.lineSpacing;
    const int unitsPerEm = int(std::lround(size / scale));

    std::vector<uint32_t> cps = decodeUtf8(o.text);
    applyCase(cps, o.textCase);
    const size_t n = cps.size();
    std::vector<int> glyph(n, 0);
    std::vector<double> advance(n, 0.0), kern(n, 0.0);
    std::vector<std::string> missing;
    for (size_t k = 0; k < n; ++k)
    {
        if (cps[k] == '\n')
            continue;
        uint32_t cp = isSpace(cps[k]) ? ' ' : cps[k];
        int g = stbtt_FindGlyphIndex(&font->info, int(cp));
        if (g == 0 && cp != ' ')
        {
            std::string ch = encodeUtf8(cp);
            if (std::find(missing.begin(), missing.end(), ch) == missing.end())
                missing.push_back(ch);
        }
        glyph[k] = g;
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font->info, g, &adv, &lsb);
        advance[k] = adv * xScale + o.letterSpacing + (cp == ' ' ? o.wordSpacing : 0.0);
        if (o.kerning && k > 0 && glyph[k - 1] && g)
            kern[k] = stbtt_GetGlyphKernAdvance(&font->info, glyph[k - 1], g) * xScale;
    }

    // From the first character's origin to the end of the last visible one, without its letter spacing.
    auto measure = [&](size_t b, size_t e, int* spaces) {
        double x = 0.0, ink = 0.0;
        int gaps = 0, pending = 0;
        bool seen = false;
        for (size_t k = b; k < e; ++k)
        {
            if (k > b)
                x += kern[k];
            x += advance[k];
            if (isSpace(cps[k]))
                ++pending;
            else
            {
                ink = x - o.letterSpacing;
                if (seen)
                    gaps += pending;
                pending = 0;
                seen = true;
            }
        }
        if (spaces)
            *spaces = gaps;
        return ink;
    };

    std::vector<TextLine> lines;
    const double wrap = std::max(0.0, o.wrapWidth);
    for (size_t ps = 0; ps <= n;)
    {
        size_t pe = ps;
        while (pe < n && cps[pe] != '\n')
            ++pe;
        if (ps == pe)
            lines.push_back({ps, pe, true});
        for (size_t ls = ps; ls < pe;)
        {
            size_t k = ls, breakAt = std::string::npos;
            bool ink = false;
            for (; k < pe; ++k)
            {
                if (isSpace(cps[k]))
                {
                    if (ink && !isSpace(cps[k - 1]))
                        breakAt = k;
                    continue;
                }
                if (wrap > 0.0 && ink && measure(ls, k + 1, nullptr) > wrap * (1.0 + 1e-9))
                    break;
                ink = true;
            }
            if (k == pe)
            {
                lines.push_back({ls, pe, true});
                break;
            }
            size_t end = breakAt != std::string::npos ? breakAt : k;
            lines.push_back({ls, end, false});
            ls = end;
            while (ls < pe && isSpace(cps[ls]))
                ++ls;
            if (ls == pe)
                lines.back().paragraphEnd = true;
        }
        ps = pe + 1;
    }

    double blockWidth = wrap;
    if (blockWidth <= 0.0)
        for (const TextLine& l : lines)
            blockWidth = std::max(blockWidth, measure(l.begin, l.end, nullptr));

    glm::dmat4 shape(1.0);
    shape[0][0] = xScale / scale;
    shape[1][0] = std::tan(glm::radians(std::clamp(o.slant, -60.0, 60.0)));
    const double underPos = fontShort(*font, "post", 8, int16_t(-unitsPerEm / 10)) * scale;
    const double underSize = std::max<int16_t>(1, fontShort(*font, "post", 10, int16_t(unitsPerEm / 20))) * scale;
    const double strikePos = fontShort(*font, "OS/2", 28, int16_t(ascent * 3 / 10)) * scale;
    const double strikeSize = std::max<int16_t>(1, fontShort(*font, "OS/2", 26, int16_t(unitsPerEm / 20))) * scale;

    std::vector<int> lineOf;
    Splines bars;
    std::vector<int> barLine;
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const TextLine& l = lines[li];
        const double y = -double(li) * lineHeight;
        int spaces = 0;
        double width = measure(l.begin, l.end, &spaces);
        double extra = 0.0, shift = 0.0;
        if (o.align == TextAlign::Justify && !l.paragraphEnd && spaces > 0 && blockWidth > width)
        {
            extra = (blockWidth - width) / spaces;
            width = blockWidth;
        }
        else if (o.align == TextAlign::Center)
            shift = -width * 0.5;
        else if (o.align == TextAlign::Right)
            shift = -width;
        double x = shift;
        bool seen = false;
        for (size_t k = l.begin; k < l.end; ++k)
        {
            if (k > l.begin)
                x += kern[k];
            if (!isSpace(cps[k]))
            {
                glm::dmat4 m = glm::translate(glm::dmat4(1.0), D3(x, y, 0.0)) * shape;
                for (Spline& s : glyphCurves(*font, glyph[k], scale, D3(0.0)))
                {
                    transform(s, m);
                    res.curves.push_back(std::move(s));
                    res.character.push_back(int(k));
                    lineOf.push_back(int(li));
                }
                seen = true;
            }
            x += advance[k];
            if (seen && isSpace(cps[k]))
                x += extra;
        }
        if (width > 0.0)
        {
            if (o.underline)
            {
                bars.push_back(rectangle(shift, y + underPos - underSize, shift + width, y + underPos));
                barLine.push_back(int(li));
            }
            if (o.strikethrough)
            {
                bars.push_back(rectangle(shift, y + strikePos - strikeSize, shift + width, y + strikePos));
                barLine.push_back(int(li));
            }
        }
    }
    if (!missing.empty())
    {
        std::string list;
        for (const std::string& m : missing)
            list += m;
        res.warnings.push_back("the font has no glyph for '" + list + "'");
    }

    PlaneFrame plane = planeFrame(D3(0.0), D3(0.0, 0.0, 1.0));
    TessOptions tess;
    tess.chordError = size * 1e-4;
    CurveBooleanOptions bo;
    bo.normal = D3(0.0, 0.0, 1.0);
    bo.nonZero = true;
    bo.tolerance = size * 1e-5;
    if (o.mergeOverlaps || o.weight != 0.0)
    {
        // A glyph whose contours overlap fills non-zero, as the font means it.
        Splines merged;
        std::vector<int> mergedChar, mergedLine;
        for (size_t c = 0; c < res.curves.size();)
        {
            size_t end = c;
            while (end < res.curves.size() && res.character[end] == res.character[c])
                ++end;
            Splines one(res.curves.begin() + std::ptrdiff_t(c), res.curves.begin() + std::ptrdiff_t(end));
            if (one.size() > 1 && loopsCross(loopsOf(one, plane, tess)))
            {
                CurveBooleanResult u = curveBoolean({one}, bo);
                if (!u.curves.empty())
                    one = std::move(u.curves);
            }
            if (o.weight != 0.0)
            {
                RegionOffsetOptions ro;
                ro.distance = o.weight;
                ro.join = JoinType::Miter;
                ro.normal = D3(0.0, 0.0, 1.0);
                ro.tolerance = size * 2e-4;
                one = regionOffset(one, ro).curves;
            }
            for (Spline& s : one)
            {
                merged.push_back(std::move(s));
                mergedChar.push_back(res.character[c]);
                mergedLine.push_back(lineOf[c]);
            }
            c = end;
        }
        res.curves = std::move(merged);
        res.character = std::move(mergedChar);
        lineOf = std::move(mergedLine);
    }

    if (!bars.empty())
    {
        // Underlines and strikes join the glyphs they cross, so each line fills once.
        Splines joined;
        std::vector<int> joinedChar;
        for (size_t li = 0; li < lines.size(); ++li)
        {
            Splines line;
            std::vector<int> chars;
            for (size_t c = 0; c < res.curves.size(); ++c)
                if (lineOf[c] == int(li))
                {
                    line.push_back(res.curves[c]);
                    chars.push_back(res.character[c]);
                }
            size_t glyphCurveCount = line.size();
            // Bars wind like the letters' outlines, so where they overlap the fill adds up instead of cancelling.
            double outer = 0.0;
            for (const Loop2& l : loopsOf(line, plane, tess))
                if (double area = signedArea(l.points); std::abs(area) > std::abs(outer))
                    outer = area;
            for (size_t b = 0; b < bars.size(); ++b)
                if (barLine[b] == int(li))
                {
                    Spline bar = bars[b];
                    Loops2 barLoop = loopsOf(Splines{bar}, plane, tess);
                    if (!barLoop.empty() && signedArea(barLoop[0].points) * outer < 0.0)
                        reverse(bar);
                    line.push_back(std::move(bar));
                }
            if (line.size() > glyphCurveCount)
            {
                CurveBooleanResult u = curveBoolean({line}, bo);
                if (!u.curves.empty())
                    line = std::move(u.curves);
                chars.assign(line.size(), int(lines[li].begin));
            }
            for (size_t c = 0; c < line.size(); ++c)
            {
                joined.push_back(std::move(line[c]));
                joinedChar.push_back(chars[c]);
            }
        }
        res.curves = std::move(joined);
        res.character = std::move(joinedChar);
    }

    if (o.anchor != TextAnchor::Baseline && !lines.empty())
    {
        double top = ascent * scale, bottom = -double(lines.size() - 1) * lineHeight + descent * scale;
        double dy = o.anchor == TextAnchor::Top ? -top : o.anchor == TextAnchor::Bottom ? -bottom : -(top + bottom) * 0.5;
        glm::dmat4 m = glm::translate(glm::dmat4(1.0), D3(0.0, dy, 0.0));
        for (Spline& s : res.curves)
            transform(s, m);
    }
    return res;
}
} // namespace lila::curve
