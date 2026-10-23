#include "scene/Picker.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/ExtensionContext.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Poses.h"
#include "scene/Rig.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace lila::scene::picker
{
namespace
{
struct Owned
{
    Document doc;
    std::string owner;
};

struct State
{
    std::vector<Document> scene;
    std::vector<Owned> templates;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}

/** Swaps the scene documents whole, so undo and redo are the same operation. */
struct SwapAction final : UndoAction
{
    std::vector<Document> other;
    explicit SwapAction(std::vector<Document> before) : other(std::move(before)) {}
    void undo() override
    {
        std::swap(state().scene, other);
        notify();
    }
    void redo() override { undo(); }
    size_t bytes() const override
    {
        size_t n = sizeof(*this);
        for (const Document& d : other)
            for (const Page& p : d.pages)
                n += sizeof(Page) + p.background.size() + p.items.size() * 256;
        return n;
    }
};

void replaceScene(std::vector<Document> next)
{
    UndoStack::get().record(std::make_unique<SwapAction>(state().scene));
    state().scene = std::move(next);
    notify();
}

template <class T> T value(const json& j, const char* key, const T& fallback)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
        return fallback;
    return it->template get<T>();
}

const std::pair<const char*, const char*> kSidePairs[] = {
    {"_l", "_r"}, {"_L", "_R"}, {"_left", "_right"}, {"_Left", "_Right"}, {"Left", "Right"}, {"left", "right"}, {"L_", "R_"}, {"l_", "r_"},
};

/** -1 left, 1 right, 0 centre. */
int sideOf(const std::string& name)
{
    std::string bare = poses::stripNamespace(name);
    for (auto [left, right] : kSidePairs)
    {
        if (rig::mirrorName(bare, left, right) != bare)
            return -1;
        if (rig::mirrorName(bare, right, left) != bare)
            return 1;
    }
    return 0;
}

double number(const json& v)
{
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_number())
        return v.get<double>();
    return 0.0;
}

bool sameValue(const json& a, const json& b)
{
    if ((a.is_number() || a.is_boolean()) && (b.is_number() || b.is_boolean()))
        return std::abs(number(a) - number(b)) < 1e-9;
    return a == b;
}
} // namespace

const char* shapeName(Shape s)
{
    switch (s)
    {
    case Shape::Rect: return "rect";
    case Shape::Circle: return "circle";
    case Shape::Polygon: return "polygon";
    case Shape::Text: return "text";
    case Shape::Image: return "image";
    }
    return "rect";
}

std::optional<Shape> shapeFromName(std::string_view name)
{
    for (Shape s : {Shape::Rect, Shape::Circle, Shape::Polygon, Shape::Text, Shape::Image})
        if (name == shapeName(s))
            return s;
    return std::nullopt;
}

const char* actionName(ActionKind k)
{
    switch (k)
    {
    case ActionKind::Select: return "select";
    case ActionKind::ToggleAttr: return "toggleAttr";
    case ActionKind::ApplyPose: return "applyPose";
    case ActionKind::RunCommand: return "runCommand";
    }
    return "select";
}

std::optional<ActionKind> actionFromName(std::string_view name)
{
    for (ActionKind k : {ActionKind::Select, ActionKind::ToggleAttr, ActionKind::ApplyPose, ActionKind::RunCommand})
        if (name == actionName(k))
            return k;
    return std::nullopt;
}

json Action::toJson() const
{
    json j = {{"kind", actionName(kind)}};
    switch (kind)
    {
    case ActionKind::Select: break;
    case ActionKind::ToggleAttr:
        j["attr"] = attr;
        j["values"] = values;
        break;
    case ActionKind::ApplyPose:
        j["pose"] = pose;
        j["weight"] = weight;
        j["mirror"] = mirror;
        break;
    case ActionKind::RunCommand:
        j["command"] = command;
        j["call"] = call;
        break;
    }
    return j;
}

Action Action::fromJson(const json& j)
{
    Action a;
    if (j.is_string())
    {
        a.kind = actionFromName(j.get<std::string>()).value_or(ActionKind::Select);
        return a;
    }
    if (!j.is_object())
        return a;
    std::string kind = value<std::string>(j, "kind", "select");
    std::optional<ActionKind> k = actionFromName(kind);
    if (!k)
        throw std::invalid_argument("unknown picker action '" + kind + "'");
    a.kind = *k;
    a.attr = value<std::string>(j, "attr", "");
    if (j.contains("values") && j["values"].is_array() && j["values"].size() == 2)
        a.values = j["values"];
    a.pose = value<std::string>(j, "pose", "");
    a.weight = value<double>(j, "weight", 1.0);
    a.mirror = value<bool>(j, "mirror", false);
    a.command = value<std::string>(j, "command", "");
    if (j.contains("call") && j["call"].is_object())
        a.call = j["call"];
    return a;
}

json Item::toJson() const
{
    json j = {{"id", id}, {"shape", shapeName(shape)}, {"x", x}, {"y", y}, {"w", w}, {"h", h}, {"color", color}};
    if (!points.empty())
    {
        json pts = json::array();
        for (const Point& p : points)
            pts.push_back({p.x, p.y});
        j["points"] = std::move(pts);
    }
    if (!textColor.empty())
        j["textColor"] = textColor;
    if (!label.empty())
        j["label"] = label;
    if (!image.empty())
        j["image"] = image;
    j["targets"] = targets;
    j["action"] = action.toJson();
    return j;
}

