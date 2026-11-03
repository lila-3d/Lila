#pragma once

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace lila::curve::detail
{
/** Knot interval index i with U[i] <= u < U[i+1], searched in [lo, hi); u at or past U[hi] gives the last non-empty one. */
int findSpan(const std::vector<double>& U, double u, int lo, int hi);
/** The p + 1 non-zero basis functions of degree p on interval i at u (Piegl & Tiller A2.2). */
void basisFuns(int i, double u, int p, const std::vector<double>& U, double* N);
/** Solves A x = b in place (A row-major n x n, b holds n rows); false when singular. */
bool solveDense(std::vector<double>& A, int n, std::vector<glm::dvec4>& b);
/** Chord-length parameters of points in [0, 1] (centripetal with alpha 0.5). Closed adds the closing chord. */
std::vector<double> chordParams(const std::vector<glm::dvec3>& pts, bool closed, double alpha);

/** Adds a face with per-corner uvs (in the face's point order); flipped faces keep their uvs on their points. */
geo::Index addFaceUv(geo::Mesh& m, std::span<const geo::Index> pts, std::span<const glm::vec2> uvs, bool flip);
/** Sets the int face attr name on f, adding the attr when missing; nothing for an invalid face. */
void setFaceInt(geo::Mesh& m, std::string_view name, geo::Index f, int value);
} // namespace lila::curve::detail
