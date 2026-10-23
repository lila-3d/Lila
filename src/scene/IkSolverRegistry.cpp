#include "scene/IkSolver.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"

namespace lila::scene::ik
{
namespace
{
void check(const Solver& s)
{
    if (s.name.empty())
        throw std::invalid_argument("an IK solver needs a name");
    if (!s.solve)
        throw std::invalid_argument("IK solver '" + s.name + "' has no solve");
}
} // namespace

Registry<Solver>& solvers()
{
    static Registry<Solver> r;
    return r;
}

const Solver& registerSolver(Solver solver)
{
    check(solver);
    std::string name = solver.name;
    return solvers().add(std::move(name), std::move(solver));
}

const Solver* registerSolver(ext::Context& ctx, Solver solver)
{
    std::string name = solver.name;
    try
    {
        check(solver);
        ctx.add(solvers(), "ikSolver", name, std::move(solver));
        return solvers().find(name);
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

const Solver* findSolver(std::string_view name)
{
    return solvers().find(name);
}
} // namespace lila::scene::ik
