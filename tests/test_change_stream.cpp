#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/Events.h"
#include "core/MainQueue.h"
#include "geo/Mesh.h"
#include "link/Socket.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "stream/ChangeStream.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

using namespace lila;
using namespace lila::scene;
using namespace testutil;
namespace fs = std::filesystem;

namespace
{
/** A wire client: one JSON object per line, reading while it drains the main queue as Lila's event loop would. */
struct Client
{
    link::TcpSocket sock;
    std::string buf;

    explicit Client(int port) { sock = link::TcpSocket::connect("127.0.0.1", uint16_t(port), 2000); }

    void send(const json& j) { REQUIRE(sock.sendAll(j.dump() + "\n")); }

    /** The next message, or null after timeoutMs. */
    json next(int timeoutMs = 3000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true)
        {
            if (size_t eol = buf.find('\n'); eol != std::string::npos)
            {
                json j = json::parse(buf.substr(0, eol));
                buf.erase(0, eol + 1);
                return j;
            }
            if (std::chrono::steady_clock::now() > deadline)
                return nullptr;
            mainq::drain();
            char tmp[65536];
            const int n = sock.recv(tmp, int(sizeof(tmp)), 5);
            if (n < 0)
                return nullptr;
            buf.append(tmp, size_t(n));
        }
    }

    /** Skips to the next message of this type. */
    json nextOf(const std::string& type, int timeoutMs = 3000)
    {
        for (json j = next(timeoutMs); !j.is_null(); j = next(timeoutMs))
            if (j["type"] == type)
                return j;
        return nullptr;
    }
};

/** The server on a free localhost port for one test, stopped after it. */
struct Serving
{
    int port = 0;
    Serving()
    {
        stream::registerCommands();
        run("newScene");
        port = run("changeStream", {{"enable", true}, {"port", 0}})["port"].get<int>();
    }
    ~Serving() { run("changeStream", {{"enable", false}}); }
};

/** A connected client past its hello. */
Client connected(const Serving& s)
{
    Client c(s.port);
    REQUIRE(c.sock.valid());
    json hello = c.next();
    REQUIRE(hello["type"] == "hello");
    CHECK(hello["protocol"] == 1);
    // The accept loop registers the client a moment after the hello goes out; a ping round trip means it is in.
    c.send({{"op", "ping"}});
    REQUIRE(c.nextOf("pong")["type"] == "pong");
    return c;
}

json changesOf(const json& group, const std::string& kind)
{
    json out = json::array();
    for (const json& c : group["changes"])
        if (c["kind"] == kind)
            out.push_back(c);
    return out;
}

void registerTestCommand()
{
    ops::registerCommand({
        .name = "streamTestEdits",
        .doc = "Sets A.translate three times and A.rotate once.",
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            for (double x : {1.0, 2.0, 3.0})
                g.setValue(g.plug("A.translate"), glm::dvec3(x, 0, 0));
            g.setValue(g.plug("A.rx"), 45.0);
            return nullptr;
        },
    });
}
} // namespace

TEST_CASE("change stream: each command arrives as one group, with repeated sets on a plug coalesced")
{
    registerTestCommand();
    Serving s;
    Client c = connected(s);

    run("spaceLocator", {{"name", "A"}});
    json created = c.nextOf("group");
    REQUIRE(created.is_object());
    CHECK(created["command"] == "spaceLocator");
    CHECK(created["source"] == "script");
    CHECK(created["ok"] == true);
    json made = changesOf(created, "created");
    REQUIRE(made.size() >= 1);
    CHECK(made[0]["path"] == "|A");
    CHECK(made[0]["type"] == Graph::current().find("A")->type().name);
    CHECK(made[0]["attrs"]["translate"] == json::array({0.0, 0.0, 0.0}));

    run("streamTestEdits");
    json g = c.nextOf("group");
    CHECK(g["command"] == "streamTestEdits");
    CHECK(g["seq"].get<uint64_t>() > created["seq"].get<uint64_t>());
    json values = changesOf(g, "value");
    REQUIRE(values.size() == 2);
    CHECK(values[0]["path"] == "|A");
    CHECK(values[0]["attr"] == "translate");
    CHECK(values[0]["value"] == json::array({3.0, 0.0, 0.0}));
    CHECK(values[1]["attr"] == "rotate");

    // A command that changes nothing sends nothing.
    CHECK_THROWS(run("setAttr", {{"args", {"A.nope", 1}}}));
    run("setAttr", {{"args", {"A.ty", 7}}});
    json set = c.nextOf("group");
    CHECK(set["command"] == "setAttr");
    CHECK(changesOf(set, "value")[0]["value"] == json::array({3.0, 7.0, 0.0}));
}

