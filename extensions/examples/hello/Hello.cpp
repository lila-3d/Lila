#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Node.h>
#include <lila/Relocate.h>

#include <cctype>
#include <memory>

using namespace lila;

namespace
{
/** output = input * factor. */
class HelloMultiply final : public scene::Node
{
public:
    enum Attr
    {
        Input,
        Factor,
        Output,
    };

    void compute(const scene::EvalContext&) override { setOutput(Output, as<double>(Input) * as<double>(Factor)); }
};
} // namespace

LILA_EXTENSION_EXPORT int lilaExtensionInit(ext::Context& ctx)
{
    CommandDef hello{
        .name = "helloExtension",
        .doc = "Say hello from a C++ extension.",
        .positionalDoc = "names",
        .flags = {{"loud", "", ArgType::Bool, false, "Shout."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string text = "Hello";
            for (const json& n : a.positional)
                if (n.is_string())
                    text += ", " + n.get<std::string>();
            if (a.get<bool>("loud"))
                for (char& c : text)
                    c = char(std::toupper((unsigned char)c));
            return text;
        },
    };
    if (!ctx.registerCommand(std::move(hello)))
        return 2;

    using namespace scene;
    NodeType type;
    type.name = "helloMultiply";
    type.doc = "Multiplies input by factor. An example of a node type registered from a C++ extension.";
    type.ownAttrs = {
        {.name = "input", .shortName = "i", .type = PlugType::Float, .defaultValue = 0.0, .doc = "Value to multiply."},
        {.name = "factor", .shortName = "f", .type = PlugType::Float, .defaultValue = 2.0, .doc = "Multiplier."},
        {.name = "output", .shortName = "o", .type = PlugType::Float, .defaultValue = 0.0, .flags = AttrOutput, .doc = "input times factor."},
    };
    type.create = [] { return std::make_unique<HelloMultiply>(); };
    lila::scene::relocate::registerNeutral(type.name);
    if (!types::registerType(ctx, std::move(type)))
        return 3;

    ctx.log(LogLevel::Info, "hello extension loaded");
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(ext::Context& ctx)
{
    // Lila removes what the extension registered; this is where an extension frees its own state.
    ctx.log(LogLevel::Info, "hello extension unloaded");
}
