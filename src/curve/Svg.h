#pragma once

#include "core/Export.h"
#include "curve/Spline.h"

#include <string>
#include <string_view>

/** SVG to Bezier curves (nanosvg: paths, basic shapes, transforms, groups, viewBox) and planar curves back to SVG paths. */
namespace lila::curve
{
enum class SvgPlane : uint8_t
{
    XY, // the front view
    XZ, // the ground
    YZ, // the side view
};
LILA_SCENE_API const char* svgPlaneName(SvgPlane p);
LILA_SCENE_API bool parseSvgPlane(std::string_view name, SvgPlane* out);

struct SvgImportOptions
{
    /** cm per SVG user unit (px after the viewBox). */
    double scale = 0.1;
    SvgPlane plane = SvgPlane::XY;
    /** Centre the drawing on the origin; otherwise its top left corner sits there. */
    bool center = true;
};

struct SvgShape
{
    std::string id;
    Splines curves;
    bool evenOdd = false;
};

struct SvgDocument
{
    std::vector<SvgShape> shapes;
    std::vector<std::string> warnings;
};

/** Parses SVG text. Lines come in as Bezier spans with vector handles, so straight edges stay straight. */
LILA_SCENE_API SvgDocument readSvg(std::string text, const SvgImportOptions& o = {});

struct SvgExportOptions
{
    double scale = 0.1; // cm per SVG unit
    SvgPlane plane = SvgPlane::XY;
    double tolerance = 0.01; // cm, for spans that are not cubic Beziers already
};

/** An SVG document with one path per curve (Beziers exactly, NURBS converted within tolerance), y flipped to read upright. */
LILA_SCENE_API std::string writeSvg(const std::vector<Splines>& curves, const SvgExportOptions& o = {});
} // namespace lila::curve
