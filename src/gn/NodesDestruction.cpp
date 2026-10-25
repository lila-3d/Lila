#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <map>

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
    for (size_t k = 0; k < links.size(); ++k)
    {
        a[k] = links[k].a;
        b[k] = links[k].b;
        type[k] = links[k].type;
        strength[k] = float(links[k].strength);
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
                               in("distance", T::Float, 0.5, "Gap still counted as touching, cm."),
                               in("strength", T::Float, 20.0, "N per cm2 of shared area."),
                               in("maxLinks", T::Int, 12, "Most links per piece; the largest contacts are kept.")},
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
                               in("tolerance", T::Float, 1.0, "How close to the ground counts, cm."),
                               in("strength", T::Float, 20.0, "N per cm2 of the piece's footprint.")},
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
                    l.strength = strength * std::max(size.x * size.y, 1.0);
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
        out.push_back({l.a, l.b, l.anchor, l.anchor, l.strength, linkTypeIndex(type)});
    return curveGeometry(linkCurves(out));
}

const std::vector<std::string>& pieceLinkTypes() { return linkTypes(); }

void registerDestructionNodes()
{
    meshToPiecesNode();
    connectPiecesNode();
    anchorPiecesNode();
    registerFractureNode();
}
} // namespace lila::gn::nodes