TEST_CASE("change stream: renames, reparents and deletes carry old and new paths")
{
    Serving s;
    Client c = connected(s);

    run("spaceLocator", {{"name", "Arm"}});
    run("createNode", {{"args", {"transform"}}, {"name", "Rig"}});
    c.nextOf("group");
    c.nextOf("group");

    run("parent", {{"args", {"Arm", "Rig"}}});
    json moved = changesOf(c.nextOf("group"), "reparented");
    REQUIRE(moved.size() == 1);
    CHECK(moved[0]["path"] == "|Rig|Arm");
    CHECK(moved[0]["oldPath"] == "|Arm");
    CHECK(moved[0]["parent"] == "|Rig");

    run("rename", {{"args", {"Rig", "Body"}}});
    json renamed = changesOf(c.nextOf("group"), "renamed");
    REQUIRE(renamed.size() == 1);
    CHECK(renamed[0]["path"] == "|Body");
    CHECK(renamed[0]["oldPath"] == "|Rig");

    const std::string armType = Graph::current().find("Arm")->type().name;
    run("delete", {{"args", {"Arm"}}});
    json deleted = changesOf(c.nextOf("group"), "deleted");
    REQUIRE(deleted.size() == 1);
    // Recorded after the rename moved it under |Body.
    CHECK(deleted[0]["path"] == "|Body|Arm");
    CHECK(deleted[0]["type"] == armType);
}

TEST_CASE("change stream: filters by node type, attribute and subtree, per client")
{
    Serving s;
    Client all = connected(s);
    Client some = connected(s);
    CHECK(stream::clientCount() == 2);

    run("createNode", {{"args", {"transform"}}, {"name", "Grp"}});
    run("spaceLocator", {{"name", "In"}});
    run("parent", {{"args", {"In", "Grp"}}});
    run("spaceLocator", {{"name", "Out"}});
    for (int i = 0; i < 4; ++i)
        all.nextOf("group");
    some.nextOf("group"); // they hear everything until they subscribe
    const std::string locator = Graph::current().find("Out")->type().name;

    some.send({{"op", "subscribe"}, {"types", json::array({locator})}, {"attrs", json::array({"translate"})}, {"under", json::array({"Grp"})}});
    json ack = some.nextOf("subscribed");
    REQUIRE(ack.is_object());
    CHECK(ack["filter"]["under"] == json::array({"|Grp"}));

    run("setAttr", {{"args", {"Out.tx", 1}}});  // outside |Grp
    run("setAttr", {{"args", {"In.sx", 2}}});   // not translate
    run("setAttr", {{"args", {"Grp.tx", 3}}});  // not a locator
    run("setAttr", {{"args", {"In.tx", 4}}});   // matches
    for (int i = 0; i < 4; ++i)
        CHECK(all.nextOf("group")["command"] == "setAttr");

    json g = some.nextOf("group");
    REQUIRE(g.is_object());
    REQUIRE(g["changes"].size() == 1);
    CHECK(g["changes"][0]["path"] == "|Grp|In");
    CHECK(g["changes"][0]["value"][0] == 4.0);

    some.send({{"op", "subscribe"}, {"types", "nope"}, {"attrs", 3}});
    CHECK(some.nextOf("error")["op"] == "subscribe");
    some.send({{"op", "frobnicate"}});
    CHECK(some.nextOf("error").is_object());
}

TEST_CASE("change stream: a snapshot gives the whole scene as of a seq, and deltas follow it")
{
    Serving s;
    Client c = connected(s);

    run("createNode", {{"args", {"transform"}}, {"name", "Root"}});
    run("spaceLocator", {{"name", "Kid"}});
    run("parent", {{"args", {"Kid", "Root"}}});
    run("setAttr", {{"args", {"Kid.translate", json::array({1, 2, 3})}}});
    run("connectAttr", {{"args", {"Root.tx", "Kid.ty"}}});
    uint64_t last = 0;
    for (int i = 0; i < 5; ++i)
        last = c.nextOf("group")["seq"].get<uint64_t>();

    c.send({{"op", "snapshot"}});
    json snap = c.nextOf("snapshot");
    REQUIRE(snap.is_object());
    CHECK(snap["seq"] == last);
    CHECK(snap["selection"] == json::array({"|Root|Kid"}));
    REQUIRE(snap["nodes"].size() == 2);
    CHECK(snap["nodes"][0]["path"] == "|Root");
    CHECK(snap["nodes"][0]["parent"].is_null());
    CHECK(snap["nodes"][1]["path"] == "|Root|Kid");
    CHECK(snap["nodes"][1]["parent"] == "|Root");
    CHECK(snap["nodes"][1]["attrs"]["translate"] == json::array({1.0, 2.0, 3.0}));
    REQUIRE(snap["connections"].size() == 1);
    CHECK(snap["connections"][0]["source"] == "|Root.translateX");
    CHECK(snap["connections"][0]["destination"] == "|Root|Kid.translateY");

    run("setAttr", {{"args", {"Kid.tz", 9}}});
    CHECK(c.nextOf("group")["seq"] == last + 1);

    // The same picture in process, filtered.
    json kids = stream::snapshot(stream::Filter::fromJson({{"under", json::array({"|Root|Kid"})}, {"attrs", json::array({"translate"})}}));
    REQUIRE(kids["nodes"].size() == 1);
    CHECK(kids["nodes"][0]["attrs"].size() == 1);
}

