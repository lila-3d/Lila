#include "ui/UserMenus.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/Menus.h"

#include <cctype>
#include <set>

namespace lila::ui::userMenus
{
namespace
{
using json = nlohmann::json;

std::string slug(const std::string& s)
{
    std::string out;
    bool upper = false;
    for (char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
        {
            out += upper && !out.empty() ? char(std::toupper(static_cast<unsigned char>(c)))
                                         : out.empty() ? char(std::tolower(static_cast<unsigned char>(c))) : c;
            upper = false;
        }
        else
            upper = true;
    }
    return out.empty() ? "menu" : out;
}

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string menuId(const json& menu)
{
    return "user:" + str(menu, "title");
}

bool asSection(const json& menu)
{
    return !str(menu, "parent").empty() && str(menu, "as") == "section";
}

/** Why the rows (at any depth) cannot be stored, or empty. */
std::string rowsProblem(const json& rows, const std::string& where)
{
    if (!rows.is_array())
        return where + ": items must be a list";
    std::set<std::string> submenus;
    for (const json& r : rows)
    {
        if (!r.is_object())
            return where + ": each item must be an object";
        if (r.contains("submenu"))
        {
            std::string label = str(r, "submenu");
            if (label.empty() || label.find('/') != std::string::npos)
                return where + ": a submenu needs a name without '/'";
            if (!submenus.insert(label).second)
                return where + ": two submenus named '" + label + "'";
            if (std::string p = rowsProblem(r.value("items", json::array()), where + " > " + label); !p.empty())
                return p;
        }
        else if (!r.contains("separator") && !r.contains("section") && r.value("action", json()).is_null())
            return where + ": an item needs an action, separator, section or submenu";
        else if (const json& a = r.value("action", json()); a.is_string() && a.get<std::string>().empty())
            return where + ": pick an action for each row";
    }
    return {};
}
} // namespace

json all()
{
    json v = PrefStore::get().value(kPrefFile, "/menus", json::array());
    return v.is_array() ? v : json::array();
}

void setAll(json menus)
{
    PrefStore::get().set(kPrefFile, "/menus", std::move(menus));
    apply();
}

std::string problem(const json& menu, const json& others)
{
    if (!menu.is_object())
        return "a menu must be an object";
    std::string title = str(menu, "title");
    if (title.empty())
        return "a menu needs a title";
    if (title.find('/') != std::string::npos)
        return "'" + title + "': titles cannot contain '/'";
    if (others.is_array())
        for (const json& o : others)
            if (o.is_object() && str(o, "title") == title)
                return "there is already a menu named '" + title + "'";
    std::string parent = str(menu, "parent");
    if (parent.empty() && menus::menuRegistry().contains(title) && *menus::menuRegistry().ownerOf(title) != kOwner)
        return "'" + title + "' is already a menu bar menu";
    return rowsProblem(menu.value("items", json::array()), title);
}

std::string rowAction(const json& menu, const json& row)
{
    if (row.value("action", json()).is_null())
        return {};
    std::string id = str(row, "id");
    return "user." + slug(str(menu, "title")) + "." + (id.empty() ? slug(str(row, "label").empty() ? actions::labelFor(row["action"]) : str(row, "label")) : id);
}

std::string rowsPath(const json& menu)
{
    std::string parent = str(menu, "parent");
    if (parent.empty())
        return menuId(menu);
    return asSection(menu) ? parent : parent + "/" + str(menu, "title");
}

namespace
{
/** Registers the row's own action when it needs one: a custom label, Python or a command. Returns the action id to show. */
std::string rowActionId(const json& menu, const json& row, std::set<std::string>& used)
{
    const json& target = row["action"];
    std::string label = str(row, "label");
    if (target.is_string() && label.empty())
        return target.get<std::string>();

    std::string id = rowAction(menu, row);
    for (int n = 2; !used.insert(id).second; ++n)
        id = rowAction(menu, row) + std::to_string(n);

    Action a;
    a.id = id;
    a.label = label.empty() ? actions::labelFor(target) : label;
    a.category = "User Menus";
    a.doc = str(row, "doc");
    if (a.doc.empty())
        a.doc = str(menu, "title") + " > " + a.label;
    a.python = actions::pythonFor(target);
    if (target.is_string())
    {
        std::string base = target.get<std::string>();
        a.icon = actions::iconFor(base);
        if (const Action* b = actions::find(base))
        {
            a.command = b->command;
            a.call = b->call;
            if (b->checked)
                a.checked = [base] {
                    const Action* now = actions::find(base);
                    return now && now->checked && now->checked();
                };
        }
        a.enabled = [base] { return actions::isEnabled(base); };
        a.run = [base] { actions::run(base); };
    }
    else if (target.contains("command") && !target.contains("python"))
    {
        a.command = str(target, "command");
        a.call = target.value("args", json::object());
    }
    else
        a.run = [target] { actions::run(target); };
    actions::add(std::move(a), kOwner);
    return id;
}

/** Adds the rows to `path`, each after the one before; returns the id of the last. */
std::string addRows(const json& menu, const json& rows, const std::string& path, std::string after, std::set<std::string>& used)
{
    for (const json& r : rows)
    {
        menus::Item item{.menu = path, .after = after};
        if (r.contains("submenu"))
        {
            item.kind = menus::Kind::Submenu;
            item.label = str(r, "submenu");
        }
        else if (r.contains("separator"))
            item.kind = menus::Kind::Separator;
        else if (r.contains("section"))
        {
            item.kind = menus::Kind::Section;
            item.label = str(r, "section");
        }
        else
            item.action = rowActionId(menu, r, used);
        after = menus::addItem(std::move(item), kOwner);
        if (r.contains("submenu"))
            addRows(menu, r.value("items", json::array()), path + "/" + str(r, "submenu"), {}, used);
    }
    return after;
}

void registerCommands()
{

    ops::registerCommand({
        .name = "userMenus",
        .doc = "The user menus, as stored: a list of {title, parent, as, before, after, sets, items}.",
        .undoable = false,
        .run = [](const Args&) -> json { return all(); },
        .examples = {"userMenus()"},
    });
    ops::registerCommand({
        .name = "setUserMenu",
        .doc = "Adds a user menu, or replaces the one with the same title. parent empty puts it on the menu bar; a menu path such "
               "as \"Mesh\" puts it in that menu, as a submenu or, with as=\"section\", as a titled section. Items take an action id, "
               "{\"python\": code} or {\"command\": name, \"args\": {}} as their action, or separator, section or submenu. "
               "Returns the stored menu.",
        .flags = {{"menu", "m", ArgType::Any, nullptr, "The menu: {title, parent, as, before, after, sets, items}."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json menu = a.flags.value("menu", json());
            json list = all();
            json others = json::array();
            size_t at = list.size();
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (list[i].is_object() && str(list[i], "title") == str(menu, "title"))
                    at = i;
                else
                    others.push_back(list[i]);
            }
            if (std::string p = problem(menu, others); !p.empty())
                throw CommandError("setUserMenu: " + p);
            if (at < list.size())
                list[at] = menu;
            else
                list.push_back(menu);
            setAll(std::move(list));
            return menu;
        },
        .examples = {"setUserMenu(menu={\"title\": \"Kit\", \"parent\": \"Mesh\", \"items\": [{\"action\": \"mesh.bevel\"}]})"},
    });
    ops::registerCommand({
        .name = "deleteUserMenu",
        .doc = "Removes the user menu with this title. Returns whether there was one.",
        .flags = {{"title", "t", ArgType::String, nullptr, "The menu's title."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string title = a.get<std::string>("title");
            json list = all();
            json kept = json::array();
            for (const json& m : list)
                if (!m.is_object() || str(m, "title") != title)
                    kept.push_back(m);
            if (kept.size() == list.size())
                return false;
            setAll(std::move(kept));
            return true;
        },
        .examples = {"deleteUserMenu(title=\"Kit\")"},
    });
}
} // namespace

void apply()
{
    menus::itemRegistry().removeOwned(kOwner);
    menus::menuRegistry().removeOwned(kOwner);
    actions::registry().removeOwned(kOwner);

    json list = all();
    json seen = json::array();
    std::set<std::string> used;
    for (const json& menu : list)
    {
        if (std::string p = problem(menu, seen); !p.empty())
        {
            log::warn("user menus: skipped, {}", p);
            continue;
        }
        seen.push_back(menu);
        std::string title = str(menu, "title");
        std::string parent = str(menu, "parent");
        std::string after = str(menu, "after");
        if (parent.empty())
        {
            menus::Menu m{.id = menuId(menu), .title = title, .before = str(menu, "before")};
            for (const json& s : menu.value("sets", json::array()))
                if (s.is_string())
                    m.sets.push_back(s.get<std::string>());
            menus::addMenu(std::move(m), kOwner);
        }
        else
        {
            menus::Item heading{.id = menuId(menu), .menu = parent, .label = title, .after = after};
            heading.kind = asSection(menu) ? menus::Kind::Section : menus::Kind::Submenu;
            after = menus::addItem(std::move(heading), kOwner);
        }
        addRows(menu, menu.value("items", json::array()), rowsPath(menu), asSection(menu) ? after : std::string(), used);
    }
}

void install()
{
    registerCommands();
    apply();
    PrefStore::get().onChanged([](const std::string& file) {
        if (file == kPrefFile)
            apply();
    });
}
} // namespace lila::ui::userMenus
