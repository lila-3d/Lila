#include "ui/Keymaps.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "ops/Command.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace lila::ui::keymaps
{
namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;

std::string prefFile(const std::string& name)
{
    return "keymaps/" + name;
}

fs::path userFile(const std::string& name)
{
    return PrefStore::get().userDir() / "keymaps" / paths::fromUtf8(name + ".json");
}

bool hasUserFile(const std::string& name)
{
    std::error_code ec;
    return fs::is_regular_file(userFile(name), ec);
}

void checkName(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name == "." || name == ".." || name.front() == ' ' || name.back() == ' ')
        throw std::invalid_argument("'" + name + "' can't be a keymap name");
    for (char c : name)
        if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32)
            throw std::invalid_argument("a keymap name can't contain '" + std::string(1, c) + "'");
}

bool exists(const std::string& name)
{
    const std::vector<Info> all = list();
    return std::any_of(all.begin(), all.end(), [&](const Info& i) { return i.name == name; });
}

/** The keymap's bindings: your own copy, else the studio's. */
json read(const std::string& name)
{
    json doc = hasUserFile(name) ? PrefStore::get().userDoc(prefFile(name)) : PrefStore::get().merged(prefFile(name));
    if (!doc.is_object())
        doc = json::object();
    doc.erase("name");
    return doc;
}

/** The name is stored too, so an empty keymap still has a file. */
void write(const std::string& name, json doc)
{
    doc["name"] = name;
    PrefStore::get().setUserDoc(prefFile(name), std::move(doc));
    // list() reads the folder.
    PrefStore::get().flush();
}

void deleteFile(const std::string& name)
{
    PrefStore::get().setUserDoc(prefFile(name), json::object());
    PrefStore::get().flush();
    std::error_code ec;
    fs::remove(userFile(name), ec);
}

/** Puts the live bindings back into the active keymap. */
void saveActive()
{
    json doc = PrefStore::get().userDoc("hotkeys");
    write(active(), doc.is_object() ? doc : json::object());
}

void load(const std::string& name)
{
    PrefStore::get().set("general", kActivePref, name);
    PrefStore::get().setUserDoc("hotkeys", read(name));
}

std::string positional(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() > i && a.positional[i].is_string() && !a.positional[i].get<std::string>().empty())
        return a.positional[i].get<std::string>();
    throw CommandError(std::string(cmd) + ": give " + what);
}

template <class F> json guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(e.what());
    }
}

json listJson()
{
    json maps = json::array();
    for (const Info& i : list())
        maps.push_back({{"name", i.name}, {"studio", i.studio}});
    return {{"active", active()}, {"keymaps", maps}};
}
} // namespace

std::vector<Info> list()
{
    std::vector<Info> out;
    auto add = [&](const std::string& name, bool studio) {
        auto it = std::find_if(out.begin(), out.end(), [&](const Info& i) { return i.name == name; });
        if (it == out.end())
            out.push_back({name, studio});
    };
    add(kDefaultName, false);
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(PrefStore::get().userDir() / "keymaps", ec))
        if (e.path().extension() == ".json")
            add(paths::toUtf8(e.path().stem()), false);
    for (const std::string& file : PrefStore::get().studioFiles("keymaps/"))
        add(file.substr(8), true);
    add(active(), false);
    std::sort(out.begin() + 1, out.end(), [](const Info& a, const Info& b) { return a.name < b.name; });
    return out;
}

std::string active()
{
    std::string name = PrefStore::get().value<std::string>("general", kActivePref, kDefaultName);
    return name.empty() ? kDefaultName : name;
}

void use(const std::string& name)
{
    if (name == active())
        return;
    if (!exists(name))
        throw std::invalid_argument("no keymap named '" + name + "'");
    saveActive();
    load(name);
}

void create(const std::string& name, bool empty)
{
    checkName(name);
    if (exists(name))
        throw std::invalid_argument("a keymap named '" + name + "' already exists");
    saveActive();
    write(name, empty ? json::object() : read(active()));
    load(name);
}

void rename(const std::string& from, const std::string& to)
{
    checkName(to);
    if (from == kDefaultName)
        throw std::invalid_argument("Default can't be renamed; make a new keymap from it instead");
    if (!exists(from))
        throw std::invalid_argument("no keymap named '" + from + "'");
    if (exists(to))
        throw std::invalid_argument("a keymap named '" + to + "' already exists");
    if (!hasUserFile(from) && from != active())
        throw std::invalid_argument("'" + from + "' comes from the studio folder and can't be renamed");
    const bool wasActive = from == active();
    if (wasActive)
        saveActive();
    write(to, read(from));
    if (wasActive)
        PrefStore::get().set("general", kActivePref, to);
    deleteFile(from);
}