TEST_CASE("change stream: new, open and batched edits ask clients to resync")
{
    Serving s;
    Client c = connected(s);

    run("spaceLocator", {{"name", "A"}});
    c.nextOf("group");
    const fs::path dir = fs::temp_directory_path() / "lila_tests" / "change_stream";
    fs::create_directories(dir);
    const fs::path file = dir / "resync.lilaa";
    run("saveScene", {{"args", {file.generic_string()}}});
    json saved = c.nextOf("event");
    REQUIRE(saved.is_object());
    CHECK(saved["event"] == "sceneSaved");

    run("newScene");
    json fresh = c.nextOf("resync");
    REQUIRE(fresh.is_object());
    CHECK(fresh["reason"] == "new");
    CHECK(fresh["command"] == "newScene");

    run("openScene", {{"args", {file.generic_string()}}, {"force", true}});
    json opened = c.nextOf("resync");
    REQUIRE(opened.is_object());
    CHECK(opened["reason"] == "open");
    CHECK(fs::path(opened["path"].get<std::string>()).filename() == "resync.lilaa");

    {
        Graph::Batch batch;
        Graph& g = Graph::current();
        g.setValue(g.plug("A.tx"), 5.0);
    }
    stream::flush();
    json reset = c.nextOf("resync");
    REQUIRE(reset.is_object());
    CHECK(reset["reason"] == "reset");
    c.send({{"op", "snapshot"}});
    CHECK(c.nextOf("snapshot")["nodes"][0]["attrs"]["translate"][0] == 5.0);
}

TEST_CASE("change stream: undo, redo, time and edits outside commands")
{
    Serving s;
    Client c = connected(s);

    run("spaceLocator", {{"name", "A"}});
    run("setAttr", {{"args", {"A.tx", 4}}});
    c.nextOf("group");
    c.nextOf("group");

    run("undo");
    json undone = c.nextOf("group");
    CHECK(undone["command"] == "undo");
    CHECK(undone["undo"] == "setAttr");
    CHECK(changesOf(undone, "value")[0]["value"][0] == 0.0);
    run("redo");
    json redone = c.nextOf("group");
    CHECK(redone["redo"] == "setAttr");
    CHECK(changesOf(redone, "value")[0]["value"][0] == 4.0);

    run("currentTime", {{"args", {2.5}}});
    json time = changesOf(c.nextOf("group"), "time");
    REQUIRE(time.size() == 1);
    CHECK(time[0]["time"] == 2.5);

    // A drag: one undo step built up outside any command goes out once its chunk closes.
    Graph& g = Graph::current();
    {
        UndoChunk drag("Move Tool");
        for (double x : {5.0, 6.0, 7.0})
        {
            g.setValue(g.plug("A.tx"), x);
            mainq::drain();
        }
    }
    json dragged = c.nextOf("group");
    REQUIRE(dragged.is_object());
    CHECK(dragged["command"] == "Move Tool");
    CHECK(dragged["source"] == "interactive");
    REQUIRE(changesOf(dragged, "value").size() == 1);
    CHECK(changesOf(dragged, "value")[0]["value"][0] == 7.0);
}

TEST_CASE("change stream: meshes go as a summary unless a client asks for full values")
{
    Serving s;
    Client brief = connected(s);
    Client full = connected(s);
    full.send({{"op", "subscribe"}, {"full", true}});
    full.nextOf("subscribed");

    json quad = {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "Quad"}};
    run("createMesh", quad);
    json g = brief.nextOf("group");
    json made = changesOf(g, "created");
    REQUIRE(made.size() == 1);
    json summary = made[0]["attrs"]["inMesh"];
    CHECK(summary["summary"] == "mesh");
    CHECK(summary["points"] == 4);
    CHECK(summary["faces"] == 1);
    CHECK(summary["hash"].get<std::string>().size() == 16);
    CHECK(changesOf(g, "value")[0]["value"] == summary);

    json whole = changesOf(full.nextOf("group"), "created")[0]["attrs"]["inMesh"];
    CHECK_FALSE(whole.contains("summary"));
    CHECK(whole == toJson(Graph::current().storedValue(Graph::current().plug("Quad.inMesh"))));

    // Same points, same hash; moved points, another.
    run("createMesh", quad);
    CHECK(changesOf(brief.nextOf("group"), "created")[0]["attrs"]["inMesh"]["hash"] == summary["hash"]);
    quad["points"][0] = {0, 0, 1};
    run("createMesh", quad);
    CHECK(changesOf(brief.nextOf("group"), "created")[0]["attrs"]["inMesh"]["hash"] != summary["hash"]);
}

