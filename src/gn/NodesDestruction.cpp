#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>

namespace lila::gn::nodes
{
namespace
{
/** Link types as the Rigid Body Solver reads the "type" curve attribute. */
const std::vector<std::string>& linkTypes()
{
    static const std::vector<std::string> v = {"glue", "spring", "hard"};
    return v;
}

int linkTypeIndex(const std::string& s)
{
    const auto& v = linkTypes();
    auto it = std::find(v.begin(), v.end(), s);
    return it == v.end() ? 0 : int(it - v.begin());
}

struct Link
{
    int a = -1, b = -1;
    glm::dvec3 anchor{0.0};
    glm::dvec3 other{0.0};
    double strength = 0.0;
    int type = 0;
    double area = 0.0;
};

/** Two-point curves from piece a's anchor to piece b's (the same point when they touch), with the solver's attributes. */
CurveSet linkCurves(const std::vector<Link>& links)
{
    CurveSet c;
    for (const Link& l : links)
    {
        const glm::dvec3 pts[2] = {l.anchor, l.other};
        c.addCurve(pts, false);
    }
    c.syncSizes();
    std::span<int32_t> a = c.curveAttrs.getOrAdd<int32_t>("pieceA");
    std::span<int32_t> b = c.curveAttrs.getOrAdd<int32_t>("pieceB");
    std::span<int32_t> type = c.curveAttrs.getOrAdd<int32_t>("type");
    std::span<float> strength = c.curveAttrs.getOrAdd<float>("strength");
    std::span<float> area = c.curveAttrs.getOrAdd<float>("area");
    for (size_t k = 0; k < links.size(); ++k)
    {
        a[k] = links[k].a;
        b[k] = links[k].b;
        type[k] = links[k].type;
        strength[k] = float(links[k].strength);
        area[k] = float(links[k].area);
    }
    return c;
}

/** Each piece's points in world space (its instance transform applied), references realized once. */
std::vector<std::vector<glm::dvec3>> pieceWorldPoints(const Instances& inst)
{
    std::map<int, std::vector<glm::dvec3>> local;
    for (int r : inst.reference)
    {
        auto [it, fresh] = local.try_emplace(r);
        if (!fresh || r < 0 || size_t(r) >= inst.references.size() || !inst.references[size_t(r)])
            continue;
        const Geometry g = realize(*inst.references[size_t(r)]);
        for (const std::shared_ptr<const geo::Mesh>& m : {g.mesh, g.points})
            if (m)
                for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                    if (m->pointValid(p))
                        it->second.push_back(m->position(p));
    }
    std::vector<std::vector<glm::dvec3>> out(inst.size());
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const std::vector<glm::dvec3>& pts = local[inst.reference[k]];
        out[k].reserve(pts.size());
        for (const glm::dvec3& p : pts)
            out[k].push_back(glm::dvec3(inst.transforms[k] * glm::dvec4(p, 1.0)));
    }
    return out;
}

geo::Bounds boundsOf(const std::vector<glm::dvec3>& pts)
{
    geo::Bounds b;
    for (const glm::dvec3& p : pts)
    {
        b.min = glm::min(b.min, p);
        b.max = glm::max(b.max, p);
    }
    return b;
}

void connectPiecesNode()
{
    registerDecl({
        .type = "ConnectPieces",
        .label = "Connect Pieces",
        .category = "Destruction",
        .doc = "Links every pair of pieces that touch (within Distance), for the Rigid Body Solver to hold together until they "
               "break. A link's strength is Strength times the area the two pieces share. Fracture makes these links itself; "
               "this is for pieces from anywhere else.",
        .props = {prop("type", linkTypes(), "glue",
                       "glue holds rigidly until it breaks; spring bends first; hard never breaks.")},
        .sockets = {.inputs = {geoIn("pieces", "Pieces, as instances."),
                               bounded(in("distance", T::Float, 0.5, "Gap still counted as touching, cm."), 0.0),
                               bounded(in("strength", T::Float, 20.0, "N per cm2 of shared area."), 0.0),
                               bounded(in("maxLinks", T::Int, 12, "Most links per piece; the largest contacts are kept."), 1)},
                    .outputs = {geoOut("links")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("pieces");
                if (!g->instances)
                {
                    c.out("links", makeGeometry({}));
                    return;
                }
                const std::vector<std::vector<glm::dvec3>> pts = pieceWorldPoints(*g->instances);
                const size_t n = pts.size();
                const double gap = std::max(0.0, c.f("distance"));
                const double strength = std::max(0.0, c.f("strength"));
                const size_t maxLinks = size_t(std::max<int64_t>(1, c.i("maxLinks")));
                const int type = linkTypeIndex(c.prop("type"));
                std::vector<geo::Bounds> box(n);
                for (size_t k = 0; k < n; ++k)
                    box[k] = boundsOf(pts[k]);
                std::vector<size_t> order(n);
                for (size_t k = 0; k < n; ++k)
                    order[k] = k;
                std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return box[a].min.x < box[b].min.x || (box[a].min.x == box[b].min.x && a < b); });

                std::vector<Link> found;
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t a = order[i];
                    if (!box[a].valid())
                        continue;
                    for (size_t j = i + 1; j < n; ++j)
                    {
                        const size_t b = order[j];
                        if (box[b].min.x > box[a].max.x + gap)
                            break;
                        if (!box[b].valid())
                            continue;
                        glm::dvec3 lo = glm::max(box[a].min, box[b].min) - gap * 0.5;
                        glm::dvec3 hi = glm::min(box[a].max, box[b].max) + gap * 0.5;
                        glm::dvec3 size = hi - lo;
                        if (size.x < 0.0 || size.y < 0.0 || size.z < 0.0)
                            continue;
                        // Touching boxes overlap thinly across their contact; its two larger sides span the shared face.
                        double s[3] = {size.x, size.y, size.z};
                        std::sort(s, s + 3);
                        const double area = s[1] * s[2];
                        Link l;
                        l.a = int(std::min(a, b));
                        l.b = int(std::max(a, b));
                        l.anchor = l.other = (lo + hi) * 0.5;
                        l.strength = strength * area;
                        l.area = area;
                        l.type = type;
                        found.push_back(l);
                    }
                }
                std::vector<size_t> perPiece(n, 0);
                std::vector<size_t> byArea(found.size());
                for (size_t k = 0; k < found.size(); ++k)
                    byArea[k] = k;
                std::stable_sort(byArea.begin(), byArea.end(), [&](size_t x, size_t y) { return found[x].strength > found[y].strength; });
                std::vector<uint8_t> keep(found.size(), 0);
                for (size_t k : byArea)
                    if (perPiece[size_t(found[k].a)] < maxLinks && perPiece[size_t(found[k].b)] < maxLinks)
                    {
                        keep[k] = 1;
                        ++perPiece[size_t(found[k].a)];
                        ++perPiece[size_t(found[k].b)];
                    }
                std::vector<Link> links;
                for (size_t k = 0; k < found.size(); ++k)
                    if (keep[k])
                        links.push_back(found[k]);
                std::sort(links.begin(), links.end(), [](const Link& x, const Link& y) { return x.a != y.a ? x.a < y.a : x.b < y.b; });
                c.out("links", curveGeometry(linkCurves(links)));
            },
    });
}

