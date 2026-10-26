#include "gn/InstanceView.h"

#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <map>
#include <mutex>

namespace lila::gn
{
namespace
{
struct Store
{
    std::mutex mutex;
    std::map<scene::NodeId, InstanceViewPtr> views;
};

Store& store()
{
    // Never destroyed: the scene graph's static nodes clear their entries during exit, after this would be gone.
    static Store* s = new Store;
    return *s;
}

bool hasAnonymous(const geo::Mesh& m)
{
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
        for (const std::string& n : m.attributes(geo::Domain(d)).names())
            if (n.starts_with("__"))
                return true;
    return false;
}

/** The mesh as the output path would clean it, shared as is when nothing needs cleaning. */
scene::MeshPtr cleaned(const std::shared_ptr<const geo::Mesh>& m)
{
    if (!m->hasDeleted() && !hasAnonymous(*m))
        return m;
    geo::Mesh copy = *m;
    stripAnonymous(copy);
    copy.compact();
    return std::make_shared<const geo::Mesh>(std::move(copy));
}

struct Collected
{
    std::vector<const geo::Mesh*> order;
    std::map<const geo::Mesh*, std::pair<std::shared_ptr<const geo::Mesh>, std::vector<glm::mat4>>> parts;
};

void collect(const Instances& inst, const glm::dmat4& xf, Collected& out, int depth)
{
    // Instances can nest but never cycle; the cap only guards a malformed geometry.
    if (depth > 32)
        return;
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[r])
            continue;
        const Geometry& ref = *inst.references[r];
        const glm::dmat4 m = xf * inst.transforms[k];
        if (ref.mesh && ref.mesh->numFaces() > 0)
        {
            auto [it, added] = out.parts.try_emplace(ref.mesh.get());
            if (added)
            {
                it->second.first = ref.mesh;
                out.order.push_back(ref.mesh.get());
            }
            it->second.second.push_back(glm::mat4(m));
        }
        if (ref.instances)
            collect(*ref.instances, m, out, depth + 1);
    }
}
} // namespace

InstanceViewPtr makeInstanceView(const Geometry& g, scene::MeshPtr output)
{
    LILA_ZONE();

    if (!g.instances || g.instances->size() == 0)
        return nullptr;
    Collected c;
    collect(*g.instances, glm::dmat4(1.0), c, 0);
    if (c.order.empty())
        return nullptr;
    auto view = std::make_shared<InstanceView>();
    view->output = std::move(output);
    if (g.mesh && g.mesh->numFaces() > 0)
        view->base = cleaned(g.mesh);
    for (const geo::Mesh* key : c.order)
    {
        auto& [mesh, transforms] = c.parts[key];
        view->instanceCount += transforms.size();
        view->parts.push_back({cleaned(mesh), std::make_shared<const std::vector<glm::mat4>>(std::move(transforms))});
    }
    return view;
}

void setInstanceView(scene::NodeId modifier, InstanceViewPtr view)
{
    std::lock_guard lock(store().mutex);
    store().views[modifier] = std::move(view);
}

void clearInstanceView(scene::NodeId modifier)
{
    std::lock_guard lock(store().mutex);
    store().views.erase(modifier);
}

InstanceViewPtr instanceView(scene::NodeId modifier)
{
    std::lock_guard lock(store().mutex);
    auto it = store().views.find(modifier);
    return it != store().views.end() ? it->second : nullptr;
}

InstanceViewPtr instanceViewFor(scene::Graph& g, scene::NodeId mesh)
{
    scene::Node* n = g.node(mesh);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    auto src = g.source({mesh, scene::MeshNode::kInMesh, -1});
    if (!src)
        return nullptr;
    InstanceViewPtr view = instanceView(src->node);
    if (!view || view->output != n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
        return nullptr;
    return view;
}
} // namespace lila::gn
