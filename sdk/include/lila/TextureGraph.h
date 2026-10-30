#pragma once

/**
 * Texture graphs (tex::Graph), compiling them to plans and evaluating those on the CPU or GPU (tex::evaluate), node
 * definitions an extension ships (tex::registerExtensionDir), and the bake maps texture sets read (tex::bakedMapsChanged).
 */
#include "tex/Evaluate.h"
#include "tex/Generators.h"
#include "tex/Graph.h"
#include "tex/NodeDef.h"
#include "tex/Plan.h"
