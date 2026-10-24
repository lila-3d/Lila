#include "ui/DisplayAids.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "geo/Normals.h"
#include "ui/Actions.h"
#include "ui/MeshCache.h"
#include "ui/SpinScrub.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportWidget.h"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QWidgetAction>

#include <glm/gtc/matrix_inverse.hpp>

#include <array>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace lila::ui::displayAids
{
namespace
{
using geo::Index;
using geo::kInvalid;

/** Display-encoded 0..1 to the linear values world-space lines take. */
glm::vec4 linear(glm::vec3 srgb)
{
    auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    return {f(srgb.r), f(srgb.g), f(srgb.b), 1.0f};
}

struct Palette
{
    glm::vec3 faceNormal, vertexNormal, tangent, binormal, bounds;
    glm::vec3 hard, soft, border, nonManifold;
    glm::vec3 vertexId, edgeId, faceId, uvId;
};

const Palette& palette(bool monotone)
{
    static const Palette colour{{0.35f, 0.85f, 0.95f}, {0.95f, 0.85f, 0.30f}, {0.95f, 0.35f, 0.30f}, {0.40f, 0.60f, 1.00f}, {0.80f, 0.80f, 0.80f},
                                {1.00f, 1.00f, 1.00f}, {0.45f, 0.45f, 0.45f}, {1.00f, 0.60f, 0.20f}, {1.00f, 0.25f, 0.75f},
                                {0.95f, 0.90f, 0.55f}, {0.60f, 0.90f, 1.00f}, {0.70f, 1.00f, 0.60f}, {1.00f, 0.70f, 0.90f}};
    static const Palette gray{glm::vec3(0.85f), glm::vec3(0.65f), glm::vec3(0.75f), glm::vec3(0.45f), glm::vec3(0.80f),
                              glm::vec3(0.95f), glm::vec3(0.55f), glm::vec3(0.80f), glm::vec3(0.05f),
                              glm::vec3(0.95f), glm::vec3(0.82f), glm::vec3(0.70f), glm::vec3(0.58f)};
    return monotone ? gray : colour;
}

bool monotone()
{
    return PrefStore::get().value<bool>("general", viewportDisplay::kMonotone, false);
}

/** What one mesh snapshot needs for its aids, built the first time an aid asks and kept while the snapshot lives. */
struct Cached
{
    std::weak_ptr<const geo::Mesh> mesh;

    bool facesBuilt = false;
    std::vector<Index> faces; // valid faces, in id order
    std::vector<glm::vec3> faceCenter, faceNormal;
    std::vector<uint32_t> faceSlot; // per face capacity: index into the arrays above

    bool pointsBuilt = false;
    std::vector<glm::vec3> pointNormal;

    std::unique_ptr<EdgeClasses> edges;

    bool uvBuilt = false;
    std::vector<Index> uvFirst;

    bool boundsBuilt = false;
    geo::Bounds bounds;

    void buildFaces(const geo::Mesh& m)
    {
        if (std::exchange(facesBuilt, true))
            return;
        faceSlot.assign(m.faceCapacity(), UINT32_MAX);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
            {
                faceSlot[f] = uint32_t(faces.size());
                faces.push_back(f);
                faceCenter.emplace_back(m.faceCentroid(f));
                faceNormal.emplace_back(m.faceNormal(f));
            }
    }

    void buildPoints(const geo::Mesh& m)
    {
        if (std::exchange(pointsBuilt, true))
            return;
        pointNormal = geo::pointNormals(m);
    }
};

std::unordered_map<const geo::Mesh*, Cached>& cache()
{
    static std::unordered_map<const geo::Mesh*, Cached> map;
    return map;
}

Cached& cached(const scene::MeshPtr& mesh)
{
    Cached& c = cache()[mesh.get()];
    if (c.mesh.lock() != mesh)
    {
        c = Cached{};
        c.mesh = mesh;
    }
    return c;
}

struct Label
{
    QPointF at;
    uint32_t id;
    uint8_t kind; // 0 vertex, 1 edge, 2 face, 3 uv
};

void push(std::vector<render::LineVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
{
    out.push_back({a, color});
    out.push_back({b, color});
}
} // namespace

const std::vector<Info>& all()
{
    static const std::vector<Info> list = {
        {Aid::TwoSidedLighting, "twoSidedLighting", "Two-Sided Lighting",
         "Back faces are lit as if they faced the camera. Off, they light from behind and read dark.", true},
        {Aid::BackfaceTint, "backfaceTint", "Tint Back Faces",
         "Back faces draw tinted, so reversed normals stand out (gray in Monotone).", false},
        {Aid::FaceNormals, "faceNormals", "Face Normals", "A line out of each face center along its normal.", false},
        {Aid::VertexNormals, "vertexNormals", "Vertex Normals",
         "A line out of each vertex along the normal it shades with; split at hard edges.", false},
        {Aid::Tangents, "tangents", "Tangents and Binormals", "Each vertex's tangent and binormal from its first UV set.", false},
        {Aid::BoundingBoxes, "boundingBoxes", "Bounding Boxes", "Each mesh's bounding box in its own space.", false},
        {Aid::VertexIds, "vertexIds", "Vertex IDs", "The index of each vertex facing the camera.", false},
        {Aid::EdgeIds, "edgeIds", "Edge IDs", "The index of each edge facing the camera.", false},
        {Aid::FaceIds, "faceIds", "Face IDs", "The index of each face facing the camera.", false},
        {Aid::UvIds, "uvIds", "UV IDs", "The index of each shared UV, beside the vertex it sits on.", false},
        {Aid::HardEdges, "hardEdges", "Hard Edges", "Highlights the edges that split shading.", false},
        {Aid::SoftEdges, "softEdges", "Soft Edges", "Highlights the edges shading runs smoothly across.", false},
        {Aid::BorderEdges, "borderEdges", "Border Edges", "Highlights edges with a face on one side only.", false},
        {Aid::NonManifold, "nonManifold", "Non-Manifold Geometry",
         "Highlights edges shared by more than two faces and vertices where separate fans of faces meet.", false},
    };
    return list;
}

const Info& info(Aid a)
{
    return all()[size_t(a)];
}

const Info* find(std::string_view id)
{
    for (const Info& i : all())
        if (id == i.id)
            return &i;
    return nullptr;
}

std::string prefKey(int panel, Aid a)
{
    return "/viewport/aids/panel" + std::to_string(panel) + "/" + info(a).id;
}

bool on(int panel, Aid a)
{
    return PrefStore::get().value<bool>("general", prefKey(panel, a), info(a).fallback);
}

void setOn(int panel, Aid a, bool value)
{
    PrefStore::get().set("general", prefKey(panel, a), value);
}

double normalLength()
{
    return std::max(1e-3, PrefStore::get().value<double>("general", kNormalLength, kDefaultNormalLength));
}

std::optional<bool> actionState(const std::string& id, const ViewportWidget* view)
{
    if (!view || !id.starts_with("viewport.aid."))
        return std::nullopt;
    const Info* i = find(std::string_view(id).substr(13));
    if (!i)
        return std::nullopt;
    return on(view->hudPanel(), i->aid);
}

QWidgetAction* normalLengthRow(QMenu* menu)
{
    auto* holder = new QWidget(menu);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(8, 2, 8, 2);
    row->setSpacing(6);
    row->addWidget(new QLabel("Normal Length", holder));
    auto* spin = new QDoubleSpinBox(holder);
    spin->setRange(0.01, 10000.0);
    spin->setDecimals(2);
    spin->setSingleStep(0.5);
    spin->setSuffix(" cm");
    spin->setKeyboardTracking(false);
    spin->setToolTip("World length of drawn normals, tangents and binormals");
    setSpinDefault(spin, kDefaultNormalLength);
    spin->setValue(normalLength());
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, holder, [](double v) { PrefStore::get().set("general", kNormalLength, v); });
    row->addWidget(spin, 1);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}

void fillMenu(QMenu* menu, ViewportWidget* view)
{
    const int panel = view ? view->hudPanel() : 0;
    auto rows = [&](const char* title, std::initializer_list<Aid> aids) {
        menu->addSection(title);
        for (Aid aid : aids)
        {
            const std::string action = std::string("viewport.aid.") + info(aid).id;
            QAction* a = menu->addAction(QString::fromUtf8(info(aid).label));
            a->setCheckable(true);
            a->setChecked(on(panel, aid));
            a->setToolTip(QString::fromStdString(actions::tooltip(action)));
            QObject::connect(a, &QAction::triggered, menu, [panel, aid] { setOn(panel, aid, !on(panel, aid)); });
        }
    };
    menu->setToolTipsVisible(true);
    rows("Lighting", {Aid::TwoSidedLighting, Aid::BackfaceTint});
    rows("Normals", {Aid::FaceNormals, Aid::VertexNormals, Aid::Tangents});
    menu->addAction(normalLengthRow(menu));
    rows("Components", {Aid::BoundingBoxes, Aid::VertexIds, Aid::EdgeIds, Aid::FaceIds, Aid::UvIds});
    rows("Edges", {Aid::HardEdges, Aid::SoftEdges, Aid::BorderEdges, Aid::NonManifold});
}

void registerActions(std::function<ViewportWidget*()> active)
{
    for (const Info& i : all())
    {
        Aid aid = i.aid;
        actions::add({.id = std::string("viewport.aid.") + i.id,
                      .label = i.label,
                      .category = "Display",
                      .run =
                          [active, aid] {
                              if (ViewportWidget* v = active())
                                  setOn(v->hudPanel(), aid, !on(v->hudPanel(), aid));
                          },
                      .doc = std::string(i.doc) + " Toggles it in the active view.",
                      .python = "lila.prefs.set(\"general\", \"" + prefKey(0, aid) + "\", " + (i.fallback ? "False" : "True") + ")"});
    }
}

glm::vec4 backfaceTint(bool mono)
{
    return mono ? glm::vec4(0.10f, 0.10f, 0.10f, 0.75f) : glm::vec4(0.78f, 0.14f, 0.12f, 0.6f);
}

void apply(render::FrameState& s, int panel)
{
    s.twoSidedLighting = on(panel, Aid::TwoSidedLighting);
    s.backfaceTint = on(panel, Aid::BackfaceTint) ? backfaceTint(monotone()) : glm::vec4(0.0f);
}

bool anyDrawn(int panel)
{
    for (const Info& i : all())
        if (i.aid != Aid::TwoSidedLighting && i.aid != Aid::BackfaceTint && on(panel, i.aid))
            return true;
    return false;
}

EdgeClasses classifyEdges(const geo::Mesh& mesh)
{
    LILA_ZONE();

    EdgeClasses out;
    const geo::Attribute* hardAttr = mesh.edgeAttrs().find(geo::attr::kHard);
    std::span<const bool> hard = hardAttr ? hardAttr->values<bool>() : std::span<const bool>();
    std::vector<uint8_t> onNonManifold(mesh.pointCapacity(), 0);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        const Index faces = mesh.edgeFaceCount(e);
        if (faces == 1)
            out.border.push_back(e);
        else if (faces > 2)
        {
            out.nonManifold.push_back(e);
            onNonManifold[mesh.edgePoint(e, 0)] = onNonManifold[mesh.edgePoint(e, 1)] = 1;
        }
        else if (faces == 2)
            (e < hard.size() && hard[e] ? out.hard : out.soft).push_back(e);
    }
    // A point is non-manifold where its faces fall into more than one fan joined across its two-face edges.
    std::vector<Index> fanFaces, parent;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p) || mesh.pointEdge(p) == kInvalid)
            continue;
        if (onNonManifold[p])
        {
            out.nonManifoldPoints.push_back(p);
            continue;
        }
        fanFaces.clear();
        mesh.forEachPointCorner(p, [&](Index c) { fanFaces.push_back(mesh.cornerFace(c)); });
        if (fanFaces.size() < 2)
            continue;
        parent.resize(fanFaces.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = Index(i);
        auto root = [&](Index i) {
            while (parent[i] != i)
                i = parent[i] = parent[parent[i]];
            return i;
        };
        auto slot = [&](Index f) { return Index(std::find(fanFaces.begin(), fanFaces.end(), f) - fanFaces.begin()); };
        mesh.forEachPointEdge(p, [&](Index e) {
            if (mesh.edgeFaceCount(e) != 2)
                return;
            Index c = mesh.edgeCorner(e);
            Index a = slot(mesh.cornerFace(c)), b = slot(mesh.cornerFace(mesh.cornerRadial(c)));
            if (a < parent.size() && b < parent.size())
                parent[root(a)] = root(b);
        });
        size_t fans = 0;
        for (size_t i = 0; i < parent.size(); ++i)
            fans += root(Index(i)) == Index(i) ? 1 : 0;
        if (fans > 1)
            out.nonManifoldPoints.push_back(p);
    }
    return out;
}