TEST_CASE("change stream: in-process subscribers hear the same groups and recording stops with the last one")
{
    stream::registerCommands();
    run("newScene");
    REQUIRE_FALSE(stream::recording());
    const size_t undoListeners = events::subscriberCount(events::Event::UndoStackChanged);
    std::vector<json> heard;
    const uint64_t id = stream::subscribe([&heard](const json& m) { heard.push_back(m); }, stream::Filter::fromJson({{"attrs", json::array({"translate"})}}));
    CHECK(stream::recording());

    run("spaceLocator", {{"name", "A"}});
    run("setAttr", {{"args", {"A.tx", 2}}});
    run("setAttr", {{"args", {"A.sx", 2}}});
    REQUIRE(heard.size() == 2);
    CHECK(heard[0]["command"] == "spaceLocator");
    CHECK(heard[0]["changes"][0]["attrs"].size() == 1);
    CHECK(heard[1]["changes"][0]["value"][0] == 2.0);

    CHECK(events::subscriberCount(events::Event::UndoStackChanged) == undoListeners + 1);
    CHECK(stream::unsubscribe(id));
    CHECK_FALSE(stream::recording());
    // Nothing of the recorder is left listening.
    CHECK(events::subscriberCount(events::Event::UndoStackChanged) == undoListeners);
    run("setAttr", {{"args", {"A.tx", 3}}});
    CHECK(heard.size() == 2);
    CHECK_THROWS_AS(stream::Filter::fromJson({{"full", "yes"}}), std::invalid_argument);
}

namespace
{
json worldChange(const json& changes, const std::string& path)
{
    for (const json& c : changes)
        if (c["kind"] == "world" && c["path"] == path)
            return c["matrix"];
    return nullptr;
}

/** The world matrix's translation, from its column-major 16 numbers. */
glm::dvec3 translation(const json& m) { return {m[12].get<double>(), m[13].get<double>(), m[14].get<double>()}; }
} // namespace

TEST_CASE("change stream: live clients see a drag while it happens, throttled per plug, then the usual group")
{
    Serving s;
    Client live = connected(s);
    Client plain = connected(s);
    live.send({{"op", "subscribe"}, {"live", true}});
    REQUIRE(live.nextOf("subscribed").is_object());
    run("spaceLocator", {{"name", "A"}});
    live.nextOf("group");
    plain.nextOf("group");

    Graph& g = Graph::current();
    std::vector<json> interim;
    {
        UndoChunk drag("Move Tool");
        // A burst well inside one throttle interval: the first value goes out, the rest wait.
        for (int i = 1; i <= 10; ++i)
        {
            g.setValue(g.plug("A.tx"), double(i));
            mainq::drain();
        }
        json first = live.next();
        REQUIRE(first["type"] == "live");
        interim.push_back(first);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        g.setValue(g.plug("A.tx"), 11.0);
        mainq::drain();
        json second = live.next();
        REQUIRE(second["type"] == "live");
        interim.push_back(second);
    }
    json group = live.nextOf("group");
    REQUIRE(group.is_object());

    CHECK(interim[0]["source"] == "interactive");
    REQUIRE(interim[0]["changes"].size() == 1);
    CHECK(interim[0]["changes"][0]["attr"] == "translate");
    CHECK(interim[0]["changes"][0]["value"][0] == 1.0);
    CHECK(interim[1]["changes"][0]["value"][0] == 11.0);
    CHECK(interim[1]["seq"] == interim[0]["seq"]);
    CHECK(group["command"] == "Move Tool");
    CHECK(group["seq"].get<uint64_t>() == interim[0]["seq"].get<uint64_t>() + 1);
    CHECK(changesOf(group, "value")[0]["value"][0] == 11.0);

    // Without live, the drag is only its group.
    json heard = plain.next();
    CHECK(heard["type"] == "group");
    CHECK(heard["seq"] == group["seq"]);

    // Commands are never live.
    run("setAttr", {{"args", {"A.ty", 2}}});
    CHECK(live.next()["type"] == "group");
}

