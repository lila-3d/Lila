#include "collada/Xml.h"

#include <charconv>
#include <cstdlib>
#include <format>

namespace lila::collada::xml
{
std::string_view Element::attr(std::string_view key) const
{
    for (const auto& [k, v] : attrs)
        if (k == key)
            return v;
    return {};
}

bool Element::has(std::string_view key) const
{
    for (const auto& [k, v] : attrs)
        if (k == key)
            return true;
    return false;
}

const Element* Element::child(std::string_view n) const
{
    for (const auto& c : children)
        if (c->name == n)
            return c.get();
    return nullptr;
}

std::vector<const Element*> Element::all(std::string_view n) const
{
    std::vector<const Element*> out;
    for (const auto& c : children)
        if (c->name == n)
            out.push_back(c.get());
    return out;
}

const Element* Element::find(std::string_view n) const
{
    for (const auto& c : children)
    {
        if (c->name == n)
            return c.get();
        if (const Element* d = c->find(n))
            return d;
    }
    return nullptr;
}

namespace
{
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string_view localName(std::string_view n)
{
    const size_t colon = n.rfind(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

void appendCodepoint(std::string& out, uint32_t cp)
{
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800)
    {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
    else
    {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

void unescapeInto(std::string& out, std::string_view s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos)
        {
            out += s[i];
            continue;
        }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp")
            out += '&';
        else if (e == "lt")
            out += '<';
        else if (e == "gt")
            out += '>';
        else if (e == "quot")
            out += '"';
        else if (e == "apos")
            out += '\'';
        else if (!e.empty() && e[0] == '#')
        {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            uint32_t cp = 0;
            const std::string_view digits = e.substr(hex ? 2 : 1);
            std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            appendCodepoint(out, cp);
        }
        else
        {
            out.append(s.substr(i, semi - i + 1));
        }
        i = semi;
    }
}

struct Parser
{
    std::string_view s;
    size_t i = 0;
    std::string error;

    bool fail(const std::string& what)
    {
        size_t line = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k)
            line += s[k] == '\n';
        error = std::format("{} (line {})", what, line);
        return false;
    }

    bool starts(std::string_view p) const { return s.substr(i, p.size()) == p; }

    bool skipPast(std::string_view end)
    {
        const size_t at = s.find(end, i);
        if (at == std::string_view::npos)
            return fail("unterminated markup");
        i = at + end.size();
        return true;
    }

    void skipSpace()
    {
        while (i < s.size() && isSpace(s[i]))
            ++i;
    }

    std::string_view name()
    {
        const size_t b = i;
        while (i < s.size() && !isSpace(s[i]) && s[i] != '>' && s[i] != '/' && s[i] != '=')
            ++i;
        return s.substr(b, i - b);
    }

    /** Skips the prolog: declaration, comments, doctype, processing instructions. */
    bool prolog()
    {
        for (;;)
        {
            skipSpace();
            if (starts("<?"))
            {
                if (!skipPast("?>"))
                    return false;
            }
            else if (starts("<!--"))
            {
                if (!skipPast("-->"))
                    return false;
            }
            else if (starts("<!"))
            {
                if (!skipPast(">"))
                    return false;
            }
            else
                return true;
        }
    }

    std::unique_ptr<Element> element(Element* parent)
    {
        if (!starts("<"))
        {
            fail("expected an element");
            return nullptr;
        }
        ++i;
        auto e = std::make_unique<Element>();
        e->parent = parent;
        e->name = std::string(localName(name()));
        if (e->name.empty())
        {
            fail("element without a name");
            return nullptr;
        }
        for (;;)
        {
            skipSpace();
            if (i >= s.size())
            {
                fail("unterminated tag <" + e->name + ">");
                return nullptr;
            }
            if (starts("/>"))
            {
                i += 2;
                return e;
            }
            if (s[i] == '>')
            {
                ++i;
                break;
            }
            const std::string key(localName(name()));
            skipSpace();
            if (i >= s.size() || s[i] != '=')
            {
                fail("attribute " + key + " without a value");
                return nullptr;
            }
            ++i;
            skipSpace();
            if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
            {
                fail("unquoted attribute " + key);
                return nullptr;
            }
            const char q = s[i++];
            const size_t end = s.find(q, i);
            if (end == std::string_view::npos)
            {
                fail("unterminated attribute " + key);
                return nullptr;
            }
            std::string value;
            unescapeInto(value, s.substr(i, end - i));
            e->attrs.emplace_back(key, std::move(value));
            i = end + 1;
        }
        // Content.
        for (;;)
        {
            const size_t lt = s.find('<', i);
            if (lt == std::string_view::npos)
            {
                fail("unterminated element <" + e->name + ">");
                return nullptr;
            }
            unescapeInto(e->text, s.substr(i, lt - i));
            i = lt;
            if (starts("</"))
            {
                i += 2;
                const std::string_view closing = localName(name());
                if (closing != e->name)
                {
                    fail("</" + std::string(closing) + "> closes <" + e->name + ">");
                    return nullptr;
                }
                skipSpace();
                if (i >= s.size() || s[i] != '>')
                {
                    fail("malformed closing tag");
                    return nullptr;
                }
                ++i;
                return e;
            }
            if (starts("<!--"))
            {
                if (!skipPast("-->"))
                    return nullptr;
                continue;
            }
            if (starts("<![CDATA["))
            {
                i += 9;
                const size_t end = s.find("]]>", i);
                if (end == std::string_view::npos)
                {
                    fail("unterminated CDATA");
                    return nullptr;
                }
                e->text.append(s.substr(i, end - i));
                i = end + 3;
                continue;
            }
            if (starts("<?"))
            {
                if (!skipPast("?>"))
                    return nullptr;
                continue;
            }
            std::unique_ptr<Element> c = element(e.get());
            if (!c)
                return nullptr;
            e->children.push_back(std::move(c));
        }
    }
};

template <class T> void parseAll(std::string_view text, std::vector<T>& out)
{
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && isSpace(text[i]))
            ++i;
        size_t j = i;
        while (j < text.size() && !isSpace(text[j]))
            ++j;
        if (j > i)
        {
            std::string_view w = text.substr(i, j - i);
            if (!w.empty() && w[0] == '+')
                w.remove_prefix(1);
            T v{};
            const auto [p, ec] = std::from_chars(w.data(), w.data() + w.size(), v);
            if (ec == std::errc())
                out.push_back(v);
            else if constexpr (std::is_floating_point_v<T>)
            {
                // nan, inf and the odd spellings from_chars refuses.
                out.push_back(T(std::strtod(std::string(w).c_str(), nullptr)));
            }
            else
                out.push_back(T(0));
        }
        i = j;
    }
}
} // namespace

