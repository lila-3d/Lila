// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace groomlab {

// A parameter value read from a map instead of a constant (PG-2). In graph
// JSON it is an object with one source key:
//   {"texture": "C:/maps/clump.png", "component": "r", "min": 0, "max": 1}
//   {"colorSet": "clumpMask"}          (a color set on the scalp mesh)
//   {"attribute": "melanin"}           (a per-strand channel)
//   {"expression": "0.5 + 0.5 * sin(u * 6.2832)"}   (see expression.h)
//   {"guide": "density"}               (a guide attribute layer, blended from the nearest guides)
// The map's [0, 1] value (the chosen component; an expression's result) is
// remapped to [min, max].
struct MapRef {
    enum class Source { Texture, ColorSet, Attribute, Expression, Guide };
    enum class Component { R, G, B, A, Luminance };

    Source source = Source::Texture;
    std::string name;  // file path, color set name, channel name or expression text
    Component component = Component::R;
    float min = 0.0f;
    float max = 1.0f;
    // An expression's constants (the Expression node's own variables; not stored in graph JSON).
    std::vector<std::pair<std::string, float>> constants;

    bool operator==(const MapRef& o) const {
        return source == o.source && name == o.name && component == o.component && min == o.min && max == o.max &&
               constants == o.constants;
    }
    bool operator!=(const MapRef& o) const { return !(*this == o); }

    static const char* sourceKey(Source s) {
        switch (s) {
            case Source::Texture: return "texture";
            case Source::ColorSet: return "colorSet";
            case Source::Attribute: return "attribute";
            case Source::Expression: return "expression";
            case Source::Guide: return "guide";
        }
        return "texture";
    }
    static const char* componentName(Component c) {
        switch (c) {
            case Component::R: return "r";
            case Component::G: return "g";
            case Component::B: return "b";
            case Component::A: return "a";
            case Component::Luminance: return "luminance";
        }
        return "r";
    }
};

}  // namespace groomlab
