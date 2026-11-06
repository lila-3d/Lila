#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace lila::collada
{
namespace fs = std::filesystem;

inline fs::path pathOf(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

inline std::string textOf(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

inline bool readText(const fs::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline bool writeText(const fs::path& path, std::string_view bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(f);
}

/** A file URI or relative reference from a COLLADA init_from: percent escapes decoded, file:// stripped. */
inline std::string decodeUri(std::string_view uri)
{
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            const int a = hex(uri[i + 1]), b = hex(uri[i + 2]);
            if (a >= 0 && b >= 0)
            {
                out += char(a * 16 + b);
                i += 2;
                continue;
            }
        }
        out += uri[i];
    }
    if (out.starts_with("file:///"))
        out.erase(0, out.size() > 9 && out[9] == ':' ? 8 : 7);
    else if (out.starts_with("file://"))
        out.erase(0, 7);
    return out;
}

/** Spaces, percent signs and non-ASCII bytes escaped for a URI. */
inline std::string encodeUri(std::string_view s)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (c == ' ' || c == '%' || c == '#' || c >= 0x80)
        {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
        else
            out += char(c);
    }
    return out;
}
} // namespace lila::collada