Item Item::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a picker item is an object");
    Item it;
    it.id = value<std::string>(j, "id", "");
    std::string shape = value<std::string>(j, "shape", "rect");
    std::optional<Shape> s = shapeFromName(shape);
    if (!s)
        throw std::invalid_argument("unknown picker shape '" + shape + "'");
    it.shape = *s;
    it.x = value<double>(j, "x", 0.0);
    it.y = value<double>(j, "y", 0.0);
    it.w = std::max(1.0, value<double>(j, "w", 24.0));
    it.h = std::max(1.0, value<double>(j, "h", 24.0));
    if (j.contains("points") && j["points"].is_array())
        for (const json& p : j["points"])
            if (p.is_array() && p.size() >= 2)
                it.points.push_back({p[0].get<double>(), p[1].get<double>()});
    it.color = value<std::string>(j, "color", "#808080");
    it.textColor = value<std::string>(j, "textColor", "");
    it.label = value<std::string>(j, "label", "");
    it.image = value<std::string>(j, "image", "");
    if (j.contains("targets") && j["targets"].is_array())
        for (const json& t : j["targets"])
            if (t.is_string())
                it.targets.push_back(t.get<std::string>());
    if (j.contains("action"))
        it.action = Action::fromJson(j["action"]);
    return it;
}

bool Item::contains(double px, double py) const
{
    if (px < x || py < y || px > x + w || py > y + h)
        return false;
    if (shape == Shape::Circle)
    {
        double rx = w * 0.5, ry = h * 0.5;
        double dx = (px - x - rx) / rx, dy = (py - y - ry) / ry;
        return dx * dx + dy * dy <= 1.0;
    }
    if (shape == Shape::Polygon && points.size() >= 3)
    {
        bool inside = false;
        for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
        {
            double xi = x + points[i].x, yi = y + points[i].y, xj = x + points[j].x, yj = y + points[j].y;
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi)
                inside = !inside;
        }
        return inside;
    }
    return true;
}

json Page::toJson() const
{
    json list = json::array();
    for (const Item& i : items)
        list.push_back(i.toJson());
    json j = {{"name", name}, {"width", width}, {"height", height}, {"items", std::move(list)}};
    if (!background.empty())
    {
        j["background"] = background;
        j["backgroundOpacity"] = backgroundOpacity;
    }
    return j;
}

Page Page::fromJson(const json& j)
{
    if (!j.is_object())
        throw std::invalid_argument("a picker page is an object");
    Page p;
    p.name = value<std::string>(j, "name", "Main");
    p.background = value<std::string>(j, "background", "");
    p.backgroundOpacity = std::clamp(value<double>(j, "backgroundOpacity", 1.0), 0.0, 1.0);
    p.width = std::max(1.0, value<double>(j, "width", 400.0));
    p.height = std::max(1.0, value<double>(j, "height", 600.0));
    if (j.contains("items") && j["items"].is_array())
        for (const json& i : j["items"])
            p.items.push_back(Item::fromJson(i));
    return p;
}

Item* Page::item(const std::string& id)
{
    for (Item& i : items)
        if (i.id == id)
            return &i;
    return nullptr;
}

const Item* Page::item(const std::string& id) const
{
    return const_cast<Page*>(this)->item(id);
}

const Item* Page::itemAt(double px, double py) const
{
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if (it->contains(px, py))
            return &*it;
    return nullptr;
}

std::vector<std::string> Page::itemsIn(double x0, double y0, double x1, double y1) const
{
    double lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    std::vector<std::string> out;
    for (const Item& i : items)
        if (i.x <= hx && i.x + i.w >= lx && i.y <= hy && i.y + i.h >= ly)
            out.push_back(i.id);
    return out;
}

json Document::toJson() const
{
    json pagesJ = json::array();
    for (const Page& p : pages)
        pagesJ.push_back(p.toJson());
    json j = {{"picker", 1}, {"name", name}, {"pages", std::move(pagesJ)}};
    if (!character.empty())
        j["character"] = character;
    if (!templateId.empty())
        j["template"] = templateId;
    if (!nameSpace.empty())
        j["namespace"] = nameSpace;
    return j;
}

Document Document::fromJson(const json& j)
{
    if (!j.is_object() || !j.contains("pages") || !j["pages"].is_array())
        throw std::invalid_argument("not a picker document (no pages)");
    Document d;
    d.name = value<std::string>(j, "name", "");
    d.character = value<std::string>(j, "character", "");
    d.templateId = value<std::string>(j, "template", "");
    d.nameSpace = value<std::string>(j, "namespace", "");
    for (const json& p : j["pages"])
        d.pages.push_back(Page::fromJson(p));
    // Ids must be unique across the document so commands can address an item without its page.
    std::set<std::string> seen;
    for (Page& p : d.pages)
        for (Item& i : p.items)
            if (i.id.empty() || !seen.insert(i.id).second)
            {
                i.id = d.uniqueItemId();
                seen.insert(i.id);
            }
    return d;
}

Page* Document::page(const std::string& n)
{
    for (Page& p : pages)
        if (p.name == n)
            return &p;
    return nullptr;
}

const Page* Document::page(const std::string& n) const
{
    return const_cast<Document*>(this)->page(n);
}

Item* Document::item(const std::string& id, Page** pageOut)
{
    for (Page& p : pages)
        if (Item* i = p.item(id))
        {
            if (pageOut)
                *pageOut = &p;
            return i;
        }
    return nullptr;
}

const Item* Document::item(const std::string& id) const
{
    return const_cast<Document*>(this)->item(id);
}

std::string Document::uniqueItemId(std::string_view stem) const
{
    std::set<std::string> used;
    for (const Page& p : pages)
        for (const Item& i : p.items)
            used.insert(i.id);
    for (int n = 1;; ++n)
        if (std::string id = std::string(stem) + std::to_string(n); !used.contains(id))
            return id;
}

