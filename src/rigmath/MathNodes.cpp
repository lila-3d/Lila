#include "rigmath/MathNodes.h"
#include "rigmath/Expressions.h"

#include "rigmath/Builder.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Relocate.h"

#include <map>
#include <mutex>
#include <stdexcept>

namespace lila::rigmath
{
thread_local scene::RotateOrder tRotateOrder = scene::RotateOrder::XYZ;

namespace
{
using json = nlohmann::json;
using scene::AttrDef;
using scene::PlugType;

const char* kKindNames[] = {"Bool", "Int", "Float", "Vector", "Rotator", "Quat", "Matrix", "Color", "Choice", "Curve"};

struct Built
{
    std::vector<Function> fns;
    std::map<std::string, std::pair<const Function*, const Overload*>, std::less<>> byType;
};

Built build()
{
    Library lib;
    addScalar(lib);
    addVector(lib);
    addRotation(lib);
    addMatrix(lib);
    addMisc(lib);
    addRamps(lib);
    Built b;
    b.fns.assign(std::make_move_iterator(lib.fns.begin()), std::make_move_iterator(lib.fns.end()));
    for (Function& f : b.fns)
        for (Overload& o : f.overloads)
        {
            std::string suffix = o.suffix;
            if (suffix.empty())
                suffix = kindName(!o.inputs.empty() ? o.inputs[0].kind : o.outputs[0].kind);
            o.nodeType = f.overloads.size() == 1 ? f.name : f.name + suffix;
        }
    for (const Function& f : b.fns)
        for (const Overload& o : f.overloads)
            if (!b.byType.emplace(o.nodeType, std::pair{&f, &o}).second)
                throw std::logic_error("rigmath: two overloads make node type " + o.nodeType);
    return b;
}

const Built& built()
{
    static const Built b = build();
    return b;
}

bool usesRotator(const Overload& o)
{
    for (const auto* pins : {&o.inputs, &o.outputs})
        for (const Pin& p : *pins)
            if (p.kind == Kind::Rotator)
                return true;
    return false;
}

const char* iconFor(const std::string& family)
{
    static const std::map<std::string, const char*, std::less<>> icons = {
        {"arithmetic", "math-symbols"}, {"comparison", "equal"},   {"range", "math"},        {"interpolation", "chart-line"},
        {"easing", "chart-line"},       {"trig", "wave-sine"},     {"exponential", "math-function"}, {"vector", "vector"},
        {"rotator", "rotate-3d"},       {"quat", "rotate-3d"},     {"matrix", "matrix"},     {"color", "palette"},
        {"select", "arrows-split"},     {"bitwise", "binary"},     {"noise", "dice"},
    };
    auto it = icons.find(family);
    return it == icons.end() ? "math-function" : it->second;
}

AttrDef attrFor(const Pin& p, bool output)
{
    AttrDef a;
    a.name = p.name;
    a.type = plugTypeOf(p.kind);
    a.defaultValue = p.defaultValue;
    const bool keyable = a.type == PlugType::Float || a.type == PlugType::Int || a.type == PlugType::Bool || a.type == PlugType::Vec3;
    a.flags = output ? scene::AttrOutput : (scene::AttrInput | scene::AttrStorable | (keyable && !p.array ? scene::AttrKeyable : 0));
    if (p.array)
        a.flags |= scene::AttrArray;
    if (p.kind == Kind::Color)
        a.flags |= scene::AttrColor;
    if (p.kind == Kind::Choice)
        a.enumNames = p.choices;
    if (a.type == PlugType::Vec3)
        for (int k = 0; k < 3; ++k)
            a.childNames[size_t(k)] = p.name + (p.kind == Kind::Color ? "RGB"[k] : "XYZ"[k]);
    if (p.kind == Kind::Curve)
        a.doc = "Keys as anim curve JSON, or [[in, out, \"tangent\"], ...]; a vector or colour ramp takes three lists, or one for all.";
    else if (p.kind == Kind::Quat)
        a.doc = "A quat, as a rotation matrix.";
    else if (p.kind == Kind::Rotator)
        a.doc = "Euler degrees in rotateOrder.";
    return a;
}

/** One overload of a math function: pure, recomputed from its inputs. */
class MathNode final : public scene::Node
{
public:
    const Overload* overload = nullptr;
    int firstOut = 0;
    int orderAttr = -1;