TEST_CASE("change stream: world clients get evaluated world matrices for a moved transform and everything under it")
{
    Serving s;
    Client world = connected(s);
    Client plain = connected(s);
    world.send({{"op", "subscribe"}, {"world", true}, {"live", true}});
    REQUIRE(world.nextOf("subscribed").is_object());

    run("createNode", {{"args", {"transform"}}, {"name", "Parent"}});
    run("spaceLocator", {{"name", "Kid"}});
    run("parent", {{"args", {"Kid", "Parent"}}});
    run("setAttr", {{"args", {"Kid.tx", 1}}});
    for (int i = 0; i < 4; ++i)
        plain.nextOf("group");
    for (int i = 0; i < 3; ++i)
        world.nextOf("group");
    json kidOnly = world.nextOf("group");
    CHECK(worldChange(kidOnly["changes"], "|Parent").is_null());
    CHECK(translation(worldChange(kidOnly["changes"], "|Parent|Kid")) == glm::dvec3(1, 0, 0));

    run("setAttr", {{"args", {"Parent.translate", json::array({10, 0, 0})}}});
    json moved = world.nextOf("group");
    REQUIRE(moved.is_object());
    const json& changes = moved["changes"];
    // Parents first, after the value changes.
    CHECK(changes[0]["kind"] == "value");
    CHECK(changes[1]["path"] == "|Parent");
    CHECK(changes[2]["path"] == "|Parent|Kid");
    CHECK(translation(worldChange(changes, "|Parent")) == glm::dvec3(10, 0, 0));
    CHECK(translation(worldChange(changes, "|Parent|Kid")) == glm::dvec3(11, 0, 0));
    CHECK(changesOf(plain.nextOf("group"), "world").empty());

    // Reparenting keeps the world position (keepWorld), so the matrix stays while the path changes.
    run("parent", {{"args", {"Kid"}}, {"world", true}});
    json unparented = world.nextOf("group");
    CHECK(translation(worldChange(unparented["changes"], "|Kid")) == glm::dvec3(11, 0, 0));

    // Live messages carry world matrices too.
    Graph& g = Graph::current();
    {
        UndoChunk drag("Move Tool");
        g.setValue(g.plug("Parent.tx"), 20.0);
        mainq::drain();
        json live = world.nextOf("live");
        REQUIRE(live.is_object());
        CHECK(translation(worldChange(live["changes"], "|Parent")) == glm::dvec3(20, 0, 0));
    }
    world.nextOf("group");

    world.send({{"op", "snapshot"}});
    json snap = world.nextOf("snapshot");
    REQUIRE(snap.is_object());
    for (const json& n : snap["nodes"])
        if (n["path"] == "|Parent")
            CHECK(translation(n["world"]) == glm::dvec3(20, 0, 0));
}

TEST_CASE("change stream: a server that needs a token refuses clients without the right one")
{
    stream::registerCommands();
    run("newScene");
    // requireToken takes allowRemote's path through the command and the server, binding only 127.0.0.1: every
    // interface would bring up a firewall prompt.
    struct Stop
    {
        ~Stop() { stream::stopServer(); }
    } stop;
    json status = run("changeStream", {{"port", 0}, {"requireToken", true}});
    CHECK(status["running"] == true);
    CHECK(status["localOnly"] == true);
    CHECK(status["requireToken"] == true);
    const int port = status["port"].get<int>();
    const std::string token = stream::tokenFromPrefs();
    CHECK(token.size() == 40);
    CHECK(status["token"] == token);
    // Restarting on another port keeps it.
    CHECK(run("changeStream", {{"port", 0}})["requireToken"] == true);
    CHECK(stream::serverRequiresToken());

    {
        Client c(port);
        CHECK(c.next()["auth"] == true);
        c.send({{"op", "ping"}});
        CHECK(c.nextOf("error")["op"] == "ping");
        CHECK(c.next(2000).is_null());
    }
    {
        Client c(port);
        c.next();
        c.send({{"op", "auth"}, {"token", "nope"}});
        CHECK(c.nextOf("error")["message"] == "wrong token");
        CHECK(c.next(2000).is_null());
    }
    {
        Client waiting(port);
        waiting.next();
        Client ok(port);
        ok.next();
        ok.send({{"op", "auth"}, {"token", token}});
        REQUIRE(ok.nextOf("authed").is_object());
        Client subscribed(port);
        subscribed.next();
        subscribed.send({{"op", "subscribe"}, {"token", token}, {"attrs", json::array({"translate"})}});
        REQUIRE(subscribed.nextOf("subscribed").is_object());

        run("spaceLocator", {{"name", "A"}});
        CHECK(ok.nextOf("group")["command"] == "spaceLocator");
        CHECK(subscribed.nextOf("group")["command"] == "spaceLocator");
        // Not yet authenticated: hears nothing.
        json first = waiting.next(300);
        CHECK(first.is_null());
    }

    // Localhost without the flag stays open.
    stream::stopServer();
    std::string err;
    REQUIRE(stream::startServer(0, true, &err));
    CHECK_FALSE(stream::serverRequiresToken());
    CHECK(run("changeStream")["token"].is_null());
    Client open(stream::serverPort());
    CHECK(open.next()["auth"] == false);
}

