#include "gn/Clipboard.h"
#include "gn/Exec.h"
#include "gn/Gn.h"
#include "gn/Routing.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "geo/Mesh.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace lila::gn
{
namespace detail
{
scene::Node* applyGroup(scene::Graph& g, scene::Node& groupNode, scene::Node& mesh);
void createGroupNode(scene::Graph& g, const Group& grp);
void registerGroupCommands();
} // namespace detail

namespace
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;

Graph& graph() { return Graph::current(); }

std::string positionalString(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

Node& groupNode(const std::string& name, const char* cmd)
{
    Node* n = findGroupNode(graph(), name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no geometry-nodes group named '" + name + "'");
    return *n;
}

/** Reads the group, lets fn change it, and commits it undoably. */
json editGroup(const Args& a, const char* cmd, const std::function<json(Group&)>& fn)
{
    Node& n = groupNode(positionalString(a, 0, cmd, "group"), cmd);
    Group grp;
    try
    {
        grp = readGroup(n);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    json result = fn(grp);
    try
    {
        commitGroup(graph(), n, std::move(grp));
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    return result;
}

/** "node.socket" split at the first dot. */
std::pair<std::string, std::string> splitPlug(const std::string& s, const char* cmd)
{
    auto dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == s.size())
        throw CommandError(std::string(cmd) + ": expected node.socket, got '" + s + "'");
    return {s.substr(0, dot), s.substr(dot + 1)};
}

const NodeDecl& declOf(const GNode& n, const char* cmd)
{
    const NodeDecl* d = findDecl(n.type);
    if (!d)
        throw CommandError(std::string(cmd) + ": " + n.name + " has unknown type " + n.type);
    return *d;
}

GNode& nodeIn(Group& grp, const std::string& name, const char* cmd)
{
    GNode* n = grp.node(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node '" + name + "' in " + grp.name);
    return *n;
}

/** True if `to` already feeds `from` (so from -> to would close a loop), portals included. */
bool reaches(const Group& grp, const std::string& from, const std::string& to)
{
    std::vector<GLink> links = withPortalLinks(grp.nodes, grp.links);
    std::vector<std::string> stack{from};
    std::set<std::string> seen;
    while (!stack.empty())
    {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == to)
            return true;
        if (!seen.insert(cur).second)
            continue;
        for (const GLink& l : links)
            if (l.toNode == cur)
                stack.push_back(l.fromNode);
    }
    return false;
}

json socketJson(const SocketDecl& s, const GNode& n, const Group& grp, bool input)
{
    json j = {{"id", s.id}, {"type", socketTypeName(s.type)}, {"field", s.field}};
    if (input)
    {
        j["multi"] = s.multi;
        j["linked"] = !grp.linksInto(n.name, s.id).empty();
        auto it = n.values.find(s.id);
        j["value"] = it != n.values.end() ? *it : s.defaultValue;
        j["default"] = s.defaultValue;
        if (s.implicit != Implicit::None)
            j["implicit"] = s.implicit == Implicit::Position ? "position" : s.implicit == Implicit::Normal ? "normal" : s.implicit == Implicit::Index ? "index" : "id";
    }
    if (!s.doc.empty())
        j["doc"] = s.doc;
    return j;
}

json nodeJson(const GNode& n, const Group& grp)
{
    json j = {{"name", n.name}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
    const NodeDecl* d = findDecl(n.type);
    if (!d)
    {
        j["error"] = "unknown node type";
        return j;
    }
    j["label"] = d->label;
    j["category"] = d->category;
    Sockets s = d->socketsFor(grp, n);
    json ins = json::array(), outs = json::array(), props = json::array();
    for (const SocketDecl& sd : s.inputs)
        ins.push_back(socketJson(sd, n, grp, true));
    for (const SocketDecl& sd : s.outputs)
        outs.push_back(socketJson(sd, n, grp, false));
    for (const PropDecl& p : d->props)
        props.push_back({{"id", p.id}, {"options", p.options}, {"value", d->propValue(n, p.id)}});
    j["inputs"] = ins;
    j["outputs"] = outs;
    j["props"] = props;
    return j;
}

/** Checks a socket value roughly fits its type, so typos fail at the command rather than evaluating as zero. */
void checkValue(const SocketDecl& s, const json& v, const char* cmd)
{
    bool ok = true;
    switch (s.type)
    {
    case SocketType::Geometry: ok = false; break;
    case SocketType::String: ok = v.is_string(); break;
    case SocketType::Vector:
    case SocketType::Rotation: ok = (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) || v.is_number(); break;
    case SocketType::Color:
        ok = (v.is_array() && (v.size() == 3 || v.size() == 4) && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); })) ||
             v.is_number() || (v.is_string() && v.get<std::string>().starts_with("#"));
        break;
    default: ok = v.is_number() || v.is_boolean(); break;
    }
    if (!ok)
        throw CommandError(std::string(cmd) + ": " + s.id + " takes a " + socketTypeName(s.type) + ", got " + v.dump());
}

fs::path userLibrary() { return PrefStore::get().userDir() / "geometryNodes"; }

fs::path workspaceLibrary(bool create)
{
    if (!Workspace::get().isOpen())
        return {};
    return Workspace::get().stateDir(create) / "geometryNodes";
}

} // namespace

std::vector<fs::path> libraryDirs()
{
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(userLibrary(), ec))
        out.push_back(userLibrary());
    fs::path ws = workspaceLibrary(false);
    if (!ws.empty() && fs::is_directory(ws, ec))
        out.push_back(ws);
    // Groups shipped with Lila come last, so a user or workspace copy of the same name wins.
    fs::path shipped = paths::resourcesDir() / "geometryNodes";
    if (fs::is_directory(shipped, ec))
        out.push_back(shipped);
    return out;
}

