#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

/** A small XML DOM for COLLADA: elements, attributes and text. No DTDs, namespaces are kept in the names. */
namespace lila::collada::xml
{
struct Element
{
    std::string name; // without a namespace prefix
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text; // concatenated character data directly inside this element
    std::vector<std::unique_ptr<Element>> children;
    Element* parent = nullptr;

    /** The attribute's value, or empty. */
    std::string_view attr(std::string_view key) const;
    bool has(std::string_view key) const;
    /** The first child of that name, or null. */
    const Element* child(std::string_view name) const;
    /** Every child of that name. */
    std::vector<const Element*> all(std::string_view name) const;
    /** The first descendant of that name, depth first, or null. */
    const Element* find(std::string_view name) const;
};

/** Parses a whole document. Null with error set when it is not well formed. */
std::unique_ptr<Element> parse(std::string_view text, std::string* error);

/** Text with &, <, >, " escaped for an attribute or character data. */
std::string escape(std::string_view s);

/** Whitespace-separated numbers and words of an element's text. */
std::vector<double> numbers(std::string_view text);
std::vector<int64_t> integers(std::string_view text);
std::vector<std::string> words(std::string_view text);

/** Builds indented XML text. */
class Writer
{
public:
    void declaration();
    /** Opens an element; attributes as name/value pairs. */
    void open(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs = {});
    void close();
    /** An element with text and no children. */
    void leaf(std::string_view name, std::string_view text, std::initializer_list<std::pair<std::string_view, std::string>> attrs = {});
    /** An element with neither. */
    void empty(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs = {});
    const std::string& str() const { return out; }

private:
    void start(std::string_view name, std::initializer_list<std::pair<std::string_view, std::string>> attrs);
    std::string out;
    std::vector<std::string> stack;
};
} // namespace lila::collada::xml