std::unique_ptr<Element> parse(std::string_view text, std::string* error)
{
    if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF)
        text.remove_prefix(3);
    Parser p{text};
    if (!p.prolog())
    {
        if (error)
            *error = p.error;
        return nullptr;
    }
    std::unique_ptr<Element> root = p.element(nullptr);
    if (!root && error)
        *error = p.error;
    return root;
}

std::string escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    return out;
}

std::vector<double> numbers(std::string_view text)
{
    std::vector<double> out;
    parseAll(text, out);
    return out;
}

std::vector<int64_t> integers(std::string_view text)
{
    std::vector<int64_t> out;
    parseAll(text, out);
    return out;
}

std::vector<std::string> words(std::string_view text)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && isSpace(text[i]))
            ++i;
        size_t j = i;
        while (j < text.size() && !isSpace(text[j]))
            ++j;
        if (j > i)
            out.emplace_back(text.substr(i, j - i));
        i = j;
    }
    return out;
}

void Writer::declaration() { out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"; }

void Writer::start(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs)
{
    out.append(stack.size() * 2, ' ');
    out += '<';
    out += name;
    for (const auto& [k, v] : attrs)
    {
        out += ' ';
        out += k;
        out += "=\"";
        out += escape(v);
        out += '"';
    }
}

void Writer::open(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs)
{
    start(name, attrs);
    out += ">\n";
    stack.emplace_back(name);
}

void Writer::close()
{
    const std::string name = std::move(stack.back());
    stack.pop_back();
    out.append(stack.size() * 2, ' ');
    out += "</";
    out += name;
    out += ">\n";
}

void Writer::leaf(std::string_view name, std::string_view text, std::initializer_list<std::pair<std::string_view, std::string>> attrs)
{
    start(name, attrs);
    out += '>';
    out += escape(text);
    out += "</";
    out += name;
    out += ">\n";
}

void Writer::empty(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs)
{
    start(name, attrs);
    out += "/>\n";
}
} // namespace lila::collada::xml
