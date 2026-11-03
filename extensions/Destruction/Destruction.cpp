// Destruction: the Rigid Body Solver geometry node, a Jolt rigid-body simulation of pieces (instances) held together by
// links (two-point curves), stepped and cached by the base simulation framework through gn/Stateful.h.

#include "Destruction.h"
#include "PieceTransform.h"
#include "Pieces.h"
#include "RigidWorld.h"

#include <RigidScene.h>

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Log.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>

using namespace lila;

namespace destruction
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using gn::SimInputs;
using T = gn::SocketType;

constexpr uint64_t kSolverVersion = 3;

double readFloat(const geo::AttributeSet& set, std::string_view name, size_t i, double fallback)
{
    const geo::Attribute* a = set.find(name);
    if (!a || i >= a->size())
        return fallback;
    switch (a->type())
    {
    case geo::AttrType::Float: return std::isnan(a->values<float>()[i]) ? fallback : a->values<float>()[i];
    case geo::AttrType::Int: return a->values<int32_t>()[i];
    case geo::AttrType::Bool: return a->values<bool>()[i] ? 1.0 : 0.0;
    default: return fallback;
    }
}

glm::dvec3 readVec3(const geo::AttributeSet& set, std::string_view name, size_t i, const glm::dvec3& fallback)
{
    const geo::Attribute* a = set.find(name);
    if (!a || i >= a->size() || a->type() != geo::AttrType::Float3)
        return fallback;
    return glm::dvec3(a->values<glm::vec3>()[i]);
}

/** Every point of a geometry, instances realized: what a piece's hull is built from. */
std::vector<glm::dvec3> allPoints(const Geometry& g)
{
    const Geometry r = gn::realize(g);
    std::vector<glm::dvec3> out;
    for (const std::shared_ptr<const geo::Mesh>& m : {r.mesh, r.points})
        if (m)
            for (geo::Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                    out.push_back(m->position(p));
    if (r.curves)
        out.insert(out.end(), r.curves->positions.begin(), r.curves->positions.end());
    return out;
}

std::vector<glm::dvec3> triangles(const Geometry& g)
{
    const Geometry r = gn::realize(g);
    std::vector<glm::dvec3> out;
    if (!r.mesh)
        return out;
    const geo::Mesh& m = *r.mesh;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
            for (geo::Index p : {pts[0], pts[k], pts[k + 1]})
                out.push_back(m.position(p));
    }
    return out;
}

Settings readSettings(const SimInputs& in)
{
    Settings s;
    s.startFrame = in.i("startFrame");
    s.substeps = int(std::clamp<int64_t>(in.i("substeps"), 1, 32));
    s.gravity = in.v("gravity");
    s.ground = in.b("ground");
    s.groundHeight = in.f("groundHeight");
    s.damping = std::max(0.0, in.f("damping"));
    s.sleep = in.b("sleep");
    s.strengthScale = std::max(0.0, in.f("strength"));
    s.blastCenter = in.v("blastCenter");
    s.blastSpeed = in.f("blastSpeed");
    s.blastRadius = std::max(1e-3, in.f("blastRadius"));
    s.blastFrame = in.i("blastFrame");
    s.guideStiffness = std::max(0.0, in.f("guideStiffness"));
    return s;
}
} // namespace