std::string namespaceOf(std::string_view name)
{
    size_t colon = name.rfind(':');
    return colon == std::string_view::npos ? std::string() : std::string(name.substr(0, colon));
}

NodeId resolve(Graph& g, const std::string& target, const std::string& nameSpace)
{
    const std::string bare = poses::stripNamespace(target);
    if (!nameSpace.empty())
    {
        Node* n = g.find(nameSpace + ":" + bare);
        return n ? n->id() : kNoNode;
    }
    if (Node* n = g.find(target))
        return n->id();
    NodeId only = kNoNode;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && poses::stripNamespace(n->name()) == bare)
        {
            if (only != kNoNode)
                return kNoNode;
            only = id;
        }
    return only;
}

std::vector<NodeId> resolveTargets(Graph& g, const Item& item, const std::string& nameSpace)
{
    std::vector<NodeId> out;
    for (const std::string& t : item.targets)
        if (NodeId id = resolve(g, t, nameSpace); id != kNoNode && std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    return out;
}

std::vector<std::string> namespacesFor(Graph& g, const Document& doc)
{
    std::set<std::string> targets;
    for (const Page& p : doc.pages)
        for (const Item& i : p.items)
            for (const std::string& t : i.targets)
                targets.insert(poses::stripNamespace(t));
    std::set<std::string> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && targets.contains(poses::stripNamespace(n->name())))
            out.insert(namespaceOf(n->name()));
    return {out.begin(), out.end()};
}

Highlight highlightOf(Graph& g, const Item& item, const std::string& nameSpace, const std::vector<NodeId>& selection)
{
    std::vector<NodeId> ids = resolveTargets(g, item, nameSpace);
    if (ids.empty())
        return Highlight::None;
    size_t hit = 0;
    for (NodeId id : ids)
        if (std::find(selection.begin(), selection.end(), id) != selection.end())
            ++hit;
    return hit == 0 ? Highlight::None : hit == ids.size() ? Highlight::Full : Highlight::Partial;
}

std::vector<NodeId> selectionFor(Graph& g, const Page& page, const std::vector<std::string>& itemIds, const std::string& nameSpace)
{
    std::vector<NodeId> out;
    for (const std::string& id : itemIds)
        if (const Item* i = page.item(id))
            for (NodeId n : resolveTargets(g, *i, nameSpace))
                if (std::find(out.begin(), out.end(), n) == out.end())
                    out.push_back(n);
    return out;
}

Item mirrored(const Item& item, double axis)
{
    Item m = item;
    m.x = 2.0 * axis - (item.x + item.w);
    for (Point& p : m.points)
        p.x = item.w - p.x;
    m.targets.clear();
    for (const std::string& t : item.targets)
        m.targets.push_back(poses::mirrorNodeName(t));
    m.label = poses::mirrorNodeName(item.label);
    if (m.action.kind == ActionKind::ApplyPose)
        m.action.mirror = !m.action.mirror;
    if (m.action.kind == ActionKind::ToggleAttr)
        m.action.attr = poses::mirrorNodeName(m.action.attr);
    int side = item.targets.empty() ? 0 : sideOf(item.targets.front());
    if (side != 0 && item.color == sideColor(item.targets.front()))
        m.color = sideColor(m.targets.front());
    return m;
}

void align(std::vector<Item*>& items, std::string_view mode)
{
    if (items.size() < 2)
        return;
    double lo = 1e300, hi = -1e300;
    const bool horizontal = mode == "left" || mode == "right" || mode == "hcenter";
    if (!horizontal && mode != "top" && mode != "bottom" && mode != "vcenter")
        throw std::invalid_argument("align: mode is left, right, top, bottom, hcenter or vcenter");
    for (Item* i : items)
    {
        double a = horizontal ? i->x : i->y, b = a + (horizontal ? i->w : i->h);
        lo = std::min(lo, a);
        hi = std::max(hi, b);
    }
    for (Item* i : items)
    {
        double& pos = horizontal ? i->x : i->y;
        double size = horizontal ? i->w : i->h;
        if (mode == "left" || mode == "top")
            pos = lo;
        else if (mode == "right" || mode == "bottom")
            pos = hi - size;
        else
            pos = (lo + hi) * 0.5 - size * 0.5;
    }
}

void distribute(std::vector<Item*>& items, std::string_view axis)
{
    const bool horizontal = axis == "horizontal" || axis == "h" || axis == "x";
    if (!horizontal && axis != "vertical" && axis != "v" && axis != "y")
        throw std::invalid_argument("distribute: axis is horizontal or vertical");
    if (items.size() < 3)
        return;
    std::vector<Item*> sorted = items;
    auto start = [&](const Item* i) { return horizontal ? i->x : i->y; };
    auto size = [&](const Item* i) { return horizontal ? i->w : i->h; };
    std::stable_sort(sorted.begin(), sorted.end(), [&](Item* a, Item* b) { return start(a) < start(b); });
    double first = start(sorted.front()), last = start(sorted.back()) + size(sorted.back());
    double total = 0.0;
    for (Item* i : sorted)
        total += size(i);
    double gap = (last - first - total) / double(sorted.size() - 1);
    double at = first;
    for (Item* i : sorted)
    {
        (horizontal ? i->x : i->y) = at;
        at += size(i) + gap;
    }
}

double snap(double v, double grid)
{
    return grid > 0.0 ? std::round(v / grid) * grid : v;
}

std::string sideColor(const std::string& nodeName)
{
    switch (sideOf(nodeName))
    {
    case -1: return "#3d7be0";
    case 1: return "#e04848";
    default: return "#e0c040";
    }
}