void anchorPiecesNode()
{
    registerDecl({
        .type = "AnchorPieces",
        .label = "Anchor Pieces",
        .category = "Destruction",
        .doc = "Ties the pieces resting on the ground to the world, so a wall stands until something breaks it free. Adds the "
               "anchors to the links coming in.",
        .props = {prop("type", linkTypes(), "glue", "glue breaks free under force; hard never lets go.")},
        .sockets = {.inputs = {geoIn("pieces", "Pieces, as instances."), geoIn("links", "Links to add the anchors to."),
                               in("height", T::Float, 0.0, "The ground, cm: pieces reaching down to it are anchored."),
                               bounded(in("tolerance", T::Float, 1.0, "How close to the ground counts, cm."), 0.0),
                               bounded(in("strength", T::Float, 20.0, "N per cm2 of the piece's footprint."), 0.0)},
                    .outputs = {geoOut("links")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("pieces");
                GeometryPtr existing = c.geometry("links");
                if (!g->instances)
                {
                    c.out("links", existing);
                    return;
                }
                const std::vector<std::vector<glm::dvec3>> pts = pieceWorldPoints(*g->instances);
                const double height = c.f("height"), tol = std::max(0.0, c.f("tolerance"));
                const double strength = std::max(0.0, c.f("strength"));
                const int type = linkTypeIndex(c.prop("type"));
                std::vector<Link> links;
                for (size_t k = 0; k < pts.size(); ++k)
                {
                    const geo::Bounds b = boundsOf(pts[k]);
                    if (!b.valid() || b.min.y > height + tol)
                        continue;
                    // Footprint: the points near the bottom, spread over the ground.
                    glm::dvec2 lo(1e300), hi(-1e300);
                    for (const glm::dvec3& p : pts[k])
                        if (p.y <= b.min.y + tol)
                        {
                            lo = glm::min(lo, glm::dvec2(p.x, p.z));
                            hi = glm::max(hi, glm::dvec2(p.x, p.z));
                        }
                    const glm::dvec2 size = glm::max(hi - lo, glm::dvec2(0.0));
                    Link l;
                    l.a = int(k);
                    l.b = -1;
                    l.anchor = l.other = glm::dvec3((lo.x + hi.x) * 0.5, b.min.y, (lo.y + hi.y) * 0.5);
                    l.area = std::max(size.x * size.y, 1.0);
                    l.strength = strength * l.area;
                    l.type = type;
                    links.push_back(l);
                }
                Geometry out = *existing;
                CurveSet added = linkCurves(links);
                if (existing->curves)
                {
                    CurveSet merged = *existing->curves;
                    merged.append(added);
                    out.curves = std::make_shared<const CurveSet>(std::move(merged));
                }
                else
                    out.curves = std::make_shared<const CurveSet>(std::move(added));
                c.out("links", makeGeometry(std::move(out)));
            },
    });
}

void meshToPiecesNode()
{
    registerDecl({
        .type = "MeshToPieces",
        .label = "Mesh to Pieces",
        .category = "Destruction",
        .doc = "Turns a mesh into pieces the Rigid Body Solver can move: one per separate part, or one per value of an integer "
               "attribute. Each piece's origin is the centre of its bounds, and every point gets a piece attribute.",
        .props = {prop("by", {"parts", "attribute"}, "parts", "parts: each connected part; attribute: faces sharing a value.")},
        .sockets = {.inputs = {geoIn("mesh"), in("attribute", T::String, "piece", "Integer face or point attribute, for by = attribute.")},
                    .outputs = {geoOut("pieces")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                Geometry src = realize(*c.geometry("mesh"));
                if (!src.mesh || src.mesh->numFaces() == 0)
                {
                    c.out("pieces", makeGeometry({}));
                    return;
                }
                const geo::Mesh& m = *src.mesh;
                std::vector<geo::Mesh> parts;
                if (c.prop("by") == "attribute")
                {
                    const std::string name = c.s("attribute");
                    std::map<int64_t, std::vector<geo::Index>> groups;
                    const geo::Attribute* fa = m.attributes(geo::Domain::Face).find(name);
                    const geo::Attribute* pa = m.attributes(geo::Domain::Point).find(name);
                    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                    {
                        if (!m.faceValid(f))
                            continue;
                        int64_t key = 0;
                        if (fa && fa->type() == geo::AttrType::Int)
                            key = fa->values<int32_t>()[f];
                        else if (pa && pa->type() == geo::AttrType::Int)
                            key = pa->values<int32_t>()[m.cornerPoint(m.faceCorner(f))];
                        groups[key].push_back(f);
                    }
                    for (const auto& [key, faces] : groups)
                        parts.push_back(geo::extractFaces(m, faces));
                }
                else
                    parts = geo::separate(m);
                c.out("pieces", piecesFromMeshes(std::move(parts), {}));
            },
    });
}

/** References with their "piece" point ids moved up by offset, kept while the originals live so a re-run costs nothing. */
std::vector<GeometryPtr> offsetPieceIds(const std::vector<GeometryPtr>& refs, int32_t offset)
{
    struct Entry
    {
        std::weak_ptr<const Geometry> source;
        int32_t offset;
        GeometryPtr result;
    };
    static std::mutex mutex;
    static std::map<const Geometry*, std::vector<Entry>> cache;
    std::lock_guard lock(mutex);
    std::erase_if(cache, [](auto& kv) {
        std::erase_if(kv.second, [](const Entry& e) { return e.source.expired(); });
        return kv.second.empty();
    });
    std::vector<GeometryPtr> out;
    out.reserve(refs.size());
    for (const GeometryPtr& r : refs)
    {
        if (!r || !r->mesh || !r->mesh->pointAttrs().find("piece"))
        {
            out.push_back(r);
            continue;
        }
        std::vector<Entry>& entries = cache[r.get()];
        auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.offset == offset && e.source.lock() == r; });
        if (it == entries.end())
        {
            geo::Mesh m = *r->mesh;
            for (int32_t& id : m.pointAttrs().getOrAdd<int32_t>("piece"))
                id += offset;
            Geometry g = *r;
            g.mesh = std::make_shared<const geo::Mesh>(std::move(m));
            entries.push_back({r, offset, makeGeometry(std::move(g))});
            it = std::prev(entries.end());
        }
        out.push_back(it->result);
    }
    return out;
}

