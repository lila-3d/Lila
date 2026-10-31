#include "gn/Exec.h"
#include "gn/NodesCommon.h"
#include "gn/Zones.h"

#include "core/Profile.h"

namespace lila::gn::nodes::bundles
{
SocketDecl itemIn(const ZoneItem& i)
{
    if (i.type == T::String)
        return in(i.name, i.type, "");
    return isDataType(i.type) ? fin(i.name, i.type, zeroJson(i.type)) : in(i.name, i.type);
}

SocketDecl itemOut(const ZoneItem& i) { return isDataType(i.type) ? fout(i.name, i.type) : out(i.name, i.type); }

/** What reached an input, or its set value, or the type's default. */
Value inputValue(const ExecContext& c, std::string_view id)
{
    int k = c.sockets.inputIndex(id);
    if (k >= 0 && size_t(k) < c.inputs.size() && !c.inputs[size_t(k)].empty())
        return c.inputs[size_t(k)][0];
    return k >= 0 ? defaultValue(c.sockets.inputs[size_t(k)].type) : Value{};
}

void bundleNodes()
{
    registerDecl({
        .type = "CombineBundle",
        .label = "Combine Bundle",
        .category = "Utilities",
        .doc = "Packs values of any type into one bundle, each under its item's name, so several values travel along one link.",
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                for (const ZoneItem& i : itemsOf(n))
                    s.inputs.push_back(itemIn(i));
                s.outputs.push_back(out("bundle", T::Bundle));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                auto b = std::make_shared<Bundle>();
                for (const ZoneItem& i : itemsOf(c.node))
                    b->items.push_back({i.name, i.type, inputValue(c, i.name)});
                c.outputs[0].bundle = std::move(b);
            },
    });
    registerDecl({
        .type = "SeparateBundle",
        .label = "Separate Bundle",
        .category = "Utilities",
        .doc = "Unpacks a bundle's values by name. An item the bundle lacks reads as its type's default; one of another type is "
               "converted where it can be.",
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                s.inputs.push_back(in("bundle", T::Bundle));
                for (const ZoneItem& i : itemsOf(n))
                    s.outputs.push_back(itemOut(i));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::shared_ptr<const Bundle> b = inputValue(c, "bundle").bundle;
                for (const ZoneItem& i : itemsOf(c.node))
                {
                    int k = c.sockets.outputIndex(i.name);
                    if (k < 0)
                        continue;
                    const Bundle::Item* found = b ? b->find(i.name) : nullptr;
                    c.outputs[size_t(k)] = found && linkable(found->type, i.type) ? convertValue(found->value, found->type, i.type) : defaultValue(i.type);
                }
            },
    });
    registerDecl({
        .type = "JoinBundle",
        .label = "Join Bundle",
        .category = "Utilities",
        .doc = "Merges bundles into one. Where two hold the same name, the later link wins.",
        .sockets = {.inputs = {{.id = "bundle", .type = T::Bundle, .multi = true}}, .outputs = {out("bundle", T::Bundle)}},
        .exec =
            [](ExecContext& c) {
                auto merged = std::make_shared<Bundle>();
                for (const Value& v : c.inputs[0])
                    if (v.bundle)
                        for (const Bundle::Item& i : v.bundle->items)
                        {
                            auto it = std::find_if(merged->items.begin(), merged->items.end(), [&](const Bundle::Item& m) { return m.name == i.name; });
                            if (it != merged->items.end())
                                *it = i;
                            else
                                merged->items.push_back(i);
                        }
                c.outputs[0].bundle = std::move(merged);
            },
    });
}

const json* closureDefinition(const Group& g, const GNode& n)
{
    auto it = n.values.find("group");
    if (it == n.values.end() || !it->is_string())
        return nullptr;
    auto def = g.subgroups.find(it->get<std::string>());
    return def == g.subgroups.end() ? nullptr : &def->second;
}

bool isCapture(const std::string& name) { return name.starts_with("__capture"); }