Document generateFromLayout(const json& layout)
{
    if (!layout.is_object())
        throw std::invalid_argument("a picker layout is an object");
    Document d;
    d.name = value<std::string>(layout, "name", "");
    d.character = value<std::string>(layout, "character", "");
    d.templateId = value<std::string>(layout, "template", "");
    d.nameSpace = value<std::string>(layout, "namespace", "");
    const double grid = value<double>(layout, "grid", 1.0);
    json pages = layout.contains("pages") ? layout["pages"] : json::array({{{"name", "Main"}, {"items", layout.value("items", json::array())}}});
    for (const json& pj : pages)
    {
        Page p;
        p.name = value<std::string>(pj, "name", "Main");
        p.background = value<std::string>(pj, "background", "");
        p.backgroundOpacity = value<double>(pj, "backgroundOpacity", 1.0);
        p.width = value<double>(pj, "width", 400.0);
        p.height = value<double>(pj, "height", 600.0);
        std::vector<Item> mirrors;
        for (const json& ij : pj.value("items", json::array()))
        {
            Item it;
            if (ij.contains("targets") && ij["targets"].is_array())
                for (const json& t : ij["targets"])
                    it.targets.push_back(t.get<std::string>());
            else if (ij.contains("target"))
                it.targets.push_back(ij["target"].get<std::string>());
            if (std::optional<Shape> s = shapeFromName(value<std::string>(ij, "shape", "rect")))
                it.shape = *s;
            else
                throw std::invalid_argument("unknown picker shape '" + ij["shape"].dump() + "'");
            double w = 1.0, h = 1.0;
            if (ij.contains("size") && ij["size"].is_array() && ij["size"].size() == 2)
                w = ij["size"][0].get<double>(), h = ij["size"][1].get<double>();
            else if (ij.contains("size"))
                w = h = ij["size"].get<double>();
            else
                w = h = 24.0 / grid;
            it.w = w * grid;
            it.h = h * grid;
            // Layout positions are centres, which mirror and align with the page centre more naturally.
            it.x = value<double>(ij, "x", 0.0) * grid - it.w * 0.5;
            it.y = value<double>(ij, "y", 0.0) * grid - it.h * 0.5;
            it.label = value<std::string>(ij, "label", "");
            if (ij.contains("points") && ij["points"].is_array())
                for (const json& pt : ij["points"])
                    it.points.push_back({pt[0].get<double>() * grid, pt[1].get<double>() * grid});
            it.image = value<std::string>(ij, "image", "");
            it.color = ij.contains("color") ? ij["color"].get<std::string>() : it.targets.empty() ? std::string("#808080") : sideColor(it.targets.front());
            if (ij.contains("action"))
                it.action = Action::fromJson(ij["action"]);
            p.items.push_back(it);
            if (value<bool>(ij, "mirror", false))
                mirrors.push_back(mirrored(it, p.width * 0.5));
        }
        d.pages.push_back(std::move(p));
        for (Item& it : d.pages.back().items)
            it.id = d.uniqueItemId();
        for (Item& m : mirrors)
        {
            m.id = d.uniqueItemId();
            d.pages.back().items.push_back(std::move(m));
        }
    }
    if (d.pages.empty())
        d.pages.push_back({});
    return d;
}

std::vector<Document> scenePickers()
{
    return state().scene;
}

const Document* findScene(const std::string& name)
{
    for (const Document& d : state().scene)
        if (d.name == name)
            return &d;
    return nullptr;
}

std::optional<Document> find(const std::string& name)
{
    if (const Document* d = findScene(name))
        return *d;
    for (const Owned& o : state().templates)
        if (o.doc.name == name)
            return o.doc;
    return std::nullopt;
}

void store(const Document& doc)
{
    std::vector<Document> next = state().scene;
    auto it = std::find_if(next.begin(), next.end(), [&](const Document& d) { return d.name == doc.name; });
    if (it != next.end())
        *it = doc;
    else
        next.push_back(doc);
    replaceScene(std::move(next));
}

bool removeScene(const std::string& name)
{
    std::vector<Document> next = state().scene;
    if (std::erase_if(next, [&](const Document& d) { return d.name == name; }) == 0)
        return false;
    replaceScene(std::move(next));
    return true;
}

void registerTemplatePicker(const Document& doc, const std::string& owner)
{
    auto& t = state().templates;
    std::erase_if(t, [&](const Owned& o) { return o.doc.name == doc.name; });
    t.push_back({doc, owner});
    notify();
}

void registerTemplatePicker(ext::Context& ctx, const Document& doc)
{
    registerTemplatePicker(doc, ctx.owner());
    std::string name = doc.name, owner = ctx.owner();
    ctx.track("picker:" + name, [name, owner] {
        auto& t = state().templates;
        if (std::erase_if(t, [&](const Owned& o) { return o.doc.name == name && o.owner == owner; }) > 0)
            notify();
    });
}

void unregisterTemplatePicker(const std::string& name)
{
    if (std::erase_if(state().templates, [&](const Owned& o) { return o.doc.name == name; }) > 0)
        notify();
}

void unregisterOwned(const std::string& owner)
{
    if (std::erase_if(state().templates, [&](const Owned& o) { return o.owner == owner; }) > 0)
        notify();
}

std::vector<Document> templatePickers()
{
    std::vector<Document> out;
    for (const Owned& o : state().templates)
        out.push_back(o.doc);
    return out;
}

Document readFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("can't read " + paths::toUtf8(file));
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error(paths::toUtf8(file) + " is not JSON");
    Document d;
    try
    {
        d = Document::fromJson(j);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(paths::toUtf8(file) + ": " + e.what());
    }
    if (d.name.empty())
        d.name = paths::toUtf8(file.stem());
    return d;
}

