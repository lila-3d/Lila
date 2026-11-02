#include "scene/ComponentValues.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Skin.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lila::scene::componentValues
{
namespace
{
using T = ArgType;

enum class ColumnKind
{
    Weight,
    Position,
    Color,
};

/** One column: a skin influence (by joint path), a position axis or a colour channel. */
struct SheetColumn
{
    std::string id;
    std::string label;
    ColumnKind kind = ColumnKind::Weight;
    int channel = 0;
};

struct SheetRow
{
    NodeId mesh = kNoNode;
    uint32_t point = 0;
};

/** What one mesh brings to the sheet, read once per call. */
struct SheetMesh
{
    NodeId mesh = kNoNode;
    NodeId skin = kNoNode;
    std::vector<skin::Influence> infs;
    std::optional<WeightTable> table;
    MeshPtr cage;
    std::string colorSet;
    std::vector<glm::vec4> colors;
};

constexpr const char* kAxes[] = {"x", "y", "z"};
constexpr const char* kChannels[] = {"r", "g", "b", "a"};

Node& sheetMeshNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (!n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** The point selection a mesh shows: the current mode's components as points, or the kept vertex selection in object mode. */
components::Ids selectedPointsOf(NodeId mesh)
{
    if (components::mode() != ComponentMode::Object)
        return components::pointsOf(mesh);
    components::Ids ids = components::selected(mesh, ComponentMode::Vertex);
    std::ranges::sort(ids);
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

/** The given meshes, else every mesh with selected components, the object selection's order first. */
std::vector<NodeId> sheetMeshes(Graph& g, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const json& v : a.positional)
    {
        NodeId id = sheetMeshNamed(g, v.get<std::string>(), cmd).id();
        if (std::ranges::find(out, id) == out.end())
            out.push_back(id);
    }
    if (!out.empty())
        return out;
    ComponentMode kind = components::mode() == ComponentMode::Object ? ComponentMode::Vertex : components::mode();
    std::vector<NodeId> withSel;
    for (const auto& [node, ids] : components::selected(kind))
        if (Node* n = g.node(node); n && n->type().isA("mesh") && !ids.empty())
            withSel.push_back(node);
    for (NodeId id : g.selection())
        if (std::ranges::find(withSel, id) != withSel.end())
            out.push_back(id);
    for (NodeId id : withSel)
        if (std::ranges::find(out, id) == out.end())
            out.push_back(id);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": select vertices or give a mesh");
    return out;
}

/** The vertices flag (ids for every mesh, or [mesh, id] pairs), else each mesh's selected points, else all points when allIfNone. */
std::vector<SheetRow> sheetRows(Graph& g, const Args& a, std::vector<NodeId>& meshes, bool allIfNone, const char* cmd)
{
    std::vector<SheetRow> rows;
    if (a.has("vertices"))
    {
        for (const json& v : a.flags["vertices"])
        {
            if (v.is_array() && v.size() == 2 && v[0].is_string())
            {
                NodeId id = sheetMeshNamed(g, v[0].get<std::string>(), cmd).id();
                if (std::ranges::find(meshes, id) == meshes.end())
                    meshes.push_back(id);
                rows.push_back({id, v[1].get<uint32_t>()});
            }
            else if (v.is_number_integer())
            {
                for (NodeId m : meshes)
                    rows.push_back({m, v.get<uint32_t>()});
            }
            else
                throw CommandError(std::string(cmd) + ": vertices are ids or [mesh, id] pairs");
        }
        return rows;
    }
    for (NodeId m : meshes)
    {
        components::Ids ids = selectedPointsOf(m);
        if (ids.empty() && allIfNone)
            if (MeshPtr cage = modifiers::cage(g, m))
                for (geo::Index p = 0; p < cage->pointCapacity(); ++p)
                    if (cage->pointValid(p))
                        ids.push_back(p);
        for (uint32_t p : ids)
            rows.push_back({m, p});
    }
    return rows;
}

SheetMesh readMesh(Graph& g, NodeId mesh, bool wantColors)
{
    SheetMesh s;
    s.mesh = mesh;
    s.cage = modifiers::cage(g, mesh);
    s.skin = skin::find(g, mesh);
    if (s.skin != kNoNode)
    {
        s.infs = skin::influences(g, s.skin);
        s.table = skin::table(g, s.skin);
    }
    if (wantColors && s.cage)
    {
        s.colorSet = geo::colors::current(*s.cage);
        if (!s.colorSet.empty())
            s.colors = geo::colors::pointColors(*s.cage, s.colorSet);
    }
    return s;
}

std::string jointKey(Graph& g, const skin::Influence& inf) { return inf.joint != kNoNode ? g.path(inf.joint) : inf.name; }

const skin::Influence* influenceFor(Graph& g, const SheetMesh& m, const std::string& column)
{
    for (const skin::Influence& i : m.infs)
        if (jointKey(g, i) == column || i.name == column)
            return &i;
    return nullptr;
}

/** "position.x", "px", "tx" or "x"; "color.r" or "r". */
std::optional<SheetColumn> parseFixedColumn(const std::string& id)
{
    for (int c = 0; c < 3; ++c)
    {
        std::string ax = kAxes[c];
        if (id == "position." + ax || id == "p" + ax || id == "t" + ax || id == ax)
            return SheetColumn{"position." + ax, std::string(1, char(std::toupper(ax[0]))), ColumnKind::Position, c};
    }
    for (int c = 0; c < 4; ++c)
    {
        std::string ch = kChannels[c];
        if (id == "color." + ch || id == "colour." + ch || id == ch)
            return SheetColumn{"color." + ch, std::string(1, char(std::toupper(ch[0]))), ColumnKind::Color, c};
    }
    return std::nullopt;
}

std::optional<double> cellValue(Graph& g, const SheetMesh& m, uint32_t point, const SheetColumn& col)
{
    switch (col.kind)
    {
    case ColumnKind::Weight:
    {
        if (!m.table || point >= m.table->points.size())
            return std::nullopt;
        const skin::Influence* inf = influenceFor(g, m, col.id);
        if (!inf)
            return std::nullopt;
        return double(m.table->get(point, inf->index));
    }
    case ColumnKind::Position:
        if (!m.cage || !m.cage->pointValid(point))
            return std::nullopt;
        return m.cage->position(point)[col.channel];
    case ColumnKind::Color:
        if (point >= m.colors.size())
            return std::nullopt;
        return double(m.colors[point][col.channel]);
    }
    return std::nullopt;
}

json columnJson(const SheetColumn& c)
{
    static const char* kinds[] = {"weight", "position", "color"};
    return {{"id", c.id}, {"label", c.label}, {"kind", kinds[int(c.kind)]}};
}

void registerQuery()
{
    ops::registerCommand({
        .name = "componentValues",
        .doc = "Per-vertex values of the selected (or given) components, one column per value: skin weights per influence, "
               "position, colour. Returns {rows: [[mesh, id], ...], columns: [{id, label, kind, ...}], values: [[...per column] per row]}; "
               "a value is None where the row's mesh has no such column.\n\n"
               "Weight columns carry joint, held (the influence's lockWeights), total (summed over the rows) and links (the "
               "skinInfluence nodes, one per mesh). With the weights filter only influences with weight on the rows appear, "
               "heaviest first.",
        .positionalDoc = "[meshes]",
        .flags = {
            {"vertices", "v", T::List, nullptr, "Point ids for every mesh, or [mesh, id] pairs; defaults to the selected components' points."},
            {"columns", "c", T::List, json::array({"weights", "position"}),
             "Which columns: weights (influences touching the rows), allWeights, position, color, or single ones: a joint, "
             "position.x, color.r."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            g.evaluate();
            std::vector<NodeId> meshes = sheetMeshes(g, a, "componentValues");
            std::vector<SheetRow> rows = sheetRows(g, a, meshes, !a.positional.empty(), "componentValues");
            std::vector<std::string> wanted;
            for (const json& c : a.flags["columns"])
                wanted.push_back(c.get<std::string>());
            auto wants = [&](const char* w) { return std::ranges::find(wanted, w) != wanted.end(); };
            bool anyColor = wants("color") || wants("colour") || std::ranges::any_of(wanted, [](const std::string& w) {
                                auto c = parseFixedColumn(w);
                                return c && c->kind == ColumnKind::Color;
                            });

            std::map<NodeId, SheetMesh> data;
            for (NodeId m : meshes)
                data.emplace(m, readMesh(g, m, anyColor));

            // Influences across the meshes, matched by joint path.
            struct InfInfo
            {
                std::string label;
                double total = 0.0;
                bool held = false;
                json links = json::array();
                std::string joint;
            };
            std::map<std::string, InfInfo> infs;
            std::vector<std::string> infOrder;
            for (NodeId m : meshes)
                for (const skin::Influence& i : data[m].infs)
                {
                    std::string key = jointKey(g, i);
                    auto [it, added] = infs.try_emplace(key);
                    if (added)
                    {
                        infOrder.push_back(key);
                        it->second.label = i.name;
                        it->second.joint = i.joint != kNoNode ? g.path(i.joint) : std::string();
                    }
                    it->second.held = it->second.held || i.locked;
                    it->second.links.push_back(g.path(i.link));
                }
            for (const SheetRow& r : rows)
            {
                const SheetMesh& m = data[r.mesh];
                if (!m.table || r.point >= m.table->points.size())
                    continue;
                for (const auto& [index, w] : m.table->points[r.point])
                    for (const skin::Influence& i : m.infs)
                        if (i.index == index)
                            infs[jointKey(g, i)].total += w;
            }

            std::vector<SheetColumn> cols;
            auto addColumn = [&](SheetColumn c) {
                if (std::ranges::none_of(cols, [&](const SheetColumn& x) { return x.id == c.id; }))
                    cols.push_back(std::move(c));
            };
            auto addWeights = [&](bool all) {
                std::vector<std::string> keys = infOrder;
                std::stable_sort(keys.begin(), keys.end(), [&](const std::string& x, const std::string& y) { return infs[x].total > infs[y].total; });
                for (const std::string& k : keys)
                    if (all || infs[k].total > 0.0)
                        addColumn({k, infs[k].label, ColumnKind::Weight, 0});
            };
            for (const std::string& w : wanted)
            {
                if (w == "weights" || w == "allWeights")
                    addWeights(w == "allWeights");
                else if (w == "position")
                    for (const char* ax : kAxes)
                        addColumn(*parseFixedColumn(std::string("position.") + ax));
                else if (w == "color" || w == "colour")
                {
                    bool has = std::ranges::any_of(meshes, [&](NodeId m) { return !data[m].colorSet.empty(); });
                    if (has)
                        for (const char* ch : kChannels)
                            addColumn(*parseFixedColumn(std::string("color.") + ch));
                }
                else if (auto fixed = parseFixedColumn(w))
                    addColumn(*fixed);
                else
                {
                    std::string key;
                    for (NodeId m : meshes)
                        if (const skin::Influence* i = influenceFor(g, data[m], w))
                        {
                            key = jointKey(g, *i);
                            break;
                        }
                    if (key.empty())
                        throw CommandError("componentValues: " + w + " is not a column (an influence, position.x or color.r)");
                    addColumn({key, infs[key].label, ColumnKind::Weight, 0});
                }
            }

            json outCols = json::array();
            for (const SheetColumn& c : cols)
            {
                json j = columnJson(c);
                if (c.kind == ColumnKind::Weight)
                {
                    const InfInfo& info = infs[c.id];
                    j["joint"] = info.joint;
                    j["held"] = info.held;
                    j["total"] = info.total;
                    j["links"] = info.links;
                }
                outCols.push_back(std::move(j));
            }
            json outRows = json::array(), values = json::array();
            for (const SheetRow& r : rows)
            {
                outRows.push_back({g.path(r.mesh), r.point});
                json line = json::array();
                for (const SheetColumn& c : cols)
                {
                    std::optional<double> v = cellValue(g, data[r.mesh], r.point, c);
                    line.push_back(v ? json(*v) : json(nullptr));
                }
                values.push_back(std::move(line));
            }
            return {{"rows", outRows}, {"columns", outCols}, {"values", values}};
        },
        .examples = {"componentValues(columns=['weights'])", "componentValues('Body', vertices=[0, 1, 2], columns=['spine_01', 'position.y'])"},
    });
}

/** values as one number per (row, column): a number for all, a list per row, or a list of lists per row and column. */
std::vector<std::vector<double>> valueGrid(const json& v, size_t rows, size_t cols)
{
    std::vector<std::vector<double>> out(rows, std::vector<double>(cols, 0.0));
    if (v.is_number())
    {
        for (auto& line : out)
            std::ranges::fill(line, v.get<double>());
        return out;
    }
    if (!v.is_array() || v.size() != rows)
        throw CommandError("setComponentValues: give one value, or one entry per vertex (" + std::to_string(rows) + ")");
    for (size_t r = 0; r < rows; ++r)
    {
        const json& e = v[r];
        if (e.is_number())
            std::ranges::fill(out[r], e.get<double>());
        else if (e.is_array() && e.size() == cols)
            for (size_t c = 0; c < cols; ++c)
                out[r][c] = e[c].get<double>();
        else
            throw CommandError("setComponentValues: entry " + std::to_string(r) + " needs " + std::to_string(cols) + " numbers");
    }
    return out;
}

void registerSet()
{
    ops::registerCommand({
        .name = "setComponentValues",
        .doc = "Set columns of the selected (or given) vertices, as the Component Spreadsheet does: a joint's weight, position.x, "
               "color.r. Setting weights rescales the point's other unheld influences so it still sums to 1, unless "
               "normalize=False; held (lockWeights) influences can't be set. Returns how many values changed.",
        .positionalDoc = "[meshes]",
        .flags = {
            {"column", "col", T::String, nullptr, "One column: a joint, position.x/y/z or color.r/g/b/a."},
            {"columns", "cols", T::List, nullptr, "Several columns; then each vertex's entry in values is a list, one per column."},
            {"values", "val", T::Any, nullptr, "One number for all, one per vertex, or per vertex a list per column."},
            {"vertices", "v", T::List, nullptr, "Point ids for every mesh, or [mesh, id] pairs; defaults to the selected components' points."},
            {"normalize", "nrm", T::Bool, true, "Rescale the other unheld influences so each point sums to 1."},
            {"relative", "r", T::Bool, false, "Add the values to what is there."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            g.evaluate();
            std::vector<std::string> ids;
            if (a.has("column"))
                ids.push_back(a.get<std::string>("column"));
            if (a.has("columns"))
                for (const json& c : a.flags["columns"])
                    ids.push_back(c.get<std::string>());
            if (ids.empty())
                throw CommandError("setComponentValues: give column= or columns=");
            if (!a.has("values"))
                throw CommandError("setComponentValues: give values=");
            std::vector<NodeId> meshes = sheetMeshes(g, a, "setComponentValues");
            std::vector<SheetRow> rows = sheetRows(g, a, meshes, false, "setComponentValues");
            if (rows.empty())
                throw CommandError("setComponentValues: no vertices given or selected");

            std::vector<SheetColumn> cols;
            bool anyColor = false;
            for (const std::string& id : ids)
            {
                if (auto fixed = parseFixedColumn(id))
                {
                    anyColor = anyColor || fixed->kind == ColumnKind::Color;
                    cols.push_back(*fixed);
                }
                else
                    cols.push_back({id, id, ColumnKind::Weight, 0});
            }
            std::vector<std::vector<double>> grid = valueGrid(a.flags["values"], rows.size(), cols.size());
            const bool relative = a.get<bool>("relative"), normalize = a.get<bool>("normalize");

            std::map<NodeId, SheetMesh> data;
            for (NodeId m : meshes)
                data.emplace(m, readMesh(g, m, anyColor));

            // Weight columns resolved per mesh to influence ids; a column no mesh has is an error.
            std::map<NodeId, std::vector<int64_t>> infIndex;
            for (size_t c = 0; c < cols.size(); ++c)
            {
                if (cols[c].kind != ColumnKind::Weight)
                    continue;
                bool found = false;
                for (NodeId m : meshes)
                {
                    auto& idx = infIndex[m];
                    idx.resize(cols.size(), -1);
                    if (const skin::Influence* i = influenceFor(g, data[m], cols[c].id))
                    {
                        if (i->locked)
                            throw CommandError("setComponentValues: " + i->name + " is held; release it to change its weights");
                        idx[c] = i->index;
                        found = true;
                    }
                }
                if (!found)
                    throw CommandError("setComponentValues: " + cols[c].id + " is not a column (an influence, position.x or color.r)");
            }

            int changed = 0;
            for (NodeId m : meshes)
            {
                SheetMesh& s = data[m];
                bool weightsTouched = false, baseTouched = false;
                std::optional<geo::Mesh> base;
                PlugRef basePlug;
                std::vector<char> mask;
                if (s.skin != kNoNode)
                    mask = skin::lockMask(s.infs);
                if (infIndex.contains(m))
                    for (int64_t i : infIndex[m])
                        if (i >= 0)
                        {
                            if (size_t(i) >= mask.size())
                                mask.resize(size_t(i) + 1, 0);
                            mask[size_t(i)] = 1;
                        }
                for (size_t r = 0; r < rows.size(); ++r)
                {
                    if (rows[r].mesh != m)
                        continue;
                    uint32_t p = rows[r].point;
                    bool weightRow = false;
                    for (size_t c = 0; c < cols.size(); ++c)
                    {
                        double v = grid[r][c];
                        switch (cols[c].kind)
                        {
                        case ColumnKind::Weight:
                        {
                            int64_t i = infIndex.contains(m) ? infIndex[m][c] : -1;
                            if (i < 0 || !s.table || p >= s.table->points.size())
                                break;
                            float w = float(relative ? s.table->get(p, uint32_t(i)) + v : v);
                            s.table->set(p, uint32_t(i), std::clamp(w, 0.0f, 1.0f));
                            weightRow = weightsTouched = true;
                            ++changed;
                            break;
                        }
                        case ColumnKind::Position:
                        case ColumnKind::Color:
                        {
                            if (!base)
                            {
                                basePlug = modifiers::editPlug(g, m);
                                g.evaluate();
                                const MeshPtr& stored = std::get<MeshPtr>(g.storedValue(basePlug));
                                base = stored ? *stored : geo::Mesh();
                            }
                            if (!base->pointValid(p))
                                break;
                            if (cols[c].kind == ColumnKind::Position)
                            {
                                double& x = base->positions()[p][cols[c].channel];
                                x = relative ? x + v : v;
                            }
                            else
                            {
                                std::span<glm::vec4> vals = geo::colors::values(*base, s.colorSet);
                                if (vals.empty())
                                    break;
                                const int ch = cols[c].channel;
                                const float now = p < s.colors.size() ? s.colors[p][ch] : 1.0f;
                                const float to = float(relative ? now + v : v);
                                base->forEachPointCorner(p, [&](geo::Index corner) {
                                    if (corner < vals.size())
                                        vals[corner][ch] = to;
                                });
                            }
                            baseTouched = true;
                            ++changed;
                            break;
                        }
                        }
                    }
                    if (weightRow && normalize)
                        s.table->normalize(p, mask);
                }
                if (weightsTouched)
                    skin::setWeights(g, s.skin, *s.table);
                if (baseTouched)
                {
                    if (anyColor && !s.colorSet.empty())
                        geo::colors::settle(*base, s.colorSet);
                    g.setValue(basePlug, MeshPtr(std::make_shared<geo::Mesh>(std::move(*base))));
                }
            }
            return changed;
        },
        .examples = {"setComponentValues(column='spine_01', values=0.5)",
                     "setComponentValues('Body', vertices=[4, 5], columns=['position.x', 'position.y'], values=[[0, 1], [0, 2]])"},
    });
}
} // namespace

void registerCommands()
{
    registerQuery();
    registerSet();
}
} // namespace lila::scene::componentValues