void reset(const std::string& name)
{
    if (!exists(name))
        throw std::invalid_argument("no keymap named '" + name + "'");
    write(name, json::object());
    if (name == active())
        PrefStore::get().setUserDoc("hotkeys", json::object());
}

std::string uniqueName(const std::string& base)
{
    std::string name = base;
    for (int n = 2; exists(name); ++n)
        name = base + " " + std::to_string(n);
    return name;
}

void remove(const std::string& name)
{
    if (name == kDefaultName)
        throw std::invalid_argument("Default can't be deleted");
    if (!exists(name))
        throw std::invalid_argument("no keymap named '" + name + "'");
    if (!hasUserFile(name) && name != active())
        throw std::invalid_argument("'" + name + "' comes from the studio folder and can't be deleted");
    if (name == active())
        load(kDefaultName);
    deleteFile(name);
}

void exportTo(const std::string& name, const fs::path& file)
{
    if (!exists(name))
        throw std::invalid_argument("no keymap named '" + name + "'");
    if (name == active())
        saveActive();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    out << json{{"lilaKeymap", 1}, {"name", name}, {"hotkeys", read(name)}}.dump(2);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
}

std::string importFrom(const fs::path& file, std::string name)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("can't read " + paths::toUtf8(file));
    json j = json::parse(in, nullptr, false);
    if (!j.is_object())
        throw std::runtime_error(paths::toUtf8(file) + " isn't a keymap");
    // A bare hotkeys.json works too.
    json hotkeys = j.contains("lilaKeymap") ? j.value("hotkeys", json::object()) : j;
    if (!hotkeys.is_object())
        throw std::runtime_error(paths::toUtf8(file) + " isn't a keymap");
    if (name.empty())
        name = j.value("name", paths::toUtf8(file.stem()));
    checkName(name);
    name = uniqueName(name);
    write(name, std::move(hotkeys));
    return name;
}

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "keymaps",
        .doc = "The hotkey keymaps: {active, keymaps: [{name, studio}]}.",
        .undoable = false,
        .run = [](const Args&) -> json { return listJson(); },
    });

    ops::registerCommand({
        .name = "useKeymap",
        .doc = "Switch to a keymap. The current one keeps the edits made to it.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                use(positional(a, 0, "useKeymap", "a keymap name"));
                return listJson();
            });
        },
    });

    ops::registerCommand({
        .name = "newKeymap",
        .doc = "Make a keymap, a copy of the current one, and switch to it.",
        .positionalDoc = "name",
        .flags = {{"empty", "", T::Bool, false, "Start from Lila's defaults instead of a copy."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                create(positional(a, 0, "newKeymap", "a keymap name"), a.get<bool>("empty", false));
                return listJson();
            });
        },
    });

    ops::registerCommand({
        .name = "resetKeymap",
        .doc = "Drop every change a keymap makes, back to Lila's default hotkeys.",
        .positionalDoc = "name (the current keymap when not given)",
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                const bool named = !a.positional.empty() && a.positional[0].is_string();
                reset(named ? a.positional[0].get<std::string>() : active());
                return listJson();
            });
        },
    });

    ops::registerCommand({
        .name = "renameKeymap",
        .doc = "Rename a keymap.",
        .positionalDoc = "name, new name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                rename(positional(a, 0, "renameKeymap", "a keymap name"), positional(a, 1, "renameKeymap", "the new name"));
                return listJson();
            });
        },
    });

    ops::registerCommand({
        .name = "deleteKeymap",
        .doc = "Delete a keymap. Deleting the current one switches to Default.",
        .positionalDoc = "name",
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                remove(positional(a, 0, "deleteKeymap", "a keymap name"));
                return listJson();
            });
        },
    });

    ops::registerCommand({
        .name = "exportKeymap",
        .doc = "Write a keymap to a .lilakeys file to share. Returns the file.",
        .positionalDoc = "file",
        .flags = {{"name", "n", T::String, nullptr, "The keymap; the current one when not given."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                const std::string file = positional(a, 0, "exportKeymap", "a file");
                exportTo(a.get<std::string>("name", active()), paths::fromUtf8(file));
                return json(file);
            });
        },
    });

    ops::registerCommand({
        .name = "importKeymap",
        .doc = "Add the keymap in a .lilakeys file (or a hotkeys.json). Returns its name.",
        .positionalDoc = "file",
        .flags = {
            {"name", "n", T::String, nullptr, "Name it this rather than the name in the file."},
            {"use", "", T::Bool, false, "Switch to it."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            return guarded([&] {
                std::string name = importFrom(paths::fromUtf8(positional(a, 0, "importKeymap", "a file")), a.get<std::string>("name", ""));
                if (a.get<bool>("use", false))
                    use(name);
                return json(name);
            });
        },
    });
}
} // namespace lila::ui::keymaps