void writeFile(const Document& doc, const fs::path& file)
{
    std::error_code ec;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("can't write " + paths::toUtf8(file));
    out << doc.toJson().dump(2);
}

json sceneToJson()
{
    json out = json::array();
    for (const Document& d : state().scene)
        out.push_back(d.toJson());
    return out;
}

void sceneFromJson(const json& j)
{
    std::vector<Document> next;
    if (j.is_array())
        for (const json& d : j)
            try
            {
                next.push_back(Document::fromJson(d));
            }
            catch (const std::exception&)
            {
            }
    state().scene = std::move(next);
    notify();
}

void clearScene()
{
    if (state().scene.empty())
        return;
    state().scene.clear();
    notify();
}

int addListener(std::function<void()> fn)
{
    State& s = state();
    int id = s.nextListener++;
    s.listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}

namespace
{
Graph& graph() { return Graph::current(); }

std::string requireName(const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
        throw CommandError(std::string(cmd) + ": name the picker");
    return a.positional[0].get<std::string>();
}

std::vector<std::string> strings(const Args& a, const char* flag)
{
    std::vector<std::string> out;
    if (!a.has(flag))
        return out;
    json v = a.flags[flag];
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& s : v)
            if (s.is_string())
                out.push_back(s.get<std::string>());
    return out;
}

Document requireDoc(const std::string& name, const char* cmd)
{
    std::optional<Document> d = find(name);
    if (!d)
        throw CommandError(std::string(cmd) + ": no picker named '" + name + "'");
    return *d;
}

Document requireSceneDoc(const std::string& name, const char* cmd)
{
    const Document* d = findScene(name);
    if (!d)
    {
        if (find(name))
            throw CommandError(std::string(cmd) + ": '" + name + "' is a template picker; createPicker(fromTemplate=...) makes an editable copy");
        throw CommandError(std::string(cmd) + ": no picker named '" + name + "'");
    }
    return *d;
}

Page& pageFor(Document& d, const Args& a, const char* cmd)
{
    if (d.pages.empty())
        d.pages.push_back({});
    if (!a.has("page"))
        return d.pages.front();
    std::string name = a.get<std::string>("page");
    if (Page* p = d.page(name))
        return *p;
    throw CommandError(std::string(cmd) + ": no page '" + name + "' in '" + d.name + "'");
}

std::string viewNamespace(const Document& d, const Args& a)
{
    return a.has("namespace") ? a.get<std::string>("namespace") : d.nameSpace;
}

std::vector<Item*> itemsNamed(Document& d, const std::vector<std::string>& ids, const char* cmd)
{
    std::vector<Item*> out;
    for (const std::string& id : ids)
    {
        Item* i = d.item(id);
        if (!i)
            throw CommandError(std::string(cmd) + ": no item '" + id + "'");
        out.push_back(i);
    }
    return out;
}

json summary(const Document& d, const char* source)
{
    json pages = json::array();
    size_t items = 0;
    for (const Page& p : d.pages)
    {
        pages.push_back(p.name);
        items += p.items.size();
    }
    return {{"name", d.name}, {"source", source}, {"character", d.character}, {"template", d.templateId},
            {"namespace", d.nameSpace}, {"pages", pages}, {"items", items}};
}

