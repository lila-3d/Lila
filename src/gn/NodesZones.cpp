#include "gn/Exec.h"
#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <tbb/parallel_for.h>

namespace lila::gn::nodes::zones
{
const json* definitionOf(const Group& outer, const GNode& n)
{
    auto it = n.values.find("group");
    if (it == n.values.end() || !it->is_string())
        return nullptr;
    auto def = outer.subgroups.find(it->get<std::string>());
    return def == outer.subgroups.end() ? nullptr : &def->second;
}

/** The inner group's inputs and outputs, plus the zone's own inputs ahead of them. Inputs the zone drives itself are left off. */
Sockets zoneSockets(const Group& outer, const GNode& n, std::vector<SocketDecl> own, const std::vector<std::string>& driven)
{
    Sockets s;
    const json* def = definitionOf(outer, n);
    if (!def)
        return s;
    Group inner;
    try
    {
        inner = Group::fromJson(*def);
    }
    catch (const std::exception&)
    {
        return s;
    }
    s.inputs.push_back(geoIn("geometry", "Geometry handed to the inner group's Group Input."));
    for (SocketDecl& d : own)
        s.inputs.push_back(std::move(d));
    for (const GInput& in : inner.inputs)
        if (std::find(driven.begin(), driven.end(), in.name) == driven.end() && s.inputIndex(in.name) < 0)
            s.inputs.push_back(fin(in.name, in.type, in.defaultValue, Implicit::None, in.description));
    s.outputs.push_back(geoOut("geometry", "The inner group's output geometry."));
    for (const GOutput& o : inner.outputs)
        s.outputs.push_back(fout(o.name, o.type, o.description));
    return s;
}

/** Parameters for the inner group from this node's inputs, linked or set. */
std::map<std::string, Value> innerParams(const ExecContext& c, const Plan& plan)
{
    std::map<std::string, Value> params;
    for (const GInput& in : plan.group.inputs)
    {
        int k = c.sockets.inputIndex(in.name);
        if (k < 0)
            continue;
        bool linked = !c.group.linksInto(c.node.name, in.name).empty();
        if (linked && !c.inputs[k].empty())
            params[in.name] = c.inputs[k][0];
        else
        {
            auto it = c.node.values.find(in.name);
            params[in.name] = paramValue(in, it != c.node.values.end() ? *it : in.defaultValue);
        }
    }
    return params;
}

std::vector<Value> runInner(const ExecContext& c, const Plan& plan, GeometryPtr input, std::map<std::string, Value> params)
{
    ExecEnv env;
    env.time = c.env.time;
    env.input = std::move(input);
    env.params = std::move(params);
    env.objects = c.env.objects;
    env.ownerMatrix = c.env.ownerMatrix;
    try
    {
        return executeOutputs(plan, env);
    }
    catch (const std::exception& e)
    {
        throw ExecError(c.node.name + " (" + plan.group.name + "): " + e.what());
    }
}

/** Sets an Int inner input by name when the group has one. */
void setIndex(std::map<std::string, Value>& params, const Plan& plan, const char* name, int64_t value)
{
    if (const GInput* in = plan.group.input(name))
        params[name] = convertValue(Value{.field = constantInt(value)}, SocketType::Int, in->type);
}

void writeOutputs(ExecContext& c, const Plan& plan, const std::vector<Value>& out)
{
    c.out("geometry", !out.empty() && out[0].geometry ? out[0].geometry : makeGeometry({}));
    for (size_t k = 0; k < plan.group.outputs.size() && k + 1 < out.size(); ++k)
    {
        int o = c.sockets.outputIndex(plan.group.outputs[k].name);
        if (o >= 0)
            c.outputs[o] = convertValue(out[k + 1], plan.group.outputs[k].type, c.sockets.outputs[o].type);
    }
}

PlanPtr planFor(const ExecContext& c)
{
    const json* def = definitionOf(c.group, c.node);
    if (!def)
        throw ExecError(c.node.name + ": no group named '" + c.prop("group") + "'");
    return cachedPlan(*def);
}

/** One geometry per element: mesh parts or faces, curves, instances (each realized in place). */
std::vector<GeometryPtr> splitElements(const Geometry& g, const std::string& by)
{
    std::vector<GeometryPtr> parts;
    if (by == "instance")
    {
        if (!g.instances)
            return parts;
        const Instances& inst = *g.instances;
        for (size_t k = 0; k < inst.size(); ++k)
        {
            int ref = inst.reference[k];
            if (ref < 0 || size_t(ref) >= inst.references.size() || !inst.references[size_t(ref)])
                continue;
            parts.push_back(makeGeometry(realize(transformed(*inst.references[size_t(ref)], inst.transforms[k]))));
        }
        return parts;
    }
    if (by == "curve")
    {
        if (!g.curves)
            return parts;
        for (size_t k = 0; k < g.curves->numCurves(); ++k)
        {
            CurveSet one;
            one.addSpline(g.curves->spline(k));
            parts.push_back(curveGeometry(std::move(one)));
        }
        return parts;
    }
    if (!g.mesh)
        return parts;
    if (by == "face")
    {
        const geo::Mesh& m = *g.mesh;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                geo::Index one[] = {f};
                parts.push_back(meshGeometry(geo::extractFaces(m, one)));
            }
        return parts;
    }
    for (geo::Mesh& part : geo::separate(*g.mesh))
        parts.push_back(meshGeometry(std::move(part)));
    return parts;
}