SceneDesc describeGeometry(const GeometryPtr& pieces, const GeometryPtr& links, const GeometryPtr& colliders, const GeometryPtr& blasts,
                           const Settings& settings, const Material& material, bool shapes)
{
    SceneDesc d;
    d.settings = settings;
    const double density = material.density, friction = material.friction, bounce = material.bounce;
    if (pieces->instances)
    {
        const gn::Instances& inst = *pieces->instances;
        std::map<std::pair<int, std::array<double, 3>>, std::shared_ptr<const std::vector<glm::dvec3>>> hulls;
        std::map<int, std::vector<glm::dvec3>> refPoints;
        d.pieces.resize(inst.size());
        for (size_t k = 0; k < inst.size(); ++k)
        {
            PieceDesc& p = d.pieces[k];
            const Decomposed t = decompose(inst.transforms[k]);
            p.position = t.position;
            p.rotation = t.rotation;
            p.scale = t.scale;
            p.density = readFloat(inst.attrs, "density", k, density);
            p.friction = readFloat(inst.attrs, "friction", k, friction);
            p.bounce = readFloat(inst.attrs, "bounce", k, bounce);
            p.active = readFloat(inst.attrs, "active", k, 1.0) != 0.0;
            p.guide = std::clamp(readFloat(inst.attrs, "guide", k, 0.0), 0.0, 1.0);
            p.velocity = readVec3(inst.attrs, "velocity", k, glm::dvec3(0.0));
            p.angularVelocity = readVec3(inst.attrs, "angularVelocity", k, glm::dvec3(0.0));
            if (!shapes)
                continue;
            const int ref = inst.reference[k];
            auto key = std::make_pair(ref, std::array<double, 3>{t.scale.x, t.scale.y, t.scale.z});
            auto& hull = hulls[key];
            if (!hull)
            {
                auto [it, fresh] = refPoints.try_emplace(ref);
                if (fresh && ref >= 0 && size_t(ref) < inst.references.size() && inst.references[size_t(ref)])
                    it->second = allPoints(*inst.references[size_t(ref)]);
                auto pts = std::make_shared<std::vector<glm::dvec3>>(it->second);
                for (glm::dvec3& q : *pts)
                    q *= t.scale;
                hull = pts;
            }
            p.hull = hull;
        }
    }
    else if (!pieces->empty())
        throw std::invalid_argument("pieces must be instances: connect Fracture, or Mesh to Pieces for separate parts");

    if (links->curves)
    {
        const gn::CurveSet& c = *links->curves;
        d.links.reserve(c.numCurves());
        for (size_t k = 0; k < c.numCurves(); ++k)
        {
            if (c.curveSize(k) == 0)
                continue;
            LinkDesc l;
            l.a = int(readFloat(c.curveAttrs, "pieceA", k, -1.0));
            l.b = int(readFloat(c.curveAttrs, "pieceB", k, -1.0));
            l.type = LinkType(std::clamp(int(readFloat(c.curveAttrs, "type", k, 0.0)), 0, 2));
            l.strength = std::max(0.0, readFloat(c.curveAttrs, "strength", k, 1000.0));
            l.area = std::max(0.0, readFloat(c.curveAttrs, "area", k, 0.0));
            l.anchor = c.curvePoints(k)[0];
            d.links.push_back(l);
        }
    }

    for (const std::shared_ptr<const geo::Mesh>& m : {blasts->points, blasts->mesh})
    {
        if (!m)
            continue;
        const geo::AttributeSet& attrs = m->pointAttrs();
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                d.blasts.push_back({m->position(p), readFloat(attrs, "speed", p, 1500.0), readFloat(attrs, "radius", p, 300.0),
                                    int64_t(std::llround(readFloat(attrs, "frame", p, double(d.settings.startFrame))))});
    }

    if (colliders->instances)
    {
        const gn::Instances& inst = *colliders->instances;
        for (size_t k = 0; k < inst.size(); ++k)
        {
            ColliderDesc c;
            const Decomposed t = decompose(inst.transforms[k]);
            c.position = t.position;
            c.rotation = t.rotation;
            c.moving = true;
            const int ref = inst.reference[k];
            if (shapes && ref >= 0 && size_t(ref) < inst.references.size() && inst.references[size_t(ref)])
            {
                auto tris = std::make_shared<std::vector<glm::dvec3>>(triangles(*inst.references[size_t(ref)]));
                for (glm::dvec3& q : *tris)
                    q *= t.scale;
                c.triangles = tris;
            }
            d.colliders.push_back(std::move(c));
        }
    }
    if (colliders->mesh)
    {
        ColliderDesc c;
        Geometry meshOnly;
        meshOnly.mesh = colliders->mesh;
        if (shapes)
            c.triangles = std::make_shared<std::vector<glm::dvec3>>(triangles(meshOnly));
        d.colliders.push_back(std::move(c));
    }
    return d;
}

namespace
{
/** The simulation's scene from what reached the node. */
SceneDesc describe(const SimInputs& in, bool shapes)
{
    const Material material{std::max(1e-3, in.f("density")), std::max(0.0, in.f("friction")), std::clamp(in.f("bounce"), 0.0, 1.0)};
    return describeGeometry(in.geometry("pieces"), in.geometry("links"), in.geometry("colliders"), in.geometry("blasts"), readSettings(in), material,
                            shapes);
}

class RigidSim final : public gn::NodeSim
{
public:
    Setup configure(const SimInputs& latest) override
    {
        Setup s;
        s.startFrame = latest.i("startFrame");
        s.substeps = int(std::clamp<int64_t>(latest.i("substeps"), 1, 32));
        s.paramsHash = kSolverVersion;
        return s;
    }

    void reset(const SimInputs& in, std::vector<uint8_t>& state) override
    {
        mWorld = std::make_unique<RigidWorld>(describe(in, true));
        mWorld->save(state);
    }

