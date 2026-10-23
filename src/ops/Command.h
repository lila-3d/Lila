#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lila
{
using json = nlohmann::json;

/** Thrown by a command to fail with a message for the user. */
struct CommandError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

enum class ArgType : uint8_t
{
    Bool,
    Int,
    Float,
    String,
    Vec3,
    List,
    Any,
};

struct FlagDef
{
    std::string name;
    std::string shortName;
    ArgType type = ArgType::Any;
    json defaultValue; // null = not passed unless given
    std::string doc;
    /** Allowed values of a String flag, for menus and combo boxes; empty means free text. */
    std::vector<std::string> choices;
};

/** Normalized call: positional args plus long-name flags, defaults filled in. */
struct LILA_FOUNDATION_API Args
{
    json positional = json::array();
    json flags = json::object();

    bool has(std::string_view flag) const { return flags.contains(flag) && !flags[std::string(flag)].is_null(); }

    template <class T> T get(std::string_view flag, const T& fallback = T{}) const
    {
        auto it = flags.find(flag);
        if (it == flags.end() || it->is_null())
            return fallback;
        return it->template get<T>();
    }

    /** Positional args, or the selection when none were given. */
    std::vector<std::string> objects() const;
};

enum class CallSource : uint8_t
{
    Script, // Python, MCP
    UI,     // menus, hotkeys, tools: echoed to the script editor
};

struct CommandDef
{
    std::string name;
    std::string doc;
    std::string positionalDoc; // empty = takes none
    std::vector<FlagDef> flags;
    bool undoable = true;
    std::function<json(const Args&)> run;
    std::string owner; // extension that registered it; empty = built in
    /** Re-running it with different flags after undoing it is safe, so the UI may offer to adjust the last call live. */
    bool adjustable = false;
};

/** One top-level execute, reported before the command runs and again after it finished or failed. */
struct ExecuteEvent
{
    std::string_view name;
    const json& call;
    CallSource source;
    bool done = false; // false: about to run
    bool ok = false;   // with done: it returned without throwing
};

namespace ops
{
/** Every command, owner-tagged; registerCommand and friends go through it. */
LILA_FOUNDATION_API Registry<CommandDef>& registry();
LILA_FOUNDATION_API void registerCommand(CommandDef def);
LILA_FOUNDATION_API void unregisterCommand(std::string_view name);
LILA_FOUNDATION_API const CommandDef* find(std::string_view name);
LILA_FOUNDATION_API std::vector<const CommandDef*> all();

/** `call` is {"args": [...], flag: value, ...}, flags by long or short name. Main thread only. Throws CommandError. */
LILA_FOUNDATION_API json execute(std::string_view name, const json& call, CallSource source = CallSource::Script);

/** Marks an already registered command adjustable (see CommandDef::adjustable). False if it isn't registered. */
LILA_FOUNDATION_API bool setAdjustable(std::string_view name, bool on = true);

/** Called around top-level executes only; commands run by other commands aren't reported. Returns an id. */
LILA_FOUNDATION_API int addExecuteListener(std::function<void(const ExecuteEvent&)> fn);
LILA_FOUNDATION_API void removeExecuteListener(int id);

/** Python source equivalent of a call, for echo. */
LILA_FOUNDATION_API std::string toPython(std::string_view name, const json& call);

/** Selection provider for Args::objects(); installed by the scene module. */
LILA_FOUNDATION_API void setSelectionProvider(std::function<std::vector<std::string>()> fn);

LILA_FOUNDATION_API void registerBuiltins();
} // namespace ops
} // namespace lila