void repeatNode()
{
    static const std::vector<std::string> driven = {"iteration"};
    registerDecl({
        .type = "RepeatGroup",
        .label = "Repeat",
        .category = "Group",
        .doc = "Runs a node group again and again, each pass taking the last one's results: its output geometry becomes the next "
               "pass's geometry, and each output named like an input feeds that input. An Int input named iteration reads the pass "
               "number, from 0.",
        .props = {prop("group", {}, "", "Name of the node group to repeat.")},
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                return zoneSockets(g, n, {bounded(in("iterations", T::Int, 1, "Number of passes; 0 passes the input through."), 0, 100000)},
                                   driven);
            },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                PlanPtr plan = planFor(c);
                std::map<std::string, Value> params = innerParams(c, *plan);
                GeometryPtr geometry = c.geometry("geometry");
                int64_t passes = std::max<int64_t>(0, c.i("iterations"));
                std::vector<Value> out;
                for (int64_t k = 0; k < passes; ++k)
                {
                    setIndex(params, *plan, "iteration", k);
                    out = runInner(c, *plan, geometry, params);
                    geometry = !out.empty() && out[0].geometry ? out[0].geometry : makeGeometry({});
                    for (size_t o = 0; o < plan->group.outputs.size() && o + 1 < out.size(); ++o)
                        if (const GInput* fed = plan->group.input(plan->group.outputs[o].name))
                            params[fed->name] = convertValue(out[o + 1], plan->group.outputs[o].type, fed->type);
                }
                if (out.empty())
                {
                    // No passes: outputs fed by inputs show those inputs.
                    out.resize(1);
                    for (const GOutput& o : plan->group.outputs)
                    {
                        auto it = params.find(o.name);
                        const GInput* fed = plan->group.input(o.name);
                        out.push_back(it != params.end() && fed ? convertValue(it->second, fed->type, o.type) : defaultValue(o.type));
                    }
                }
                out[0].geometry = geometry;
                writeOutputs(c, *plan, out);
            },
    });
}

void forEachNode()
{
    static const std::vector<std::string> driven = {"index", "count"};
    registerDecl({
        .type = "ForEachElement",
        .label = "For Each Element",
        .category = "Group",
        .doc = "Runs a node group once per element of the geometry, each run seeing only that element, and joins what they return. "
               "Int inputs named index and count read the element's number and how many there are. Runs are evaluated in parallel; "
               "data outputs come from the last element.",
        .props = {prop("group", {}, "", "Name of the node group to run per element."),
                  prop("element", {"part", "face", "curve", "instance"}, "part",
                       "part: each connected mesh part; face: each mesh face; curve: each curve; instance: each instance, realized.")},
        .dynamicSockets = [](const Group& g, const GNode& n) { return zoneSockets(g, n, {}, driven); },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                PlanPtr plan = planFor(c);
                std::map<std::string, Value> params = innerParams(c, *plan);
                std::vector<GeometryPtr> parts = splitElements(*c.geometry("geometry"), c.prop("element"));
                std::vector<std::vector<Value>> results(parts.size());
                tbb::parallel_for(size_t(0), parts.size(), [&](size_t k) {
                    std::map<std::string, Value> p = params;
                    setIndex(p, *plan, "index", int64_t(k));
                    setIndex(p, *plan, "count", int64_t(parts.size()));
                    results[k] = runInner(c, *plan, parts[k], std::move(p));
                });
                std::vector<GeometryPtr> outs;
                outs.reserve(results.size());
                for (const std::vector<Value>& r : results)
                    if (!r.empty() && r[0].geometry)
                        outs.push_back(r[0].geometry);
                std::vector<Value> out = results.empty() || results.back().empty() ? std::vector<Value>(1) : results.back();
                out[0].geometry = makeGeometry(join(outs));
                writeOutputs(c, *plan, out);
            },
    });
}
} // namespace lila::gn::nodes::zones

namespace lila::gn::nodes
{
void registerZoneNodes()
{
    zones::repeatNode();
    zones::forEachNode();
}
} // namespace lila::gn::nodes
