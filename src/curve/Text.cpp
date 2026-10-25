#include "curve/Text.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "curve/CurveBoolean.h"
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
using D3 = glm::dvec3;
using D4 = glm::dvec4;

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

const std::vector<FontEntry>& systemFonts()
{
    static const std::vector<FontEntry> fonts = scan(systemFontDirs());
    return fonts;
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
    if (const FontEntry* e = match(systemFonts(), q))
        return entryPath(*e);
    return {};
}

std::vector<std::string> fontNames()
{
    std::vector<std::string> out;
    for (const auto* list : {&bundledFonts(), &systemFonts()})
        for (const FontEntry& e : *list)
            out.push_back(e.family + (e.style.empty() ? "" : " " + e.style));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

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
    double size = std::max(o.size, 1e-6);
    double scale = stbtt_ScaleForMappingEmToPixels(&font->info, float(size));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    double lineHeight = (ascent - descent + gap) * scale * o.lineSpacing;

    std::vector<uint32_t> cps = decodeUtf8(o.text);
    std::vector<std::string> missing;
    double y = 0.0;
    size_t lineStart = 0;
    for (size_t at = 0; at <= cps.size(); ++at)
    {
        if (at < cps.size() && cps[at] != '\n')
            continue;
        // Lay the line out from x = 0, then shift it for the alignment.
        double x = 0.0;
        int prev = 0;
        size_t firstCurve = res.curves.size();
        for (size_t k = lineStart; k < at; ++k)
        {
            uint32_t cp = cps[k] == '\t' ? ' ' : cps[k];
            int g = stbtt_FindGlyphIndex(&font->info, int(cp));
            if (g == 0 && cp != ' ' && cp != '\r')
            {
                std::string ch = encodeUtf8(cp);
                if (std::find(missing.begin(), missing.end(), ch) == missing.end())
                    missing.push_back(ch);
            }
            if (o.kerning && prev && g)
                x += stbtt_GetGlyphKernAdvance(&font->info, prev, g) * scale;
            for (Spline& s : glyphCurves(*font, g, scale, D3(x, y, 0.0)))
            {
                res.curves.push_back(std::move(s));
                res.character.push_back(int(k));
            }
            int advance = 0, lsb = 0;
            stbtt_GetGlyphHMetrics(&font->info, g, &advance, &lsb);
            x += advance * scale + o.letterSpacing;
            prev = g;
        }
        double width = std::max(0.0, x - (at > lineStart ? o.letterSpacing : 0.0));
        double shift = o.align == TextAlign::Center ? -width * 0.5 : o.align == TextAlign::Right ? -width : 0.0;
        if (shift != 0.0)
            for (size_t c = firstCurve; c < res.curves.size(); ++c)
                transform(res.curves[c], glm::translate(glm::dmat4(1.0), D3(shift, 0.0, 0.0)));
        y -= lineHeight;
        lineStart = at + 1;
    }
    if (!missing.empty())
    {
        std::string list;
        for (const std::string& m : missing)
            list += m;
        res.warnings.push_back("the font has no glyph for '" + list + "'");
    }

    if (o.mergeOverlaps)
    {
        // A glyph whose contours overlap fills non-zero, as the font means it.
        Splines merged;
        std::vector<int> mergedChar;
        PlaneFrame plane = planeFrame(D3(0.0), D3(0.0, 0.0, 1.0));
        TessOptions tess;
        tess.chordError = size * 1e-4;
        for (size_t c = 0; c < res.curves.size();)
        {
            size_t end = c;
            while (end < res.curves.size() && res.character[end] == res.character[c])
                ++end;
            Splines glyph(res.curves.begin() + std::ptrdiff_t(c), res.curves.begin() + std::ptrdiff_t(end));
            if (glyph.size() > 1 && loopsCross(loopsOf(glyph, plane, tess)))
            {
                CurveBooleanOptions bo;
                bo.normal = D3(0.0, 0.0, 1.0);
                bo.nonZero = true;
                bo.tolerance = size * 1e-5;
                CurveBooleanResult u = curveBoolean({glyph}, bo);
                if (!u.curves.empty())
                    glyph = std::move(u.curves);
            }
            for (Spline& s : glyph)
            {
                merged.push_back(std::move(s));
                mergedChar.push_back(res.character[c]);
            }
            c = end;
        }
        res.curves = std::move(merged);
        res.character = std::move(mergedChar);
    }
    return res;
}
} // namespace lila::curve
