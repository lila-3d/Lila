#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Node;
}

/**
 * The rig math node library: Unreal's math (UKismetMathLibrary and Control Rig's RigVM math) as DG nodes. Each function is
 * declared once with its typed overloads; every overload becomes one node type named after the function, plus the
 * overload's type when there are several ("lerp", "addFloat", "addVector"), so a Control Rig generator can map them 1:1.
 *
 * Value kinds and the plugs that carry them:
 *   bool, int, float           Bool, Int, Float.
 *   vector                     Vec3.
 *   colour                     Vec3 flagged AttrColor (linear RGB, no alpha).
 *   rotator                    Vec3 of Euler degrees, read in the node's rotateOrder, as a transform's rotate.
 *   quat                       Matrix holding a pure rotation: there is no four-component plug, and a rotation matrix
 *                              wires straight into matrix inputs. Quat inputs read the matrix's normalised rotation.
 *   matrix, transform          Matrix (Lila's column-vector glm layout). Composition follows Unreal: compose(A, B)
 *                              applies A first, then B.
 * Rotations are on Lila's own local axes: makeRotFromX aims the rotation's X axis, getAxes returns its X, Y, Z.
 */
namespace lila::rigmath
{
enum class Kind : uint8_t
{
    Bool,
    Int,
    Float,
    Vector,
    Rotator,
    Quat,
    Matrix,
    Color,
    Choice, // an Enum plug
};

LILA_SCENE_API const char* kindName(Kind k);
LILA_SCENE_API scene::PlugType plugTypeOf(Kind k);

struct Pin
{
    std::string name;
    Kind kind = Kind::Float;
    scene::PlugValue defaultValue;
    bool array = false;
    std::vector<std::string> choices; // Choice only
};

struct Overload
{
    /** The node type it registers as. */
    std::string nodeType;
    /** The type part of nodeType when the function has several overloads; empty takes the first input's kind. */
    std::string suffix;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    /** Reads the node's inputs (attrs 0..inputs-1) and returns one value per output. */
    std::function<std::vector<scene::PlugValue>(const scene::Node&)> run;
};

struct Function
{
    /** Unreal's function name in lowerCamel, e.g. "mapRangeClamped". */
    std::string name;
    std::string family;
    std::string doc;
    std::vector<Overload> overloads;
};

/** Every function, in declaration order. Built on first use. */
LILA_SCENE_API const std::vector<Function>& functions();
/** The overload a node type was made from, or null. */
LILA_SCENE_API const Overload* overloadFor(std::string_view nodeType);
/** The function a node type belongs to, or null. */
LILA_SCENE_API const Function* functionFor(std::string_view nodeType);

/** Registers one node type per overload. Safe to call again. */
LILA_SCENE_API void registerTypes();
/** The mathNodes query command. */
LILA_SCENE_API void registerCommands();
} // namespace lila::rigmath