namespace
{
json ofKind(const json& changes, const std::string& kind, const std::string& path, const std::string& attr = {})
{
    for (const json& c : changes)
        if (c["kind"] == kind && c["path"] == path && (attr.empty() || c["attr"] == attr))
            return c;
    return nullptr;
}

/** Every message that arrives until the line goes quiet for quietMs. */
std::vector<json> drainAll(Client& c, int quietMs = 300)
{
    std::vector<json> out;
    for (json j = c.next(quietMs); !j.is_null(); j = c.next(quietMs))
        out.push_back(j);
    return out;
}

scene::MeshPtr quadMesh(double z)
{
    const glm::dvec3 pts[] = {{0, 0, z}, {1, 0, z}, {1, 1, z}, {0, 1, z}};
    const geo::Index sizes[] = {4};
    const geo::Index idx[] = {0, 1, 2, 3};
    return std::make_shared<const geo::Mesh>(geo::Mesh::fromPolygons(pts, sizes, idx));
}

/** For tests that edit a mesh in place, which Lila itself never should. */
geo::Mesh& inPlace(const scene::MeshPtr& m) { return const_cast<geo::Mesh&>(*m); }

void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
} // namespace

TEST_CASE("change stream: evaluated clients get changed outputs, and only when they change")
{
    Serving s;
    Client ev = connected(s);
    Client whole = connected(s);
    Client plain = connected(s);
    ev.send({{"op", "subscribe"}, {"evaluated", true}, {"attrs", json::array({"outMesh"})}});
    REQUIRE(ev.nextOf("subscribed")["filter"]["evaluated"] == true);
    whole.send({{"op", "subscribe"}, {"evaluated", true}, {"full", true}, {"attrs", json::array({"outMesh"})}});
    REQUIRE(whole.nextOf("subscribed").is_object());

    json quad = {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "Quad"}};
    run("createMesh", quad);
    json made = ofKind(ev.nextOf("group")["changes"], "evaluated", "|Quad", "outMesh");
    REQUIRE(made.is_object());
    CHECK(made["value"]["summary"] == "mesh");
    CHECK(made["value"]["points"] == 4);
    Graph& g = Graph::current();
    json full = ofKind(whole.nextOf("group")["changes"], "evaluated", "|Quad", "outMesh");
    REQUIRE(full.is_object());
    CHECK(full["value"] == toJson(g.value(g.plug("Quad.outMesh"))));
    CHECK(changesOf(plain.nextOf("group"), "evaluated").empty());

    // Edits that leave the output alone send none.
    run("createNode", {{"args", {"transform"}}, {"name", "Other"}});
    CHECK(changesOf(ev.nextOf("group"), "evaluated").empty());
    whole.nextOf("group");

    // A new input outside any command: its group carries the new output.
    g.setValue(g.plug("Quad.inMesh"), quadMesh(2.0));
    json moved = ev.nextOf("group");
    REQUIRE(moved.is_object());
    json now = ofKind(moved["changes"], "evaluated", "|Quad", "outMesh");
    REQUIRE(now.is_object());
    CHECK(now["value"]["hash"] != made["value"]["hash"]);
    CHECK(changesOf(moved, "value").empty()); // inMesh is outside the filter's attrs
    whole.nextOf("group");

    // Recomputed to the same value: nothing.
    const NodeId id = g.find("Quad")->id();
    g.touch(id);
    g.evaluate();
    CHECK(ev.next(300).is_null());

    // The same mesh object edited in place: its content hash changes, so the frame after the evaluation sends it.
    scene::MeshPtr in = std::get<scene::MeshPtr>(g.storedValue(g.plug("Quad.inMesh")));
    inPlace(in).setPosition(0, {0, 0, 5});
    g.touch(id);
    g.evaluate();
    json frame = ev.nextOf("frame");
    REQUIRE(frame.is_object());
    json edited = ofKind(frame["changes"], "evaluated", "|Quad", "outMesh");
    REQUIRE(edited.is_object());
    CHECK(edited["value"]["hash"] != now["value"]["hash"]);
    for (const json& m : drainAll(plain))
        CHECK(m["type"] == "group");

    // A snapshot for an evaluated filter carries the outputs.
    ev.send({{"op", "snapshot"}});
    json snap = ev.nextOf("snapshot");
    REQUIRE(snap.is_object());
    bool found = false;
    for (const json& n : snap["nodes"])
        if (n["path"] == "|Quad")
        {
            found = true;
            CHECK(n["outputs"]["outMesh"]["hash"] == edited["value"]["hash"]);
        }
    CHECK(found);
}