std::vector<Index> uvIds(const geo::Mesh& mesh, std::vector<Index>& ids, int uvSet)
{
    LILA_ZONE();

    ids.assign(mesh.cornerCapacity(), kInvalid);
    std::vector<Index> first;
    const geo::Attribute* attr = mesh.cornerAttrs().find(geo::attr::uvName(uvSet));
    std::span<const glm::vec2> uv = attr ? attr->values<glm::vec2>() : std::span<const glm::vec2>();
    if (uv.size() < mesh.cornerCapacity())
        return first;
    std::vector<Index> here; // ids made at the current point
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        here.clear();
        mesh.forEachPointCorner(p, [&](Index c) {
            for (Index id : here)
                if (glm::all(glm::lessThan(glm::abs(uv[first[id]] - uv[c]), glm::vec2(1e-6f))))
                {
                    ids[c] = id;
                    return;
                }
            ids[c] = Index(first.size());
            here.push_back(ids[c]);
            first.push_back(c);
        });
    }
    return first;
}

size_t strideFor(size_t count, size_t budget)
{
    if (budget == 0 || count <= budget)
        return 1;
    return (count + budget - 1) / budget;
}

void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view, int panel, const std::vector<MeshEntry>& meshes, double dpr)
{
    LILA_ZONE();

    std::array<bool, size_t(Aid::Count)> aid{};
    bool any = false;
    for (const Info& i : all())
        if (i.aid != Aid::TwoSidedLighting && i.aid != Aid::BackfaceTint)
            any |= aid[size_t(i.aid)] = on(panel, i.aid);
    // Edits make a new snapshot each time, so what was built for the old ones goes as soon as they do.
    std::erase_if(cache(), [](const auto& kv) { return kv.second.mesh.expired(); });
    if (!any || meshes.empty())
        return;
    auto is = [&](Aid a) { return aid[size_t(a)]; };
    const Palette& pal = palette(monotone());
    const float length = float(normalLength());
    const glm::vec3 eye(view.eye);
    const glm::vec3 forward = -glm::vec3(glm::transpose(glm::dmat3(view.view))[2]);
    const glm::mat4 viewProj(view.viewProj());
    const bool edgesShown = is(Aid::HardEdges) || is(Aid::SoftEdges) || is(Aid::BorderEdges) || is(Aid::NonManifold);
    const bool labels = is(Aid::VertexIds) || is(Aid::EdgeIds) || is(Aid::FaceIds) || is(Aid::UvIds);

    // One stride for everything, so a dense mesh thins out evenly instead of starving the others.
    size_t want = 0;
    for (const MeshEntry& m : meshes)
        if (m.mesh)
            want += (is(Aid::FaceNormals) ? m.mesh->numFaces() : 0) + (is(Aid::VertexNormals) ? m.mesh->numCorners() : 0) +
                    (is(Aid::Tangents) ? 2 * m.mesh->numCorners() : 0) + (edgesShown ? m.mesh->numEdges() : 0);
    const size_t stride = strideFor(want, kMaxSegments);

    Nearest<Label> nearest(kMaxLabels);
    std::vector<QPointF> nonManifoldPoints;
    for (const MeshEntry& m : meshes)
    {
        if (!m.mesh || m.mesh->empty())
            continue;
        const geo::Mesh& mesh = *m.mesh;
        Cached& c = cached(m.mesh);
        const glm::mat4 world(m.world);
        const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(world));
        const glm::mat4 mvp = viewProj * world;
        auto at = [&](const glm::vec3& v) { return glm::vec3(world * glm::vec4(v, 1.0f)); };
        auto dir = [&](const glm::vec3& n) {
            glm::vec3 w = normalMatrix * n;
            float l = glm::length(w);
            return l > 1e-12f ? w / l : w;
        };
        auto facing = [&](const glm::vec3& worldPos, const glm::vec3& worldNormal) {
            return glm::dot(worldNormal, view.ortho ? forward : worldPos - eye) < 0.0f;
        };
        auto depthOf = [&](const glm::vec3& worldPos) { return view.ortho ? glm::dot(worldPos - eye, forward) : glm::length(worldPos - eye); };
        auto screen = [&](const glm::vec3& local) -> std::optional<QPointF> {
            glm::vec4 clip = mvp * glm::vec4(local, 1.0f);
            if (clip.w <= 1e-6f)
                return std::nullopt;
            double x = (clip.x / clip.w * 0.5 + 0.5) * view.width, y = (clip.y / clip.w * 0.5 + 0.5) * view.height;
            if (x < 0.0 || y < 0.0 || x > view.width || y > view.height)
                return std::nullopt;
            return QPointF(x, y);
        };
        // A label is offered only while it could still make the cut, before the projection that costs the most.
        auto offer = [&](const glm::vec3& local, const glm::vec3& worldNormal, uint32_t id, uint8_t kind, QPointF nudge = {}) {
            glm::vec3 w = at(local);
            float d = depthOf(w);
            if (!nearest.wants(d) || !facing(w, worldNormal))
                return;
            if (std::optional<QPointF> px = screen(local))
                nearest.offer(d, {*px + nudge, id, kind});
        };

        if (is(Aid::FaceNormals) || is(Aid::FaceIds) || is(Aid::EdgeIds) || is(Aid::UvIds))
            c.buildFaces(mesh);
        if (is(Aid::FaceNormals))
        {
            const glm::vec4 color = linear(pal.faceNormal);
            for (size_t i = 0; i < c.faces.size(); i += stride)
            {
                glm::vec3 base = at(c.faceCenter[i]);
                push(s.lines, base, base + dir(c.faceNormal[i]) * length, color);
            }
        }
        if (is(Aid::VertexNormals) || is(Aid::Tangents))
            if (auto rm = renderMeshFor(m.mesh, is(Aid::Tangents)))
            {
                const glm::vec4 normal = linear(pal.vertexNormal), tangent = linear(pal.tangent), binormal = linear(pal.binormal);
                for (size_t v = 0; v < rm->vertices.size(); v += stride)
                {
                    const render::MeshVertex& mv = rm->vertices[v];
                    glm::vec3 base = at(mv.pos), n = dir(mv.normal);
                    if (is(Aid::VertexNormals))
                        push(s.lines, base, base + n * length, normal);
                    if (is(Aid::Tangents) && glm::dot(glm::vec3(mv.tangent), glm::vec3(mv.tangent)) > 1e-12f)
                    {
                        glm::vec3 t = glm::normalize(glm::vec3(world * glm::vec4(glm::vec3(mv.tangent), 0.0f)));
                        glm::vec3 b = glm::cross(n, t) * (mv.tangent.w < 0.0f ? -1.0f : 1.0f);
                        push(s.lines, base, base + t * length, tangent);
                        push(s.lines, base, base + b * length, binormal);
                    }
                }
            }
        if (is(Aid::BoundingBoxes))
        {
            if (!std::exchange(c.boundsBuilt, true))
                c.bounds = mesh.bounds();
            if (c.bounds.valid())
            {
                const glm::vec4 color = linear(pal.bounds);
                glm::vec3 lo(c.bounds.min), hi(c.bounds.max);
                auto corner = [&](int i) { return at({i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z}); };
                for (int i = 0; i < 8; ++i)
                    for (int bit : {1, 2, 4})
                        if (!(i & bit))
                            push(s.lines, corner(i), corner(i | bit), color);
            }
        }
        if (edgesShown)
        {
            if (!c.edges)
                c.edges = std::make_unique<EdgeClasses>(classifyEdges(mesh));
            auto edges = [&](const std::vector<Index>& list, const glm::vec3& srgb) {
                const glm::vec4 color = linear(srgb);
                for (size_t i = 0; i < list.size(); i += stride)
                    push(s.highlightEdges, at(mesh.position(mesh.edgePoint(list[i], 0))), at(mesh.position(mesh.edgePoint(list[i], 1))), color);
            };
            if (is(Aid::SoftEdges))
                edges(c.edges->soft, pal.soft);
            if (is(Aid::HardEdges))
                edges(c.edges->hard, pal.hard);
            if (is(Aid::BorderEdges))
                edges(c.edges->border, pal.border);
            if (is(Aid::NonManifold))
            {
                edges(c.edges->nonManifold, pal.nonManifold);
                for (Index pt : c.edges->nonManifoldPoints)
                {
                    if (nonManifoldPoints.size() >= kMaxLabels)
                        break;
                    if (std::optional<QPointF> px = screen(mesh.position(pt)))
                        nonManifoldPoints.push_back(*px);
                }
            }
        }
        if (!labels)
            continue;
        if (is(Aid::VertexIds))
        {
            c.buildPoints(mesh);
            for (Index pt = 0; pt < mesh.pointCapacity(); ++pt)
                if (mesh.pointValid(pt))
                    offer(mesh.position(pt), dir(c.pointNormal[pt]), pt, 0, QPointF(6, -4));
        }
        if (is(Aid::FaceIds))
            for (size_t i = 0; i < c.faces.size(); ++i)
                offer(c.faceCenter[i], dir(c.faceNormal[i]), c.faces[i], 2);
        if (is(Aid::EdgeIds))
            for (Index e = 0; e < mesh.edgeCapacity(); ++e)
            {
                if (!mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid)
                    continue;
                // An edge faces the camera when any of its faces does; their mean normal stands in for that.
                glm::vec3 n(0.0f);
                mesh.forEachEdgeCorner(e, [&](Index corner) { n += c.faceNormal[c.faceSlot[mesh.cornerFace(corner)]]; });
                glm::vec3 mid = (glm::vec3(mesh.position(mesh.edgePoint(e, 0))) + glm::vec3(mesh.position(mesh.edgePoint(e, 1)))) * 0.5f;
                offer(mid, dir(n), e, 1);
            }
        if (is(Aid::UvIds))
        {
            std::vector<Index> perCorner;
            if (!std::exchange(c.uvBuilt, true))
                c.uvFirst = uvIds(mesh, perCorner);
            for (size_t id = 0; id < c.uvFirst.size(); ++id)
            {
                Index corner = c.uvFirst[id];
                uint32_t slot = c.faceSlot[mesh.cornerFace(corner)];
                // Pulled a little into its face, so the UVs split at a seam don't sit on top of each other.
                glm::vec3 pos = glm::mix(glm::vec3(mesh.position(mesh.cornerPoint(corner))), c.faceCenter[slot], 0.25f);
                offer(pos, dir(c.faceNormal[slot]), uint32_t(id), 3);
            }
        }
    }
    s.highlightEdgeWidth = viewportDisplay::hairlineWidth(2.0, dpr);

    if (!nonManifoldPoints.empty())
    {
        const glm::vec4 color(pal.nonManifold, 1.0f);
        for (QPointF px : nonManifoldPoints)
            p.disc(px, 4.0f, color, 12);
    }
    std::vector<Label> shown = nearest.take();
    if (shown.empty())
        return;
    const glm::vec4 colors[4] = {glm::vec4(pal.vertexId, 1.0f), glm::vec4(pal.edgeId, 1.0f), glm::vec4(pal.faceId, 1.0f), glm::vec4(pal.uvId, 1.0f)};
    p.setShadow(true);
    // Nearest last, so it draws over farther labels it overlaps.
    for (auto it = shown.rbegin(); it != shown.rend(); ++it)
        p.text(it->at, QString::number(it->id), colors[it->kind], it->kind == 0 ? OverlayPainter::Left : OverlayPainter::Center);
    p.setShadow(false);
}
} // namespace lila::ui::displayAids