void joinPiecesNode()
{
    registerDecl({
        .type = "JoinPieces",
        .label = "Join Pieces",
        .category = "Destruction",
        .doc = "Puts two sets of pieces and their links into one, so they simulate together and hit each other: walls and the "
               "ground they stand on, say. The second set's links are renumbered to follow it. Chain more for more sets.",
        .sockets = {.inputs = {geoIn("pieces", "The first pieces."), geoIn("links", "The first pieces' links."),
                               geoIn("pieces2", "The second pieces."), geoIn("links2", "The second pieces' links.")},
                    .outputs = {geoOut("pieces"), geoOut("links")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr a = c.geometry("pieces"), b = c.geometry("pieces2");
                const size_t na = a->instances ? a->instances->size() : 0;
                auto inst = std::make_shared<Instances>();
                for (const GeometryPtr& g : {a, b})
                {
                    if (!g->instances)
                        continue;
                    const Instances& o = *g->instances;
                    const int base = int(inst->references.size());
                    const std::vector<GeometryPtr> refs = g == b ? offsetPieceIds(o.references, int32_t(na)) : o.references;
                    inst->references.insert(inst->references.end(), refs.begin(), refs.end());
                    const size_t before = inst->size();
                    inst->attrs.mergeLayout(o.attrs);
                    for (size_t k = 0; k < o.size(); ++k)
                    {
                        inst->add(o.transforms[k], o.reference[k] < 0 ? -1 : o.reference[k] + base);
                        if (k < o.attrs.size())
                            inst->attrs.copyFrom(o.attrs, before + k, k);
                    }
                }
                // A set without a layer the other has: active pieces, and NaN for floats, which the solver reads as unset.
                for (const GeometryPtr& g : {a, b})
                {
                    if (!g->instances)
                        continue;
                    const size_t first = g == a ? 0 : na, count = g->instances->size();
                    for (size_t l = 0; l < inst->attrs.layerCount(); ++l)
                    {
                        geo::Attribute& layer = inst->attrs.layer(l);
                        if (g->instances->attrs.find(layer.name()))
                            continue;
                        if (layer.name() == "active" && layer.type() == geo::AttrType::Int)
                            std::fill_n(layer.values<int32_t>().begin() + first, count, 1);
                        else if (layer.type() == geo::AttrType::Float)
                            std::fill_n(layer.values<float>().begin() + first, count, std::numeric_limits<float>::quiet_NaN());
                    }
                }
                if (std::span<int32_t> piece = inst->attrs.get<int32_t>("piece"); piece.size() == inst->size())
                    for (size_t k = 0; k < piece.size(); ++k)
                        piece[k] = int32_t(k);
                Geometry pieces;
                pieces.instances = inst;
                c.out("pieces", makeGeometry(std::move(pieces)));

                CurveSet links;
                GeometryPtr la = c.geometry("links"), lb = c.geometry("links2");
                if (la->curves)
                    links = *la->curves;
                if (lb->curves)
                {
                    CurveSet second = *lb->curves;
                    for (const char* attr : {"pieceA", "pieceB"})
                        for (int32_t& p : second.curveAttrs.getOrAdd<int32_t>(attr))
                            if (p >= 0)
                                p += int32_t(na);
                    links.append(second);
                }
                c.out("links", curveGeometry(std::move(links)));
            },
    });
}

void explosionNode()
{
    registerDecl({
        .type = "Explosion",
        .label = "Explosion",
        .category = "Destruction",
        .doc = "One explosion for the Rigid Body Solver's blasts: on its frame, pieces within Radius are thrown outward, hardest "
               "at the centre. Join several with Join Geometry. Invisible: add your own flash or smoke.",
        .sockets = {.inputs = {in("position", T::Vector, vec(0, 0, 0), "Its centre, cm."), in("frame", T::Int, 1, "Frame it goes off."),
                               in("speed", T::Float, 1500.0, "Speed given to pieces at the centre, cm/s."),
                               bounded(in("radius", T::Float, 300.0, "Pieces this far away or more aren't pushed, cm."), 0.0)},
                    .outputs = {geoOut("blast")}},
        .exec =
            [](ExecContext& c) {
                geo::Mesh m;
                m.addPoint(c.v("position"));
                m.pointAttrs().getOrAdd<int32_t>("frame")[0] = int32_t(c.i("frame"));
                m.pointAttrs().getOrAdd<float>("speed")[0] = float(c.f("speed"));
                m.pointAttrs().getOrAdd<float>("radius")[0] = float(std::max(0.0, c.f("radius")));
                c.out("blast", pointGeometry(std::move(m)));
            },
    });
}

/** 0 to 1, the same for the same seed and index on every machine. */
double hash01(int64_t seed, size_t k)
{
    uint64_t x = uint64_t(seed) * 0x9E3779B97F4A7C15ull + uint64_t(k) * 0xBF58476D1CE4E5B9ull + 0x94D049BB133111EBull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return double(x >> 11) * (1.0 / 9007199254740992.0);
}

bool isInactive(const geo::Attribute* a, size_t k)
{
    if (!a || k >= a->size())
        return false;
    switch (a->type())
    {
    case geo::AttrType::Int: return a->values<int32_t>()[k] == 0;
    case geo::AttrType::Float: return a->values<float>()[k] == 0.0f;
    case geo::AttrType::Bool: return !a->values<bool>()[k];
    default: return false;
    }
}

void collapsePiecesNode()
{
    registerDecl({
        .type = "CollapsePieces",
        .label = "Collapse",
        .category = "Destruction",
        .doc = "Gives way from one side to the other, like ground or a floor failing under structural damage. Pieces hold still "
               "until the failure reaches them, rumble harder as it nears, then come loose and fall: their links are cut and "
               "they start simulating. Connect Scene Time's frame to Frame.",
        .sockets = {.inputs = {geoIn("pieces", "Pieces, as instances."), geoIn("links", "Their links."),
                               in("frame", T::Float, 1.0, "The current frame: connect Scene Time."),
                               in("start", T::Float, 60.0, "Frame the first pieces fall."),
                               bounded(in("duration", T::Float, 60.0, "Frames for the failure to cross to the far side."), 0.0),
                               in("direction", T::Vector, vec(1, 0, 0), "Which way the failure travels."),
                               bounded(in("rumble", T::Float, 2.0, "How far pieces shake just before they fall, cm."), 0.0),
                               bounded(in("lead", T::Float, 30.0, "Frames of rumbling before a piece falls."), 1e-6),
                               bounded(in("raggedness", T::Float, 0.25, "0 is a straight front; 1 drops pieces in any order."), 0.0, 1.0),
                               in("seed", T::Int, 0, "Another seed gives another ragged front.")},
                    .outputs = {geoOut("pieces"), geoOut("links")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr pieces = c.geometry("pieces");
                GeometryPtr links = c.geometry("links");
                if (!pieces->instances || pieces->instances->size() == 0)
                {
                    c.out("pieces", pieces);
                    c.out("links", links);
                    return;
                }
                const Instances& in = *pieces->instances;
                const size_t n = in.size();
                const double t = c.f("frame"), start = c.f("start"), duration = std::max(0.0, c.f("duration"));
                const double rumble = std::max(0.0, c.f("rumble")), lead = std::max(1e-6, c.f("lead"));
                const double ragged = std::clamp(c.f("raggedness"), 0.0, 1.0);
                const int64_t seed = c.i("seed");
                glm::dvec3 dir = c.v("direction");
                dir = glm::length(dir) > 1e-12 ? glm::normalize(dir) : glm::dvec3(1, 0, 0);

                double lo = 1e300, hi = -1e300;
                for (const glm::dmat4& m : in.transforms)
                {
                    const double s = glm::dot(glm::dvec3(m[3]), dir);
                    lo = std::min(lo, s);
                    hi = std::max(hi, s);
                }
                std::vector<double> fallAt(n);
                for (size_t k = 0; k < n; ++k)
                {
                    const double along = hi > lo ? (glm::dot(glm::dvec3(in.transforms[k][3]), dir) - lo) / (hi - lo) : 0.0;
                    fallAt[k] = start + duration * (along * (1.0 - ragged) + hash01(seed, k) * ragged);
                }

                auto moved = std::make_shared<Instances>(in);
                moved->attrs.resize(n);
                const geo::Attribute* upstream = in.attrs.find("active");
                std::vector<uint8_t> held(n, 0);
                for (size_t k = 0; k < n; ++k)
                    held[k] = isInactive(upstream, k) ? 1 : 0;
                if (upstream && upstream->type() != geo::AttrType::Int)
                    moved->attrs.remove("active");
                std::span<int32_t> active = moved->attrs.getOrAdd<int32_t>("active");
                for (size_t k = 0; k < n; ++k)
                {
                    const bool fallen = t >= fallAt[k];
                    active[k] = fallen && !held[k] ? 1 : 0;
                    const double into = (t - (fallAt[k] - lead)) / lead;
                    if (fallen || into <= 0.0 || rumble <= 0.0)
                        continue;
                    // Shaking at a few Hz with a phase of its own, growing as the failure arrives.
                    const double amp = rumble * into * into;
                    const double p0 = hash01(seed + 1, k) * 6.2832, p1 = hash01(seed + 2, k) * 6.2832, p2 = hash01(seed + 3, k) * 6.2832;
                    const glm::dvec3 shake(std::sin(t * 1.9 + p0), 0.6 * std::sin(t * 2.7 + p1), std::sin(t * 2.3 + p2));
                    const glm::dvec3 axis = glm::normalize(glm::dvec3(std::sin(p1), 0.3, std::cos(p2)));
                    const double tilt = amp * 0.003 * std::sin(t * 1.5 + p0);
                    glm::dmat4& m = moved->transforms[k];
                    const glm::dvec3 at = glm::dvec3(m[3]);
                    m = glm::translate(glm::dmat4(1.0), at + shake * amp) * glm::rotate(glm::dmat4(1.0), tilt, axis) *
                        glm::translate(glm::dmat4(1.0), -at) * m;
                }
                Geometry outPieces = *pieces;
                outPieces.instances = moved;
                c.out("pieces", makeGeometry(std::move(outPieces)));

                if (!links->curves)
                {
                    c.out("links", links);
                    return;
                }
                auto curves = std::make_shared<CurveSet>(*links->curves);
                curves->curveAttrs.resize(curves->numCurves());
                std::span<const int32_t> pa = curves->curveAttrs.get<int32_t>("pieceA"), pb = curves->curveAttrs.get<int32_t>("pieceB");
                std::span<float> strength = curves->curveAttrs.getOrAdd<float>("strength");
                auto fell = [&](int32_t p) { return p >= 0 && size_t(p) < n && t >= fallAt[size_t(p)]; };
                for (size_t k = 0; k < curves->numCurves(); ++k)
                    if ((k < pa.size() && fell(pa[k])) || (k < pb.size() && fell(pb[k])))
                        strength[k] = 0.0f;
                Geometry outLinks = *links;
                outLinks.curves = curves;
                c.out("links", makeGeometry(std::move(outLinks)));
            },
    });
}
} // namespace

GeometryPtr piecesFromMeshes(std::vector<geo::Mesh> parts, const std::vector<glm::dvec3>& origins)
{
    LILA_ZONE();

    auto inst = std::make_shared<Instances>();
    for (size_t k = 0; k < parts.size(); ++k)
    {
        geo::Mesh& piece = parts[k];
        const glm::dvec3 origin = k < origins.size() ? origins[k] : piece.bounds().center();
        transformMesh(piece, glm::translate(glm::dmat4(1.0), -origin));
        std::span<int32_t> id = piece.pointAttrs().getOrAdd<int32_t>("piece");
        std::fill(id.begin(), id.end(), int32_t(k));
        inst->references.push_back(meshGeometry(std::move(piece)));
        inst->add(glm::translate(glm::dmat4(1.0), origin), int(k));
    }
    inst->attrs.resize(inst->size());
    std::span<int32_t> pieceAttr = inst->attrs.getOrAdd<int32_t>("piece");
    for (size_t k = 0; k < pieceAttr.size(); ++k)
        pieceAttr[k] = int32_t(k);
    Geometry g;
    g.instances = inst;
    return makeGeometry(std::move(g));
}

GeometryPtr linkGeometry(const std::vector<PieceLink>& links, const std::string& type)
{
    std::vector<Link> out;
    out.reserve(links.size());
    for (const PieceLink& l : links)
        out.push_back({l.a, l.b, l.anchor, l.anchor, l.strength, linkTypeIndex(type), l.area});
    return curveGeometry(linkCurves(out));
}

const std::vector<std::string>& pieceLinkTypes() { return linkTypes(); }

void registerDestructionNodes()
{
    meshToPiecesNode();
    connectPiecesNode();
    anchorPiecesNode();
    joinPiecesNode();
    explosionNode();
    collapsePiecesNode();
    registerFractureNode();
}
} // namespace lila::gn::nodes
