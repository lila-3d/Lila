#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "scene/MeshBoolean.h"

namespace lila::gn::nodes
{
void registerBooleanNode()
{
    registerDecl({
        .type = "MeshBoolean",
        .label = "Mesh Boolean",
        .category = "Mesh",
        .doc = "Union, difference or intersection of closed meshes (Manifold), with the seam cleaned up. Difference subtracts "
               "every mesh B from mesh A.",
        .props = {prop("operation", {"difference", "union", "intersect"}, "difference")},
        .sockets = {.inputs = {geoIn("meshA"), multiIn("meshB"), in("cleanup", T::Bool, true, "Weld slivers and stray points along the seam."),
                               in("splitNgons", T::Bool, true, "Cut the n-gons along the seam into quads that carry on the edge loops."),
                               in("filletWidth", T::Float, 0.0, "Width of a rounded strip along the seam, cm; 0 keeps it sharp."),
                               in("filletSegments", T::Int, 2, "Rows of faces across the fillet.")},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::string op = c.prop("operation");
                std::vector<Geometry> parts;
                auto add = [&](const GeometryPtr& g, bool first) {
                    Geometry r = realize(*g);
                    if (r.mesh && r.mesh->numFaces() > 0)
                        parts.push_back(std::move(r));
                    else if (first && op == "difference")
                        return false; // nothing to subtract from
                    return true;
                };
                bool any = add(c.geometry("meshA"), true);
                if (any)
                    for (const GeometryPtr& b : c.geometries("meshB"))
                        add(b, false);
                if (parts.empty())
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                std::vector<scene::BooleanOperand> operands;
                for (const Geometry& g : parts)
                    operands.push_back({g.mesh.get(), glm::dmat4(1.0)});
                scene::BooleanOptions opts{.op = *scene::booleanOpFromName(op),
                                           .cleanup = c.b("cleanup"),
                                           .splitNgons = c.b("splitNgons"),
                                           .filletWidth = std::max(0.0, c.f("filletWidth")),
                                           .filletSegments = int(std::clamp<int64_t>(c.i("filletSegments"), 1, 64))};
                scene::BooleanResult r = scene::meshBoolean(operands, glm::dmat4(1.0), opts);
                if (!r.ok())
                    throw ExecError("an input isn't a closed solid");
                c.out("mesh", meshGeometry(std::move(r.mesh)));
            },
    });
}
} // namespace lila::gn::nodes