void closureNodes()
{
    registerDecl({
        .type = "MakeClosure",
        .label = "Closure",
        .category = "Zones",
        .doc = "A closure zone after evaluation lowered it: makes the closure, capturing what its body reads from outside.",
        .dynamicSockets =
            [](const Group& g, const GNode& n) {
                Sockets s;
                if (const json* def = closureDefinition(g, n))
                    for (const json& i : def->value("inputs", json::array()))
                    {
                        SocketType t{};
                        std::string name = i.value("name", "");
                        if (isCapture(name) && parseSocketType(i.value("type", ""), &t))
                            s.inputs.push_back(isDataType(t) ? fin(name, t) : in(name, t));
                    }
                s.outputs.push_back(out("closure", T::Closure));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                const json* def = closureDefinition(c.group, c.node);
                if (!def)
                    throw ExecError(c.node.name + ": the closure zone has no body");
                auto cl = std::make_shared<Closure>();
                cl->plan = cachedPlan(*def);
                for (const GInput& i : cl->plan->group.inputs)
                    if (isCapture(i.name))
                        cl->captured[i.name] = inputValue(c, i.name);
                    else
                        cl->inputs.push_back({i.name, i.type});
                for (const GOutput& o : cl->plan->group.outputs)
                    cl->outputs.push_back({o.name, o.type});
                c.outputs[0].closure = std::move(cl);
            },
        .hidden = true,
    });
    registerDecl({
        .type = "EvaluateClosure",
        .label = "Evaluate Closure",
        .category = "Utilities",
        .doc = "Runs a closure with these inputs and gives back its outputs, matched by name. Linking a closure in fills the inputs and "
               "outputs from it when the node has none.",
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                s.inputs.push_back(in("closure", T::Closure));
                for (const ZoneItem& i : itemsOf(n, "inputs"))
                    s.inputs.push_back(itemIn(i));
                for (const ZoneItem& i : itemsOf(n, "outputs"))
                    s.outputs.push_back(itemOut(i));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::shared_ptr<const Closure> cl = inputValue(c, "closure").closure;
                std::vector<ZoneItem> outs = itemsOf(c.node, "outputs");
                if (!cl || !cl->plan)
                {
                    for (const ZoneItem& o : outs)
                        if (int k = c.sockets.outputIndex(o.name); k >= 0)
                            c.outputs[size_t(k)] = defaultValue(o.type);
                    return;
                }
                ExecEnv env;
                env.time = c.env.time;
                env.objects = c.env.objects;
                env.ownerMatrix = c.env.ownerMatrix;
                env.params = cl->captured;
                for (const auto& [name, type] : cl->inputs)
                {
                    int k = c.sockets.inputIndex(name);
                    if (k < 0)
                        continue;
                    SocketType given = c.sockets.inputs[size_t(k)].type;
                    if (linkable(given, type))
                        env.params[name] = convertValue(inputValue(c, name), given, type);
                }
                std::vector<Value> result;
                try
                {
                    result = executeOutputs(*cl->plan, env);
                }
                catch (const std::exception& e)
                {
                    throw ExecError(c.node.name + ": " + e.what());
                }
                for (const ZoneItem& o : outs)
                {
                    int k = c.sockets.outputIndex(o.name);
                    if (k < 0)
                        continue;
                    c.outputs[size_t(k)] = defaultValue(o.type);
                    for (size_t r = 0; r < cl->outputs.size() && r + 1 < result.size(); ++r)
                        if (cl->outputs[r].first == o.name && linkable(cl->outputs[r].second, o.type))
                            c.outputs[size_t(k)] = convertValue(result[r + 1], cl->outputs[r].second, o.type);
                }
            },
    });
}
} // namespace lila::gn::nodes::bundles

namespace lila::gn::nodes
{
void registerBundleNodes()
{
    bundles::bundleNodes();
    bundles::closureNodes();
}
} // namespace lila::gn::nodes
