#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <cstdint>
#include <string>
#include <string_view>

/** Text as curves: glyph outlines from TrueType and OpenType fonts (stb_truetype), exact Beziers. */
namespace lila::curve
{
enum class TextAlign : uint8_t
{
    Left,
    Center,
    Right,
    /** Wrapped lines stretch to the wrap width (the widest line without wrapping); a paragraph's last line stays left. */
    Justify,
};

/** Which part of the text block sits on y = 0. */
enum class TextAnchor : uint8_t
{
    Baseline,
    Top,
    Middle,
    Bottom,
};

enum class TextCase : uint8_t
{
    AsTyped,
    Upper,
    Lower,
    Title,
};

/** A font family and the styles it comes in ("Regular", "Bold Italic"), bundled or installed. */
struct FontFamily
{
    std::string family;
    std::vector<std::string> styles;
};

struct TextOptions
{
    std::string text;
    /** A font file, or a family and style ("Inter", "Inter SemiBold", "Arial Bold") from the bundled or installed fonts. */
    std::string font;
    /** Em size, cm. */
    double size = 10.0;
    /** Extra space after each character, cm. */
    double letterSpacing = 0.0;
    /** Extra space after each space character, cm. */
    double wordSpacing = 0.0;
    /** Baseline to baseline, in the font's line heights. */
    double lineSpacing = 1.0;
    TextAlign align = TextAlign::Left;
    TextAnchor anchor = TextAnchor::Baseline;
    /** Lines break between words to fit this width, cm; 0 breaks only at new lines. */
    double wrapWidth = 0.0;
    TextCase textCase = TextCase::AsTyped;
    /** Horizontal scale of the glyphs (1 is the font's width). */
    double width = 1.0;
    /** Oblique slant, degrees; positive leans right. For fonts without an italic. */
    double slant = 0.0;
    /** Grows each glyph's outline by this much, cm (negative thins it). For fonts without a bolder weight. */
    double weight = 0.0;
    bool underline = false;
    bool strikethrough = false;
    bool kerning = true;
    /** Join a glyph's overlapping contours (variable and some display fonts overlap them) so each fills once. */
    bool mergeOverlaps = true;
};

struct TextResult
{
    /** Closed Bezier outlines in the XY plane, the first line's baseline on y = 0 from x = 0 (as aligned). */
    Splines curves;
    /** Per curve, the index of the character it belongs to (UTF-8 code points from 0, newlines counted). */
    std::vector<int> character;
    std::string fontPath;
    std::vector<std::string> warnings;
};

LILA_SCENE_API TextResult textCurves(const TextOptions& o);

/** The font file for a path, or a family and style among the bundled then installed fonts; empty when nothing matches. */
LILA_SCENE_API std::string findFont(std::string_view nameOrPath);

/** "Family Style" of every bundled and installed font, sorted, for pickers. */
LILA_SCENE_API std::vector<std::string> fontNames();

/** Every bundled and installed family with its styles, sorted by family. */
LILA_SCENE_API std::vector<FontFamily> fontFamilies();

/** Changes when the installed fonts do (checked on each call), so pickers know to refill. */
LILA_SCENE_API uint64_t fontListVersion();
} // namespace lila::curve