TEST_CASE("change stream: evaluated values are diffed per filter")
{
    stream::registerCommands();
    run("newScene");
    run("createMesh", {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}}, {"faces", {{0, 1, 2}}}, {"name", "Tri"}});
    std::vector<json> meshes, all;
    const uint64_t a = stream::subscribe([&meshes](const json& m) { meshes.push_back(m); },
                                         stream::Filter::fromJson({{"evaluated", true}, {"attrs", json::array({"outMesh"})}}));
    const uint64_t b = stream::subscribe([&all](const json& m) { all.push_back(m); }, stream::Filter::fromJson({{"evaluated", true}, {"under", "Tri"}}));

    // Each filter hears an output the first time it computes after subscribing.
    run("setAttr", {{"args", {"Tri.tx", 3}}});
    REQUIRE(all.size() == 1);
    json matrix = ofKind(all[0]["changes"], "evaluated", "|Tri", "matrix");
    REQUIRE(matrix.is_object());
    CHECK(matrix["value"][12] == 3.0);
    // worldMatrix is left to world.
    CHECK(ofKind(all[0]["changes"], "evaluated", "|Tri", "worldMatrix").is_null());
    REQUIRE(meshes.size() == 1);
    CHECK(ofKind(meshes[0]["changes"], "evaluated", "|Tri", "outMesh").is_object());

    // Then only what changed for it: the matrix for one, nothing for the one that wants outMesh.
    run("setAttr", {{"args", {"Tri.tx", 4}}});
    REQUIRE(all.size() == 2);
    CHECK(ofKind(all[1]["changes"], "evaluated", "|Tri", "matrix")["value"][12] == 4.0);
    CHECK(ofKind(all[1]["changes"], "evaluated", "|Tri", "outMesh").is_null());
    CHECK(meshes.size() == 1);

    CHECK(stream::unsubscribe(a));
    CHECK(stream::unsubscribe(b));
}

