#pragma once

#include "core/Export.h"
#include "core/PrefStore.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::ui::themes
{
/** sRGB, 8 bits a channel. */
struct Rgba
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba&) const = default;
};

/** "#rgb", "#rrggbb" or "#rrggbbaa"; nullopt for anything else. */
LILA_SCENE_API std::optional<Rgba> parseColor(std::string_view s);
/** "#rrggbb", or "#rrggbbaa" when not opaque. */
LILA_SCENE_API std::string formatColor(Rgba c);
/** WCAG 2 relative luminance, 0 black to 1 white; alpha ignored. */
LILA_SCENE_API double luminance(Rgba c);
/** WCAG 2 contrast ratio, 1 to 21. */
LILA_SCENE_API double contrast(Rgba a, Rgba b);

/**
 * One named colour a theme sets. `level` is the grey the neutral tokens have in Monotone (-1 for the rest): painters
 * that draw in plain greys are mapped through those anchors (see shade), so a theme recolours them without naming
 * every one.
 */
struct TokenInfo
{
    const char* id;
    const char* label;
    const char* group; // "Interface", "Icons", "Viewport", "Node Editor"
    int level = -1;
};

/** Every token, in the order the Preferences editor lists them. */
LILA_SCENE_API const std::vector<TokenInfo>& tokens();
LILA_SCENE_API const TokenInfo* token(std::string_view id);

/**
 * A theme: name, author, a base ("dark" or "light") and colours by token id. Tokens a theme leaves out come from its
 * base's built-in theme (Monotone for dark, Light for light).
 */
struct Theme
{
    std::string name;
    std::string author;
    std::string base = "dark";
    std::map<std::string, Rgba, std::less<>> colors;
    std::string description; // built-ins only

    /** The token's colour here, else the base theme's; magenta for an id no theme knows. */
    LILA_SCENE_API Rgba color(std::string_view id) const;
};

struct ParseResult
{
    std::optional<Theme> theme;
    std::vector<std::string> errors;   // fatal: no theme
    std::vector<std::string> warnings; // skipped parts (unknown tokens)
};

/**
 * Reads a theme document: {"lilaTheme": 1, "name", "author", "base", "colors": {token: "#rrggbb"}}. Missing tokens are
 * fine; unknown ones and bad colour values are reported, and a bad value is an error since it was meant to be set.
 */
LILA_SCENE_API ParseResult parse(const json& doc);
LILA_SCENE_API ParseResult parseText(std::string_view text);
/** The document for a theme; full=true writes every token resolved, else only the ones it sets. */
LILA_SCENE_API json toJson(const Theme& t, bool full = false);
/** Pretty JSON text of toJson, for copy/paste and .lilatheme files. */
LILA_SCENE_API std::string toText(const Theme& t, bool full = false);

/** Monotone (the default), Colour, Light and High Contrast. */
LILA_SCENE_API const std::vector<Theme>& builtIns();
LILA_SCENE_API const Theme* builtIn(std::string_view name);
/** The complete built-in theme a base falls back to. */
LILA_SCENE_API const Theme& baseTheme(std::string_view base);

inline constexpr const char* kDefaultTheme = "Monotone";
/** general pref: the current theme's name. */
inline constexpr const char* kThemePref = "/ui/theme";
/** general pref: saved themes by name, each a theme document. A workspace's .lila/general.json may add or override. */
inline constexpr const char* kThemesPref = "/ui/themes";

/** Built-ins then saved themes, saved ones sorted. */
LILA_SCENE_API std::vector<std::string> names();
LILA_SCENE_API bool isBuiltIn(std::string_view name);
/** A built-in or saved theme by name. */
LILA_SCENE_API std::optional<Theme> find(std::string_view name);
LILA_SCENE_API std::string currentName();
/** The current theme (Monotone when the pref names none that exists). Cached until the prefs change. */
LILA_SCENE_API std::shared_ptr<const Theme> current();
/** A counter bumped whenever the current theme may have changed, for caches keyed on it. */
LILA_SCENE_API uint64_t generation();
/** Drops the cache; prefs changes call it, and anything setting theme prefs directly should too. */
LILA_SCENE_API void invalidate();

/** Throws CommandError for an unknown name. */
LILA_SCENE_API void setCurrent(const std::string& name, PrefScope scope = PrefScope::User);
/** Saves a theme under its name; throws CommandError for an empty name or a built-in's. */
LILA_SCENE_API void save(const Theme& t, PrefScope scope = PrefScope::User);
/** False if no saved theme has that name. Built-ins can't be removed. */
LILA_SCENE_API bool remove(const std::string& name);
/** Throws CommandError when there is nothing to rename or the new name is taken. */
LILA_SCENE_API void rename(const std::string& from, const std::string& to);
/** Copies a theme (built-in or saved) to a new saved one; an empty `to` picks "<name> Copy". Returns the new name. */
LILA_SCENE_API std::string duplicate(const std::string& from, std::string to = {});

/**
 * The current theme's colour for a grey level of the Monotone design (0..255): interpolated between the neutral
 * tokens' Monotone levels, so Monotone returns the grey unchanged and other themes move it to their own neutrals.
 */
LILA_SCENE_API Rgba shade(int level, uint8_t alpha = 255);
/** shade() for a given theme, for tests and previews. */
LILA_SCENE_API Rgba shade(const Theme& t, int level, uint8_t alpha = 255);
/** Whether the theme's token equals Monotone's, so callers can keep their exact built-in values for it. */
LILA_SCENE_API bool isDefault(const Theme& t, std::string_view id);

/** setTheme, listThemes, currentTheme, exportTheme, importTheme, deleteTheme, renameTheme, duplicateTheme, setThemeColor. */
LILA_SCENE_API void registerCommands();
} // namespace lila::ui::themes
