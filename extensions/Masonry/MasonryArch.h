#pragma once

#include "MasonryCore.h"

#include <glm/glm.hpp>

#include <vector>

namespace masonry
{
/** An arch in its own plane: s across the span (0 at the middle), v up from the springing line. Lengths are cm. */
struct ArchSpec
{
    ArchKind kind = ArchKind::Round;
    double span = 100.0;  // between the springers, at the intrados
    double rise = 0.0;    // intrados apex above the springing; 0 is the kind's own (half the span for round)
    double ring = 20.0;   // voussoir depth across the arch
    double unit = 10.0;   // voussoir width wanted at the intrados, joint included
    double joint = 1.0;
    bool keystone = true; // an odd count with a taller stone at the crown
};

/** One stone of an arch in its plane: an isosceles trapezoid, narrow end (bottom) toward the intrados. */
struct ArchStone
{
    glm::dvec2 center{0.0};
    glm::dvec2 axisX{1.0, 0.0}; // across the stone
    glm::dvec2 axisY{0.0, 1.0}; // from the intrados out
    double top = 0.0;    // width at the outer end
    double bottom = 0.0; // width at the inner end
    double height = 0.0;
    Role role = Role::Voussoir;
};

/** Intrados apex above the springing. */
double archRise(const ArchSpec& a);

/** Voussoirs from springer to springer, radial to the arch's centres, with joint gaps, and the keystone. */
std::vector<ArchStone> archStones(const ArchSpec& a);

/** Half the arch's outer width (keystone included) at height v above the springing; negative above the arch. */
double extradosHalfWidth(const ArchSpec& a, double v);
} // namespace masonry
