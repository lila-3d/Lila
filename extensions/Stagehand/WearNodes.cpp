// Wear from contact: the geometry nodes that turn a settle's contacts into imperfections. Contact Wear paints where
// things touched, slid and hit; Dent At Contacts pushes the surface in where hits were hard; Sag Under Load bends a board
// under what rests on it; Variant Pool gives instances a bounded set of deformed variants (or one each) so a row of the
// same book reads as many.

#include "NodeSockets.h"
#include "Stagehand.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace stagehand
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;

struct ContactPoint
{
    glm::dvec3 p, n;
    double impulse, slide, resting, load;
};

std::vector<ContactPoint> contactsOf(const GeometryPtr& g)
{
    std::vector<ContactPoint> out;
    if (!g)
        return out;
    // Instances stand in for resting contacts at their origins, weighing their mass attribute (1 kg without one).
    if (g->instances)
    {
        const auto mass = g->instances->attrs.get<float>("mass");
        for (size_t k = 0; k < g->instances->size(); ++k)
            out.push_back({glm::dvec3(g->instances->transforms[k][3]), {0, 1, 0}, 0.0, 0.0, 1.0, k < mass.size() ? double(mass[k]) : 1.0});
    }
    const geo::Mesh* m = g->points ? g->points.get() : g->mesh.get();
    if (!m)
        return out;
    const auto& a = m->pointAttrs();
    const auto impulse = a.get<float>("impulse");
    const auto slide = a.get<float>("slide");
    const auto resting = a.get<float>("resting");
    const auto load = a.get<float>("load");
    const auto normal = a.get<glm::vec3>("contactNormal");
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
    {
        if (!m->pointValid(p))
            continue;
        auto at = [&](std::span<const float> s) { return p < s.size() ? double(s[p]) : 0.0; };
        out.push_back({m->position(p), p < normal.size() ? glm::dvec3(normal[p]) : glm::dvec3(0, 1, 0), at(impulse), at(slide), at(resting), at(load)});
    }
    return out;
}

/** Smooth 1 to 0 over the radius. */
double falloff(double d, double radius)
{
    if (d >= radius)
        return 0.0;
    const double t = 1.0 - d / std::max(radius, 1e-9);
    return t * t * (3.0 - 2.0 * t);
}

/** Area-weighted point normals. */
std::vector<glm::dvec3> pointNormals(const geo::Mesh& m)
{
    std::vector<glm::dvec3> n(m.pointCapacity(), glm::dvec3(0.0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
        {
            const glm::dvec3 c = glm::cross(m.position(p[k]) - m.position(p[0]), m.position(p[k + 1]) - m.position(p[0]));
            for (geo::Index v : {p[0], p[k], p[k + 1]})
                n[v] += c;
        }
    }
    for (glm::dvec3& v : n)
        v = glm::length(v) > 1e-12 ? glm::normalize(v) : glm::dvec3(0, 1, 0);
    return n;
}

void contactWearExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr in = c.geometry("geometry");
    if (!in || !in->mesh)
    {
        c.out("geometry", in ? in : gn::makeGeometry({}));
        return;
    }
    const std::vector<ContactPoint> contacts = contactsOf(c.geometry("contacts"));
    geo::Mesh m = *in->mesh;
    const double radius = std::max(c.f("radius"), 1e-3);
    const double hitScale = std::max(c.f("hitImpulse"), 1e-6);
    const double restW = c.f("resting"), slideW = c.f("sliding"), hitW = c.f("hits");
    auto wear = m.pointAttrs().getOrAdd<float>(c.s("attribute").empty() ? std::string("wear") : c.s("attribute"));
    tbb::parallel_for(geo::Index(0), m.pointCapacity(), [&](geo::Index p) {
        if (!m.pointValid(p))
            return;
        double w = 0.0;
        for (const ContactPoint& k : contacts)
        {
            const double f = falloff(glm::length(m.position(p) - k.p), radius);
            if (f <= 0.0)
                continue;
            // Resting marks where things sat (rings, worn patches), sliding scuffs, hard hits chip.
            w += f * (restW * k.resting + slideW * std::min(1.0, k.slide / radius) + hitW * std::min(1.0, k.impulse / hitScale));
        }
        wear[p] = float(std::clamp(w, 0.0, 1.0));
    });
    c.out("geometry", gn::meshGeometry(std::move(m)));
}

void dentExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr in = c.geometry("geometry");
    if (!in || !in->mesh)
    {
        c.out("geometry", in ? in : gn::makeGeometry({}));
        return;
    }
    const std::vector<ContactPoint> contacts = contactsOf(c.geometry("contacts"));
    geo::Mesh m = *in->mesh;
    const std::vector<glm::dvec3> normals = pointNormals(m);
    const double radius = std::max(c.f("radius"), 1e-3);
    const double depth = c.f("depth");
    const double minImpulse = c.f("minImpulse");
    const double fullImpulse = std::max(c.f("fullImpulse"), minImpulse + 1e-6);
    std::vector<glm::dvec3> moved(m.pointCapacity());
    tbb::parallel_for(geo::Index(0), m.pointCapacity(), [&](geo::Index p) {
        if (!m.pointValid(p))
            return;
        double d = 0.0;
        for (const ContactPoint& k : contacts)
        {
            if (k.impulse < minImpulse)
                continue;
            const double strength = std::min(1.0, (k.impulse - minImpulse) / (fullImpulse - minImpulse));
            d = std::max(d, falloff(glm::length(m.position(p) - k.p), radius) * strength);
        }
        moved[p] = m.position(p) - normals[p] * (d * depth);
    });
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, moved[p]);
    c.out("geometry", gn::meshGeometry(std::move(m)));
}

/**
 * Deflection of a simply supported span (length 1) at t under a unit point load at a: the textbook beam curve, so loads
 * near the ends bend it little and one in the middle most (1/48 there).
 */
double beam(double t, double a)
{
    const double b = 1.0 - a;
    if (t <= a)
        return b * t * (1.0 - b * b - t * t) / 6.0;
    const double u = 1.0 - t;
    return a * u * (1.0 - a * a - u * u) / 6.0;
}

void sagExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr in = c.geometry("geometry");
    if (!in || !in->mesh)
    {
        c.out("geometry", in ? in : gn::makeGeometry({}));
        return;
    }
    geo::Mesh m = *in->mesh;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    if (lo.x > hi.x)
    {
        c.out("geometry", in);
        return;
    }
    // The span runs along the longer horizontal side; loads count when they sit on the top, within the footprint.
    const int axis = hi.x - lo.x >= hi.z - lo.z ? 0 : 2;
    const double span = std::max(hi[axis] - lo[axis], 1e-6);
    const double referenceKg = 10.0;
    const double perUnit = c.f("amount") / (referenceKg / 48.0);
    std::vector<std::pair<double, double>> loads; // position along the span 0..1, kg
    // Contacts come in world space; the board may be in its own (a modifier's input), so they're moved into it first.
    const glm::dvec3 ax = c.v("toLocalX"), ay = c.v("toLocalY"), az = c.v("toLocalZ"), ao = c.v("toLocalOrigin");
    for (ContactPoint k : contactsOf(c.geometry("contacts")))
    {
        k.p = ax * k.p.x + ay * k.p.y + az * k.p.z + ao;
        if (k.load <= 0.0 || k.p.y < hi.y - 2.0 || k.p.x < lo.x - 1.0 || k.p.x > hi.x + 1.0 || k.p.z < lo.z - 1.0 || k.p.z > hi.z + 1.0)
            continue;
        loads.emplace_back(std::clamp((k.p[axis] - lo[axis]) / span, 0.0, 1.0), k.load);
    }
    const double extra = c.f("ownWeight");
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        const double t = (m.position(p)[axis] - lo[axis]) / span;
        double d = 0.0;
        for (const auto& [a, kg] : loads)
            d += kg * beam(t, a);
        // Its own weight spread evenly: the uniform-load curve, 5/384 at the middle.
        d += extra * t * (1.0 - 2.0 * t * t + t * t * t) / 24.0;
        glm::dvec3 q = m.position(p);
        q.y -= d * perUnit;
        m.setPosition(p, q);
    }
    c.out("geometry", gn::meshGeometry(std::move(m)));
}