    void step(const SimInputs& in, std::vector<uint8_t>& state, const scene::sim::StepInfo& at) override
    {
        const StateView view(state);
        if (!view.valid())
            throw std::runtime_error("the cached simulation state is unreadable");
        SceneDesc d = describe(in, false);
        if (!mWorld || view.header().serial != mWorld->serial() || view.header().step != mWorld->step())
        {
            if (!mWorld || !mWorld->matches(d) || mWorld->origin() != view.origin())
            {
                d = describe(in, true);
                mWorld = std::make_unique<RigidWorld>(d, view.origin());
            }
            mWorld->restore(view);
        }
        mWorld->advance(d, at.index, at.dt);
        const int64_t substeps = std::max<int64_t>(1, view.header().substeps);
        mWorld->save(state, at.index % substeps == 0);
    }

private:
    std::unique_ptr<RigidWorld> mWorld;
};

/** The node's outputs from a frame's state: pieces moved, links moved and marked broken, and the breaks so far as points. */
void solverOutput(ExecContext& c, const std::vector<uint8_t>* state)
{
    GeometryPtr pieces = c.geometry("pieces");
    GeometryPtr links = c.geometry("links");
    c.out("pieces", pieces);
    c.out("links", links);
    c.out("breaks", gn::makeGeometry({}));
    if (!state || !pieces->instances)
        return;
    const StateView view(*state);
    const gn::Instances& in = *pieces->instances;
    if (!view.valid() || view.header().bodies != in.size())
        return;

    auto moved = std::make_shared<gn::Instances>(in);
    std::vector<glm::dmat4> delta(in.size(), glm::dmat4(1.0));
    moved->attrs.resize(in.size());
    std::span<glm::vec3> velocity = moved->attrs.getOrAdd<glm::vec3>("velocity");
    std::span<int32_t> active = moved->attrs.getOrAdd<int32_t>("active");
    for (size_t k = 0; k < in.size(); ++k)
    {
        const BodyRecord b = view.body(k);
        const Decomposed t = decompose(in.transforms[k]);
        moved->transforms[k] = compose(view.position(k), view.rotation(k), t.scale);
        delta[k] = moved->transforms[k] * glm::inverse(in.transforms[k]);
        velocity[k] = glm::vec3(b.velocity[0], b.velocity[1], b.velocity[2]) / float(kM);
        active[k] = b.active;
    }
    Geometry out = *pieces;
    out.instances = moved;
    c.out("pieces", gn::makeGeometry(std::move(out)));

    if (!links->curves || view.header().links == 0)
        return;
    auto curves = std::make_shared<gn::CurveSet>(*links->curves);
    curves->curveAttrs.resize(curves->numCurves());
    std::span<int32_t> broken = curves->curveAttrs.getOrAdd<int32_t>("broken");
    geo::Mesh breaks;
    std::span<int32_t> frameOut, aOut, bOut;
    std::span<float> impulseOut;
    struct Break
    {
        glm::dvec3 at;
        int32_t frame, a, b;
        float impulse;
    };
    std::vector<Break> found;
    size_t link = 0;
    for (size_t k = 0; k < curves->numCurves(); ++k)
    {
        if (curves->curveSize(k) == 0)
            continue;
        if (link >= view.header().links)
            break;
        const LinkRecord r = view.link(link++);
        const int a = int(readFloat(curves->curveAttrs, "pieceA", k, -1.0));
        const int b = int(readFloat(curves->curveAttrs, "pieceB", k, -1.0));
        const size_t first = curves->offsets[k], last = curves->offsets[k + 1];
        for (size_t p = first; p < last; ++p)
        {
            const int owner = p == first || b < 0 ? a : b;
            if (owner >= 0 && size_t(owner) < delta.size())
                curves->positions[p] = glm::dvec3(delta[size_t(owner)] * glm::dvec4(curves->positions[p], 1.0));
        }
        broken[k] = r.brokenStep >= 0 ? 1 : 0;
        if (r.brokenStep >= 0)
            found.push_back({curves->positions[first], int32_t(view.frameOfStep(r.brokenStep)), a, b, r.impulse});
    }
    Geometry linkGeo = *links;
    linkGeo.curves = curves;
    c.out("links", gn::makeGeometry(std::move(linkGeo)));
    for (const Break& f : found)
        breaks.addPoint(f.at);
    frameOut = breaks.pointAttrs().getOrAdd<int32_t>("frame");
    aOut = breaks.pointAttrs().getOrAdd<int32_t>("pieceA");
    bOut = breaks.pointAttrs().getOrAdd<int32_t>("pieceB");
    impulseOut = breaks.pointAttrs().getOrAdd<float>("impulse");
    for (size_t k = 0; k < found.size(); ++k)
    {
        frameOut[k] = found[k].frame;
        aOut[k] = found[k].a;
        bOut[k] = found[k].b;
        impulseOut[k] = found[k].impulse;
    }
    c.out("breaks", gn::pointGeometry(std::move(breaks)));
}

gn::SocketDecl in(std::string id, T t, gn::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

gn::SocketDecl in(std::string id, T t, gn::json def, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    gn::SocketDecl s = in(std::move(id), t, std::move(def), std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}

gn::SocketDecl geo(std::string id, std::string doc) { return {.id = std::move(id), .type = T::Geometry, .doc = std::move(doc)}; }

gn::json vec(double x, double y, double z) { return gn::json::array({x, y, z}); }

gn::NodeDecl solverDecl()
{
    auto stateful = std::make_shared<gn::StatefulDecl>();
    stateful->create = [] { return std::make_unique<RigidSim>(); };
    stateful->output = solverOutput;
    return {
        .type = "RigidBodySolver",
        .label = "Rigid Body Solver",
        .category = "Destruction",
        .doc = "Simulates pieces falling, colliding and breaking apart, the same on every machine. Pieces are instances (from "
               "Fracture or Mesh to Pieces); links are two-point curves (from Connect Pieces) that hold pieces together until "
               "the force through them passes their strength; a link's strength can change over time, and 0 cuts it. Pieces with "
               "active = 0 follow their own transform until it turns 1. The simulation is cached frame by frame; changing anything upstream re-simulates from the first frame it "
               "affects.",
        .sockets = {.inputs = {geo("pieces", "The pieces, as instances."), geo("links", "Links holding pieces together, as two-point curves."),
                               geo("colliders", "Things pieces hit: instances move with their transforms, a mesh stays put."),
                               geo("blasts", "Explosions, as points with frame, speed (cm/s) and radius (cm) attributes: join Explosion nodes here."),
                               in("startFrame", T::Int, 1, "Frame the simulation starts on."),
                               in("substeps", T::Int, 4, "Physics steps per frame. More is more accurate and slower.", 1, 32),
                               in("gravity", T::Vector, vec(0, -981, 0), "cm/s2."), in("ground", T::Bool, true, "A floor at Ground Height."),
                               in("groundHeight", T::Float, 0.0, "cm."),
                               in("density", T::Float, 2400.0, "kg/m3 (2400 concrete, 700 wood, 2500 glass). A piece's density attribute overrides it.", 1.0),
                               in("friction", T::Float, 0.6, "How much pieces grip as they slide.", 0.0),
                               in("bounce", T::Float, 0.1, "0 to 1.", 0.0, 1.0), in("damping", T::Float, 0.05, "Air drag on moving pieces.", 0.0),
                               in("sleep", T::Bool, true, "Pieces that come to rest stop being simulated."),
                               in("strength", T::Float, 1.0, "Multiplies every link's strength: lower breaks more.", 0.0),
                               in("blastCenter", T::Vector, vec(0, 0, 0), "Where a blast pushes pieces out from, cm."),
                               in("blastSpeed", T::Float, 0.0, "Speed given to pieces at the blast centre, cm/s; 0 for none."),
                               in("blastRadius", T::Float, 100.0, "Pieces this far away or more aren't pushed, cm.", 1e-3),
                               in("blastFrame", T::Int, 1, "Frame the blast happens on."),
                               in("guideStiffness", T::Float, 2.0,
                                  "How fast pieces with a guide attribute (0 to 1) are pulled toward their own animated transform, Hz.", 0.0)},
                    .outputs = {geo("pieces", "The pieces where the simulation put them; velocity and active attributes."),
                                geo("links", "The links, moved with their pieces; broken = 1 once broken."),
                                geo("breaks", "A point where each link broke so far: frame, impulse, pieceA, pieceB.")}},
        .stateful = stateful,
    };
}
} // namespace

bool registerNodes(ext::Context& ctx)
{
    lila::physics::initJolt();
    return gn::registerDecl(ctx, solverDecl()) != nullptr && registerDamageNodes(ctx);
}
} // namespace destruction

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!destruction::registerNodes(ctx))
        return 1;
    destruction::registerCommands(ctx);
    destruction::registerFreeze(ctx);
    destruction::registerUi(ctx);
    destruction::registerSounds(ctx);
    return 0;
}