json runAction(const Item& item, const std::vector<NodeId>& ids)
{
    std::vector<std::string> names;
    for (NodeId id : ids)
        names.push_back(graph().node(id)->name());
    const Action& act = item.action;
    switch (act.kind)
    {
    case ActionKind::Select:
        graph().select(ids, SelectMode::Replace);
        return names;
    case ActionKind::ToggleAttr:
    {
        if (act.attr.empty())
            throw CommandError("pickerRun: item '" + item.id + "' toggles no attr");
        // Every target follows the first, so a mixed set lines up instead of each flipping its own way.
        json now = cmds::execute("getAttr", {{"args", {names.front() + "." + act.attr}}});
        json next = sameValue(now, act.values[1]) ? act.values[0] : act.values[1];
        for (const std::string& n : names)
            cmds::execute("setAttr", {{"args", {n + "." + act.attr, next}}});
        return next;
    }
    case ActionKind::ApplyPose:
        if (act.pose.empty())
            throw CommandError("pickerRun: item '" + item.id + "' names no pose");
        return cmds::execute("applyPose", {{"args", {act.pose}}, {"nodes", names}, {"weight", act.weight}, {"mirror", act.mirror}});
    case ActionKind::RunCommand:
    {
        if (act.command.empty())
            throw CommandError("pickerRun: item '" + item.id + "' names no command");
        json call = act.call.is_object() ? act.call : json::object();
        if (!call.contains("args"))
            call["args"] = names;
        return cmds::execute(act.command, call);
    }
    }
    return nullptr;
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "createPicker",
        .doc = "Make a control picker in the scene: empty, from a layout ({pages: [{items: [{target, x, y, size, mirror}]}]}), "
               "a copy of a template picker, or a .lilapicker file. Returns the picker's summary.",
        .positionalDoc = "name",
        .flags = {
            {"layout", "l", T::Any, nullptr, "A compact layout, as rig templates write them (see generateFromLayout)."},
            {"fromTemplate", "ft", T::String, nullptr, "Copy this template picker."},
            {"file", "f", T::String, nullptr, "Read a .lilapicker file."},
            {"character", "ch", T::String, nullptr, "The character (template id or root name) it drives."},
            {"namespace", "ns", T::String, nullptr, "Default namespace for its targets."},
            {"replace", "r", T::Bool, false, "Replace a scene picker of the same name."},
        },
        .run = [](const Args& a) -> json {
            Document d;
            try
            {
                if (a.has("fromTemplate"))
                {
                    std::string from = a.get<std::string>("fromTemplate");
                    auto t = templatePickers();
                    auto it = std::find_if(t.begin(), t.end(), [&](const Document& x) { return x.name == from; });
                    if (it == t.end())
                        throw CommandError("createPicker: no template picker '" + from + "'");
                    d = *it;
                }
                else if (a.has("file"))
                    d = readFile(paths::fromUtf8(a.get<std::string>("file")));
                else if (a.has("layout"))
                    d = generateFromLayout(a.flags["layout"]);
                else
                    d.pages.push_back({});
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("createPicker: ") + e.what());
            }
            if (!a.positional.empty() && a.positional[0].is_string() && !a.positional[0].get<std::string>().empty())
                d.name = a.positional[0].get<std::string>();
            if (d.name.empty())
                throw CommandError("createPicker: name the picker");
            if (a.has("character"))
                d.character = a.get<std::string>("character");
            if (a.has("namespace"))
                d.nameSpace = a.get<std::string>("namespace");
            if (findScene(d.name) && !a.get<bool>("replace"))
                throw CommandError("createPicker: a picker named '" + d.name + "' exists (replace=True overwrites it)");
            store(d);
            return summary(d, "scene");
        },
    });

    cmds::registerCommand({
        .name = "deletePicker",
        .doc = "Delete a scene picker. Template pickers belong to their extension and can't be deleted.",
        .positionalDoc = "name",
        .run = [](const Args& a) -> json {
            std::string name = requireName(a, "deletePicker");
            requireSceneDoc(name, "deletePicker");
            removeScene(name);
            return name;
        },
    });

    cmds::registerCommand({
        .name = "listPickers",
        .doc = "Scene and template pickers: [{name, source, character, template, namespace, pages, items}]. full=True "
               "returns whole documents; namespaces=True adds the scene namespaces each one's targets resolve in.",
        .flags = {
            {"source", "s", T::String, "all", "scene, template or all.", {"scene", "template", "all"}},
            {"full", "f", T::Bool, false, "Return the documents themselves."},
            {"namespaces", "ns", T::Bool, false, "Add the namespaces holding each picker's targets."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string source = a.get<std::string>("source", "all");
            json out = json::array();
            auto add = [&](const Document& d, const char* src) {
                json j = a.get<bool>("full") ? d.toJson() : summary(d, src);
                j["source"] = src;
                if (a.get<bool>("namespaces"))
                    j["namespaces"] = namespacesFor(graph(), d);
                out.push_back(std::move(j));
            };
            if (source != "template")
                for (const Document& d : state().scene)
                    add(d, "scene");
            if (source != "scene")
                for (const Owned& o : state().templates)
                    if (!findScene(o.doc.name) || source == "template")
                        add(o.doc, "template");
            return out;
        },
    });

    cmds::registerCommand({
        .name = "editPicker",
        .doc = "Edit a scene picker. Operations run in this order: rename/character/namespace, page add/remove/rename, "
               "setPage, addItem, addFromSelection, removeItems, setItems (patches by id), moveItems by offset, mirrorItems, "
               "align, distribute, snapGrid. Returns the edited document.",
        .positionalDoc = "name",
        .flags = {
            {"page", "p", T::String, nullptr, "Page the item operations work on; the first page by default."},
            {"rename", "rn", T::String, nullptr, "New picker name."},
            {"character", "ch", T::String, nullptr, "The character it drives."},
            {"namespace", "ns", T::String, nullptr, "Default namespace."},
            {"addPage", "ap", T::String, nullptr, "Add a page."},
            {"removePage", "rp", T::String, nullptr, "Remove a page and its items."},
            {"renamePage", "rpg", T::List, nullptr, "[from, to]."},
            {"setPage", "sp", T::Any, nullptr, "Page fields to change: {background, backgroundOpacity, width, height}."},
            {"addItem", "ai", T::Any, nullptr, "An item or a list of items (shape, x, y, w, h, color, label, targets, action)."},
            {"addFromSelection", "afs", T::Bool, false, "Add one item per selected node, laid out in a row below the page's items."},
            {"shape", "sh", T::String, "rect", "Shape for addFromSelection.", {"rect", "circle", "polygon", "text", "image"}},
            {"removeItems", "ri", T::List, nullptr, "Item ids to remove."},
            {"setItems", "si", T::Any, nullptr, "Patches: [{id, field: value, ...}]."},
            {"moveItems", "mi", T::List, nullptr, "Item ids to move by offset."},
            {"offset", "o", T::List, nullptr, "[dx, dy] for moveItems."},
            {"mirrorItems", "mir", T::List, nullptr, "Item ids to copy to the opposite side."},
            {"mirrorAxis", "ma", T::Float, nullptr, "Vertical line to mirror across; the page centre by default."},
            {"items", "it", T::List, nullptr, "Item ids for align, distribute and snapGrid."},
            {"align", "al", T::String, nullptr, "left, right, top, bottom, hcenter or vcenter.", {"left", "right", "top", "bottom", "hcenter", "vcenter"}},
            {"distribute", "di", T::String, nullptr, "horizontal or vertical.", {"horizontal", "vertical"}},
            {"snapGrid", "sg", T::Float, nullptr, "Snap the items' positions and sizes to this grid."},
        },
        .run = [](const Args& a) -> json {
            const char* cmd = "editPicker";
            std::string name = requireName(a, cmd);
            Document d = requireSceneDoc(name, cmd);
            try
            {
                if (a.has("rename"))
                {
                    std::string to = a.get<std::string>("rename");
                    if (to.empty())
                        throw CommandError("editPicker: rename to what?");
                    if (to != name && findScene(to))
                        throw CommandError("editPicker: a picker named '" + to + "' exists");
                    d.name = to;
                }
                if (a.has("character"))
                    d.character = a.get<std::string>("character");
                if (a.has("namespace"))
                    d.nameSpace = a.get<std::string>("namespace");
                if (a.has("addPage"))
                {
                    std::string pn = a.get<std::string>("addPage");
                    if (d.page(pn))
                        throw CommandError("editPicker: page '" + pn + "' exists");
                    Page p;
                    p.name = pn;
                    d.pages.push_back(p);
                }
                if (a.has("removePage"))
                {
                    std::string pn = a.get<std::string>("removePage");
                    if (std::erase_if(d.pages, [&](const Page& p) { return p.name == pn; }) == 0)
                        throw CommandError("editPicker: no page '" + pn + "'");
                    if (d.pages.empty())
                        d.pages.push_back({});
                }
                if (std::vector<std::string> rp = strings(a, "renamePage"); !rp.empty())
                {
                    if (rp.size() != 2)
                        throw CommandError("editPicker: renamePage takes [from, to]");
                    Page* p = d.page(rp[0]);
                    if (!p)
                        throw CommandError("editPicker: no page '" + rp[0] + "'");
                    if (rp[0] != rp[1] && d.page(rp[1]))
                        throw CommandError("editPicker: page '" + rp[1] + "' exists");
                    p->name = rp[1];
                }
                Page& page = pageFor(d, a, cmd);
                if (a.has("setPage"))
                {
                    json pj = page.toJson();
                    pj.merge_patch(a.flags["setPage"]);
                    pj["items"] = json::array();
                    Page next = Page::fromJson(pj);
                    next.items = std::move(page.items);
                    page = std::move(next);
                }
                json added = json::array();
                if (a.has("addItem"))
                {
                    json list = a.flags["addItem"].is_array() ? a.flags["addItem"] : json::array({a.flags["addItem"]});
                    for (const json& ij : list)
                    {
                        Item it = Item::fromJson(ij);
                        if (it.id.empty() || d.item(it.id))
                            it.id = d.uniqueItemId();
                        added.push_back(it.id);
                        page.items.push_back(std::move(it));
                    }
                }
                if (a.get<bool>("addFromSelection"))
                {
                    double y = 8.0;
                    for (const Item& i : page.items)
                        y = std::max(y, i.y + i.h + 8.0);
                    double x = 8.0;
                    Shape shape = shapeFromName(a.get<std::string>("shape", "rect")).value_or(Shape::Rect);
                    for (NodeId id : graph().selection())
                    {
                        Node* n = graph().node(id);
                        if (!n)
                            continue;
                        Item it;
                        it.id = d.uniqueItemId();
                        it.shape = shape;
                        it.x = x;
                        it.y = y;
                        std::string bare = poses::stripNamespace(n->name());
                        it.targets = {bare};
                        it.label = bare;
                        it.color = sideColor(bare);
                        added.push_back(it.id);
                        page.items.push_back(std::move(it));
                        x += 32.0;
                        if (x + 24.0 > page.width)
                            x = 8.0, y += 32.0;
                    }
                }
                for (const std::string& id : strings(a, "removeItems"))
                {
                    Page* holder = nullptr;
                    if (!d.item(id, &holder))
                        throw CommandError("editPicker: no item '" + id + "'");
                    std::erase_if(holder->items, [&](const Item& i) { return i.id == id; });
                }
                if (a.has("setItems"))
                {
                    json list = a.flags["setItems"].is_array() ? a.flags["setItems"] : json::array({a.flags["setItems"]});
                    for (const json& patch : list)
                    {
                        std::string id = value<std::string>(patch, "id", "");
                        Item* it = d.item(id);
                        if (!it)
                            throw CommandError("editPicker: no item '" + id + "'");
                        json ij = it->toJson();
                        ij.merge_patch(patch);
                        // merge_patch merges objects, but a new action replaces the old one outright.
                        if (patch.contains("action"))
                            ij["action"] = patch["action"];
                        ij["id"] = id;
                        *it = Item::fromJson(ij);
                    }
                }
                if (std::vector<std::string> ids = strings(a, "moveItems"); !ids.empty())
                {
                    json off = a.has("offset") ? a.flags["offset"] : json::array({0.0, 0.0});
                    if (!off.is_array() || off.size() != 2)
                        throw CommandError("editPicker: offset is [dx, dy]");
                    for (Item* it : itemsNamed(d, ids, cmd))
                    {
                        it->x += off[0].get<double>();
                        it->y += off[1].get<double>();
                    }
                }
                if (std::vector<std::string> ids = strings(a, "mirrorItems"); !ids.empty())
                {
                    double axis = a.has("mirrorAxis") ? a.get<double>("mirrorAxis") : page.width * 0.5;
                    std::vector<Item> copies;
                    for (Item* it : itemsNamed(d, ids, cmd))
                        copies.push_back(mirrored(*it, axis));
                    for (Item& c : copies)
                    {
                        c.id = d.uniqueItemId();
                        added.push_back(c.id);
                        page.items.push_back(std::move(c));
                    }
                }
                std::vector<std::string> targetIds = strings(a, "items");
                if (a.has("align"))
                {
                    std::vector<Item*> items = itemsNamed(d, targetIds, cmd);
                    align(items, a.get<std::string>("align"));
                }
                if (a.has("distribute"))
                {
                    std::vector<Item*> items = itemsNamed(d, targetIds, cmd);
                    distribute(items, a.get<std::string>("distribute"));
                }
                if (a.has("snapGrid"))
                {
                    double g = a.get<double>("snapGrid");
                    std::vector<std::string> ids = targetIds.empty() ? strings(a, "moveItems") : targetIds;
                    for (Item* it : itemsNamed(d, ids, cmd))
                    {
                        it->x = snap(it->x, g);
                        it->y = snap(it->y, g);
                        it->w = std::max(g, snap(it->w, g));
                        it->h = std::max(g, snap(it->h, g));
                    }
                }
                std::vector<Document> next = state().scene;
                for (Document& x : next)
                    if (x.name == name)
                        x = d;
                replaceScene(std::move(next));
                json out = d.toJson();
                out["added"] = added;
                return out;
            }
            catch (const CommandError&)
            {
                throw;
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("editPicker: ") + e.what());
            }
        },
    });

    cmds::registerCommand({
        .name = "pickerSelect",
        .doc = "Select the nodes a picker's items target, resolved in a namespace (the picker's own by default). mode "
               "replace, add, remove or toggle; no items with replace clears the selection. Returns the selected names.",
        .positionalDoc = "picker",
        .flags = {
            {"items", "it", T::List, nullptr, "Item ids."},
            {"page", "p", T::String, nullptr, "Page holding the items; any page by default."},
            {"namespace", "ns", T::String, nullptr, "Namespace to resolve targets in."},
            {"mode", "m", T::String, "replace", "replace, add, remove or toggle.", {"replace", "add", "remove", "toggle"}},
        },
        .run = [](const Args& a) -> json {
            Document d = requireDoc(requireName(a, "pickerSelect"), "pickerSelect");
            std::string ns = viewNamespace(d, a);
            std::vector<std::string> ids = strings(a, "items");
            std::vector<NodeId> nodes;
            for (const std::string& id : ids)
            {
                Page* holder = nullptr;
                if (a.has("page"))
                    holder = d.page(a.get<std::string>("page"));
                else
                    d.item(id, &holder);
                if (!holder || !holder->item(id))
                    throw CommandError("pickerSelect: no item '" + id + "'");
                for (NodeId n : selectionFor(graph(), *holder, {id}, ns))
                    if (std::find(nodes.begin(), nodes.end(), n) == nodes.end())
                        nodes.push_back(n);
            }
            std::string mode = a.get<std::string>("mode", "replace");
            SelectMode m = mode == "add" ? SelectMode::Add : mode == "remove" ? SelectMode::Remove : mode == "toggle" ? SelectMode::Toggle : SelectMode::Replace;
            graph().select(nodes, m);
            json out = json::array();
            for (NodeId id : graph().selection())
                if (Node* n = graph().node(id))
                    out.push_back(n->name());
            return out;
        },
    });

    cmds::registerCommand({
        .name = "pickerRun",
        .doc = "Run a picker item's action on its targets resolved in a namespace: select them, toggle an attr (every "
               "target follows the first), apply a pose to their character, or run a command (args default to the "
               "targets). Returns what the action returned.",
        .positionalDoc = "picker item",
        .flags = {{"namespace", "ns", T::String, nullptr, "Namespace to resolve targets in."}},
        .run = [](const Args& a) -> json {
            Document d = requireDoc(requireName(a, "pickerRun"), "pickerRun");
            if (a.positional.size() < 2 || !a.positional[1].is_string())
                throw CommandError("pickerRun: name the item");
            std::string id = a.positional[1].get<std::string>();
            Item* it = d.item(id);
            if (!it)
                throw CommandError("pickerRun: no item '" + id + "'");
            std::vector<NodeId> ids = resolveTargets(graph(), *it, viewNamespace(d, a));
            if (ids.empty() && it->action.kind != ActionKind::RunCommand)
                throw CommandError("pickerRun: none of item '" + id + "''s targets are in the scene");
            return runAction(*it, ids);
        },
    });

    cmds::registerCommand({
        .name = "exportPicker",
        .doc = "Write a picker (scene or template) to a .lilapicker file. Returns the path.",
        .positionalDoc = "name",
        .flags = {{"file", "f", T::String, nullptr, "Where to write it."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Document d = requireDoc(requireName(a, "exportPicker"), "exportPicker");
            if (!a.has("file"))
                throw CommandError("exportPicker: file= is required");
            fs::path file = paths::fromUtf8(a.get<std::string>("file"));
            if (file.extension().empty())
                file += ".lilapicker";
            try
            {
                writeFile(d, file);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("exportPicker: ") + e.what());
            }
            return paths::toUtf8(file);
        },
    });

    cmds::registerCommand({
        .name = "importPicker",
        .doc = "Read a .lilapicker file into the scene, renamed with name= or replacing one of the same name with "
               "replace=True. Returns the picker's summary.",
        .positionalDoc = "file",
        .flags = {
            {"name", "n", T::String, nullptr, "Name in the scene; the file's own by default."},
            {"replace", "r", T::Bool, false, "Replace a scene picker of the same name."},
        },
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("importPicker: name the file");
            Document d;
            try
            {
                d = readFile(paths::fromUtf8(a.positional[0].get<std::string>()));
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("importPicker: ") + e.what());
            }
            if (a.has("name"))
                d.name = a.get<std::string>("name");
            if (findScene(d.name) && !a.get<bool>("replace"))
                throw CommandError("importPicker: a picker named '" + d.name + "' exists (replace=True overwrites it)");
            store(d);
            return summary(d, "scene");
        },
    });
}
} // namespace lila::scene::picker
