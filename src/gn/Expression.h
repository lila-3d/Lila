#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * The per-element language of the Wrangle and Expression nodes, VEX-like:
 *
 *   float h = fit(@P.y, 0, 100, 0, 1);
 *   if (h > a) { @Cd = set(1, h, 0); v@up = normalize(@N); }
 *   for (int i = 0; i < 4; i++) @P.y += noise(@P * i) * b;
 *
 * Types are float, int, vector and vector4; @name reads or writes an attribute (f@, i@, v@ and p@ give its type, float
 * otherwise), a, b, c and d (or ch("a")) read the node's inputs. Values are held in dvec4 lanes with unused components
 * zero, so the program is compiled once to a typed tree and run in parallel over the elements.
 */
namespace lila::gn::expr
{
enum class Type : uint8_t
{
    Float,
    Int,
    Vector,
    Vector4,
};

LILA_SCENE_API const char* typeName(Type t);

enum class AttrKind : uint8_t
{
    Attribute,
    Index, // @ptnum, @elemnum
    Count, // @numelem, @numpt
};

/** An attribute the code touches, by its name in the code (P, N, Cd, id, or any stored name). */
struct AttrRef
{
    std::string name;
    Type type = Type::Float;
    AttrKind kind = AttrKind::Attribute;
    bool readOnly = false;
    bool written = false;
};

struct LILA_SCENE_API CompileError : std::runtime_error
{
    CompileError(const std::string& message, int line, int column);
    int line = 0, column = 0;
};

struct Options
{
    /** Off for the Expression node, which only computes a value. */
    bool allowWrites = true;
    /** The code must end in an expression, whose value is the result. */
    bool needResult = false;
};

/**
 * What a run reads and writes, one dvec4 lane per element. attrs follow Program::attributes() (Index and Count ones
 * are filled by the program and may be left empty); inputs a..d hold one value per element or one for all.
 */
struct Bindings
{
    size_t count = 0;
    std::vector<std::vector<glm::dvec4>> attrs;
    std::array<std::vector<glm::dvec4>, 4> inputs;
    /** Elements to run on; empty runs on all. Unselected elements keep their attribute values. */
    std::vector<uint8_t> selection;
    std::vector<glm::dvec4> result;
};

class LILA_SCENE_API Program
{
public:
    struct Impl;
    explicit Program(std::shared_ptr<const Impl> impl) : mImpl(std::move(impl)) {}

    const std::vector<AttrRef>& attributes() const;
    bool usesInput(int k) const;
    bool hasResult() const;
    Type resultType() const;
    /** Runs over every element. Throws std::runtime_error when a loop runs away. */
    void run(Bindings& b) const;

private:
    std::shared_ptr<const Impl> mImpl;
};

using ProgramPtr = std::shared_ptr<const Program>;

/** Throws CompileError with the line and column of the problem. */
LILA_SCENE_API ProgramPtr compile(std::string_view code, const Options& options = {});
} // namespace lila::gn::expr
