#include "core/CrashHandler.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "stream/ChangeStream.h"

#include <cstdlib>
#include <optional>
#include <random>
#include <utility>
#include <tuple>

namespace lila::stream
{
namespace
{
json status()
{
    return {{"running", serverRunning()},
            {"port", serverRunning() ? serverPort() : PrefStore::get().value<int>("general", "/changeStream/port", kDefaultPort)},
            {"localOnly", serverRunning() ? serverLocalOnly() : PrefStore::get().value<bool>("general", "/changeStream/localOnly", true)},
            {"requireToken", serverRequiresToken()},
            {"clients", clientCount()},
            {"seq", sequence()},
            {"token", serverRequiresToken() ? json(tokenFromPrefs()) : json(nullptr)}};
}

using PrefState = std::tuple<bool, int, bool>;

PrefState prefState()
{
    const PrefStore& p = PrefStore::get();
    return {p.value<bool>("general", "/changeStream/enabled", false), p.value<int>("general", "/changeStream/port", kDefaultPort),
            p.value<bool>("general", "/changeStream/localOnly", true)};
}

/** Starts or stops the server to match the prefs. Only a change to them acts, so a session's changeStream call stays. */
void applyPrefs(std::optional<PrefState>& last)
{
    const PrefState now = prefState();
    if (last && *last == now)
        return;
    last = now;
    const auto [enabled, port, localOnly] = now;
    if (!enabled)
    {
        stopServer();
        return;
    }
    std::string err;
    if (!startServer(port, localOnly, &err))
        log::error("change stream: {}", err);
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "changeStream",
        .doc = "Serve scene changes to other processes as JSON lines over TCP: one message per command or undo step, "
               "plus resync, scene events and snapshots on request. Returns the server's state.\n\n"
               "Without flags it only reports; port or allowRemote alone start or restart it. The general prefs "
               "/changeStream/enabled, /port and /localOnly start it with Lila; this command changes the running "
               "session only.",
        .flags = {
            {"enable", "e", T::Bool, nullptr, "Start (True) or stop (False) the server."},
            {"port", "p", T::Int, nullptr, "Port to listen on; 0 picks a free one. Default: the /changeStream/port pref, 8742."},
            {"allowRemote", "ar", T::Bool, nullptr,
             "Listen on every network interface rather than 127.0.0.1 only. Clients must then send the token this "
             "returns (general.json /changeStream/token). Default False."},
            {"requireToken", "rt", T::Bool, nullptr,
             "Make clients send the token even on 127.0.0.1, as allowRemote does: for machines other users share. "
             "Default False."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            const bool enable = a.has("enable") ? a.get<bool>("enable") : a.has("port") || a.has("allowRemote") || a.has("requireToken");
            if (a.has("enable") && !enable)
            {
                stopServer();
                return status();
            }
            if (!enable)
                return status();
            const PrefStore& p = PrefStore::get();
            const int port = a.get<int>("port", serverRunning() ? serverPort() : p.value<int>("general", "/changeStream/port", kDefaultPort));
            const bool localOnly = a.has("allowRemote") ? !a.get<bool>("allowRemote")
                                   : serverRunning()     ? serverLocalOnly()
                                                         : p.value<bool>("general", "/changeStream/localOnly", true);
            // allowRemote implies the token; both go through the same check in the server.
            const bool requireToken = a.has("requireToken") ? a.get<bool>("requireToken") : serverRunning() && localOnly && serverRequiresToken();
            std::string err;
            if (!startServer(port, localOnly, &err, requireToken))
                throw CommandError("changeStream: " + err);
            return status();
        },
        .examples = {
            "changeStream(enable=True)",
            "changeStream(enable=True, port=0)[\"port\"]",
            "changeStream(enable=True, requireToken=True)[\"token\"]",
            "changeStream(enable=False)",
        },
    });
}

std::string tokenFromPrefs()
{
    PrefStore& prefs = PrefStore::get();
    std::string token = prefs.value<std::string>("general", "/changeStream/token", "");
    if (token.empty())
    {
        std::random_device rd;
        std::uniform_int_distribution<int> d(0, 15);
        for (int i = 0; i < 40; ++i)
            token += "0123456789abcdef"[d(rd)];
        prefs.set("general", "/changeStream/token", token);
    }
    // Kept out of crash reports, once per token.
    static std::string hidden;
    if (hidden != token)
        crash::addSecret(hidden = token);
    return token;
}

void startFromPrefs()
{
    static std::optional<PrefState> last;
    applyPrefs(last);
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == "general")
            applyPrefs(last);
    });
    events::subscribe(events::Event::Shutdown, [](const events::EventData&) { stopServer(); });
}

bool startForBatch(int cliPort, std::string* err)
{
    int port = cliPort;
    if (port < 0)
        if (const char* env = std::getenv("LILA_CHANGE_STREAM"); env && *env)
        {
            char* end = nullptr;
            const long v = std::strtol(env, &end, 10);
            if (*end || v < 0 || v > 65535)
            {
                if (err)
                    *err = "LILA_CHANGE_STREAM must be a port number, or 0 for any free one";
                return false;
            }
            port = int(v);
        }
    if (port < 0)
        return true;
    if (!startServer(port, true, err))
        return false;
    static bool stopsOnShutdown = false;
    if (!std::exchange(stopsOnShutdown, true))
        events::subscribe(events::Event::Shutdown, [](const events::EventData&) { stopServer(); });
    return true;
}
} // namespace lila::stream