void registerCommands()
{
    using T = ArgType;
    registerNodeLibrary();
    detail::registerGroupCommands();

    ops::registerCommand({
        .name = "gnCreateGroup",
        .doc = "Create a geometry-nodes group (Group Input wired to Group Output). Returns its name.",
        .positionalDoc = "name",
        .run = [](const Args& a) -> json {
            std::string name = positionalString(a, 0, "gnCreateGroup", "name");
            if (!validIdentifier(name))
                throw CommandError("gnCreateGroup: '" + name + "' isn't a valid name (letters, digits, underscores)");
            try
            {
                detail::createGroupNode(graph(), Group::makeDefault(name));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnCreateGroup: ") + e.what());
            }
            return name;
        },
    });

    ops::registerCommand({
        .name = "gnAddNode",
        .doc = "Add a node to a group. Returns its name. gnNodeTypes lists the types.",
        .positionalDoc = "group type",
        .flags = {{"position", "", T::List, nullptr, "[x, y] on the canvas."}, {"name", "", T::String, nullptr, "Wanted name; made unique."}},
        .run = [](const Args& a) -> json {
            std::string type = positionalString(a, 1, "gnAddNode", "type");
            const NodeDecl* d = findDecl(type);
            if (!d)
                throw CommandError("gnAddNode: unknown node type '" + type + "'");
            return editGroup(a, "gnAddNode", [&](Group& grp) -> json {
                std::string base = a.get<std::string>("name", type);
                if (!validIdentifier(base))
                    throw CommandError("gnAddNode: '" + base + "' isn't a valid name");
                GNode n{grp.uniqueNodeName(base), type, {0.0, 0.0}, json::object()};
                if (a.has("position"))
                    n.position = {a.flags["position"][0].get<double>(), a.flags["position"][1].get<double>()};
                grp.nodes.push_back(n);
                return n.name;
            });
        },
    });

    ops::registerCommand({
        .name = "gnDeleteNode",
        .doc = "Delete nodes from a group, with their links.",
        .positionalDoc = "group nodes...",
        .run = [](const Args& a) -> json {
            return editGroup(a, "gnDeleteNode", [&](Group& grp) -> json {
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    std::string name = a.positional[i].get<std::string>();
                    nodeIn(grp, name, "gnDeleteNode");
                    std::erase_if(grp.nodes, [&](const GNode& n) { return n.name == name; });
                    std::erase_if(grp.links, [&](const GLink& l) { return l.fromNode == name || l.toNode == name; });
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnConnect",
        .doc = "Link an output socket to an input socket: gnConnect(group, 'Grid.mesh', 'SetPosition.geometry'). Replaces the input's "
               "link unless it takes several.",
        .positionalDoc = "group from to",
        .run = [](const Args& a) -> json {
            auto [fn, fs] = splitPlug(positionalString(a, 1, "gnConnect", "from"), "gnConnect");
            auto [tn, ts] = splitPlug(positionalString(a, 2, "gnConnect", "to"), "gnConnect");
            return editGroup(a, "gnConnect", [&](Group& grp) -> json {
                GNode& from = nodeIn(grp, fn, "gnConnect");
                GNode& to = nodeIn(grp, tn, "gnConnect");
                Sockets fsock = declOf(from, "gnConnect").socketsFor(grp, from);
                Sockets tsock = declOf(to, "gnConnect").socketsFor(grp, to);
                int oi = fsock.outputIndex(fs), ii = tsock.inputIndex(ts);
                if (oi < 0)
                    throw CommandError("gnConnect: " + fn + " has no output '" + fs + "'");
                if (ii < 0)
                    throw CommandError("gnConnect: " + tn + " has no input '" + ts + "'");
                // A routing node takes the type of whatever feeds it.
                if (!isRoutingType(to.type) && !linkable(fsock.outputs[oi].type, tsock.inputs[ii].type))
                    throw CommandError(std::string("gnConnect: can't link ") + socketTypeName(fsock.outputs[oi].type) + " to " +
                                       socketTypeName(tsock.inputs[ii].type));
                if (fn == tn || reaches(grp, fn, tn))
                    throw CommandError("gnConnect: that link would make a cycle");
                GLink link{fn, fs, tn, ts};
                if (!tsock.inputs[ii].multi)
                    std::erase_if(grp.links, [&](const GLink& l) { return l.toNode == tn && l.toSocket == ts; });
                if (std::find(grp.links.begin(), grp.links.end(), link) == grp.links.end())
                    grp.links.push_back(link);
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnDisconnect",
        .doc = "Remove the links into an input socket, or only the one from `source` when given.",
        .positionalDoc = "group to",
        .flags = {{"source", "", T::String, nullptr, "node.socket of the one link to remove."}},
        .run = [](const Args& a) -> json {
            auto [tn, ts] = splitPlug(positionalString(a, 1, "gnDisconnect", "to"), "gnDisconnect");
            std::string src = a.get<std::string>("source", "");
            return editGroup(a, "gnDisconnect", [&](Group& grp) -> json {
                size_t before = grp.links.size();
                std::erase_if(grp.links, [&](const GLink& l) {
                    return l.toNode == tn && l.toSocket == ts && (src.empty() || l.fromNode + "." + l.fromSocket == src);
                });
                return int(before - grp.links.size());
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetValue",
        .doc = "Set an unlinked input's value or a property: gnSetValue(group, 'Grid.sizeX', 200), gnSetValue(group, 'Math.operation', "
               "'multiply'). GroupInput.<input> sets an exposed input's default. Rotations are in degrees.",
        .positionalDoc = "group node.socket value",
        .run = [](const Args& a) -> json {
            auto [nn, sid] = splitPlug(positionalString(a, 1, "gnSetValue", "node.socket"), "gnSetValue");
            if (a.positional.size() < 3)
                throw CommandError("gnSetValue: value required");
            json value = a.positional[2];
            return editGroup(a, "gnSetValue", [&](Group& grp) -> json {
                GNode& n = nodeIn(grp, nn, "gnSetValue");
                const NodeDecl& d = declOf(n, "gnSetValue");
                if (n.type == "GroupInput")
                {
                    auto it = std::find_if(grp.inputs.begin(), grp.inputs.end(), [&](const GInput& in) { return in.name == sid; });
                    if (it == grp.inputs.end())
                        throw CommandError("gnSetValue: no exposed input '" + sid + "'");
                    checkValue({.id = sid, .type = it->type}, value, "gnSetValue");
                    it->defaultValue = value;
                    return nullptr;
                }
                if (const PropDecl* p = d.prop(sid))
                {
                    if (!value.is_string())
                        throw CommandError("gnSetValue: " + sid + " takes a string");
                    std::string v = value.get<std::string>();
                    if (!p->options.empty() && std::find(p->options.begin(), p->options.end(), v) == p->options.end())
                    {
                        std::string opts;
                        for (const std::string& o : p->options)
                            opts += (opts.empty() ? "" : ", ") + o;
                        throw CommandError("gnSetValue: " + sid + " must be one of " + opts);
                    }
                    n.values[sid] = v;
                    if (isRoutingType(n.type) && hasCycle(withPortalLinks(grp.nodes, grp.links)))
                        throw CommandError("gnSetValue: " + nn + " reading " + v + " would feed its own Send, a loop");
                    return nullptr;
                }
                Sockets s = d.socketsFor(grp, n);
                int ii = s.inputIndex(sid);
                if (ii < 0)
                    throw CommandError("gnSetValue: " + nn + " has no input or property '" + sid + "'");
                checkValue(s.inputs[ii], value, "gnSetValue");
                n.values[sid] = value;
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnExposeInput",
        .doc = "Expose an input socket as a group parameter: it becomes a Group Input output wired to the socket and a keyable attr "
               "on every modifier using the group. Returns the parameter name.",
        .positionalDoc = "group node.socket [name]",
        .run = [](const Args& a) -> json {
            auto [nn, sid] = splitPlug(positionalString(a, 1, "gnExposeInput", "node.socket"), "gnExposeInput");
            std::string name = a.positional.size() > 2 ? a.positional[2].get<std::string>() : sid;
            if (!validInputName(name))
                throw CommandError("gnExposeInput: '" + name + "' can't be used as a parameter name");
            return editGroup(a, "gnExposeInput", [&](Group& grp) -> json {
                GNode& n = nodeIn(grp, nn, "gnExposeInput");
                Sockets s = declOf(n, "gnExposeInput").socketsFor(grp, n);
                int ii = s.inputIndex(sid);
                if (ii < 0)
                    throw CommandError("gnExposeInput: " + nn + " has no input '" + sid + "'");
                const SocketDecl& sd = s.inputs[ii];
                if (sd.type == SocketType::Geometry)
                    throw CommandError("gnExposeInput: geometry inputs can't be exposed");
                auto existing = std::find_if(grp.inputs.begin(), grp.inputs.end(), [&](const GInput& in) { return in.name == name; });
                if (existing != grp.inputs.end() && existing->type != sd.type)
                    throw CommandError("gnExposeInput: " + name + " is already a " + socketTypeName(existing->type) + " input");
                if (existing == grp.inputs.end())
                {
                    auto it = n.values.find(sid);
                    grp.inputs.push_back({name, sd.type, it != n.values.end() ? *it : sd.defaultValue});
                }
                const GNode* gi = grp.firstOfType("GroupInput");
                std::string giName;
                if (!gi)
                {
                    giName = grp.uniqueNodeName("GroupInput");
                    grp.nodes.push_back({giName, "GroupInput", n.position + glm::dvec2(-250.0, 0.0), json::object()});
                }
                else
                    giName = gi->name;
                std::erase_if(grp.links, [&](const GLink& l) { return l.toNode == nn && l.toSocket == sid; });
                grp.links.push_back({giName, name, nn, sid});
                return name;
            });
        },
    });

    ops::registerCommand({
        .name = "gnRemoveInput",
        .doc = "Remove an exposed group parameter and its links.",
        .positionalDoc = "group name",
        .run = [](const Args& a) -> json {
            std::string name = positionalString(a, 1, "gnRemoveInput", "name");
            return editGroup(a, "gnRemoveInput", [&](Group& grp) -> json {
                if (!grp.input(name))
                    throw CommandError("gnRemoveInput: no input '" + name + "'");
                std::erase_if(grp.inputs, [&](const GInput& in) { return in.name == name; });
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnMoveNodes",
        .doc = "Move nodes on the canvas: gnMoveNodes(group, positions={'Grid': [0, 100]}).",
        .positionalDoc = "group",
        .flags = {{"positions", "", T::Any, nullptr, "{node: [x, y]}"}},
        .run = [](const Args& a) -> json {
            return editGroup(a, "gnMoveNodes", [&](Group& grp) -> json {
                if (!a.has("positions"))
                    return nullptr;
                for (auto& [name, p] : a.flags["positions"].items())
                    nodeIn(grp, name, "gnMoveNodes").position = {p[0].get<double>(), p[1].get<double>()};
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnDuplicateNodes",
        .doc = "Copy nodes with the links between them. Returns the new names in order.",
        .positionalDoc = "group nodes...",
        .flags = {{"offset", "", T::List, json::array({40, 40}), "[x, y] shift for the copies."}},
        .run = [](const Args& a) -> json {
            return editGroup(a, "gnDuplicateNodes", [&](Group& grp) -> json {
                glm::dvec2 off(40.0, 40.0);
                if (a.has("offset"))
                    off = {a.flags["offset"][0].get<double>(), a.flags["offset"][1].get<double>()};
                std::map<std::string, std::string> renamed;
                json names = json::array();
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    GNode copy = nodeIn(grp, a.positional[i].get<std::string>(), "gnDuplicateNodes");
                    if (copy.type == "GroupOutput")
                        continue;
                    std::string old = copy.name;
                    copy.name = grp.uniqueNodeName(copy.type == "GroupInput" ? "GroupInput" : copy.name);
                    copy.position += off;
                    renamed[old] = copy.name;
                    grp.nodes.push_back(copy);
                    names.push_back(copy.name);
                }
                std::vector<GLink> extra;
                for (const GLink& l : grp.links)
                    if (renamed.contains(l.fromNode) && renamed.contains(l.toNode))
                        extra.push_back({renamed[l.fromNode], l.fromSocket, renamed[l.toNode], l.toSocket});
                grp.links.insert(grp.links.end(), extra.begin(), extra.end());
                std::set<std::string> copied;
                for (auto& [old, fresh] : renamed)
                    copied.insert(old);
                for (GTie& t : tiesAmong(grp.ties, copied, renamed, off))
                    grp.ties.push_back(std::move(t));
                // Copies join the frames of their originals.
                for (GFrame& f : grp.frames)
                    for (size_t k = 0, n = f.nodes.size(); k < n; ++k)
                        if (auto it = renamed.find(f.nodes[k]); it != renamed.end())
                            f.nodes.push_back(it->second);
                return names;
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetTies",
        .doc = "Replace a group's zip ties: gnSetTies(group, [{links: [[fromNode, fromSocket, toNode, toSocket], ...], position: [x, y], "
               "angle: 0, color: ''}]). Ties only change how the canvas draws links; links not in the group are dropped, and so is a "
               "tie left with fewer than two. Returns how many ties the group has.",
        .positionalDoc = "group ties",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError("gnSetTies: ties must be a list");
            std::vector<GTie> ties = tiesFromJson(a.positional[1]);
            return editGroup(a, "gnSetTies", [&](Group& grp) -> json {
                grp.ties = ties;
                pruneTies(grp.ties, grp.links);
                return int(grp.ties.size());
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetNodeFlags",
        .doc = "Set how nodes draw and run: gnSetNodeFlags(group, 'Math', 'Noise', muted=True). muted bypasses a node (its first input of a "
               "matching type passes through to each output); collapsed draws only its header; hideUnused draws only its linked sockets. "
               "Flags left out stay as they are.",
        .positionalDoc = "group nodes...",
        .flags = {{"muted", "", T::Bool, nullptr, "Bypass the nodes."},
                  {"collapsed", "", T::Bool, nullptr, "Draw only their headers."},
                  {"hideUnused", "", T::Bool, nullptr, "Draw only their linked sockets."}},
        .run = [](const Args& a) -> json {
            return editGroup(a, "gnSetNodeFlags", [&](Group& grp) -> json {
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    GNode& n = nodeIn(grp, a.positional[i].get<std::string>(), "gnSetNodeFlags");
                    if (a.has("muted"))
                    {
                        bool m = a.get<bool>("muted", false);
                        if (m && (n.type == "GroupInput" || n.type == "GroupOutput" || isRoutingType(n.type)))
                            throw CommandError("gnSetNodeFlags: " + n.name + " can't be muted");
                        n.muted = m;
                    }
                    if (a.has("collapsed"))
                        n.collapsed = a.get<bool>("collapsed", false);
                    if (a.has("hideUnused"))
                        n.hideUnused = a.get<bool>("hideUnused", false);
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetBookmarks",
        .doc = "Replace a group's saved canvas views: gnSetBookmarks(group, {'1': [x, y, zoom]}), centre and zoom by slot. Malformed "
               "entries are dropped.",
        .positionalDoc = "group bookmarks",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_object())
                throw CommandError("gnSetBookmarks: bookmarks must be an object of slot: [x, y, zoom]");
            json marks = cleanBookmarks(a.positional[1]);
            return editGroup(a, "gnSetBookmarks", [&](Group& grp) -> json {
                grp.bookmarks = marks;
                return int(marks.size());
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetFrames",
        .doc = "Replace a group's frames: gnSetFrames(group, [{label: 'Inputs', position: [x, y], size: [w, h], color: '', collapsed: false, "
               "nodes: [names]}]). Frames only change the canvas; names not in the group are dropped. Returns how many frames there are.",
        .positionalDoc = "group frames",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError("gnSetFrames: frames must be a list");
            std::vector<GFrame> frames = framesFromJson(a.positional[1]);
            return editGroup(a, "gnSetFrames", [&](Group& grp) -> json {
                grp.frames = frames;
                return int(grp.frames.size());
            });
        },
    });

    ops::registerCommand({
        .name = "gnCopyNodes",
        .doc = "Nodes as shareable text (JSON with a format header): their values and the links between them. Paste it with "
               "gnPasteNodes, here or in another scene.",
        .positionalDoc = "group nodes...",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = groupNode(positionalString(a, 0, "gnCopyNodes", "group"), "gnCopyNodes");
            std::vector<std::string> names;
            for (size_t i = 1; i < a.positional.size(); ++i)
                if (a.positional[i].is_string())
                    names.push_back(a.positional[i].get<std::string>());
            try
            {
                return copyNodesText(readGroup(n), names);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnCopyNodes: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "gnPasteNodes",
        .doc = "Add nodes from text made by gnCopyNodes (or a saved group's JSON), centred at position; names are made unique. "
               "Returns the new names. Bad text fails with every problem listed and changes nothing.",
        .positionalDoc = "group text",
        .flags = {{"position", "", T::List, json::array({0, 0}), "[x, y] canvas centre for the pasted nodes."}},
        .run = [](const Args& a) -> json {
            std::string text = positionalString(a, 1, "gnPasteNodes", "text");
            glm::dvec2 at(0.0);
            if (a.has("position"))
            {
                const json& p = a.flags["position"];
                if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
                    throw CommandError("gnPasteNodes: position must be [x, y]");
                at = {p[0].get<double>(), p[1].get<double>()};
            }
            return editGroup(a, "gnPasteNodes", [&](Group& grp) -> json {
                try
                {
                    return pasteNodesText(grp, text, at);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("gnPasteNodes: ") + e.what());
                }
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetDefinition",
        .doc = "Replace a group's whole definition (JSON as gnGroupInfo returns under 'definition'). The name is kept.",
        .positionalDoc = "group definition",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2)
                throw CommandError("gnSetDefinition: definition required");
            json def = a.positional[1].is_string() ? json::parse(a.positional[1].get<std::string>()) : a.positional[1];
            return editGroup(a, "gnSetDefinition", [&](Group& grp) -> json {
                try
                {
                    Group next = Group::fromJson(def);
                    next.name = grp.name;
                    next.typeVersion = grp.typeVersion;
                    grp = std::move(next);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("gnSetDefinition: ") + e.what());
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnGroups",
        .doc = "Names of the geometry-nodes groups in the scene.",
        .undoable = false,
        .run = [](const Args&) -> json { return groupNames(graph()); },
    });

    ops::registerCommand({
        .name = "gnGroupInfo",
        .doc = "A group's definition, its nodes with resolved sockets and values, its modifiers, and any compile error.",
        .positionalDoc = "group",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = groupNode(positionalString(a, 0, "gnGroupInfo", "group"), "gnGroupInfo");
            Group grp = readGroup(n);
            json nodes = json::array();
            std::map<std::string, std::string> problems = portalProblems(grp.nodes, grp.links);
            for (const GNode& gn : grp.nodes)
            {
                nodes.push_back(nodeJson(gn, grp));
                if (auto it = problems.find(gn.name); it != problems.end())
                    nodes.back()["problem"] = it->second;
            }
            json inst = json::array();
            for (scene::NodeId id : instancesOf(graph(), n.id()))
                inst.push_back(graph().node(id)->path());
            PlanPtr plan = compile(grp);
            json out = {{"name", grp.name}, {"definition", grp.toJson()}, {"nodes", nodes}, {"modifiers", inst}, {"typeName", typeNameFor(grp)}};
            if (!plan->error.empty())
                out["error"] = plan->error;
            return out;
        },
    });

    ops::registerCommand({
        .name = "gnNodeTypes",
        .doc = "The node library: type, label, category and description of each node. With linkType, only nodes that can take a "
               "link from (or, with linkInput, give one to) a socket of that type, each with the socket it would use.",
        .flags = {{"linkType", "", T::String, nullptr, "Socket type of a dragged link's loose end."},
                  {"linkInput", "", T::Bool, false, "The drag started at an input, so the node needs a matching output."},
                  {"group", "", T::String, nullptr, "Group the node would go in; its interface sets Group Input's sockets."},
                  {"detail", "", T::Bool, false, "Also list each node's properties (with options and defaults) and its sockets (id, type, field, "
                                                 "default, implicit input, doc) as they are with default properties."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::optional<SocketType> link;
            if (a.has("linkType"))
            {
                SocketType t;
                if (!parseSocketType(a.get<std::string>("linkType"), &t))
                    throw CommandError("gnNodeTypes: unknown socket type '" + a.get<std::string>("linkType") + "'");
                link = t;
            }
            Group grp;
            if (a.has("group"))
                if (Node* n = findGroupNode(graph(), a.get<std::string>("group")))
                    grp = readGroup(*n);
            bool fromInput = a.get<bool>("linkInput", false);
            json out = json::array();
            for (const NodeDecl* d : allDecls())
            {
                json e = {{"type", d->type}, {"label", d->label}, {"category", d->category}, {"doc", d->doc}};
                if (a.get<bool>("detail", false))
                {
                    GNode probe{d->type, d->type, {0.0, 0.0}, json::object()};
                    json props = json::array();
                    for (const PropDecl& p : d->props)
                    {
                        json pj = {{"id", p.id}, {"default", p.defaultValue}, {"options", p.options}};
                        if (!p.doc.empty())
                            pj["doc"] = p.doc;
                        props.push_back(std::move(pj));
                    }
                    Sockets s = d->socketsFor(grp, probe);
                    json ins = json::array(), outs = json::array();
                    for (const SocketDecl& sd : s.inputs)
                    {
                        json sj = socketJson(sd, probe, grp, true);
                        sj.erase("linked");
                        sj.erase("value");
                        ins.push_back(std::move(sj));
                    }
                    for (const SocketDecl& sd : s.outputs)
                        outs.push_back(socketJson(sd, probe, grp, false));
                    e["props"] = std::move(props);
                    e["inputs"] = std::move(ins);
                    e["outputs"] = std::move(outs);
                    // Sockets of these follow their properties (or the group interface); the list shows the defaults' sockets.
                    e["dynamicSockets"] = bool(d->dynamicSockets);
                }
                if (link)
                {
                    if (d->type == "GroupOutput" || (d->type == "GroupInput" && !fromInput))
                        continue;
                    std::string socket = compatibleSocket(grp, *d, *link, !fromInput);
                    if (socket.empty())
                        continue;
                    e["socket"] = socket;
                }
                out.push_back(std::move(e));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "gnApply",
        .doc = "Add a group's modifier to the top of a mesh's stack. Its exposed inputs are keyable attrs. Returns the modifier name.",
        .positionalDoc = "mesh group",
        .run = [](const Args& a) -> json {
            std::string meshName = positionalString(a, 0, "gnApply", "mesh");
            Node& gn = groupNode(positionalString(a, 1, "gnApply", "group"), "gnApply");
            Node* mesh = graph().find(meshName);
            if (!mesh || !mesh->type().isA("mesh"))
                throw CommandError("gnApply: " + meshName + " is not a mesh");
            try
            {
                return detail::applyGroup(graph(), gn, *mesh)->path();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnApply: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "gnSaveGroup",
        .doc = "Save a group to the user library (prefs folder) or the workspace's .lila/geometryNodes. Returns the file path.",
        .positionalDoc = "group",
        .flags = {{"scope", "", T::String, "user", "user or workspace."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = groupNode(positionalString(a, 0, "gnSaveGroup", "group"), "gnSaveGroup");
            Group grp = readGroup(n);
            bool ws = a.get<std::string>("scope", "user") == "workspace";
            fs::path dir = ws ? workspaceLibrary(true) : userLibrary();
            if (dir.empty())
                throw CommandError("gnSaveGroup: no workspace is open");
            std::error_code ec;
            fs::create_directories(dir, ec);
            fs::path path = dir / (grp.name + ".json");
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!(out << grp.toJson().dump(2)))
                throw CommandError("gnSaveGroup: can't write " + path.string());
            return path.generic_string();
        },
    });

    ops::registerCommand({
        .name = "gnLibrary",
        .doc = "Groups saved in the user and workspace libraries.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            fs::path user = userLibrary();
            for (const fs::path& dir : libraryDirs())
                for (const auto& e : fs::directory_iterator(dir))
                    if (e.path().extension() == ".json" || e.path().extension() == ".lilanodes")
                        out.push_back({{"name", e.path().stem().string()}, {"scope", dir == user ? "user" : "workspace"}, {"path", e.path().generic_string()}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "gnLoadGroup",
        .doc = "Bring a library group (by name, workspace first, or a .json path) into the scene. If the scene already has it, its "
               "definition is replaced and its modifiers update. Returns the group name.",
        .positionalDoc = "nameOrPath",
        .run = [](const Args& a) -> json {
            std::string what = positionalString(a, 0, "gnLoadGroup", "name or path");
            fs::path path(what);
            if (path.extension() != ".json" && path.extension() != ".lilanodes")
            {
                path.clear();
                std::vector<fs::path> dirs = libraryDirs();
                // Workspace before user before shipped.
                std::vector<fs::path> order;
                if (dirs.size() >= 2 && dirs[1] == workspaceLibrary(false))
                    order.push_back(dirs[1]);
                for (const fs::path& d : dirs)
                    if (std::find(order.begin(), order.end(), d) == order.end())
                        order.push_back(d);
                for (auto it = order.begin(); it != order.end() && path.empty(); ++it)
                    for (const char* ext : {".json", ".lilanodes"})
                        if (path.empty() && fs::exists(*it / (what + ext)))
                            path = *it / (what + ext);
                if (path.empty())
                    throw CommandError("gnLoadGroup: no library group named " + what);
            }
            if (path.extension() == ".lilanodes")
            {
                json names = ops::execute("gnImportGroup", {{"args", json::array({paths::toUtf8(path)})}});
                return names.empty() ? json() : names.back();
            }
            std::ifstream in(path, std::ios::binary);
            if (!in)
                throw CommandError("gnLoadGroup: can't read " + path.string());
            Group grp;
            try
            {
                grp = Group::fromJson(json::parse(in));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnLoadGroup: ") + e.what());
            }
            if (Node* existing = findGroupNode(graph(), grp.name))
            {
                Group cur = readGroup(*existing);
                grp.typeVersion = cur.typeVersion;
                commitGroup(graph(), *existing, std::move(grp));
                return existing->name();
            }
            grp.typeVersion = 1;
            detail::createGroupNode(graph(), grp);
            return grp.name;
        },
    });
}
} // namespace lila::gn
