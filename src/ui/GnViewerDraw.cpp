#include "ui/GnViewerDraw.h"

#include "core/Profile.h"
#include "gn/Viewer.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/ViewportHooks.h"

namespace lila::ui
{
namespace
{
constexpr float kFillAlpha = 0.85f;
const glm::vec3 kPlain(0.75f);

void drawViewers(render::FrameState& s, OverlayPainter&, const ViewInfo&)
{
    LILA_ZONE();

    scene::Graph& g = scene::Graph::current();
    for (const auto& [modifier, data] : gn::viewerSnapshots())
    {
        if (!data || !data->mesh || !g.node(modifier))
            continue;
        scene::NodeId meshId = scene::modifiers::meshOf(g, modifier);
        scene::Node* meshNode = g.node(meshId);
        glm::dmat4 world = meshNode ? meshNode->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
        const geo::Mesh& m = *data->mesh;
        auto at = [&](geo::Index i) -> glm::vec3 {
            if (!data->hasValues || i >= data->values.size())
                return kPlain;
            return gn::viewerColor(*data, data->values[i]);
        };
        auto wp = [&](geo::Index p) { return glm::vec3(world * glm::dvec4(m.position(p), 1.0)); };
        // Per-element colour of a corner of a face, by the domain the values live on.
        auto cornerColor = [&](geo::Index f, geo::Index c) {
            switch (data->domain)
            {
            case gn::Domain::Face: return at(f);
            case gn::Domain::Corner: return at(c);
            case gn::Domain::Point: return at(m.cornerPoint(c));
            default: return kPlain;
            }
        };
        bool anyFace = false;
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            anyFace = true;
            std::vector<geo::Index> corners;
            m.forEachFaceCorner(f, [&](geo::Index c) { corners.push_back(c); });
            for (size_t k = 1; k + 1 < corners.size(); ++k)
                for (geo::Index c : {corners[0], corners[k], corners[k + 1]})
                    s.tris3d.push_back({wp(m.cornerPoint(c)), glm::vec4(cornerColor(f, c), kFillAlpha)});
        }
        for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e))
                continue;
            glm::vec3 col = data->domain == gn::Domain::Edge ? at(e) : glm::vec3(0.1f);
            float alpha = data->domain == gn::Domain::Edge ? 1.0f : 0.5f;
            s.linesOnTop.push_back({wp(m.edgePoint(e, 0)), glm::vec4(col, alpha)});
            s.linesOnTop.push_back({wp(m.edgePoint(e, 1)), glm::vec4(col, alpha)});
        }
        if (anyFace)
            continue;
        // Point clouds and loose points: a small cross each, sized from the point spread.
        glm::dvec3 lo(1e30), hi(-1e30);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                lo = glm::min(lo, m.position(p));
                hi = glm::max(hi, m.position(p));
            }
        double r = std::max(0.2, glm::length(hi - lo) * 0.004);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            glm::vec4 col(data->domain == gn::Domain::Point ? at(p) : kPlain, 1.0f);
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::dvec3 d(0.0);
                d[axis] = r;
                s.linesOnTop.push_back({glm::vec3(world * glm::dvec4(m.position(p) - d, 1.0)), col});
                s.linesOnTop.push_back({glm::vec3(world * glm::dvec4(m.position(p) + d, 1.0)), col});
            }
        }
    }
}
} // namespace

void registerGnViewerDrawer() { viewportHooks::addDrawer("lila.gnViewer", drawViewers); }
} // namespace lila::ui