    void compute(const scene::EvalContext&) override
    {
        LILA_ZONE();
        tRotateOrder = orderAttr >= 0 ? scene::RotateOrder(std::clamp<int64_t>(as<int64_t>(orderAttr), 0, 5)) : scene::RotateOrder::XYZ;
        std::vector<scene::PlugValue> out;
        try
        {
            out = overload->run(*this);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
        }
        for (size_t k = 0; k < overload->outputs.size(); ++k)
            setOutput(firstOut + int(k), k < out.size() ? std::move(out[k]) : overload->outputs[k].defaultValue);
    }
};

json pinJson(const Pin& p)
{
    json j = {{"name", p.name}, {"kind", kindName(p.kind)}, {"default", scene::toJson(p.defaultValue)}};
    if (p.array)
        j["array"] = true;
    if (!p.choices.empty())
        j["choices"] = p.choices;
    return j;
}

json functionJson(const Function& f)
{
    json overloads = json::array();
    for (const Overload& o : f.overloads)
    {
        json ins = json::array(), outs = json::array();
        for (const Pin& p : o.inputs)
            ins.push_back(pinJson(p));
        for (const Pin& p : o.outputs)
            outs.push_back(pinJson(p));
        overloads.push_back({{"nodeType", o.nodeType}, {"inputs", ins}, {"outputs", outs}, {"rotateOrder", usesRotator(o)}});
    }
    return {{"name", f.name}, {"family", f.family}, {"doc", f.doc}, {"overloads", overloads}};
}
} // namespace

const char* kindName(Kind k) { return kKindNames[size_t(k)]; }

scene::PlugType plugTypeOf(Kind k)
{
    switch (k)
    {
    case Kind::Bool: return PlugType::Bool;
    case Kind::Int: return PlugType::Int;
    case Kind::Float: return PlugType::Float;
    case Kind::Vector:
    case Kind::Rotator:
    case Kind::Color: return PlugType::Vec3;
    case Kind::Quat:
    case Kind::Matrix: return PlugType::Matrix;
    case Kind::Choice: return PlugType::Enum;
    case Kind::Curve: return PlugType::String;
    }
    return PlugType::Float;
}

const std::vector<Function>& functions() { return built().fns; }

const Overload* overloadFor(std::string_view nodeType)
{
    auto it = built().byType.find(nodeType);
    return it == built().byType.end() ? nullptr : it->second.second;
}

const Function* functionFor(std::string_view nodeType)
{
    auto it = built().byType.find(nodeType);
    return it == built().byType.end() ? nullptr : it->second.first;
}

void registerTypes()
{
    scene::expressions::registerTypes();
    for (const Function& f : functions())
        for (const Overload& o : f.overloads)
        {
            if (scene::types::find(o.nodeType))
                continue;
            scene::NodeType t;
            t.name = o.nodeType;
            t.icon = iconFor(f.family);
            t.doc = f.doc;
            for (const Pin& p : o.inputs)
                t.ownAttrs.push_back(attrFor(p, false));
            int orderAttr = -1;
            if (usesRotator(o))
            {
                orderAttr = int(t.ownAttrs.size());
                t.ownAttrs.push_back({.name = "rotateOrder", .type = PlugType::Enum, .defaultValue = int64_t(0),
                                      .flags = scene::AttrInput | scene::AttrStorable, .enumNames = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"},
                                      .doc = "The order the rotators' Euler angles apply in."});
            }
            const int firstOut = int(t.ownAttrs.size());
            for (const Pin& p : o.outputs)
                t.ownAttrs.push_back(attrFor(p, true));
            const Overload* ov = &o;
            t.create = [ov, firstOut, orderAttr] {
                auto n = std::make_unique<MathNode>();
                n->overload = ov;
                n->firstOut = firstOut;
                n->orderAttr = orderAttr;
                return n;
            };
            scene::types::registerType(std::move(t));
            scene::relocate::registerNeutral(o.nodeType);
        }
}

void registerCommands()
{
    scene::expressions::registerCommands();
    using T = ArgType;
    ops::registerCommand({
        .name = "mathNodes",
        .doc = "The rig math node library: each function with its family, doc and overloads, every overload's node type and its typed "
               "inputs and outputs. Filter by function name, node type or family.",
        .flags = {{"name", "n", T::String, nullptr, "A function name (\"lerp\") or node type (\"addVector\")."},
                  {"family", "f", T::String, nullptr, "Only this family: arithmetic, comparison, range, interpolation, easing, trig, exponential, "
                                                      "vector, rotator, quat, matrix, color, select, bitwise or noise."},
                  {"namesOnly", "no", T::Bool, false, "Return just the node type names."}},
        .undoable = false,
        .run =
            [](const Args& a) {
                const std::string name = a.get<std::string>("name"), family = a.get<std::string>("family");
                const bool namesOnly = a.get<bool>("namesOnly", false);
                json out = json::array();
                for (const Function& f : functions())
                {
                    if (!family.empty() && f.family != family)
                        continue;
                    if (!name.empty() && f.name != name && functionFor(name) != &f)
                        continue;
                    if (namesOnly)
                    {
                        for (const Overload& o : f.overloads)
                            out.push_back(o.nodeType);
                    }
                    else
                        out.push_back(functionJson(f));
                }
                if (!name.empty() && out.empty())
                    throw CommandError("no math function or node type named " + name);
                return out;
            },
        .examples = {"mathNodes(family='vector', namesOnly=True)", "n = createNode('mapRangeClamped')\nsetAttr(n + '.value', 0.25)"},
    });
}
} // namespace lila::rigmath
