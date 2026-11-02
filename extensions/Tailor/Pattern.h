#pragma once

// Pattern drafting on the flat pieces: edits that keep seams, notches and linked twins in step, darts, mirrored twins,
// grading and automatic sewing.

#include "TailorCore.h"

#include <vector>

namespace tailor
{

/** Total outline length of a seam end's runs, metres. */
float seamSideLength(const Garment& g, const std::vector<SeamSide>& sides);

/** Inserts an outline point before index `index` (0..size). Seams keep sewing the same stretch of outline. */
void insertOutlinePoint(Garment& g, int panel, int index, const Vec2& at);
/** Deletes an outline point; seams ending there end at the next point, and a seam left without length is removed.
 *  Returns the number of seams removed. */
int deleteOutlinePoint(Garment& g, int panel, int index);
/** Replaces a panel's outline with one of the same point count. */
void setOutline(Garment& g, int panel, const std::vector<Vec2>& outline);
/** Removes panels and the seams sewing them. Returns the number of seams removed. */
int removePanels(Garment& g, std::vector<int> panels);

/** Marks a notch at the outline point nearest `at`, inserting a point there when none is within `snap`. Returns the
 *  point index. */
int addNotch(Garment& g, int panel, const Vec2& at, float snap = 0.005f);

/**
 * Cuts a dart into the outline edge from point `edge` to the next: legs `width` apart centred `t` along the edge, the
 * point `depth` in from it, legs sewn together. A seam along that edge keeps sewing the edge on both sides of the dart.
 * Returns the dart's seam index, or -1 when the edge is too short for it.
 */
int addDart(Garment& g, int panel, int edge, float t, float width, float depth);

/**
 * Adds a mirror image of a panel, placed mirrored across the plane x = planeX. Linked, every outline edit to either is
 * mirrored onto the other. Returns the new panel's index.
 */
int mirrorPanel(Garment& g, int panel, float planeX, bool link);

/**
 * Grades a pattern: widths by `width`, lengths by `length`. Set-in sleeve caps are then re-eased to their armholes:
 * each cap is widened or narrowed until it carries the same ease as before.
 */
void gradePattern(Garment& g, float width, float length);

/**
 * Seams a pattern maker would sew next: unsewn outline edges (corner to corner) on placed panels whose 3D positions lie
 * within maxGap of each other and whose lengths match within 15%. Each edge is used once, closest pairs first.
 */
std::vector<Seam> suggestSeams(const Garment& g, float maxGap);

/** A seam split at its notches when both ends carry the same number, so notch meets notch. Otherwise the seam itself. */
std::vector<Seam> splitSeamAtNotches(const Garment& g, const Seam& seam);

} // namespace tailor