TEST_CASE("change stream: time moving sends frames of changed world matrices and evaluated values, throttled")
{
    Serving s;
    Client c = connected(s);
    Client plain = connected(s);
    c.send({{"op", "subscribe"}, {"world", true}, {"evaluated", true}, {"attrs", json::array({"translate"})}});
    REQUIRE(c.nextOf("subscribed").is_object());

    run("spaceLocator", {{"name", "A"}});
    run("spaceLocator", {{"name", "Still"}});
    run("setKeyframe", {{"args", {"A.tx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"A.tx"}}, {"time", 1.0}, {"value", 10.0}});
    drainAll(c);
    drainAll(plain);

    // Playback: one currentTime a frame, far faster than frame messages may go out.
    const int steps = 20;
    for (int i = 1; i <= steps; ++i)
    {
        run("currentTime", {{"args", {double(i) / steps}}});
        mainq::drain();
    }
    std::vector<json> frames, groups;
    for (const json& m : drainAll(c))
        (m["type"] == "frame" ? frames : groups).push_back(m);
    REQUIRE(!frames.empty());
    CHECK(frames.size() < size_t(steps));
    CHECK(groups.size() == size_t(steps));
    for (const json& gr : groups)
        CHECK(changesOf(gr, "world").empty());
    for (const json& f : frames)
        CHECK(ofKind(f["changes"], "world", "|Still").is_null());
    // The last frame catches up with where playback stopped.
    const json& last = frames.back();
    CHECK(last["time"] == 1.0);
    json world = ofKind(last["changes"], "world", "|A");
    REQUIRE(world.is_object());
    CHECK(translation(world["matrix"]).x == doctest::Approx(10.0));
    json tx = ofKind(last["changes"], "evaluated", "|A", "translate");
    REQUIRE(tx.is_object());
    CHECK(tx["value"][0].get<double>() == doctest::Approx(10.0));

    for (const json& m : drainAll(plain))
        CHECK(m["type"] == "group");
}

namespace
{
std::function<void()> gDuringCommand;

void registerModalCommand()
{
    ops::registerCommand({
        .name = "streamTestModal",
        .doc = "Adds 1 to A.tx, runs the test's callback as a modal dialog would, then adds 1 to A.ty.",
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            const glm::dvec3 t = std::get<glm::dvec3>(g.storedValue(g.plug("A.translate")));
            g.setValue(g.plug("A.tx"), t.x + 1.0);
            if (gDuringCommand)
                gDuringCommand();
            g.setValue(g.plug("A.ty"), t.y + 1.0);
            return nullptr;
        },
    });
}
} // namespace

TEST_CASE("change stream: a snapshot taken while a command is open says exactly which seq it reflects")
{
    registerModalCommand();
    Serving s;
    Client c = connected(s);
    run("spaceLocator", {{"name", "A"}});
    const uint64_t before = c.nextOf("group")["seq"].get<uint64_t>();

    // A client asks for a snapshot while the command waits on its dialog, which keeps the main queue draining.
    json snap;
    gDuringCommand = [&] {
        c.send({{"op", "snapshot"}});
        snap = c.nextOf("snapshot");
    };
    run("streamTestModal");
    gDuringCommand = nullptr;
    REQUIRE(snap.is_object());
    const uint64_t at = snap["seq"].get<uint64_t>();
    CHECK(at == before + 1);
    CHECK(snap["nodes"][0]["attrs"]["translate"] == json::array({1.0, 0.0, 0.0}));

    // The part before the snapshot has its seq, for a client holding the snapshot to drop; the rest is the next group.
    json first = c.nextOf("group");
    json rest = c.nextOf("group");
    CHECK(first["seq"] == at);
    CHECK(first["command"] == "streamTestModal");
    CHECK(rest["seq"] == at + 1);
    CHECK(rest["command"] == "streamTestModal");
    CHECK(changesOf(rest, "value")[0]["value"] == json::array({1.0, 1.0, 0.0}));

    // In process, from inside the command.
    std::vector<json> heard;
    const uint64_t sub = stream::subscribe([&heard](const json& m) { heard.push_back(m); });
    json local;
    gDuringCommand = [&] { local = stream::snapshot(); };
    run("streamTestModal");
    gDuringCommand = nullptr;
    REQUIRE(heard.size() == 2);
    CHECK(heard[0]["seq"] == local["seq"]);
    CHECK(heard[1]["seq"].get<uint64_t>() == local["seq"].get<uint64_t>() + 1);
    CHECK(local["nodes"][0]["attrs"]["translate"] == json::array({2.0, 1.0, 0.0}));
    CHECK(changesOf(heard[1], "value")[0]["value"] == json::array({2.0, 2.0, 0.0}));

    // An interactive edit still open is split the same way, both parts under its undo label.
    heard.clear();
    Graph& g = Graph::current();
    {
        UndoChunk drag("Move Tool");
        g.setValue(g.plug("A.tz"), 4.0);
        local = stream::snapshot();
        g.setValue(g.plug("A.tz"), 5.0);
    }
    mainq::drain();
    REQUIRE(heard.size() == 2);
    CHECK(heard[0]["command"] == "Move Tool");
    CHECK(heard[0]["seq"] == local["seq"]);
    CHECK(heard[1]["command"] == "Move Tool");
    CHECK(changesOf(heard[1], "value")[0]["value"][2] == 5.0);
    CHECK(local["nodes"][0]["attrs"]["translate"][2] == 4.0);

    // With nothing open, a snapshot is simply the newest seq.
    CHECK(stream::snapshot()["seq"] == heard[1]["seq"]);
    CHECK(stream::unsubscribe(sub));
}

TEST_CASE("change stream: mesh hashes follow the content, attribute layers and in-place edits included")
{
    stream::registerCommands();
    run("newScene");
    Graph& g = Graph::current();
    run("createMesh", {{"points", {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {"faces", {{0, 1, 2, 3}}}, {"name", "Q"}});
    const stream::Filter only = stream::Filter::fromJson({{"under", "Q"}, {"attrs", json::array({"inMesh"})}});
    auto hash = [&] { return stream::snapshot(only)["nodes"][0]["attrs"]["inMesh"]["hash"].get<std::string>(); };
    const std::string first = hash();
    CHECK(hash() == first);

    scene::MeshPtr m = std::get<scene::MeshPtr>(g.storedValue(g.plug("Q.inMesh")));
    inPlace(m).setPosition(2, {1, 1, 1});
    const std::string moved = hash();
    CHECK(moved != first);
    inPlace(m).pointAttrs().add("weight", geo::AttrType::Float);
    CHECK(hash() != moved);
}

TEST_CASE("change stream: lilabatch serves it only when asked, by flag or environment")
{
    stream::registerCommands();
    run("newScene");
    struct Stop
    {
        ~Stop()
        {
            stream::stopServer();
            setEnv("LILA_CHANGE_STREAM", "");
        }
    } stop;
    setEnv("LILA_CHANGE_STREAM", "");
    std::string err;
    REQUIRE(stream::startForBatch(-1, &err));
    CHECK_FALSE(stream::serverRunning());

    setEnv("LILA_CHANGE_STREAM", "nope");
    CHECK_FALSE(stream::startForBatch(-1, &err));
    CHECK(err.find("LILA_CHANGE_STREAM") != std::string::npos);
    CHECK_FALSE(stream::serverRunning());

    setEnv("LILA_CHANGE_STREAM", "0");
    REQUIRE(stream::startForBatch(-1, &err));
    REQUIRE(stream::serverRunning());
    CHECK(stream::serverLocalOnly());
    CHECK_FALSE(stream::serverRequiresToken());
    {
        Client c(stream::serverPort());
        REQUIRE(c.sock.valid());
        CHECK(c.next()["type"] == "hello");
        c.send({{"op", "ping"}});
        REQUIRE(c.nextOf("pong").is_object());
        run("spaceLocator", {{"name", "B"}});
        CHECK(c.nextOf("group")["command"] == "spaceLocator");
    }
    stream::stopServer();

    // The flag wins over the environment.
    setEnv("LILA_CHANGE_STREAM", "nope");
    REQUIRE(stream::startForBatch(0, &err));
    CHECK(stream::serverRunning());
}

TEST_CASE("change stream: stopping the server delivers what is queued first")
{
    Serving s;
    Client c = connected(s);
    run("spaceLocator", {{"name", "Last"}});
    run("changeStream", {{"enable", false}});
    json g = c.nextOf("group");
    REQUIRE(g.is_object());
    CHECK(g["command"] == "spaceLocator");
}
