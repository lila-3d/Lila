#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Expressions on plugs, in Python syntax: `ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)`. The subset of numbers, other plugs
 * by name, arithmetic, comparisons, and/or/not, `a if c else b` and the math functions below compiles to rig math nodes,
 * so it evaluates as fast as nodes and shows in the node editor; anything else runs as Python in a pythonExpression node
 * (slower, Lila only). Every node an expression makes carries a hidden lilaExpression attr naming its target.
 */
namespace expressions
{
/** A parsed expression. */
struct Ast
{
    enum Kind : uint8_t
    {
        Number,
        Plug,
        Unary,  // op: "-", "not"
        Binary, // op: + - * / % ** and or
        Compare, // op: < <= > >= == !=
        Ternary, // args: then, condition, else
        Call,    // op: function name
    };
    Kind kind = Number;
    double number = 0.0;
    std::string op;   // operator, function name or plug path
    std::vector<std::unique_ptr<Ast>> args;
};

/** Parses the Python subset; throws std::invalid_argument naming what it can't read. */
LILA_SCENE_API std::unique_ptr<Ast> parse(const std::string& text);
/** The functions the subset compiles: name to the rig math function it becomes. */
LILA_SCENE_API const std::vector<std::pair<std::string, std::string>>& functions();
/** Whether text compiles to rig math nodes; else why not, in why. */
LILA_SCENE_API bool compiles(const std::string& text, std::string* why = nullptr);

/** Python for the expressions that don't compile: code with plugs as v0, v1 ... and their values, returning a number. */
using PythonEvaluator = std::function<std::optional<double>(const std::string& code, const std::vector<double>& values, std::string* error)>;
LILA_SCENE_API void setPythonEvaluator(PythonEvaluator fn);

/** The pythonExpression node type. */
LILA_SCENE_API void registerTypes();
/** expression: set, query or delete a plug's expression. */
LILA_SCENE_API void registerCommands();
} // namespace expressions
} // namespace lila::scene