/** A bow across an item's height (its middle pushed along X) and a bulge (its faces swelling at mid height). */
geo::Mesh deformed(const geo::Mesh& src, double bow, double bulge)
{
    geo::Mesh m = src;
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    if (lo.x > hi.x)
        return m;
    const glm::dvec3 size = glm::max(hi - lo, glm::dvec3(1e-6));
    const glm::dvec3 mid = (lo + hi) * 0.5;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        glm::dvec3 q = m.position(p);
        const double yn = (q.y - lo.y) / size.y;
        const double arch = std::sin(kPi * yn);
        q.x += bow * arch;
        // Swell grows from the item's centre line outward, so faces belly while the edges stay put.
        const double zn = (q.z - mid.z) / (size.z * 0.5);
        q.x += bulge * arch * (1.0 - zn * zn) * ((q.x >= mid.x) ? 1.0 : -1.0);
        m.setPosition(p, q);
    }
    return m;
}

void variantsExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr in = c.geometry("instances");
    if (!in || !in->instances || in->instances->size() == 0)
    {
        c.out("instances", in ? in : gn::makeGeometry({}));
        return;
    }
    const gn::Instances& src = *in->instances;
    const bool unique = c.prop("mode") == "unique";
    const int count = int(std::clamp<int64_t>(c.i("variants"), 1, 64));
    const double bow = c.f("bow"), bulge = c.f("bulge");
    const std::string driverName = c.s("driver").empty() ? std::string("random") : c.s("driver");
    const auto driverF = src.attrs.get<float>(driverName);
    const auto driverI = src.attrs.get<int32_t>(driverName);
    std::vector<geo::Mesh> bases;
    for (const GeometryPtr& r : src.references)
        bases.push_back(r ? gn::toOutputMesh(*r) : geo::Mesh());

    auto out = std::make_shared<gn::Instances>(src);
    out->references.clear();
    // Signed 0..1 per instance: the driver, then alternating bow direction by instance so a row doesn't all lean one way.
    auto amountOf = [&](size_t k) {
        double v = k < driverF.size() ? double(driverF[k]) : k < driverI.size() ? std::min(1.0, double(driverI[k]) / 2.0) : 0.5;
        return std::clamp(v, 0.0, 1.0);
    };
    if (unique)
    {
        for (size_t k = 0; k < src.size(); ++k)
        {
            const double v = amountOf(k) * ((k % 2) ? -1.0 : 1.0);
            out->references.push_back(gn::meshGeometry(deformed(bases[size_t(src.reference[k])], bow * v, bulge * std::abs(v))));
            out->reference[k] = int(k);
        }
    }
    else
    {
        // count variants per base, from -bow to +bow; each instance takes the one nearest its amount.
        for (size_t r = 0; r < bases.size(); ++r)
            for (int j = 0; j < count; ++j)
            {
                const double v = count == 1 ? 0.0 : -1.0 + 2.0 * j / double(count - 1);
                out->references.push_back(gn::meshGeometry(deformed(bases[r], bow * v, bulge * std::abs(v))));
            }
        for (size_t k = 0; k < src.size(); ++k)
        {
            const double v = amountOf(k) * ((k % 2) ? -1.0 : 1.0);
            const int j = count == 1 ? 0 : int(std::lround((v + 1.0) * 0.5 * (count - 1)));
            out->reference[k] = src.reference[k] * count + std::clamp(j, 0, count - 1);
        }
    }
    auto variant = out->attrs.getOrAdd<int32_t>("variant");
    for (size_t k = 0; k < out->size(); ++k)
        variant[k] = out->reference[k];
    Geometry g;
    g.instances = std::move(out);
    c.out("instances", gn::makeGeometry(std::move(g)));
}

const char* kContactsDoc = "Contacts from Settle Instances (points with impulse, slide, resting, contactNormal, load), or settled "
                           "instances, which count as resting at their origins with their mass.";
} // namespace

