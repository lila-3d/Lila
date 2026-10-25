#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

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
    /** Baseline to baseline, in the font's line heights. */
    double lineSpacing = 1.0;
    TextAlign align = TextAlign::Left;
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
} // namespace lila::curve
