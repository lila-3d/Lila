#include "curve/CurveNodes.h"
#include "curve/EnvironmentNodes.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using T = ArgType;

void registerLiveSurfaceCommands()
{
    ops::registerCommand({
        .name = "conformCurve",
        .doc = "Keep curves on a mesh surface: their points are placed on it, lifted by offset, whenever the curve or the mesh "
               "changes, so moving points slides them along it (Maya's Make Live for curves). release=True lets them go again, "
               "back to where they were last moved. Returns the curves.",
        .positionalDoc = "curves (default: the selected curves)",
        .flags = {nodeFlag({"mesh", "m", T::String, nullptr, "The surface (default: the selected mesh)."}, "mesh"),
                  {"offset", "o", T::Float, nullptr, "Lift off the surface along its normal, cm (default: keep the curve's own, 0 at first)."},
                  {"release", "r", T::Bool, false, "Stop conforming instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<Node*> curves;
            Node* mesh = nullptr;
            auto take = [&](Node* n) {
                if (n && n->type().isA("curve") && std::ranges::find(curves, n) == curves.end())
                    curves.push_back(n);
                else if (n && n->type().isA("mesh") && !mesh)
                    mesh = n;
            };
            for (const json& p : a.positional)
            {
                Node* n = g.find(p.get<std::string>());
                if (!n)
                    throw CommandError("conformCurve: no node named '" + p.get<std::string>() + "'");
                take(n);
            }
            if (a.positional.empty())
                for (NodeId id : g.selection())
                    take(g.node(id));
            if (curves.empty())
                throw CommandError("conformCurve: select curves (and the mesh to keep them on)");
            const bool release = a.get<bool>("release", false);
            if (a.has("mesh"))
            {
                mesh = g.find(a.get<std::string>("mesh"));
                if (!mesh || !mesh->type().isA("mesh"))
                    throw CommandError("conformCurve: " + a.get<std::string>("mesh") + " is not a mesh");
            }
            if (!release && !mesh)
                throw CommandError("conformCurve: give mesh= or select the mesh to keep the curves on");

            json names = json::array();
            for (Node* c : curves)
            {
                PlugRef surface{c->id(), CurveShape::kLiveSurface, -1}, matrix{c->id(), CurveShape::kLiveSurfaceMatrix, -1};
                if (release)
                {
                    // What the curve shows becomes what it stores, so letting go leaves it where it is.
                    g.evaluate();
                    if (g.source(surface))
                    {
                        PlugValue shown = g.value({c->id(), CurveShape::kLocal, -1});
                        g.disconnect(surface);
                        if (g.source(matrix))
                            g.disconnect(matrix);
                        PlugRef create{c->id(), CurveShape::kCreate, -1};
                        if (g.source(create))
                            g.disconnect(create);
                        g.setValue(create, shown);
                    }
                }
                else
                {
                    g.connect({mesh->id(), MeshNode::kOutMesh, -1}, surface, true);
                    g.connect({mesh->id(), Transform::kWorldMatrix, -1}, matrix, true);
                    if (a.has("offset"))
                        g.setValue({c->id(), CurveShape::kSurfaceOffset, -1}, a.get<double>("offset"));
                }
                names.push_back(c->path());
            }
            return names;
        },
    });
}
} // namespace lila::curve
