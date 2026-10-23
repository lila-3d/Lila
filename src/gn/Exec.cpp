#include "gn/Exec.h"

#include "core/Profile.h"

#include <functional>
#include <map>
#include <mutex>

namespace lila::gn
{
PlanPtr compile(const Group& g)
{
    LILA_ZONE();

    registerNodeLibrary();
    auto plan = std::make_shared<Plan>();
    plan->group = g;
    const Group& grp = plan->group;

    std::map<std::string, size_t, std::less<>> byName;
    for (size_t i = 0; i < grp.nodes.size(); ++i)
        byName[grp.nodes[i].name] = i;

    const GNode* outNode = grp.firstOfType("GroupOutput");
    if (!outNode)
    {
        plan->error = "the group has no Group Output node";
        return plan;
    }

    std::vector<int> stepOf(grp.nodes.size(), -1);
    std::vector<uint8_t> state(grp.nodes.size(), 0); // 1 visiting, 2 done
    std::vector<Sockets> sockets(grp.nodes.size());
    std::vector<const NodeDecl*> decls(grp.nodes.size(), nullptr);
    for (size_t i = 0; i < grp.nodes.size(); ++i)
    {
        decls[i] = findDecl(grp.nodes[i].type);
        if (decls[i])
            sockets[i] = decls[i]->socketsFor(grp, grp.nodes[i]);
    }

    std::function<bool(size_t)> visit = [&](size_t n) -> bool {
        if (state[n] == 2)
            return true;
        if (state[n] == 1)
        {
            plan->error = "the group has a cycle through " + grp.nodes[n].name;
            return false;
        }
        if (!decls[n])
        {
            plan->error = grp.nodes[n].name + ": unknown node type " + grp.nodes[n].type;
            return false;
        }
        state[n] = 1;
        Plan::Step step;
        step.node = n;
        step.decl = decls[n];
        step.sockets = sockets[n];
        step.inputs.resize(step.sockets.inputs.size());
        for (size_t k = 0; k < step.sockets.inputs.size(); ++k)
            for (const GLink& l : grp.links)
            {
                if (l.toNode != grp.nodes[n].name || l.toSocket != step.sockets.inputs[k].id)
                    continue;
                auto it = byName.find(l.fromNode);
                if (it == byName.end() || !decls[it->second])
                    continue;
                int out = sockets[it->second].outputIndex(l.fromSocket);
                if (out < 0)
                    continue;
                if (!visit(it->second))
                    return false;
                step.inputs[k].push_back({stepOf[it->second], out, sockets[it->second].outputs[out].type});
                if (!step.sockets.inputs[k].multi)
                    break;
            }
        stepOf[n] = int(plan->steps.size());
        plan->steps.push_back(std::move(step));
        state[n] = 2;
        return true;
    };
    plan->usesTime = usesSceneTime(grp);
    size_t outIndex = size_t(outNode - grp.nodes.data());
    if (!visit(outIndex))
    {
        plan->steps.clear();
        return plan;
    }
    plan->outputStep = stepOf[outIndex];
    if (const GNode* viewer = activeViewer(grp))
    {
        std::string error = plan->error;
        size_t vi = size_t(viewer - grp.nodes.data());
        // A broken viewer branch only loses the preview, never the result.
        if (visit(vi))
            plan->viewerStep = stepOf[vi];
        plan->error = error;
    }
    return plan;
}

const GNode* activeViewer(const Group& g)
{
    for (const GNode& n : g.nodes)
        if (n.type == "Viewer")
            if (auto it = n.values.find("active"); it != n.values.end() && it->is_boolean() && it->get<bool>())
                return &n;
    return nullptr;
}

PlanPtr cachedPlan(const json& definition)
{
    static std::mutex mutex;
    static std::map<std::string, PlanPtr> cache;
    std::string key = definition.dump();
    {
        std::lock_guard lock(mutex);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    PlanPtr plan = compile(Group::fromJson(definition));
    std::lock_guard lock(mutex);
    if (cache.size() > 128)
        cache.clear();
    cache[key] = plan;
    return plan;
}

std::vector<Value> executeOutputs(const Plan& plan, const ExecEnv& env)
{
    LILA_ZONE();

    if (!plan.error.empty() || plan.outputStep < 0)
        throw ExecError(plan.error.empty() ? "nothing to evaluate" : plan.error);

    std::vector<std::vector<Value>> results(plan.steps.size());
    std::vector<Value> outputs;
    for (size_t s = 0; s < plan.steps.size(); ++s)
    {
        const Plan::Step& step = plan.steps[s];
        const GNode& node = plan.group.nodes[step.node];
        ExecContext ctx(plan.group, node, *step.decl, step.sockets, env);
        ctx.inputs.resize(step.sockets.inputs.size());
        ctx.outputs.resize(step.sockets.outputs.size());
        for (size_t k = 0; k < step.sockets.inputs.size(); ++k)
        {
            const SocketDecl& sd = step.sockets.inputs[k];
            for (const Plan::Source& src : step.inputs[k])
                ctx.inputs[k].push_back(convertValue(results[src.step][src.output], src.type, sd.type));
            if (!ctx.inputs[k].empty() || sd.multi)
                continue;
            auto it = node.values.find(sd.id);
            if (it == node.values.end() && sd.implicit != Implicit::None)
            {
                Value v;
                switch (sd.implicit)
                {
                case Implicit::Position: v.field = positionField(); break;
                case Implicit::Normal: v.field = normalField(); break;
                case Implicit::Index: v.field = indexField(); break;
                default: v.field = idField(); break;
                }
                v.field = convertField(v.field, sd.type);
                ctx.inputs[k].push_back(std::move(v));
                continue;
            }
            ctx.inputs[k].push_back(valueFromJson(sd.type, it != node.values.end() ? *it : sd.defaultValue));
        }
        for (size_t k = 0; k < step.sockets.outputs.size(); ++k)
            ctx.outputs[k] = defaultValue(step.sockets.outputs[k].type);
        bool isViewer = int(s) == plan.viewerStep;
        try
        {
            if (step.decl->exec)
                step.decl->exec(ctx);
        }
        catch (const std::exception& e)
        {
            if (isViewer)
                continue;
            if (dynamic_cast<const ExecError*>(&e))
                throw;
            throw ExecError(node.name + ": " + e.what());
        }
        if (int(s) == plan.outputStep)
            for (size_t k = 0; k < ctx.inputs.size(); ++k)
                outputs.push_back(ctx.inputs[k].empty() ? defaultValue(step.sockets.inputs[k].type) : ctx.inputs[k][0]);
        results[s] = std::move(ctx.outputs);
    }
    return outputs;
}

GeometryPtr execute(const Plan& plan, const ExecEnv& env)
{
    std::vector<Value> out = executeOutputs(plan, env);
    if (!out.empty() && out[0].geometry)
        return out[0].geometry;
    return makeGeometry({});
}
} // namespace lila::gn