bool registerWearNodes(ext::Context& ctx)
{
    const gn::NodeDecl decls[] = {
        {.type = "ContactWear",
         .label = "Contact Wear",
         .category = "Stagehand",
         .doc = "Writes a 0 to 1 point attribute where things touched the mesh: patches where they rested, scuffs where they slid, "
                "chips where they hit hard. Feed it to vertex colour or a material's wear mask.",
         .sockets = {.inputs = {geoIn("geometry", "The worn mesh, in the same space as the contacts (world for Object Info in world)."),
                                geoIn("contacts", kContactsDoc),
                                in("radius", Socket::Float, 3.0, "How far around a contact the wear spreads, cm.", 1e-3),
                                in("resting", Socket::Float, 0.5, "Weight of resting contacts.", 0.0),
                                in("sliding", Socket::Float, 0.8, "Weight of sliding.", 0.0),
                                in("hits", Socket::Float, 1.0, "Weight of hard hits.", 0.0),
                                in("hitImpulse", Socket::Float, 200.0, "Impulse counted as a full-strength hit, kg cm/s.", 1e-6),
                                in("attribute", Socket::String, "wear", "Name of the point attribute written.")},
                     .outputs = {geoOut("geometry", "The mesh with the attribute.")}},
         .exec = contactWearExec},
        {.type = "DentAtContacts",
         .label = "Dent At Contacts",
         .category = "Stagehand",
         .doc = "Pushes a mesh's surface in where it was hit hard: a dinged corner, a dented tin. Hits under minImpulse leave no "
                "mark; at fullImpulse the dent is the full depth.",
         .sockets = {.inputs = {geoIn("geometry", "The mesh to dent, in the contacts' space."),
                                geoIn("contacts", kContactsDoc),
                                in("radius", Socket::Float, 2.0, "Dent size, cm.", 1e-3),
                                in("depth", Socket::Float, 0.4, "Deepest dent, cm.", 0.0),
                                in("minImpulse", Socket::Float, 50.0, "Softest hit that marks, kg cm/s.", 0.0),
                                in("fullImpulse", Socket::Float, 400.0, "Hit that dents the full depth, kg cm/s.", 0.0)},
                     .outputs = {geoOut("geometry", "The dented mesh.")}},
         .exec = dentExec},
        {.type = "SagUnderLoad",
         .label = "Sag Under Load",
         .category = "Stagehand",
         .doc = "Bends a board (a shelf, a plank) down under what rests on it, as a beam held at both ends: heavy things in the "
                "middle bend it most. The span runs along its longer horizontal side.",
         .sockets = {.inputs = {geoIn("geometry", "The board, in the contacts' space."),
                                geoIn("contacts", kContactsDoc),
                                in("amount", Socket::Float, 0.3, "Sag at the middle for 10 kg resting there, cm.", 0.0),
                                in("toLocalX", Socket::Vector, gn::json::array({1, 0, 0}), "Columns of the matrix taking contacts into the board's space."),
                                in("toLocalY", Socket::Vector, gn::json::array({0, 1, 0}), "Second column."),
                                in("toLocalZ", Socket::Vector, gn::json::array({0, 0, 1}), "Third column."),
                                in("toLocalOrigin", Socket::Vector, gn::json::array({0, 0, 0}), "Translation."),
                                in("ownWeight", Socket::Float, 0.0, "The board's own weight as an even load, kg.", 0.0)},
                     .outputs = {geoOut("geometry", "The bent board.")}},
         .exec = sagExec},
        {.type = "VariantPool",
         .label = "Variant Pool",
         .category = "Stagehand",
         .doc = "Gives instances bowed and bulged versions of their meshes, driven by an attribute (random, pose, impact...). "
                "pool: a few variants per mesh, shared, so it stays cheap to draw and ships as instanced meshes. unique: one "
                "mesh per instance. Adds variant.",
         .props = {{.id = "mode", .options = {"pool", "unique"}, .defaultValue = "pool", .doc = "pool shares a few variants; unique makes one per instance."}},
         .sockets = {.inputs = {geoIn("instances", "The instances to vary."),
                                in("variants", Socket::Int, 5, "Variants per mesh in pool mode.", 1, 64),
                                in("bow", Socket::Float, 0.6, "Most an item's middle bows sideways, cm.", 0.0),
                                in("bulge", Socket::Float, 0.15, "Most its faces swell at mid height, cm.", 0.0),
                                in("driver", Socket::String, "random", "Instance attribute (0 to 1) that picks how deformed each one is.")},
                     .outputs = {geoOut("instances", "The instances on their variants.")}},
         .exec = variantsExec},
    };
    for (const gn::NodeDecl& d : decls)
        if (!gn::registerDecl(ctx, d))
            return false;
    return true;
}
} // namespace stagehand
